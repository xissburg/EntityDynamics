#ifndef EDYN_SERIALIZATION_MEMORY_ARCHIVE_HPP
#define EDYN_SERIALIZATION_MEMORY_ARCHIVE_HPP

#include <cstdint>
#include <type_traits>
#include <vector>
#include <array>
#include <map>

namespace edyn {

class memory_input_archive {
public:
    using data_type = uint8_t;
    using buffer_type = const data_type*;
    using is_input = std::true_type;
    using is_output = std::false_type;

    memory_input_archive(buffer_type buffer, size_t size)
        : m_buffer(buffer)
        , m_size(size)
        , m_position(0)
    {}

    template<typename... Ts>
    void operator()(Ts&&... t) {
        if constexpr(sizeof...(Ts) == 1) {
            (serialize(*this, t), ...);
        } else {
            (operator()(t), ...);
        }
    }

    void operator()(bool &t) {
        read_bytes(t);
    }

    void operator()(char &t) {
        read_bytes(t);
    }

    void operator()(unsigned char &t) {
        read_bytes(t);
    }

    void operator()(short &t) {
        read_bytes(t);
    }

    void operator()(unsigned short &t) {
        read_bytes(t);
    }

    void operator()(int &t) {
        read_bytes(t);
    }

    void operator()(unsigned int &t) {
        read_bytes(t);
    }

    void operator()(long &t) {
        read_bytes(t);
    }

    void operator()(unsigned long &t) {
        read_bytes(t);
    }

    void operator()(long long &t) {
        read_bytes(t);
    }

    void operator()(unsigned long long &t) {
        read_bytes(t);
    }

    void operator()(float &t) {
        read_bytes(t);
    }

    void operator()(double &t) {
        read_bytes(t);
    }

    template<typename T>
    void read_bytes(T &t) {
        EDYN_ASSERT(m_position + sizeof(T) < m_size);
        auto* buff = reinterpret_cast<const T*>(m_buffer + m_position);
        t = *buff;
        m_position += sizeof(T);
    }

protected:
    buffer_type m_buffer;
    const size_t m_size;
    size_t m_position;
};

class memory_output_archive {
public:
    using data_type = uint8_t;
    using buffer_type = std::vector<data_type>;
    using is_input = std::false_type;
    using is_output = std::true_type;

    memory_output_archive(buffer_type& buffer) 
        : m_buffer(&buffer) 
    {}

    template<typename... Ts>
    void operator()(Ts&&... t) {
        if constexpr(sizeof...(Ts) == 1) {
            (serialize(*this, const_cast<std::add_lvalue_reference_t<std::remove_const_t<std::remove_reference_t<Ts>>>>(t)), ...);
        } else {
            (operator()(t), ...);
        }
    }

    void operator()(bool &t) {
        write_bytes(t);
    }

    void operator()(char &t) {
        write_bytes(t);
    }

    void operator()(unsigned char &t) {
        write_bytes(t);
    }

    void operator()(short &t) {
        write_bytes(t);
    }

    void operator()(unsigned short &t) {
        write_bytes(t);
    }

    void operator()(int &t) {
        write_bytes(t);
    }

    void operator()(unsigned int &t) {
        write_bytes(t);
    }

    void operator()(long &t) {
        write_bytes(t);
    }

    void operator()(unsigned long &t) {
        write_bytes(t);
    }

    void operator()(long long &t) {
        write_bytes(t);
    }

    void operator()(unsigned long long &t) {
        write_bytes(t);
    }

    void operator()(float &t) {
        write_bytes(t);
    }

    void operator()(double &t) {
        write_bytes(t);
    }

    template<typename T>
    void write_bytes(T &t) { 
        auto idx = m_buffer->size();
        m_buffer->resize(idx + sizeof(T));
        auto *dest = reinterpret_cast<T*>(&(*m_buffer)[idx]);
        *dest = t;
    }

protected:
    buffer_type *m_buffer;
};

class fixed_memory_output_archive {
public:
    using data_type = uint8_t;
    using buffer_type = data_type*;
    using is_input = std::false_type;
    using is_output = std::true_type;

    fixed_memory_output_archive(buffer_type buffer, size_t size) 
        : m_buffer(buffer)
        , m_size(size)
        , m_position(0)
    {}

    template<typename... Ts>
    void operator()(Ts&&... t) {
        if constexpr(sizeof...(Ts) == 1) {
            (serialize(*this, const_cast<std::add_lvalue_reference_t<std::remove_const_t<std::remove_reference_t<Ts>>>>(t)), ...);
        } else {
            (operator()(t), ...);
        }
    }

    void operator()(bool &t) {
        write_bytes(t);
    }

    void operator()(char &t) {
        write_bytes(t);
    }

    void operator()(unsigned char &t) {
        write_bytes(t);
    }

    void operator()(short &t) {
        write_bytes(t);
    }

    void operator()(unsigned short &t) {
        write_bytes(t);
    }

    void operator()(int &t) {
        write_bytes(t);
    }

    void operator()(unsigned int &t) {
        write_bytes(t);
    }

    void operator()(long &t) {
        write_bytes(t);
    }

    void operator()(unsigned long &t) {
        write_bytes(t);
    }

    void operator()(long long &t) {
        write_bytes(t);
    }

    void operator()(unsigned long long &t) {
        write_bytes(t);
    }

    void operator()(float &t) {
        write_bytes(t);
    }

    void operator()(double &t) {
        write_bytes(t);
    }

    template<typename T>
    void write_bytes(T &t) { 
        EDYN_ASSERT(m_position + sizeof(T) < m_size);
        auto *dest = reinterpret_cast<T*>(m_buffer + m_position);
        *dest = t;
        m_position += sizeof(T);
    }

protected:
    buffer_type m_buffer;
    size_t m_size;
    size_t m_position;
};

}

#endif // EDYN_SERIALIZATION_MEMORY_ARCHIVE_HPP