// SPDX-License-Identifier: Apache-2.0
//
// jsonrpc/message.hpp — the wire message as a closed sum, parsed once.
//
//   Every JSON-RPC 2.0 envelope is exactly one of
//
//       Message = Call          (a request: method + params + id)
//               | Notification  (method + params, no id)
//               | Reply         (an id + result | error)
//
//   parse() decides which, once, at the edge. Everything after works on the
//   typed value with a total match() — nobody re-inspects Json for "has a
//   method? has an id? has a result?" again. A frame that is none of the
//   three is a Malformed value, not an exception.
//
//   encode() is the inverse; parse(encode(m)) == m.
#pragma once

#include <expected>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "error.hpp"

namespace jsonrpc {

struct Call {
    Id          id;
    std::string method;
    Json        params;   // object or array; {} when absent
    friend bool operator==(const Call&, const Call&) = default;
};

struct Notification {
    std::string method;
    Json        params;   // {} when absent
    friend bool operator==(const Notification&, const Notification&) = default;
};

// A reply to a call. `outcome` is the result or the error — never both,
// never neither, by construction.
struct Reply {
    Id                            id;
    std::expected<Json, RpcError> outcome;
    friend bool operator==(const Reply& a, const Reply& b) {
        if (a.id != b.id || a.outcome.has_value() != b.outcome.has_value()) return false;
        if (a.outcome) return *a.outcome == *b.outcome;
        return a.outcome.error().code == b.outcome.error().code
            && std::string_view{a.outcome.error().what()} == b.outcome.error().what()
            && a.outcome.error().data == b.outcome.error().data;
    }
};

using Message = Sum<Call, Notification, Reply>;

// What could not be parsed, and the error reply it deserves. `id` is known
// when the envelope got that far (an invalid request still carries its id).
struct Malformed {
    Maybe<Id> id;
    RpcError  error;
};

// One received frame: one message, or a batch (JSON-RPC 2.0 §6). Each
// element parses independently; a bad one doesn't spoil its neighbours.
using Parsed = List<std::expected<Message, Malformed>>;

namespace detail {

inline std::expected<Message, Malformed> parse_one(Json j) {
    if (!j.is_object() || j.value("jsonrpc", "") != "2.0") {
        Maybe<Id> id;
        if (j.is_object()) if (auto it = j.find("id"); it != j.end())
            try { id = from_json<Id>(*it); } catch (const CodecError&) {}
        return std::unexpected(Malformed{std::move(id),
            RpcError(errc::InvalidRequest, "missing or invalid 'jsonrpc' field")});
    }
    Maybe<Id> id;
    if (auto it = j.find("id"); it != j.end() && !it->is_null()) {
        try { id = from_json<Id>(*it); }
        catch (const CodecError& e) {
            return std::unexpected(Malformed{Nothing, RpcError(errc::InvalidRequest, e.what())});
        }
    }
    auto params = [&]() -> Json {
        auto p = j.find("params");
        if (p == j.end() || p->is_null()) return Json::object();
        return std::move(*p);
    };
    if (auto m = j.find("method"); m != j.end()) {
        if (!m->is_string())
            return std::unexpected(Malformed{std::move(id),
                RpcError(errc::InvalidRequest, "'method' must be a string")});
        std::string method = m->get<std::string>();
        if (id) return Call{std::move(*id), std::move(method), params()};
        return Notification{std::move(method), params()};
    }
    const auto result = j.find("result");
    const auto error  = j.find("error");
    if (id && (result != j.end()) != (error != j.end())) {
        if (result != j.end()) return Reply{std::move(*id), std::move(*result)};
        try {
            return Reply{std::move(*id), std::unexpected(from_json<RpcError>(*error))};
        } catch (const CodecError& e) {
            return Reply{std::move(*id), std::unexpected(RpcError(errc::InternalError, e.what()))};
        }
    }
    return std::unexpected(Malformed{std::move(id),
        RpcError(errc::InvalidRequest, "neither a call, a notification nor a reply")});
}

}  // namespace detail

// Parse one received frame. Unparsable JSON is one Malformed (ParseError);
// an empty batch is one Malformed (InvalidRequest).
[[nodiscard]] inline Parsed parse(std::string_view frame) {
    Json j;
    try {
        j = Json::parse(frame);
    } catch (const std::exception& e) {
        return {std::unexpected(Malformed{Nothing, RpcError(errc::ParseError, e.what())})};
    }
    if (!j.is_array()) return {detail::parse_one(std::move(j))};
    if (j.empty())
        return {std::unexpected(Malformed{Nothing, RpcError(errc::InvalidRequest, "empty batch")})};
    Parsed out;
    out.reserve(j.size());
    for (auto& el : j) out.push_back(detail::parse_one(std::move(el)));
    return out;
}

[[nodiscard]] inline Json to_wire(const Message& m) {
    return match(m,
        [](const Call& c) {
            return Json{{"jsonrpc", "2.0"}, {"id", to_json(c.id)},
                        {"method", c.method}, {"params", c.params}};
        },
        [](const Notification& n) {
            return Json{{"jsonrpc", "2.0"}, {"method", n.method}, {"params", n.params}};
        },
        [](const Reply& r) {
            Json env = {{"jsonrpc", "2.0"}, {"id", to_json(r.id)}};
            if (r.outcome) env["result"] = *r.outcome;
            else           env["error"]  = to_json(r.outcome.error());
            return env;
        });
}

// The frame for a message (one line of JSON, no trailing newline).
[[nodiscard]] inline std::string encode(const Message& m) { return to_wire(m).dump(); }

// The error reply a Malformed earns. JSON-RPC answers with id null when the
// id couldn't be read.
[[nodiscard]] inline std::string encode(const Malformed& bad) {
    Json env = {{"jsonrpc", "2.0"},
                {"id", bad.id ? to_json(*bad.id) : Json(nullptr)},
                {"error", to_json(bad.error)}};
    return env.dump();
}

}  // namespace jsonrpc
