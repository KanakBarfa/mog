// Runtime ISA detection and kernel table construction; baseline is always correct.
#pragma once

#include <mog/Build.hpp>

#include <cstdint>

namespace mog {

enum class Isa : int { sse4_baseline = 0, avx2 = 1, avx512 = 2 };

[[nodiscard]] inline Isa detect_isa() noexcept {
#if defined(__x86_64__)
    __builtin_cpu_init();
    if (__builtin_cpu_supports("avx512f"))
        return Isa::avx512;
    if (__builtin_cpu_supports("avx2"))
        return Isa::avx2;
#endif
    return Isa::sse4_baseline;
}

[[nodiscard]] constexpr const char* isa_name(Isa isa) noexcept {
    switch (isa) {
    case Isa::avx512:
        return "avx512";
    case Isa::avx2:
        return "avx2";
    case Isa::sse4_baseline:
        return "sse4_baseline";
    }
    return "?";
}

} // namespace mog
