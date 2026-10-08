// SPDX-License-Identifier: Apache-2.0
//
// jsonrpc/engine.hpp — one JSON-RPC 2.0 connection, as an Elm model.
//
//   The engine is the bookkeeping of a connection and nothing else: which of
//   OUR requests are waiting, under which ids, until when. It is a value with
//   pure transitions. It does no I/O, owns no thread, reads no clock and runs
//   no handler:
//
//       Effects step(Engine&, Event)
//
//       Event   = Received{line}      a frame arrived
//               | Tick{now}           time passed (the host's clock)
//               | Cancel{id}          give up on one of our requests
//               | Closed{reason}      the transport ended
//
//       Effects = send           frames to write, in order
//               + completed      our requests, answered (or failed)
//               + calls          the peer asks us; answer with reply()
//               + notifications  the peer tells us
//
//   Starting a request is the one transition that also returns a value (the
//   id it minted): request<M>(engine, params, deadline).
//
//   The host runs it the way an Elm runtime runs update(): feed events,
//   perform the effects, arm a timer for next_deadline(). The engine is
//   single-owner by design; the host serialises access. See agentty's
//   docs/PROTOCOL_LIBRARIES.md.
#pragma once

#include <chrono>
#include <cstdint>
#include <expected>
#include <iterator>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

#include "message.hpp"
#include "method.hpp"

namespace jsonrpc {

// Time is an argument, never read. steady_clock only names the type.
using Clock    = std::chrono::steady_clock;
using Deadline = Maybe<Clock::time_point>;   // Nothing: wait forever

// ── Events ──────────────────────────────────────────────────────────────────

struct Received { std::string line; };
struct Tick     { Clock::time_point now; };
struct Cancel   { RequestId id; };
struct Closed   { std::string reason = "connection closed"; int code = errc::ConnectionLost; };

using Event = Sum<Received, Tick, Cancel, Closed>;

// ── Effects ─────────────────────────────────────────────────────────────────

// One of our requests, finished. `method` says which request it was; decode
// `outcome` with result<M>().
struct Completed {
    RequestId                     id;
    std::string                   method;
    std::expected<Json, RpcError> outcome;
};

struct Effects {
    List<std::string>  send;            // frames to write, in order
    List<Completed>    completed;       // our requests, answered or failed
    List<Call>         calls;           // the peer asks; answer with reply()
    List<Notification> notifications;   // the peer tells; nothing to answer

    Effects& operator+=(Effects o) {
        auto cat = [](auto& a, auto& b) {
            a.insert(a.end(), std::make_move_iterator(b.begin()), std::make_move_iterator(b.end()));
        };
        cat(send, o.send);
        cat(completed, o.completed);
        cat(calls, o.calls);
        cat(notifications, o.notifications);
        return *this;
    }
};

// ── The model ───────────────────────────────────────────────────────────────

class Engine {
public:
    Engine() = default;

    // Merged into every outbound request's params._meta without overriding a
    // key the caller set (MCP's stateless per-request metadata).
    void set_request_meta(Json meta) {
        meta_ = meta.is_object() ? std::move(meta) : Json::object();
    }

    [[nodiscard]] std::size_t pending() const noexcept { return pending_.size(); }
    [[nodiscard]] bool is_pending(RequestId id) const { return pending_.contains(id.value); }

    // When a Tick is next worth sending; Nothing when no request has a deadline.
    [[nodiscard]] Deadline next_deadline() const {
        Deadline next;
        for (const auto& [_, p] : pending_)
            if (p.deadline && (!next || *p.deadline < *next)) next = p.deadline;
        return next;
    }

private:
    struct Pending {
        std::string method;
        Deadline    deadline;
    };

    std::int64_t                              next_id_ = 1;
    std::unordered_map<std::int64_t, Pending> pending_;
    Json                                      meta_ = Json::object();

    friend Effects step(Engine&, Event);
    friend std::pair<RequestId, Effects> request_raw(Engine&, std::string_view, Json, Deadline);
    friend void settle(Engine&, RequestId, std::expected<Json, RpcError>, Effects&);
};

// ── Transitions ─────────────────────────────────────────────────────────────

// Start one of our requests. Returns its id and the frame to send.
[[nodiscard]] inline std::pair<RequestId, Effects>
request_raw(Engine& e, std::string_view method, Json params, Deadline deadline = Nothing) {
    const RequestId id{e.next_id_++};
    if (!e.meta_.empty()) {
        if (!params.is_object()) params = Json::object();
        Json& m = params["_meta"];
        if (!m.is_object()) m = Json::object();
        for (auto it = e.meta_.begin(); it != e.meta_.end(); ++it)
            if (!m.contains(it.key())) m[it.key()] = it.value();
    }
    e.pending_.emplace(id.value, Engine::Pending{std::string(method), deadline});
    Effects fx;
    fx.send.push_back(encode(Message{Call{Id{id.value}, std::string(method), std::move(params)}}));
    return {id, std::move(fx)};
}

// Typed: params must be M::params. Decode the completion with result<M>().
template <IsMethod M>
[[nodiscard]] std::pair<RequestId, Effects>
request(Engine& e, const typename M::params& params, Deadline deadline = Nothing) {
    return request_raw(e, M::name, to_json(params), deadline);
}

// Finish one pending request (if it is still pending) into `fx`.
inline void settle(Engine& e, RequestId id, std::expected<Json, RpcError> outcome, Effects& fx) {
    auto it = e.pending_.find(id.value);
    if (it == e.pending_.end()) return;   // unknown, finished, or given up
    fx.completed.push_back({id, std::move(it->second.method), std::move(outcome)});
    e.pending_.erase(it);
}

// The update function.
[[nodiscard]] inline Effects step(Engine& e, Event ev) {
    Effects fx;
    match(ev,
        [&](Received& r) {
            for (auto& parsed : parse(r.line)) {
                if (!parsed) {
                    fx.send.push_back(encode(parsed.error()));
                    continue;
                }
                match(*parsed,
                    [&](Call& c)         { fx.calls.push_back(std::move(c)); },
                    [&](Notification& n) { fx.notifications.push_back(std::move(n)); },
                    [&](Reply& rep) {
                        // Only ids we minted are integers we can match.
                        if (const auto* n = std::get_if<std::int64_t>(&rep.id))
                            settle(e, RequestId{*n}, std::move(rep.outcome), fx);
                    });
            }
        },
        [&](Tick& t) {
            List<std::int64_t> due;
            for (const auto& [id, p] : e.pending_)
                if (p.deadline && *p.deadline <= t.now) due.push_back(id);
            for (auto id : due)
                settle(e, RequestId{id}, std::unexpected(RpcError(errc::Timeout, "request timed out")), fx);
        },
        [&](Cancel& c) {
            settle(e, c.id, std::unexpected(RpcError(errc::Cancelled, "request cancelled")), fx);
        },
        [&](Closed& c) {
            List<std::int64_t> all;
            for (const auto& [id, _] : e.pending_) all.push_back(id);
            for (auto id : all)
                settle(e, RequestId{id}, std::unexpected(RpcError(c.code, c.reason)), fx);
        });
    return fx;
}

// ── Values to send that need no state ───────────────────────────────────────

template <IsNote N>
[[nodiscard]] std::string notify(const typename N::params& params) {
    return encode(Message{Notification{std::string(N::name), to_json(params)}});
}
[[nodiscard]] inline std::string notify_raw(std::string_view method, Json params = Json::object()) {
    return encode(Message{Notification{std::string(method), std::move(params)}});
}

// Answer one of the peer's calls.
[[nodiscard]] inline std::string reply(const Id& id, std::expected<Json, RpcError> outcome) {
    return encode(Message{Reply{id, std::move(outcome)}});
}
template <IsMethod M>
[[nodiscard]] std::string reply(const Id& id, const typename M::result& r) {
    return reply(id, to_json(r));
}

// A completion's outcome as M::result. A peer error and a decode failure
// both come back as RpcError.
template <IsMethod M>
[[nodiscard]] std::expected<typename M::result, RpcError> result(const Completed& c) {
    if (!c.outcome) return std::unexpected(c.outcome.error());
    try {
        if constexpr (std::is_same_v<typename M::result, Unit>) return Unit{};
        else return from_json<typename M::result>(*c.outcome);
    } catch (const CodecError& err) {
        return std::unexpected(RpcError(errc::InternalError,
            std::string("bad result for ") + std::string(M::name) + ": " + err.what()));
    }
}

}  // namespace jsonrpc
