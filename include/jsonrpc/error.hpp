// SPDX-License-Identifier: Apache-2.0
//
// jsonrpc/error.hpp — ids and errors.
#pragma once

#include <compare>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>

#include "codec.hpp"

namespace jsonrpc {

// ── Ids ─────────────────────────────────────────────────────────────────────
//
// Two different things, two different types:
//
//   Id         an id ON THE WIRE, as the peer sent it: a number or a string
//              (JSON-RPC 2.0 §4). We answer the peer's requests with it.
//   RequestId  an id WE minted for one of our own requests. Always an integer,
//              unique per engine, and the only key a response is matched on.
//
// Mixing them up (answering the peer with one of our ids, matching a peer's
// string id against our table) does not compile.

using Id = Sum<std::int64_t, std::string>;

struct RequestIdTag;
using RequestId = Newtype<RequestIdTag, std::int64_t>;

template <> struct CodecOf<Id> {
    static Codec<Id> get() {
        return {[](const Id& id) -> Json {
                    return match(id, [](std::int64_t n) { return Json(n); },
                                     [](const std::string& s) { return Json(s); });
                },
                [](const Json& j) -> Id {
                    if (j.is_number_integer())  return j.get<std::int64_t>();
                    if (j.is_number_unsigned()) return static_cast<std::int64_t>(j.get<std::uint64_t>());
                    if (j.is_string())          return j.get<std::string>();
                    throw CodecError("id must be an integer or a string");
                }};
    }
};

// ── Errors ──────────────────────────────────────────────────────────────────

namespace errc {
inline constexpr int ParseError     = -32700;
inline constexpr int InvalidRequest = -32600;
inline constexpr int MethodNotFound = -32601;
inline constexpr int InvalidParams  = -32602;
inline constexpr int InternalError  = -32603;
// The implementation-defined range (-32000..-32099). The protocols on top
// (MCP, ACP) define their own codes there too; these are the engine's.
inline constexpr int Timeout        = -32001;   // a request's deadline passed
inline constexpr int Cancelled      = -32002;   // given up locally
inline constexpr int ConnectionLost = -32003;   // the transport ended mid-flight
}  // namespace errc

// The wire error object. Also throwable: a handler throws one to answer with it.
struct RpcError : std::runtime_error {
    int  code;
    Json data;
    RpcError(int c, std::string message, Json d = Json())
        : std::runtime_error(std::move(message)), code(c), data(std::move(d)) {}
};

template <> struct CodecOf<RpcError> {
    static Codec<RpcError> get() {
        return {[](const RpcError& e) -> Json {
                    Json j = {{"code", e.code}, {"message", e.what()}};
                    if (!e.data.is_null()) j["data"] = e.data;
                    return j;
                },
                [](const Json& j) -> RpcError {
                    if (!j.is_object()) throw CodecError("error: expected object");
                    return {j.value("code", errc::InternalError),
                            j.value("message", std::string{"rpc error"}),
                            j.value("data", Json())};
                }};
    }
};

}  // namespace jsonrpc
