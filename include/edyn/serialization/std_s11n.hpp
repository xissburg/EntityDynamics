#ifndef EDYN_SERIALIZATION_STD_S11N_HPP
#define EDYN_SERIALIZATION_STD_S11N_HPP

#include <array>
#include <vector>
#include <cstdint>
#include <memory>
#include <variant>
#include <type_traits>
#include "edyn/util/tuple.hpp"

namespace edyn {

template<typename Archive>
void serialize(Archive &archive, std::string& str) {
    auto size = str.size();
    archive(size);
    str.resize(size);

    for (size_t i = 0; i < size; ++i) {
        archive(str[i]);
    }
}

template<typename Archive, typename T>
void serialize(Archive &archive, std::vector<T> &vector) {
    auto size = vector.size();
    archive(size);
    vector.resize(size);

    for (size_t i = 0; i < size; ++i) {
        archive(vector[i]);
    }
}

template<typename Archive>
void serialize(Archive &archive, std::vector<bool> &vector) {
    auto size = vector.size();
    archive(size);
    vector.resize(size);

    // Serialize individual bits.
    using set_type = uint32_t;
    constexpr auto set_num_bits = sizeof(set_type) * 8;
    // Number of sets of bits of size `set_num_bits`.
    // Use ceiling on integer division.
    const auto num_sets = size / set_num_bits + (size % set_num_bits != 0); 

    for (size_t i = 0; i < num_sets; ++i) {
        const auto start = i * set_num_bits;
        const auto count = std::min(size - start, set_num_bits);

        if constexpr(Archive::is_output::value) {
            set_type set = 0;
            for (size_t j = 0; j < count; ++j) {
                set |= static_cast<bool>(vector[start + j]) << j;
            }
            archive(set);
        } else {
            set_type set;
            archive(set);
            for (size_t j = 0; j < count; ++j) {
                vector[start + j] = (set & (1 << j)) > 0;
            }
        }
    }
}

template<typename T>
size_t serialization_sizeof(const std::vector<T> &vec) {
    return sizeof(size_t) + vec.size() * sizeof(typename std::vector<T>::value_type);
}

inline
size_t serialization_sizeof(const std::vector<bool> &vec) {
    using set_type = uint32_t;
    constexpr auto set_num_bits = sizeof(set_type) * 8;
    const auto num_sets = vec.size() / set_num_bits + (vec.size() % set_num_bits != 0); 
    return sizeof(size_t) + num_sets * sizeof(set_type);
}

template<typename Archive, typename T, size_t N>
void serialize(Archive &archive, std::array<T, N> &arr) {
    for (size_t i = 0; i < arr.size(); ++i) {
        archive(arr[i]);
    }
}

namespace internal {
    template<typename T, typename Archive, typename... Ts>
    void read_variant(Archive& archive, std::variant<Ts...>& var) {
        auto t = T{};
        archive(t);
        var = std::variant<Ts...>{t};
    }

    template<typename Archive, typename... Ts, std::size_t... Indexes>
    void read_variant(Archive& archive, typename entt::identifier<Ts...>::identifier_type id, std::variant<Ts...>& var, std::index_sequence<Indexes...>)
    {
        ((id == Indexes ? read_variant<std::tuple_element_t<Indexes, std::tuple<Ts...>>>(archive, var) : (void)0), ...);
    }

    template<typename Archive, typename... Ts>
    void read_variant(Archive& archive, typename entt::identifier<Ts...>::identifier_type id, std::variant<Ts...>& var)
    {
        read_variant(archive, id, var, std::make_index_sequence<sizeof...(Ts)>{});
    }
}

template<typename Archive, typename... Ts>
void serialize(Archive& archive, std::variant<Ts...>& var) {
    if constexpr(Archive::is_input::value) {
        size_t id;
        archive(id);
        internal::read_variant(archive, id, var);
    } else {
        std::visit([&archive] (auto &&t) {
            using T = std::decay_t<decltype(t)>;
            auto id = index_of_v<size_t, T, Ts...>;
            archive(id);
            archive(t);
        }, var);
    }
}

}

#endif // EDYN_SERIALIZATION_STD_S11N_HPP