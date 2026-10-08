// SPDX-License-Identifier: Apache-2.0
//
// jsonrpc/method.hpp — a method as a type: its name, its params, its result.
//
//   A protocol declares each method once:
//
//       using Initialize = Method<"initialize", InitializeParams, InitializeResult>;
//       using Cancelled  = Note<"notifications/cancelled", CancelledParams>;
//
//   and everything that touches the method is checked against that one
//   declaration: the engine's typed request<M>() takes exactly M::params and
//   its completion decodes exactly M::result; a Router<Ctx> handler for M
//   must take M::params and return M::result. A method's params and result
//   can't drift between the call site, the handler and the codec, because
//   there is only one place that says what they are.
#pragma once

#include <string_view>

#include "core.hpp"

namespace jsonrpc {

// A request: answered with Result (Unit when the reply is just {}).
template <StaticString Name, class Params, class Result>
struct Method {
    static constexpr std::string_view name = Name.view();
    using params = Params;
    using result = Result;
};

// A notification: no answer.
template <StaticString Name, class Params>
struct Note {
    static constexpr std::string_view name = Name.view();
    using params = Params;
};

template <class M>
concept IsMethod = requires {
    { M::name } -> std::convertible_to<std::string_view>;
    typename M::params;
    typename M::result;
};

template <class N>
concept IsNote = requires {
    { N::name } -> std::convertible_to<std::string_view>;
    typename N::params;
} && !IsMethod<N>;

}  // namespace jsonrpc
