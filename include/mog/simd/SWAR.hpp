// Word-at-a-time (SWAR) primitives for stream decoding. Pure integer ops:
// no intrinsics, identical semantics on every target.
#pragma once

#include <cstdint>
#include <cstring>

namespace mog::swar {

// Unaligned 8-byte load/store; byte order is preserved end to end.
[[nodiscard]] inline std::uint64_t load64(const unsigned char* p) noexcept {
    std::uint64_t v;
    std::memcpy(&v, p, sizeof(v));
    return v;
}

inline void store64(unsigned char* p, std::uint64_t v) noexcept {
    std::memcpy(p, &v, sizeof(v));
}

// Packs an 8-byte ASCII symbol directly into a uint64_t word.
[[nodiscard]] inline std::uint64_t pack_symbol8(const char* p) noexcept {
    std::uint64_t v = 0;
    std::memcpy(&v, p, 8);
    return v;
}

// Computes the trimmed length of an 8-byte ASCII symbol packed little-endian.
[[nodiscard]] inline std::size_t symbol8_len(std::uint64_t sym_le) noexcept {
    for (int i = 7; i >= 0; --i) {
        const auto c = static_cast<unsigned char>((sym_le >> (i * 8)) & 0xFF);
        if (c != ' ' && c != '\0')
            return static_cast<std::size_t>(i + 1);
    }
    return 0;
}

} // namespace mog::swar
