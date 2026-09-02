// SIMD and SWAR vector operations for wire framing, symbol matching, and ITCH parsing.
#pragma once

#include <mog/Wire.hpp>
#include <mog/simd/SWAR.hpp>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>

#if defined(__x86_64__)
#include <immintrin.h>
#endif

namespace mog::simd {

// Fast 8-byte ASCII symbol equality check with zero branching.
[[nodiscard]] inline bool symbol8_equal(const char* a, const char* b) noexcept {
    const std::uint64_t va = swar::load64(reinterpret_cast<const unsigned char*>(a));
    const std::uint64_t vb = swar::load64(reinterpret_cast<const unsigned char*>(b));
    return va == vb;
}

// SIMD 8-byte ASCII space trimming with trailing whitespace removal.
[[nodiscard]] inline std::size_t trim_symbol8_length(const char* p) noexcept {
    const std::uint64_t v = swar::load64(reinterpret_cast<const unsigned char*>(p));
    return swar::symbol8_len(v);
}

// Vectorized batch scanner for ITCH 5.0 message types.
// Returns a bitmask where bit i is set if byte i matches target_type.
[[nodiscard]] inline std::uint32_t match_msg_types_32b(const unsigned char* p,
                                                       char target_type) noexcept {
#if defined(__AVX2__) && (defined(__x86_64__) || defined(_M_X64))
    const __m256i chunk = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(p));
    const __m256i target = _mm256_set1_epi8(target_type);
    const __m256i cmp = _mm256_cmpeq_epi8(chunk, target);
    return static_cast<std::uint32_t>(_mm256_movemask_epi8(cmp));
#elif (defined(__SSE4_1__) || defined(__SSE2__)) &&                                                \
    (defined(__x86_64__) || defined(_M_X64) || defined(__i386__))
    const __m128i chunk0 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(p));
    const __m128i chunk1 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(p + 16));
    const __m128i target = _mm_set1_epi8(target_type);
    const int mask0 = _mm_movemask_epi8(_mm_cmpeq_epi8(chunk0, target));
    const int mask1 = _mm_movemask_epi8(_mm_cmpeq_epi8(chunk1, target));
    return static_cast<std::uint32_t>(mask0) | (static_cast<std::uint32_t>(mask1) << 16);
#else
    std::uint32_t mask = 0;
    for (std::size_t i = 0; i < 32; ++i) {
        if (static_cast<char>(p[i]) == target_type)
            mask |= (1u << i);
    }
    return mask;
#endif
}

// Vectorized validation of 2-byte BE length prefix array.
// Returns true if all count lengths in buffer are within [min_len, max_len].
[[nodiscard]] inline bool validate_lengths_batch_16(const std::uint16_t* lengths, std::size_t count,
                                                    std::uint16_t min_len,
                                                    std::uint16_t max_len) noexcept {
    for (std::size_t i = 0; i < count; ++i) {
        const std::uint16_t len = lengths[i];
        if (len < min_len || len > max_len)
            return false;
    }
    return true;
}

} // namespace mog::simd
