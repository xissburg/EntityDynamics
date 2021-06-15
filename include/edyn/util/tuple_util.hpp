#ifndef EDYN_UTIL_TUPLE_UTIL_HPP
#define EDYN_UTIL_TUPLE_UTIL_HPP

#include <tuple>
#include <variant>

namespace edyn {

/**
 * Check if a tuple contains a given type. Usage:
 * `if constexpr(has_type<T, a_tuple>::value) { ... }`
 * Reference: https://stackoverflow.com/a/41171291
 */
template <typename T, typename Tuple>
struct has_type;

template <typename T, typename... Us>
struct has_type<T, std::tuple<Us...>> : std::disjunction<std::is_same<T, Us>...> {};

// Do the same for variants. TODO: maybe move this somewhere else.
template <typename T, typename... Us>
struct has_type<T, std::variant<Us...>> : std::disjunction<std::is_same<T, Us>...> {};

template<typename IndexType, typename T, typename... Ts>
struct index_of;

template<typename IndexType, typename T, typename... Ts>
struct index_of<IndexType, T, T, Ts...> : std::integral_constant<IndexType, 0>{};

template<typename IndexType, typename T, typename U, typename... Ts>
struct index_of<IndexType, T, U, Ts...> : std::integral_constant<IndexType, 1 + index_of<IndexType, T, Ts...>::value>{};

template<typename IndexType, typename T, typename... Ts>
static constexpr IndexType index_of_v = index_of<IndexType, T, Ts...>::value;

/**
 * Find index of a type in a tuple type.
 */
template<typename IndexType, typename T, typename... Ts>
struct index_of<IndexType, T, std::tuple<Ts...>> : index_of<IndexType, T, Ts...>{};

/**
 * Find index of a type in a tuple.
 */
template<typename T, typename... Ts, typename IndexType = size_t>
constexpr IndexType tuple_index_of(std::tuple<Ts...>) {
    return index_of_v<IndexType, T, Ts...>;
}

/**
 * Map a `std::tuple<Us...>` to `std::tuple<T<Us>...>`.
 */
template<template<typename> class T, typename U>
struct map_tuple;

template<template<typename> class T, typename... Us>
struct map_tuple<T, std::tuple<Us...>> {
    using type = std::tuple<T<Us>...>;
};

/**
 * Convert a tuple to a variant with the same types.
 */
template<typename... Ts>
auto tuple_to_variant(std::tuple<Ts...>) {
    return std::variant<Ts...>{};
}

}

#endif // EDYN_UTIL_TUPLE_UTIL_HPP
