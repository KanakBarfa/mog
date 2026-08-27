// Minimal compile-time reflection: a record declares `mog_fields`, a tuple of
// Field descriptors holding member pointers. Every serializer, schema table,
// and column layout in the telemetry stack is generated from that one
// declaration, so schemas cannot drift from the message structs they
// describe.
//
//     struct Tick { std::uint64_t ts; std::int64_t px; };
//     static constexpr auto mog_fields =
//         std::tuple{mog_field("ts", &Tick::ts), mog_field("px", &Tick::px)};
#pragma once

#include <mog/Contracts.hpp>

#include <concepts>
#include <cstdint>
#include <string_view>
#include <tuple>
#include <type_traits>

namespace mog {

template <class S, class T>
struct Field {
    std::string_view name;
    T S::* ptr;
};

template <class S, class T>
[[nodiscard]] constexpr auto field(std::string_view name, T S::* ptr) noexcept {
    return Field<S, T>{name, ptr};
}

template <class S>
concept Reflected =
    requires { S::mog_fields; } && requires { std::tuple_size_v<decltype(S::mog_fields)>; };

namespace detail {

// Dependent-false for uninstantiated-template static_asserts: plain `false`
// is a hard error on frontends without CWG2518 (older AppleClang).
template <class>
inline constexpr bool always_false_v = false;

template <class S, class Tuple, class Fn, std::size_t... I>
constexpr void for_each_field_impl(const Tuple& fields, Fn&& fn, std::index_sequence<I...>) {
    (fn(std::get<I>(fields)), ...);
}

} // namespace detail

// Calls fn(Field) for every declared field, in declaration order.
template <Reflected S, class Fn>
constexpr void for_each_field(Fn&& fn) {
    detail::for_each_field_impl<S>(
        S::mog_fields, fn, std::make_index_sequence<std::tuple_size_v<decltype(S::mog_fields)>>{});
}

template <Reflected S>
[[nodiscard]] constexpr std::size_t field_count() noexcept {
    return std::tuple_size_v<decltype(S::mog_fields)>;
}

// Field value access through the descriptor.
template <Reflected S, class F>
[[nodiscard]] constexpr auto& field_of(S& rec, const F& f) noexcept {
    return rec.*(f.ptr);
}
template <Reflected S, class F>
[[nodiscard]] constexpr const auto& field_of(const S& rec, const F& f) noexcept {
    return rec.*(f.ptr);
}

// Stable type codes shared by the columnar log format and its readers.
enum class ColumnType : std::uint8_t {
    u8 = 0,
    u32 = 1,
    u64 = 2,
    i64 = 3,
    f64 = 4,
};

template <class T>
[[nodiscard]] constexpr ColumnType column_type_of() noexcept {
    if constexpr (std::same_as<T, std::uint8_t>)
        return ColumnType::u8;
    else if constexpr (std::is_enum_v<T>)
        return column_type_of<std::underlying_type_t<T>>();
    else if constexpr (std::same_as<T, std::uint32_t>)
        return ColumnType::u32;
    else if constexpr (std::same_as<T, std::uint64_t>)
        return ColumnType::u64;
    else if constexpr (std::same_as<T, std::int64_t>)
        return ColumnType::i64;
    else if constexpr (std::same_as<T, double>)
        return ColumnType::f64;
    else
        static_assert(detail::always_false_v<T>, "unsupported column type");
}

[[nodiscard]] constexpr std::string_view column_type_name(ColumnType t) noexcept {
    switch (t) {
    case ColumnType::u8:
        return "u8";
    case ColumnType::u32:
        return "u32";
    case ColumnType::u64:
        return "u64";
    case ColumnType::i64:
        return "i64";
    case ColumnType::f64:
        return "f64";
    }
    return "?";
}

} // namespace mog
