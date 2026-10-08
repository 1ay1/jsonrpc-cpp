// SPDX-License-Identifier: Apache-2.0
//
// jsonrpc/core.hpp — the vocabulary every protocol type is built from.
//
//   A JSON-RPC protocol (MCP, ACP) is a closed family of types built from a
//   few constructors:
//
//       Unit         1        no information, encoded as {}
//       A × B        product  a record with named fields
//       Sum<A, B>    A + B    a tagged union
//       List<A>      List A   a finite sequence
//       Maybe<A>     A + 1    an optional value
//       Newtype<P,A> A        A, nominally distinct (SessionId vs ToolCallId)
//       Json                  the untyped term the wire carries
//
//   Every spec type is a closed expression in this algebra, and
//   serialization (codec.hpp) is a fold over it.
//
//   jsonrpc/ is a self-contained leaf: it includes only the standard library,
//   nlohmann/json and itself. It has no runtime of any kind (see
//   docs/PROTOCOL_LIBRARIES.md). mcp-cpp and acp-cpp build on it and
//   re-export these names into their own namespaces.
#pragma once

#include <compare>
#include <cstddef>
#include <functional>
#include <optional>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include <nlohmann/json.hpp>

namespace jsonrpc {

using Json = nlohmann::json;

// ── Unit ───────────────────────────────────────────────────────────────────
struct Unit {
    friend constexpr bool operator==(Unit, Unit) noexcept { return true; }
};

// ── Maybe A  ≅  A + 1 ─────────────────────────────────────────────────────
// std::optional under the name that says what it means: optionality is a
// sum, not a sentinel.
template <class A> using Maybe = std::optional<A>;

template <class A>
constexpr Maybe<A> Just(A x) { return Maybe<A>{std::move(x)}; }

inline constexpr std::nullopt_t Nothing = std::nullopt;

// ── List A ─────────────────────────────────────────────────────────────────
template <class A> using List = std::vector<A>;

// ── Sum<A₀, …, Aₙ> ─────────────────────────────────────────────────────────
// The discriminator is the alternative's position; sum_tagged (codec.hpp)
// maps positions to wire labels.
template <class... As> using Sum = std::variant<As...>;

// match: a total case analysis over a Sum. std::visit refuses to compile a
// non-exhaustive set of branches.
template <class S, class... Fs>
constexpr decltype(auto) match(S&& s, Fs&&... fs) {
    struct overload : std::remove_reference_t<Fs>... {
        using std::remove_reference_t<Fs>::operator()...;
    };
    return std::visit(overload{std::forward<Fs>(fs)...}, std::forward<S>(s));
}

// ── Compile-time strings ───────────────────────────────────────────────────
// A string literal usable as a template argument: wire labels, method names,
// capability tags.
template <std::size_t N>
struct StaticString {
    char data[N]{};
    constexpr StaticString(const char (&s)[N]) {
        for (std::size_t i = 0; i < N; ++i) data[i] = s[i];
    }
    [[nodiscard]] constexpr std::string_view view() const noexcept { return {data, N - 1}; }
    [[nodiscard]] constexpr std::size_t size() const noexcept { return N - 1; }
    constexpr bool operator==(const StaticString&) const = default;
};

// A phantom carrier for a compile-time tag, for indexing typed tables without
// ever making a value.
template <StaticString S>
struct Tag {
    static constexpr std::string_view name = S.view();
};

// ── Newtype<Phantom, A> ────────────────────────────────────────────────────
// A with a nominal type: SessionId and ToolCallId are both strings on the
// wire and cannot be mixed up in C++. Equality, ordering and hashing are A's.
//
// No implicit conversion to A. One would make every Newtype silently decay
// to its carrier and then convert onward (a RequestId into a wire Id, a
// SessionId into any string parameter) — the very mix-ups the type exists to
// stop. Unwrap with .get() or .value, where it is meant.
template <class Phantom, class A>
struct Newtype {
    A value{};
    constexpr Newtype() = default;
    constexpr explicit Newtype(A v) : value(std::move(v)) {}
    [[nodiscard]] constexpr const A& get() const noexcept { return value; }
    friend constexpr bool operator==(const Newtype&, const Newtype&) = default;
    friend constexpr auto operator<=>(const Newtype&, const Newtype&) = default;
};

}  // namespace jsonrpc

template <class P, class A>
struct std::hash<jsonrpc::Newtype<P, A>> {
    std::size_t operator()(const jsonrpc::Newtype<P, A>& n) const
        noexcept(noexcept(std::hash<A>{}(n.value))) {
        return std::hash<A>{}(n.value);
    }
};
