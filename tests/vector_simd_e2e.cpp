// SIMD and SWAR vector operations E2E test: verifies symbol matching,
// message classification, and length prefix validation across architectures.
#include <mog/Contracts.hpp>
#include <mog/simd/SWAR.hpp>
#include <mog/simd/VectorScan.hpp>

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace {

#define CHECK(expr)                                                                                \
    do {                                                                                           \
        if (!(expr)) {                                                                             \
            std::fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, #expr);        \
            std::exit(1);                                                                          \
        }                                                                                          \
    } while (0)

void test_simd_symbol_equality() {
    const char sym1[8] = {'A', 'A', 'P', 'L', ' ', ' ', ' ', ' '};
    const char sym2[8] = {'A', 'A', 'P', 'L', ' ', ' ', ' ', ' '};
    const char sym3[8] = {'M', 'S', 'F', 'T', ' ', ' ', ' ', ' '};

    CHECK(mog::simd::symbol8_equal(sym1, sym2));
    CHECK(!mog::simd::symbol8_equal(sym1, sym3));

    CHECK(mog::simd::trim_symbol8_length(sym1) == 4);
    CHECK(mog::simd::trim_symbol8_length(sym3) == 4);

    const char sym_short[8] = {'C', ' ', ' ', ' ', ' ', ' ', ' ', ' '};
    CHECK(mog::simd::trim_symbol8_length(sym_short) == 1);
}

void test_simd_msg_type_classification() {
    unsigned char buffer[32] = {};
    for (std::size_t i = 0; i < 32; ++i) {
        buffer[i] = (i % 4 == 0) ? 'A' : 'E';
    }

    const std::uint32_t mask_a = mog::simd::match_msg_types_32b(buffer, 'A');
    // Bits 0, 4, 8, 12, 16, 20, 24, 28 should be 1
    const std::uint32_t expected_a = 0x11111111u;
    CHECK(mask_a == expected_a);

    const std::uint32_t mask_e = mog::simd::match_msg_types_32b(buffer, 'E');
    CHECK(mask_e == ~expected_a);

    const std::uint32_t mask_z = mog::simd::match_msg_types_32b(buffer, 'Z');
    CHECK(mask_z == 0);
}

void test_simd_length_validation() {
    const std::uint16_t lengths[8] = {36, 40, 31, 36, 40, 31, 36, 40};
    CHECK(mog::simd::validate_lengths_batch_16(lengths, 8, 12, 50));
    CHECK(!mog::simd::validate_lengths_batch_16(lengths, 8, 35, 50)); // 31 fails min_len
    CHECK(!mog::simd::validate_lengths_batch_16(lengths, 8, 12, 38)); // 40 fails max_len
}

} // namespace

int main() {
    test_simd_symbol_equality();
    test_simd_msg_type_classification();
    test_simd_length_validation();
    return 0;
}
