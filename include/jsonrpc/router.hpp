// SPDX-License-Identifier: Apache-2.0
//
// jsonrpc/router.hpp — the peer's calls, dispatched by method type.
//
//   A Router<Ctx> maps methods to handlers and runs them synchronously, on
//   the caller's thread, inside the call:
//
//       Router<Session> r;
//       r.on<Initialize>([](Session& s, const InitializeParams& p) { return s.init(p); });
//       r.on<Cancelled>([](Session& s, const CancelledParams& p) { s.cancel(p.requestId); });
//       r.defer<Prompt>();     // the caller answers later, with reply<Prompt>(id, …)
//
//       for (auto& call : fx.calls)
//           if (auto frame = r.handle(session, call)) send(*frame);
//       for (auto& note : fx.notifications) r.handle(session, note);
//
//   The handler's signature is checked against the method's declaration:
//   on<M> requires f(Ctx&, const M::params&) -> M::result (or a Note's
//   f(Ctx&, const N::params&)). A handler can't take the wrong params or
//   return the wrong result.
//
//   handle(Call) answers: the result; a thrown RpcError; InvalidParams when
//   the params don't decode; InternalError for anything else thrown;
//   MethodNotFound for an unknown method. A deferred method returns Nothing
//   and the caller owns the reply.
//
//   The table is built once and then only read. No locks, no threads.
#pragma once

#include <concepts>
#include <functional>
#include <optional>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "engine.hpp"

namespace jsonrpc {

template <class Ctx>
class Router {
public:
    // ── registration ─────────────────────────────────────────────────────

    template <IsMethod M, class F>
        requires std::invocable<F&, Ctx&, const typename M::params&>
              && std::convertible_to<std::invoke_result_t<F&, Ctx&, const typename M::params&>,
                                     typename M::result>
    void on(F f) {
        calls_[std::string(M::name)] = [f = std::move(f)](Ctx& c, const Json& j) mutable -> Json {
            const auto p = decode<typename M::params>(j);
            if constexpr (std::is_same_v<typename M::result, Unit>) {
                (void)f(c, p);
                return Json::object();
            } else {
                return to_json<typename M::result>(f(c, p));
            }
        };
    }

    template <IsNote N, class F>
        requires std::invocable<F&, Ctx&, const typename N::params&>
    void on(F f) {
        notes_[std::string(N::name)] = [f = std::move(f)](Ctx& c, const Json& j) mutable {
            f(c, decode<typename N::params>(j));
        };
    }

    // Untyped escape hatches, for extension methods whose shape is not
    // declared ahead of time.
    void on_raw(std::string method, std::function<Json(Ctx&, const Json&)> f) {
        calls_[std::move(method)] = std::move(f);
    }
    void on_raw_note(std::string method, std::function<void(Ctx&, const Json&)> f) {
        notes_[std::move(method)] = std::move(f);
    }

    // A method the caller answers itself, later: handle() returns Nothing.
    template <IsMethod M> void defer() { deferred_.insert(std::string(M::name)); }
    void defer_raw(std::string method) { deferred_.insert(std::move(method)); }

    // ── dispatch ─────────────────────────────────────────────────────────

    [[nodiscard]] bool deferred(const std::string& method) const { return deferred_.contains(method); }

    // The reply frame for one call, or Nothing when the method is deferred.
    [[nodiscard]] Maybe<std::string> handle(Ctx& c, const Call& call) {
        if (deferred(call.method)) return Nothing;
        auto it = calls_.find(call.method);
        if (it == calls_.end())
            return reply(call.id, std::unexpected(
                RpcError(errc::MethodNotFound, "Method not found: " + call.method)));
        try {
            return reply(call.id, it->second(c, call.params));
        } catch (const RpcError& e) {
            return reply(call.id, std::unexpected(e));
        } catch (const CodecError& e) {
            return reply(call.id, std::unexpected(RpcError(errc::InvalidParams, e.what())));
        } catch (const std::exception& e) {
            return reply(call.id, std::unexpected(RpcError(errc::InternalError, e.what())));
        } catch (...) {
            return reply(call.id, std::unexpected(RpcError(errc::InternalError, "unknown exception")));
        }
    }

    // Run one notification's handler. A notification gets no answer, so an
    // unknown method or a throwing handler is ignored. Returns whether one ran.
    bool handle(Ctx& c, const Notification& n) {
        auto it = notes_.find(n.method);
        if (it == notes_.end()) return false;
        try { it->second(c, n.params); } catch (...) {}
        return true;
    }

private:
    template <class P>
    static P decode(const Json& j) {
        if constexpr (std::is_same_v<P, Json>) return j;
        else return from_json<P>(j);
    }

    std::unordered_map<std::string, std::function<Json(Ctx&, const Json&)>> calls_;
    std::unordered_map<std::string, std::function<void(Ctx&, const Json&)>> notes_;
    std::unordered_set<std::string>                                         deferred_;
};

}  // namespace jsonrpc
