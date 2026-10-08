// tests/jsonrpc_test.cpp — jsonrpc-cpp, single-threaded.
//
// Built WITHOUT -pthread and WITHOUT Threads::Threads: if the library ever
// needs a thread, this fails to link. The engine is driven by hand, the way a
// host drives it: events in, effects out, time as a value.

#include <jsonrpc/jsonrpc.hpp>

#include <chrono>
#include <cstdio>
#include <string>

using namespace jsonrpc;
using namespace std::chrono_literals;

namespace {

int g_fail = 0;
void check(bool ok, const char* what) {
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_fail;
}

// ── a tiny protocol, declared once ──────────────────────────────────────────
struct NameTag;
using Name = Newtype<NameTag, std::string>;

enum class Color { Red, Blue };

struct Point {
    std::int64_t x = 0;
    Maybe<std::int64_t> y;
    std::string label = "none";
    Json meta = Json::object();
    friend bool operator==(const Point&, const Point&) = default;
};

struct Circle { std::int64_t r = 0;    friend bool operator==(const Circle&, const Circle&) = default; };
struct Square { std::int64_t side = 0; friend bool operator==(const Square&, const Square&) = default; };
using Shape = Sum<Circle, Square>;

using Move  = Method<"move", Point, Point>;
using Fail  = Method<"fail", Unit, Unit>;
using Slow  = Method<"slow", Unit, std::int64_t>;
using Ping  = Note<"ping", Point>;

}  // namespace

template <> struct jsonrpc::CodecOf<Point> {
    static Codec<Point> get() {
        return record(required("x", &Point::x), optional("y", &Point::y),
                      defaulted("label", &Point::label, std::string{"none"}),
                      meta("_meta", &Point::meta));
    }
};
template <> struct jsonrpc::CodecOf<Color> {
    static Codec<Color> get() {
        return enum_codec(EnumMapping<Color>{Color::Red, "red"}, EnumMapping<Color>{Color::Blue, "blue"});
    }
};
template <> struct jsonrpc::CodecOf<Circle> {
    static Codec<Circle> get() { return record(required("r", &Circle::r)); }
};
template <> struct jsonrpc::CodecOf<Square> {
    static Codec<Square> get() { return record(required("side", &Square::side)); }
};
template <> struct jsonrpc::CodecOf<Shape> {
    static Codec<Shape> get() {
        return sum_tagged("kind", arm<Shape, Circle>("circle"), arm<Shape, Square>("square"));
    }
};

namespace {

// ── compile-time guarantees ─────────────────────────────────────────────────
struct Ctx { int calls = 0; std::string last; };
template <class M, class F>
concept Registrable = requires(Router<Ctx> r, F f) { r.template on<M>(f); };

static_assert(Registrable<Move, Point (*)(Ctx&, const Point&)>, "a matching handler registers");
static_assert(!Registrable<Move, Point (*)(Ctx&, const Circle&)>, "wrong params: rejected");
static_assert(!Registrable<Move, Circle (*)(Ctx&, const Point&)>, "wrong result: rejected");
static_assert(IsMethod<Move> && !IsNote<Move> && IsNote<Ping> && !IsMethod<Ping>, "methods and notes are distinct");
static_assert(!std::is_convertible_v<RequestId, Id>, "our id is not a wire id");
static_assert(!std::is_constructible_v<Cancel, Id>, "cancel takes only an id we minted");

template <class T>
bool round_trips(const T& v) { return from_json<T>(to_json(v)) == v; }

bool throws_codec(auto&& f) {
    try { f(); } catch (const CodecError&) { return true; }
    return false;
}

void codecs() {
    std::printf("codecs\n");
    check(round_trips(Point{1, 2, "a", Json::object()}), "record round-trips");
    check(!to_json(Point{1, Nothing, "none", Json::object()}).contains("y"), "Nothing is omitted, not null");
    check(!to_json(Point{}).contains("_meta"), "empty _meta is omitted");
    check(from_json<Point>(Json{{"x", 3}}).label == "none", "defaulted fills in when absent");
    check(throws_codec([] { (void)from_json<Point>(Json{{"y", 1}}); }), "missing required field throws");
    check(to_json(Color::Blue) == "blue" && from_json<Color>("red") == Color::Red, "enum maps labels");
    check(throws_codec([] { (void)from_json<Color>("green"); }), "unknown enum label throws");
    check(to_json(Shape{Circle{2}}) == Json{{"kind", "circle"}, {"r", 2}}, "sum carries its tag");
    check(round_trips(Shape{Square{4}}), "sum round-trips");
    check(throws_codec([] { (void)from_json<Shape>(Json{{"kind", "hexagon"}}); }), "unknown sum label throws");
    check(to_json(Name{"ada"}) == "ada" && from_json<Name>("ada") == Name{"ada"}, "newtype encodes as its carrier");
    check(round_trips(List<Name>{Name{"a"}, Name{"b"}}), "list of newtypes round-trips");
    check(round_trips(Id{std::int64_t{7}}) && round_trips(Id{std::string{"x"}}), "wire ids: number or string");
    check(throws_codec([] { (void)from_json<Id>(Json(1.5)); }), "a fractional id is not an id");
    check(&codec<Point>() == &codec<Point>(), "codec<T>() is built once");
}

void messages() {
    std::printf("messages\n");
    const Message samples[] = {
        Call{Id{std::int64_t{1}}, "m", Json{{"a", 1}}},
        Call{Id{std::string{"s"}}, "m", Json::object()},
        Notification{"n", Json{{"b", 2}}},
        Reply{Id{std::int64_t{3}}, Json(5)},
        Reply{Id{std::int64_t{4}}, std::unexpected(RpcError(-32000, "no", Json{{"why", 1}}))},
    };
    bool all = true;
    for (const auto& m : samples) {
        auto back = parse(encode(m));
        all = all && back.size() == 1 && back[0] && *back[0] == m;
    }
    check(all, "parse(encode(m)) == m for every kind");
    check(!parse("{x")[0] && parse("{x")[0].error().error.code == errc::ParseError, "bad JSON is ParseError");
    auto ver = parse(R"({"id":3,"method":"x"})");
    check(!ver[0] && ver[0].error().id == Maybe<Id>{Id{std::int64_t{3}}}
              && ver[0].error().error.code == errc::InvalidRequest,
          "no jsonrpc field: InvalidRequest, id kept");
    check(!parse("[]")[0], "an empty batch is malformed");
    check(!parse(R"({"jsonrpc":"2.0","id":1,"result":1,"error":{"code":1,"message":"m"}})")[0],
          "result AND error is malformed");
    check(!parse(R"({"jsonrpc":"2.0","id":4})")[0], "neither kind is malformed");
    auto batch = parse(R"([{"jsonrpc":"2.0","method":"a"}, 7])");
    check(batch.size() == 2 && batch[0] && !batch[1], "a bad element doesn't spoil its batch");
}

void requests_and_replies() {
    std::printf("requests and replies\n");
    Engine e;
    auto [a, fa] = request<Move>(e, Point{1, Nothing, "p", Json::object()});
    auto [b, fb] = request_raw(e, "sum", Json{{"a", 2}});
    check(a.value == 1 && b.value == 2, "ids are minted in order");
    check(fa.send.size() == 1 && Json::parse(fa.send[0])["params"]["x"] == 1, "a typed request encodes its params");
    check(e.pending() == 2, "both are pending");

    auto fx = step(e, Received{R"([{"jsonrpc":"2.0","id":2,"result":5},)"
                               R"({"jsonrpc":"2.0","id":1,"result":{"x":9}}])"});
    check(fx.completed.size() == 2, "a batch completes both, out of order");
    const auto moved = result<Move>(fx.completed[1]);
    check(fx.completed[1].method == "move" && moved && moved->x == 9, "result<M> decodes the typed result");
    check(e.pending() == 0, "nothing left pending");

    auto [c, fc] = request<Move>(e, Point{});
    auto bad = step(e, Received{R"({"jsonrpc":"2.0","id":3,"result":{"y":1}})"});
    auto decoded = result<Move>(bad.completed[0]);
    check(!decoded && decoded.error().code == errc::InternalError, "a result that doesn't decode is an error");
    auto [d, fd] = request<Move>(e, Point{});
    auto err = step(e, Received{R"({"jsonrpc":"2.0","id":4,"error":{"code":-32602,"message":"bad"}})"});
    check(!result<Move>(err.completed[0]) && result<Move>(err.completed[0]).error().code == errc::InvalidParams,
          "a peer error comes back as that error");

    check(step(e, Received{R"({"jsonrpc":"2.0","id":4,"result":1})"}).completed.empty(),
          "a duplicate reply is ignored");
    check(step(e, Received{R"({"jsonrpc":"2.0","id":"abc","result":1})"}).completed.empty(),
          "a reply to an id we never minted is ignored");
    (void)c; (void)d; (void)fc; (void)fd; (void)fb;
}

void peer_calls_and_router() {
    std::printf("peer calls and the router\n");
    Router<Ctx> r;
    r.on<Move>([](Ctx& c, const Point& p) { ++c.calls; return Point{p.x + 1, p.y, p.label, Json::object()}; });
    r.on<Fail>([](Ctx&, const Unit&) -> Unit { throw RpcError(-32000, "nope"); });
    r.on<Ping>([](Ctx& c, const Point& p) { c.last = std::to_string(p.x); });
    r.defer<Slow>();

    Engine e;
    Ctx ctx;
    auto fx = step(e, Received{
        R"([{"jsonrpc":"2.0","id":"r1","method":"move","params":{"x":1}},)"
        R"({"jsonrpc":"2.0","id":7,"method":"missing"},)"
        R"({"jsonrpc":"2.0","id":8,"method":"move","params":{"y":1}},)"
        R"({"jsonrpc":"2.0","id":9,"method":"fail"},)"
        R"({"jsonrpc":"2.0","id":10,"method":"slow"},)"
        R"({"jsonrpc":"2.0","method":"ping","params":{"x":4}}])"});
    check(fx.calls.size() == 5 && fx.notifications.size() == 1, "a batch splits into calls and notifications");

    auto ok = Json::parse(*r.handle(ctx, fx.calls[0]));
    check(ok["id"] == "r1" && ok["result"]["x"] == 2 && ctx.calls == 1, "a handler answers with its typed result");
    check(Json::parse(*r.handle(ctx, fx.calls[1]))["error"]["code"] == errc::MethodNotFound,
          "an unknown method answers MethodNotFound");
    check(Json::parse(*r.handle(ctx, fx.calls[2]))["error"]["code"] == errc::InvalidParams,
          "params that don't decode answer InvalidParams");
    check(Json::parse(*r.handle(ctx, fx.calls[3]))["error"]["code"] == -32000,
          "a thrown RpcError answers with that error");
    check(!r.handle(ctx, fx.calls[4]), "a deferred method leaves the reply to the caller");
    check(Json::parse(reply<Slow>(fx.calls[4].id, 42))["result"] == 42, "the caller answers later, typed");
    check(r.handle(ctx, fx.notifications[0]) && ctx.last == "4", "a notification runs its handler");
    check(!r.handle(ctx, Notification{"unknown", Json::object()}), "an unknown notification is ignored");
    check(Json::parse(notify<Ping>(Point{5, Nothing, "x", Json::object()}))["params"]["x"] == 5,
          "a typed notification encodes its params");
}

void time_cancel_close() {
    std::printf("time, cancel, close\n");
    Engine e;
    const auto t0 = Clock::time_point{} + 1h;   // time is a value; no clock is read
    auto [a, fa] = request_raw(e, "a", Json::object(), t0 + 100ms);
    auto [b, fb] = request_raw(e, "b", Json::object(), t0 + 50ms);
    auto [c, fc] = request_raw(e, "c", Json::object());
    check(e.next_deadline() == t0 + 50ms, "next_deadline is the nearest");
    check(step(e, Tick{t0 + 10ms}).completed.empty(), "nothing expires early");
    auto late = step(e, Tick{t0 + 60ms});
    check(late.completed.size() == 1 && late.completed[0].id == b
              && late.completed[0].outcome.error().code == errc::Timeout,
          "the passed one fails with Timeout");
    check(e.next_deadline() == t0 + 100ms, "next_deadline moves on");

    auto cancelled = step(e, Cancel{a});
    check(cancelled.completed.size() == 1 && cancelled.completed[0].outcome.error().code == errc::Cancelled,
          "Cancel completes with Cancelled");
    check(step(e, Cancel{a}).completed.empty(), "cancelling twice is a no-op");
    check(step(e, Received{R"({"jsonrpc":"2.0","id":1,"result":1})"}).completed.empty(),
          "a late reply to a cancelled request is ignored");
    check(!e.next_deadline(), "no deadline left (c has none)");

    auto closed = step(e, Closed{"eof"});
    check(closed.completed.size() == 1 && closed.completed[0].id == c
              && closed.completed[0].outcome.error().code == errc::ConnectionLost,
          "Closed fails what is still waiting");
    check(e.pending() == 0, "nothing pending after close");
    (void)fa; (void)fb; (void)fc;
}

void malformed_replies() {
    std::printf("malformed input is answered\n");
    Engine e;
    auto fx = step(e, Received{"{not json"});
    check(fx.send.size() == 1 && Json::parse(fx.send[0])["error"]["code"] == errc::ParseError
              && Json::parse(fx.send[0])["id"].is_null(),
          "unparsable input answers ParseError with id null");
    auto ver = step(e, Received{R"({"id":3,"method":"x"})"});
    check(Json::parse(ver.send[0])["id"] == 3, "an invalid request is answered with its id");
    check(step(e, Received{""}).send.size() == 1, "an empty frame is a parse error");
}

void request_meta() {
    std::printf("request _meta\n");
    Engine e;
    e.set_request_meta(Json{{"proto/version", "x"}});
    auto merged = Json::parse(request_raw(e, "m", Json{{"_meta", {{"progressToken", 1}}}}).second.send[0]);
    check(merged["params"]["_meta"]["progressToken"] == 1 && merged["params"]["_meta"]["proto/version"] == "x",
          "meta merges into the caller's _meta");
    auto keep = Json::parse(request_raw(e, "m", Json{{"_meta", {{"proto/version", "mine"}}}}).second.send[0]);
    check(keep["params"]["_meta"]["proto/version"] == "mine", "the caller's own key wins");
    Engine plain;
    check(!Json::parse(request_raw(plain, "m", Json::object()).second.send[0])["params"].contains("_meta"),
          "no meta set, nothing added");
}

void framing() {
    std::printf("framing\n");
    LineSplitter s;
    auto a = s.feed("{\"a\":1}\n{\"b\"");
    check(a.size() == 1 && a[0] == "{\"a\":1}", "a whole line comes out, a partial one waits");
    auto b = s.feed(":2}\r\n\n{\"c\":3}");
    check(b.size() == 1 && b[0] == "{\"b\":2}", "the partial line completes; CR and blank lines drop");
    auto last = s.finish();
    check(last && *last == "{\"c\":3}", "finish returns the unterminated last line");
    check(!s.finish(), "nothing left after finish");
    LineSplitter small(8);
    auto c = small.feed("0123456789abc\nok\n");
    check(c.size() == 1 && c[0] == "ok" && small.dropped() == 1, "an over-long line is dropped whole");
    check(line("{}") == "{}\n", "line() frames an envelope");
}

}  // namespace

int main() {
    std::printf("=== jsonrpc_test ===\n");
    codecs();
    messages();
    requests_and_replies();
    peer_calls_and_router();
    time_cancel_close();
    malformed_replies();
    request_meta();
    framing();
    std::printf(g_fail ? "FAILED (%d)\n" : "PASS\n", g_fail);
    return g_fail ? 1 : 0;
}
