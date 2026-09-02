// Big-endian wire load/store; std::byteswap when available, builtin fallback.
#pragma once

#include <bit>
#include <cstdint>
#include <cstring>

namespace mog::wire {

inline constexpr bool kHostIsLittle = std::endian::native == std::endian::little;

inline std::uint16_t swap16(std::uint16_t v) noexcept {
#if defined(__cpp_lib_byteswap)
    return std::byteswap(v);
#else
    return __builtin_bswap16(v);
#endif
}

inline std::uint32_t swap32(std::uint32_t v) noexcept {
#if defined(__cpp_lib_byteswap)
    return std::byteswap(v);
#else
    return __builtin_bswap32(v);
#endif
}

inline std::uint64_t swap64(std::uint64_t v) noexcept {
#if defined(__cpp_lib_byteswap)
    return std::byteswap(v);
#else
    return __builtin_bswap64(v);
#endif
}

inline std::uint16_t load_be16(const unsigned char* p) noexcept {
    std::uint16_t raw;
    std::memcpy(&raw, p, sizeof(raw));
    return kHostIsLittle ? swap16(raw) : raw;
}

inline std::uint32_t load_be32(const unsigned char* p) noexcept {
    std::uint32_t raw;
    std::memcpy(&raw, p, sizeof(raw));
    return kHostIsLittle ? swap32(raw) : raw;
}

inline std::uint64_t load_be64(const unsigned char* p) noexcept {
    std::uint64_t raw;
    std::memcpy(&raw, p, sizeof(raw));
    return kHostIsLittle ? swap64(raw) : raw;
}

inline std::uint16_t load_le16(const unsigned char* p) noexcept {
    std::uint16_t raw;
    std::memcpy(&raw, p, sizeof(raw));
    return kHostIsLittle ? raw : swap16(raw);
}

inline std::uint32_t load_le32(const unsigned char* p) noexcept {
    std::uint32_t raw;
    std::memcpy(&raw, p, sizeof(raw));
    return kHostIsLittle ? raw : swap32(raw);
}

inline std::uint64_t load_le64(const unsigned char* p) noexcept {
    std::uint64_t raw;
    std::memcpy(&raw, p, sizeof(raw));
    return kHostIsLittle ? raw : swap64(raw);
}

// ITCH 5.0 timestamps are 48-bit big-endian.
inline std::uint64_t load_be48(const unsigned char* p) noexcept {
    std::uint64_t v = 0;
    std::memcpy(&v, p, 6);
    return kHostIsLittle ? (swap64(v) >> 16) : (v >> 16);
}

inline void store_be16(unsigned char* p, std::uint16_t v) noexcept {
    std::uint16_t raw = kHostIsLittle ? swap16(v) : v;
    std::memcpy(p, &raw, sizeof(raw));
}

inline void store_be32(unsigned char* p, std::uint32_t v) noexcept {
    std::uint32_t raw = kHostIsLittle ? swap32(v) : v;
    std::memcpy(p, &raw, sizeof(raw));
}

inline void store_be64(unsigned char* p, std::uint64_t v) noexcept {
    std::uint64_t raw = kHostIsLittle ? swap64(v) : v;
    std::memcpy(p, &raw, sizeof(raw));
}

inline void store_be48(unsigned char* p, std::uint64_t v) noexcept {
    const std::uint64_t raw = kHostIsLittle ? swap64(v << 16) : (v << 16);
    std::memcpy(p, &raw, 6);
}

} // namespace mog::wire
