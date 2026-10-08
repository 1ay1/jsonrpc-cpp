// SPDX-License-Identifier: Apache-2.0
//
// jsonrpc/codec.hpp — codecs: serialization as a fold over the type algebra.
//
//   A Codec<T> witnesses a partial isomorphism  T ⇄ Json  with the law
//
//       decode(encode(t)) == t        (encode is total, decode may throw)
//
//   Codecs compose, so no protocol type writes to_json/from_json by hand:
//
//       codec<T>()              the canonical codec for T (CodecOf<T>, or a lift)
//       list_codec(c)           List functor
//       maybe_codec(c)          Maybe functor (Nothing ⇄ null)
//       newtype_codec<P, A>()   Newtype<P, A> encodes as A
//       enum_codec({v, "x"}, …) closed enum ⇄ string labels
//       record(field, …)        product: named fields
//       sum_tagged(key, arm, …) coproduct with a string discriminator
//       variant_codec(c, …)     coproduct told apart by shape
//
//   Pure: codecs are immutable values. Building one allocates, so codec<T>()
//   builds each type's codec once and hands out the same reference.
#pragma once

#include <array>
#include <concepts>
#include <cstdint>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>

#include "core.hpp"

namespace jsonrpc {

// The failure mode of decode: the Json didn't have the shape T needs.
struct CodecError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

template <class T>
struct Codec {
    std::function<Json(const T&)> encode;
    std::function<T(const Json&)> decode;
};

// The customisation point for the BUILT-IN types (bool, string, numbers,
// Unit, Json, RpcError, Id).
template <class T> struct CodecOf;

// How a protocol supplies codecs for ITS types, without opening namespace
// jsonrpc and without touching any of its existing codec definitions.
//
// A protocol keeps its own table, `ns::CodecOf<T>`, and its types live in
// `ns`. It writes, once, in `ns`:
//
//     template <class T> struct CodecOf;      // the protocol's own table
//     JSONRPC_PROTOCOL_CODECS(CodecOf)        // let jsonrpc find it
//
// The macro defines a function template `jsonrpc_codec_of(T*)` in `ns`.
// codec<T>() calls it unqualified with a `T*`, so argument-dependent lookup
// finds it in the namespace T was declared in. A type in another namespace
// (a test's own types) uses that namespace's table, declared the same way.
#define JSONRPC_PROTOCOL_CODECS(Table)                                              \
    template <class T__>                                                            \
        requires requires { Table<T__>::get(); }                                    \
    ::jsonrpc::Codec<T__> jsonrpc_codec_of(T__*) { return Table<T__>::get(); }

namespace detail {
template <class T>
concept has_builtin_codec = requires { { CodecOf<T>::get() } -> std::same_as<Codec<T>>; };
template <class T>
concept has_protocol_codec =
    requires(T* p) { { jsonrpc_codec_of(p) } -> std::same_as<Codec<T>>; };
}  // namespace detail

namespace detail {
template <class T> struct is_maybe                    : std::false_type {};
template <class A> struct is_maybe<Maybe<A>>          : std::true_type  {};
template <class T> struct is_list                     : std::false_type {};
template <class A> struct is_list<List<A>>            : std::true_type  {};
template <class T> struct is_newtype                  : std::false_type {};
template <class P, class A>
struct is_newtype<Newtype<P, A>> : std::true_type {
    using Phantom = P;
    using Carrier = A;
};
}  // namespace detail

template <class A> Codec<List<A>>  list_codec(Codec<A> inner);
template <class A> Codec<Maybe<A>> maybe_codec(Codec<A> inner);
template <class P, class A> Codec<Newtype<P, A>> newtype_codec();
template <class T> const Codec<T>& codec();

// The fold: Maybe/List/Newtype lift their component's codec; everything else
// is a protocol type's own CodecOf.
template <class T>
Codec<T> build_codec() {
    if constexpr (detail::is_maybe<T>::value) {
        using A = typename T::value_type;
        return maybe_codec<A>(codec<A>());
    } else if constexpr (detail::is_list<T>::value) {
        using A = typename T::value_type;
        return list_codec<A>(codec<A>());
    } else if constexpr (detail::is_newtype<T>::value) {
        return newtype_codec<typename detail::is_newtype<T>::Phantom,
                             typename detail::is_newtype<T>::Carrier>();
    } else if constexpr (detail::has_protocol_codec<T>) {
        return jsonrpc_codec_of(static_cast<T*>(nullptr));
    } else {
        static_assert(detail::has_builtin_codec<T>,
                      "jsonrpc: no codec for T. Specialise your protocol's CodecOf<T> "
                      "(declared with JSONRPC_PROTOCOL_CODECS in T's namespace).");
        return CodecOf<T>::get();
    }
}

// One immutable codec per type, built on first use and never destroyed.
//
// Never destroyed on purpose: a `static const Codec<T>` is torn down at exit,
// but the host's threads that encode are not all joined by then, and an
// encode after teardown calls a freed std::function. One small leak per type
// matches the real contract: a codec is readable for as long as anything can
// run. Static initialisation is once-only by the standard, and the value is
// const after, so concurrent use needs no lock.
template <class T>
const Codec<T>& codec() {
    static const Codec<T>* const instance = new Codec<T>(build_codec<T>());
    return *instance;
}

template <class T> Json to_json(const T& v)       { return codec<T>().encode(v); }
template <class T> T    from_json(const Json& j)  { return codec<T>().decode(j); }

// ── Generators ─────────────────────────────────────────────────────────────

template <> struct CodecOf<bool> {
    static Codec<bool> get() {
        return {[](const bool& v) -> Json { return v; },
                [](const Json& j) -> bool {
                    if (!j.is_boolean()) throw CodecError("expected bool");
                    return j.get<bool>();
                }};
    }
};
template <> struct CodecOf<std::string> {
    static Codec<std::string> get() {
        return {[](const std::string& v) -> Json { return v; },
                [](const Json& j) -> std::string {
                    if (!j.is_string()) throw CodecError("expected string");
                    return j.get<std::string>();
                }};
    }
};
template <> struct CodecOf<std::int64_t> {
    static Codec<std::int64_t> get() {
        return {[](const std::int64_t& v) -> Json { return v; },
                [](const Json& j) -> std::int64_t {
                    if (!j.is_number_integer()) throw CodecError("expected integer");
                    return j.get<std::int64_t>();
                }};
    }
};
template <> struct CodecOf<int> {
    static Codec<int> get() {
        auto inner = CodecOf<std::int64_t>::get();
        return {[enc = inner.encode](const int& v) { return enc(static_cast<std::int64_t>(v)); },
                [dec = inner.decode](const Json& j) { return static_cast<int>(dec(j)); }};
    }
};
template <> struct CodecOf<double> {
    static Codec<double> get() {
        return {[](const double& v) -> Json { return v; },
                [](const Json& j) -> double {
                    if (!j.is_number()) throw CodecError("expected number");
                    return j.get<double>();
                }};
    }
};
template <> struct CodecOf<Unit> {
    static Codec<Unit> get() {
        return {[](const Unit&) -> Json { return Json::object(); },
                [](const Json& j) -> Unit {
                    if (!j.is_object()) throw CodecError("expected object for Unit");
                    return Unit{};
                }};
    }
};
// Json itself: the identity codec, for opaque blobs (_meta, arbitrary args).
template <> struct CodecOf<Json> {
    static Codec<Json> get() {
        return {[](const Json& v) { return v; }, [](const Json& j) { return j; }};
    }
};

// ── Lifts ──────────────────────────────────────────────────────────────────

template <class A>
Codec<List<A>> list_codec(Codec<A> inner) {
    return {[enc = inner.encode](const List<A>& xs) -> Json {
                Json out = Json::array();
                for (const auto& x : xs) out.push_back(enc(x));
                return out;
            },
            [dec = inner.decode](const Json& j) -> List<A> {
                if (!j.is_array()) throw CodecError("expected array");
                List<A> out;
                out.reserve(j.size());
                for (const auto& el : j) out.push_back(dec(el));
                return out;
            }};
}

template <class A>
Codec<Maybe<A>> maybe_codec(Codec<A> inner) {
    return {[enc = inner.encode](const Maybe<A>& v) -> Json {
                return v.has_value() ? enc(*v) : Json(nullptr);
            },
            [dec = inner.decode](const Json& j) -> Maybe<A> {
                if (j.is_null()) return Nothing;
                return Just(dec(j));
            }};
}

template <class P, class A>
Codec<Newtype<P, A>> newtype_codec() {
    const auto& inner = codec<A>();
    return {[enc = inner.encode](const Newtype<P, A>& n) -> Json { return enc(n.value); },
            [dec = inner.decode](const Json& j) { return Newtype<P, A>{dec(j)}; }};
}

// ── Enums ──────────────────────────────────────────────────────────────────
// A bijection between a closed set of values and string labels. Decoding an
// unknown label throws: the protocols state their label sets.

template <class E>
struct EnumMapping {
    E value;
    std::string_view label;
};

template <class E, std::size_t N>
Codec<E> enum_codec(std::array<EnumMapping<E>, N> table) {
    return {[table](const E& v) -> Json {
                for (const auto& m : table)
                    if (m.value == v) return std::string(m.label);
                throw CodecError("enum value outside declared mapping");
            },
            [table](const Json& j) -> E {
                if (!j.is_string()) throw CodecError("expected string for enum");
                const auto& s = j.get_ref<const std::string&>();
                for (const auto& m : table)
                    if (m.label == s) return m.value;
                throw CodecError("unknown enum label: '" + s + "'");
            }};
}

template <class E, class... Rest>
Codec<E> enum_codec(EnumMapping<E> first, Rest... rest) {
    std::array<EnumMapping<E>, 1 + sizeof...(Rest)> table{first, rest...};
    return enum_codec(table);
}

// ── Records: T ≅ Π fᵢ : Tᵢ ─────────────────────────────────────────────────
//
//   required(key, &T::f)          the key must be present
//   optional(key, &T::f)          &T::f is Maybe<U>; omitted when Nothing
//   defaulted(key, &T::f, dflt)   absent means dflt
//   meta(key, &T::f)              a _meta extension blob; omitted when empty,
//                                 {} when absent (it is optional in both specs)

template <class T, class U>
struct Field {
    enum class Kind : unsigned char { Required, Optional, Defaulted, Meta };
    std::string_view key;
    U T::*lens;
    Codec<U> codec_;
    Kind kind;
    U default_value{};
};

template <class T, class U>
Field<T, U> required(std::string_view key, U T::*lens) {
    return {key, lens, codec<U>(), Field<T, U>::Kind::Required, U{}};
}
template <class T, class U>
Field<T, U> required(std::string_view key, U T::*lens, Codec<U> c) {
    return {key, lens, std::move(c), Field<T, U>::Kind::Required, U{}};
}
template <class T, class U>
Field<T, Maybe<U>> optional(std::string_view key, Maybe<U> T::*lens) {
    return {key, lens, maybe_codec(codec<U>()), Field<T, Maybe<U>>::Kind::Optional, Maybe<U>{}};
}
template <class T, class U>
Field<T, Maybe<U>> optional(std::string_view key, Maybe<U> T::*lens, Codec<U> c) {
    return {key, lens, maybe_codec(std::move(c)), Field<T, Maybe<U>>::Kind::Optional, Maybe<U>{}};
}
template <class T, class U>
Field<T, U> defaulted(std::string_view key, U T::*lens, U dflt) {
    return {key, lens, codec<U>(), Field<T, U>::Kind::Defaulted, std::move(dflt)};
}
template <class T>
Field<T, Json> meta(std::string_view key, Json T::*lens) {
    return {key, lens, CodecOf<Json>::get(), Field<T, Json>::Kind::Meta, Json::object()};
}

template <class T, class... Us>
Codec<T> record(Field<T, Us>... fields_) {
    auto fields = std::make_tuple(std::move(fields_)...);
    return {
        [fields](const T& v) -> Json {
            Json out = Json::object();
            std::apply([&](const auto&... f) {
                ([&] {
                    using FT = std::decay_t<decltype(v.*(f.lens))>;
                    using K  = typename std::decay_t<decltype(f)>::Kind;
                    if constexpr (std::is_same_v<FT, Json>) {
                        if (f.kind == K::Meta) {
                            const Json& m = v.*(f.lens);
                            if (!m.is_null() && !(m.is_object() && m.empty()))
                                out[std::string(f.key)] = m;
                            return;
                        }
                    }
                    if constexpr (detail::is_maybe<FT>::value) {
                        if ((v.*(f.lens)).has_value())
                            out[std::string(f.key)] = f.codec_.encode(v.*(f.lens));
                    } else {
                        out[std::string(f.key)] = f.codec_.encode(v.*(f.lens));
                    }
                }(), ...);
            }, fields);
            return out;
        },
        [fields](const Json& j) -> T {
            if (!j.is_object()) throw CodecError("expected object for record");
            T out{};
            std::apply([&](const auto&... f) {
                ([&] {
                    using K = typename std::decay_t<decltype(f)>::Kind;
                    auto it = j.find(std::string(f.key));
                    if (it == j.end() || it->is_null()) {
                        if (f.kind == K::Required)
                            throw CodecError("missing required field: " + std::string(f.key));
                        if (f.kind == K::Defaulted || f.kind == K::Meta)
                            out.*(f.lens) = f.default_value;
                        return;   // Optional: stays Nothing
                    }
                    out.*(f.lens) = f.codec_.decode(*it);
                }(), ...);
            }, fields);
            return out;
        }};
}

// ── Tagged sums ────────────────────────────────────────────────────────────
//
//   Wire form: { "<key>": "<label>", …the active alternative's fields… }
//   e.g.       { "type": "text", "text": "hello" }
//
//   Every arm must encode to a JSON object (the tag is merged into it).

template <class Sum_, class Alt>
struct Arm {
    using AltT = Alt;
    std::string_view label;
    Codec<Alt> inner;
};

template <class Sum_, class Alt>
Arm<Sum_, Alt> arm(std::string_view label) { return {label, codec<Alt>()}; }
template <class Sum_, class Alt>
Arm<Sum_, Alt> arm(std::string_view label, Codec<Alt> c) { return {label, std::move(c)}; }

template <class Sum_, class... Alts>
Codec<Sum_> sum_tagged(std::string_view tag_key, Arm<Sum_, Alts>... arms_) {
    auto arms = std::make_tuple(std::move(arms_)...);
    std::string key{tag_key};
    return {
        [arms, key](const Sum_& s) -> Json {
            return std::visit([&](const auto& x) -> Json {
                using X = std::decay_t<decltype(x)>;
                Json out;
                bool matched = false;
                std::apply([&](const auto&... a) {
                    ([&] {
                        if constexpr (std::is_same_v<X, typename std::decay_t<decltype(a)>::AltT>) {
                            if (matched) return;
                            out = a.inner.encode(x);
                            if (!out.is_object())
                                throw CodecError("sum_tagged arm '" + std::string(a.label)
                                                 + "' must encode to a JSON object");
                            out[key] = std::string(a.label);
                            matched = true;
                        }
                    }(), ...);
                }, arms);
                if (!matched) throw CodecError("sum_tagged: no arm for active alternative");
                return out;
            }, s);
        },
        [arms, key](const Json& j) -> Sum_ {
            if (!j.is_object()) throw CodecError("sum_tagged: expected object");
            auto it = j.find(key);
            if (it == j.end() || !it->is_string())
                throw CodecError("sum_tagged: missing tag '" + key + "'");
            const auto& label = it->template get_ref<const std::string&>();
            Maybe<Sum_> out;
            std::apply([&](const auto&... a) {
                ([&] {
                    if (!out.has_value() && a.label == label) out = Sum_{a.inner.decode(j)};
                }(), ...);
            }, arms);
            if (!out.has_value()) throw CodecError("sum_tagged: unknown label '" + label + "'");
            return std::move(*out);
        }};
}

// An untagged union, told apart by shape (e.g. string | number). Encode uses
// the active alternative's codec; decode tries each arm in order and takes
// the first that decodes.
template <class Sum_, class... Alts>
Codec<Sum_> variant_codec(Codec<Alts>... arms_) {
    auto arms = std::make_shared<std::tuple<Codec<Alts>...>>(std::move(arms_)...);
    return {
        [arms](const Sum_& s) -> Json {
            Json out;
            bool matched = false;
            [&]<std::size_t... I>(std::index_sequence<I...>) {
                ([&] {
                    if (matched) return;
                    using A = std::tuple_element_t<I, std::tuple<Alts...>>;
                    if (const A* p = std::get_if<A>(&s)) {
                        out = std::get<I>(*arms).encode(*p);
                        matched = true;
                    }
                }(), ...);
            }(std::index_sequence_for<Alts...>{});
            if (!matched) throw CodecError("variant_codec: no arm for active alternative");
            return out;
        },
        [arms](const Json& j) -> Sum_ {
            Maybe<Sum_> out;
            [&]<std::size_t... I>(std::index_sequence<I...>) {
                ([&] {
                    if (out.has_value()) return;
                    try { out = Sum_{std::get<I>(*arms).decode(j)}; }
                    catch (const CodecError&) {}
                }(), ...);
            }(std::index_sequence_for<Alts...>{});
            if (!out.has_value()) throw CodecError("variant_codec: no arm decoded the value");
            return std::move(*out);
        }};
}

}  // namespace jsonrpc
