// SPDX-License-Identifier: Apache-2.0
//
// jsonrpc/jsonrpc.hpp — the whole library.
//
//   core.hpp     the algebra: Unit, Maybe, List, Sum, Newtype, StaticString, match
//   codec.hpp    Codec<T>: serialization as a fold over the algebra
//   error.hpp    Id (the wire's), RequestId (ours), RpcError, errc
//   message.hpp  Message = Call | Notification | Reply, parsed once
//   method.hpp   Method<"name", Params, Result>, Note<"name", Params>
//   engine.hpp   Engine + step(Engine&, Event) -> Effects  (the Elm model)
//   router.hpp   Router<Ctx>: the peer's calls, dispatched by method type
//   framing.hpp  LineSplitter: bytes to lines
//
// A passive library: no threads, locks, clocks, futures, I/O or global
// mutable state. The host drives it.
#pragma once

#include "core.hpp"
#include "codec.hpp"
#include "error.hpp"
#include "message.hpp"
#include "method.hpp"
#include "engine.hpp"
#include "router.hpp"
#include "framing.hpp"
