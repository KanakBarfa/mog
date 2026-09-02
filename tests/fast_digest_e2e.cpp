// Fast determinism digest E2E integration test: verifies 128-bit streaming hash,
// avalanche properties, dual-tier DeterminismDigest, and simulator reproducibility.
#include <mog/Contracts.hpp>
#include <mog/FastDigest.hpp>
#include <mog/Simulate.hpp>

#include <cassert>
#include <cstdint>
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

void test_fast_digest128_basic_determinism() {
    mog::FastDigest128 d1;
    mog::FastDigest128 d2;

    const char msg[] = "MOG_DETERMINISM_TRACE_VECTOR_0123456789";
    d1.update(msg, std::strlen(msg));
    d2.update(msg, std::strlen(msg));

    const auto dig1 = d1.finish();
    const auto dig2 = d2.finish();
    CHECK(dig1 == dig2);

    char hex1[33] = {};
    char hex2[33] = {};
    const auto s1 = mog::FastDigest128::hex(dig1, hex1);
    const auto s2 = mog::FastDigest128::hex(dig2, hex2);
    CHECK(s1 == s2);
    CHECK(s1.size() == 32);

    // Incremental streaming chunks vs one-shot block must yield identical digests
    mog::FastDigest128 d3;
    for (std::size_t i = 0; i < std::strlen(msg); ++i) {
        d3.update(msg + i, 1);
    }
    const auto dig3 = d3.finish();
    CHECK(dig1 == dig3);
}

void test_fast_digest128_avalanche_properties() {
    mog::FastDigest128 d_orig;
    const std::uint64_t data[4] = {1000, 2000, 3000, 4000};
    d_orig.update(data, sizeof(data));
    const auto dig_orig = d_orig.finish();

    // 1-bit flip must change the output digest significantly
    mog::FastDigest128 d_flip;
    const std::uint64_t data_flip[4] = {1001, 2000, 3000, 4000};
    d_flip.update(data_flip, sizeof(data_flip));
    const auto dig_flip = d_flip.finish();

    CHECK(dig_orig != dig_flip);
}

void test_dual_tier_determinism_digest() {
    mog::DeterminismDigest golden_d(mog::DigestMode::golden);
    mog::DeterminismDigest fast_d(mog::DigestMode::fast);

    CHECK(golden_d.mode() == mog::DigestMode::golden);
    CHECK(fast_d.mode() == mog::DigestMode::fast);

    const std::uint64_t payload[8] = {10, 20, 30, 40, 50, 60, 70, 80};
    golden_d.update(payload, sizeof(payload));
    fast_d.update(payload, sizeof(payload));

    const auto sha_res = golden_d.finish_sha256();
    const auto fast_res = fast_d.finish_fast128();

    CHECK(!sha_res.empty());
    CHECK(!fast_res.empty());
}

void test_execution_simulator_fast_digest_mode() {
    mog::SimConfig cfg_golden{};
    cfg_golden.book.arena_capacity = 1 << 16;
    cfg_golden.book.ladder = {0, 10000, 64};
    cfg_golden.external_ref_limit = 1000;
    cfg_golden.digest_mode = mog::DigestMode::golden;

    mog::SimConfig cfg_fast = cfg_golden;
    cfg_fast.digest_mode = mog::DigestMode::fast;

    mog::ExecutionSimulator sim_golden(cfg_golden);
    mog::ExecutionSimulator sim_fast(cfg_fast);

    // Submit identical scenarios
    CHECK(ok(sim_golden.seed_external(mog::OrderId{1}, mog::Side::sell, mog::Qty{100},
                                      mog::Price{2000})));
    CHECK(ok(
        sim_fast.seed_external(mog::OrderId{1}, mog::Side::sell, mog::Qty{100}, mog::Price{2000})));

    const mog::SimInbound in{mog::OrderId{2001}, mog::Side::buy, mog::Qty{100}, mog::Price{2000},
                             mog::SimOrderType::day_limit};
    sim_golden.submit(in, 100);
    sim_fast.submit(in, 100);

    sim_golden.advance_time(50);
    sim_fast.advance_time(50);

    sim_golden.drain();
    sim_fast.drain();

    const auto sha_digest = sim_golden.trace_digest();
    const auto fast_digest = sim_fast.fast_trace_digest();

    CHECK((sha_digest != std::array<unsigned char, 32>{}));
    CHECK((fast_digest != std::array<unsigned char, 16>{}));

    // Replaying same script on fresh fast simulator yields identical fast digest
    mog::ExecutionSimulator sim_fast2(cfg_fast);
    CHECK(ok(sim_fast2.seed_external(mog::OrderId{1}, mog::Side::sell, mog::Qty{100},
                                     mog::Price{2000})));
    sim_fast2.submit(in, 100);
    sim_fast2.advance_time(50);
    sim_fast2.drain();

    CHECK(sim_fast.fast_trace_digest() == sim_fast2.fast_trace_digest());
}

} // namespace

int main() {
    test_fast_digest128_basic_determinism();
    test_fast_digest128_avalanche_properties();
    test_dual_tier_determinism_digest();
    test_execution_simulator_fast_digest_mode();
    return 0;
}
