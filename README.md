# jsonrpc-cpp

JSON-RPC 2.0 as a passive C++23 library. Header-only, depends only on
[nlohmann/json](https://github.com/nlohmann/json).

It has no runtime: no threads, locks, atomics, futures, clocks or I/O. A
connection is a value with pure transitions, in the Elm shape:

```cpp
#include <jsonrpc/jsonrpc.hpp>
using namespace jsonrpc;

// A method is declared once, as a type.
using Initialize = Method<"initialize", InitParams, InitResult>;
using Cancelled  = Note<"notifications/cancelled", CancelledParams>;

Engine e;

// Our request: an id, and the frame to send.
auto [id, fx] = request<Initialize>(e, params, now + 30s);
write(fx.send);

// Events in, effects out. The host reads, writes and keeps the time.
Effects fx = step(e, Received{line});      // a frame arrived
fx += step(e, Tick{now});                  // time passed: deadlines fire
fx += step(e, Cancel{id});                 // give up locally
fx += step(e, Closed{"eof"});              // the transport ended

for (auto& c : fx.completed)               // our requests, answered
    if (c.method == Initialize::name) use(result<Initialize>(c));
for (auto& call : fx.calls)                // the peer asks us
    if (auto frame = router.handle(ctx, call)) write(*frame);
for (auto& note : fx.notifications)        // the peer tells us
    router.handle(ctx, note);
arm_timer(e.next_deadline());              // when to send the next Tick
```

## Pieces

| header | what |
|---|---|
| `core.hpp` | the algebra: `Unit`, `Maybe`, `List`, `Sum`, `Newtype`, `StaticString`, `match` |
| `codec.hpp` | `Codec<T>`: serialization as a fold (`record`, `sum_tagged`, `enum_codec`, …) |
| `error.hpp` | `Id` (the wire's), `RequestId` (ours), `RpcError`, `errc` |
| `message.hpp` | `Message = Call \| Notification \| Reply`, parsed once at the edge |
| `method.hpp` | `Method<"name", Params, Result>`, `Note<"name", Params>` |
| `engine.hpp` | `Engine`, `step(Engine&, Event) -> Effects`, `request<M>`, `result<M>` |
| `router.hpp` | `Router<Ctx>`: the peer's calls, dispatched by method type |
| `framing.hpp` | `LineSplitter`: a byte stream to lines |

## Guarantees

- **Single-owner.** An `Engine` is not shared between threads; the host
  serialises access. Nothing inside is locked or atomic.
- **Time is an argument.** Deadlines are values; `Tick{now}` advances them.
  No clock is read.
- **Types carry the protocol.** A handler registered for `Method<"m", P, R>`
  must take `P` and return `R`, or it does not compile. Our request ids
  (`RequestId`) and the peer's (`Id`) are different types.
- **Parsed once.** A frame becomes a `Message` at the edge; everything after
  is a total `match`. Malformed input is a value with the error reply it
  earns, not an exception.
- **No threads in the tests.** The tests build without `-pthread`, so a
  regression that needs a thread fails to link.

## Build

```sh
cmake -B build && cmake --build build && ctest --test-dir build
```

As a subproject: `add_subdirectory(jsonrpc-cpp)` and link `jsonrpc::jsonrpc`.
If the parent already defines `nlohmann_json::nlohmann_json`, that one is used.

Used by [mcp-cpp](https://github.com/1ay1/mcp-cpp) and
[acp-cpp](https://github.com/1ay1/acp-cpp), driven by
[agentty](https://github.com/1ay1/agentty).

License: Apache-2.0.
