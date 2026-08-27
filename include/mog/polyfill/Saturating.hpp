// Saturating integer operations per P0543 semantics; std implementation when present.
#pragma once

#include <mog/BuildFeatures.hpp>

#include <cstdint>
#include <limits>

#if MOG_HAS_SATURATE_ARITHMETIC
#include <numeric>

namespace mog::sat {
using std::add_sat;
using std::mul_sat;
using std::sub_sat;
} // namespace mog::sat

#else

namespace mog::sat {

inline long long add_sat(long long a, long long b) noexcept {
    long long r = 0;
    if (!__builtin_add_overflow(a, b, &r))
        return r;
    constexpr long long kMax = std::numeric_limits<long long>::max();
    constexpr long long kMin = std::numeric_limits<long long>::min();
    return (a > 0) == (b > 0) ? (a > 0 ? kMax : kMin) : 0;
}

inline long long sub_sat(long long a, long long b) noexcept {
    long long r = 0;
    if (!__builtin_sub_overflow(a, b, &r))
        return r;
    constexpr long long kMax = std::numeric_limits<long long>::max();
    constexpr long long kMin = std::numeric_limits<long long>::min();
    return b > 0 ? kMin : kMax;
}

inline long long mul_sat(long long a, long long b) noexcept {
    long long r = 0;
    if (!__builtin_mul_overflow(a, b, &r))
        return r;
    constexpr long long kMax = std::numeric_limits<long long>::max();
    constexpr long long kMin = std::numeric_limits<long long>::min();
    const bool negative = (a > 0) != (b > 0);
    return negative ? kMin : kMax;
}

} // namespace mog::sat

#endif
