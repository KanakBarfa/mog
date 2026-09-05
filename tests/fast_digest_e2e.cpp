// FastDigest128 e2e tests: operations, bitflip sensitivity, dual-tier digest, reproducibility.
#include <mog/Contracts.hpp>
#include <mog/FastDigest.hpp>
#include <mog/Orchestrate.hpp>
#include <mog/SimRun.hpp>
#include <mog/Simulate.hpp>

#include <cassert>
#include <cstdint>
#include <cstring>
#include <string>
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

int g_violations = 0;
void count_violation(const char*, const char*, int, const char*) noexcept {
    ++g_violations;
}

mog::SimConfig tiny_config(mog::DigestMode mode) {
    mog::SimConfig cfg{};
    cfg.book.arena_capacity = 1 << 12;
    cfg.book.ladder = {9000, 11000, 64};
    cfg.external_ref_limit = 1000; // strategy refs (2001+) sit above this
    cfg.digest_mode = mode;
    return cfg;
}

void drive_tiny(mog::ExecutionSimulator& sim) {
    CHECK(
        ok(sim.seed_external(mog::OrderId{1}, mog::Side::sell, mog::Qty{100}, mog::Price{10000})));
    const mog::SimInbound in{mog::OrderId{2001}, mog::Side::buy, mog::Qty{40}, mog::Price{10000},
                             mog::SimOrderType::day_limit};
    sim.submit(in, 100);
    sim.advance_time(50);
    sim.drain();
}

void test_fast_digest_variance() {
    mog::ExecutionSimulator base(tiny_config(mog::DigestMode::fast));
    drive_tiny(base);
    mog::ExecutionSimulator other(tiny_config(mog::DigestMode::fast));
    CHECK(ok(
        other.seed_external(mog::OrderId{1}, mog::Side::sell, mog::Qty{100}, mog::Price{10000})));
    const mog::SimInbound in{mog::OrderId{2001}, mog::Side::buy, mog::Qty{41}, mog::Price{10000},
                             mog::SimOrderType::day_limit};
    other.submit(in, 100);
    other.advance_time(50);
    other.drain();
    CHECK(base.fast_trace_digest() != other.fast_trace_digest());
}

void test_wrong_mode_access_refused() {
    // Observe mode makes refusal countable instead of aborting.
    const auto prev_mode = mog::contracts::set_mode(mog::contracts::Mode::observe);
    const auto prev_handler = mog::contracts::set_handler(count_violation);
    g_violations = 0;
    mog::ExecutionSimulator sim_fast(tiny_config(mog::DigestMode::fast));
    mog::ExecutionSimulator sim_golden(tiny_config(mog::DigestMode::golden));
    drive_tiny(sim_fast);
    drive_tiny(sim_golden);
    static_cast<void>(sim_fast.trace_digest());        // wrong tier
    static_cast<void>(sim_golden.fast_trace_digest()); // wrong tier
    CHECK(g_violations == 2);
    static_cast<void>(sim_fast.fast_trace_digest()); // right tier
    static_cast<void>(sim_golden.trace_digest());    // right tier
    CHECK(g_violations == 2);
    static_cast<void>(mog::contracts::set_handler(prev_handler));
    static_cast<void>(mog::contracts::set_mode(prev_mode));
}

const std::string kTaintScript = "kind,ts_ns,side,price_ticks,qty,ref\n"
                                 "ext_add,100,B,9900,100,1\n"
                                 "ext_add,110,S,10100,100,2\n"
                                 "strat_limit,150,B,10000,10,10\n"
                                 "trade,200,S,10000,50,3\n";

void test_simrun_fast_taint() {
    mog::SimConfig fast = tiny_config(mog::DigestMode::fast);
    mog::SimConfig golden = tiny_config(mog::DigestMode::golden);
    const auto f1 = mog::simrun::run(kTaintScript, fast);
    const auto f2 = mog::simrun::run(kTaintScript, fast);
    const auto g = mog::simrun::run(kTaintScript, golden);
    CHECK(f1.has_value() && f2.has_value() && g.has_value());
    CHECK(f1->digest_mode == mog::DigestMode::fast);
    CHECK(g->digest_mode == mog::DigestMode::golden);
    CHECK(f1->digest_high == f2->digest_high);
    CHECK(f1->digest_high != 0);
    CHECK(f1->digest_high != g->digest_high);
}

void test_orchestrate_mode_uniformity() {
    mog::Orchestrator a, b;
    const std::size_t a0 = a.add_instrument(tiny_config(mog::DigestMode::fast), "A");
    const std::size_t b0 = b.add_instrument(tiny_config(mog::DigestMode::fast), "A");
    static_cast<void>(
        a.seed_external(a0, mog::OrderId{1}, mog::Side::sell, mog::Qty{100}, mog::Price{10000}));
    static_cast<void>(
        b.seed_external(b0, mog::OrderId{1}, mog::Side::sell, mog::Qty{100}, mog::Price{10000}));
    a.drain(a0);
    b.drain(b0);
    CHECK(a.global_digest_mode() == mog::DigestMode::fast);
    CHECK(a.global_digest() == b.global_digest());
    const auto prev_mode = mog::contracts::set_mode(mog::contracts::Mode::observe);
    const auto prev_handler = mog::contracts::set_handler(count_violation);
    g_violations = 0;
    mog::Orchestrator m;
    static_cast<void>(m.add_instrument(tiny_config(mog::DigestMode::golden), "G"));
    static_cast<void>(m.add_instrument(tiny_config(mog::DigestMode::fast), "F"));
    static_cast<void>(m.global_digest());
    CHECK(g_violations >= 1);
    static_cast<void>(mog::contracts::set_handler(prev_handler));
    static_cast<void>(mog::contracts::set_mode(prev_mode));
}

} // namespace

int main() {
    test_fast_digest128_basic_determinism();
    test_fast_digest128_avalanche_properties();
    test_dual_tier_determinism_digest();
    test_execution_simulator_fast_digest_mode();
    test_fast_digest_variance();
    test_wrong_mode_access_refused();
    test_simrun_fast_taint();
    test_orchestrate_mode_uniformity();
    return 0;
}
