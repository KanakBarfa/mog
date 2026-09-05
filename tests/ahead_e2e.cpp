// Differential: incremental queue-ahead must match the exact-walk twin, past map capacity.
#include <mog/Simulate.hpp>

#include <cstdint>
#include <cstdio>
#include <random>
#include <vector>

namespace {

using mog::ExecutionSimulator;
using mog::OrderId;
using mog::Price;
using mog::Qty;
using mog::Side;
using mog::SimConfig;
using mog::SimInbound;
using mog::SimOrderType;

constexpr std::uint64_t kExt = std::uint64_t{1} << 62;

int failures = 0;

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            std::fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #cond);                    \
            ++failures;                                                                            \
        }                                                                                          \
    } while (false)

SimConfig small_config() noexcept {
    SimConfig cfg{};
    cfg.book.arena_capacity = 4096;
    cfg.book.ladder.lo_tick = 900;
    cfg.book.ladder.hi_tick = 1100;
    cfg.book.ladder.page_pool = 64;
    cfg.event_capacity = 4096;
    cfg.bid_depletion_per_us = 0.05;
    cfg.ask_depletion_per_us = 0.05;
    return cfg;
}

// Snapshot live aheads, re-derive via the twin, require exact equality.
void check_twin_exact(ExecutionSimulator& sim, const std::vector<std::uint64_t>& refs) {
    std::vector<std::int64_t> before;
    before.reserve(refs.size());
    for (const auto r : refs)
        before.push_back(sim.queue_ahead_of(OrderId{r}));
    sim.recompute_all_tracked();
    for (std::size_t i = 0; i < refs.size(); ++i)
        CHECK(sim.queue_ahead_of(OrderId{refs[i]}) == before[i]);
}

void differential_seed(std::uint64_t seed) {
    std::mt19937_64 rng(seed);
    ExecutionSimulator sim(small_config());
    std::vector<std::uint64_t> posted;
    std::uint64_t ref = kExt + 1;
    for (int step = 0; step < 400; ++step) {
        const int op = static_cast<int>(rng() % 7);
        const auto side = (rng() & 1) ? Side::buy : Side::sell;
        const auto px = Price{900 + static_cast<std::int64_t>(rng() % 200)};
        if (op <= 1) {
            const auto q = Qty{1 + static_cast<std::int64_t>(rng() % 50)};
            const auto ty = op == 0 ? SimOrderType::day_limit : SimOrderType::ioc;
            static_cast<void>(sim.submit(SimInbound{OrderId{ref}, side, q, px, ty},
                                         10 * static_cast<std::uint64_t>(step)));
            posted.push_back(ref);
            ++ref;
        } else if (op == 2 && !posted.empty()) {
            static_cast<void>(sim.cancel_strategy(OrderId{posted[rng() % posted.size()]}));
        } else if (op == 3 && !posted.empty()) {
            const auto old = posted[rng() % posted.size()];
            static_cast<void>(sim.replace_strategy(
                OrderId{old}, OrderId{ref}, Qty{1 + static_cast<std::int64_t>(rng() % 50)},
                Price{900 + static_cast<std::int64_t>(rng() % 200)}));
            posted.push_back(ref);
            ++ref;
        } else if (op == 4) {
            static_cast<void>(sim.seed_external(OrderId{1 + (rng() % 500)}, side,
                                                Qty{1 + static_cast<std::int64_t>(rng() % 100)},
                                                px));
        } else if (op == 5) {
            sim.apply_external(side, px, 1 + static_cast<std::int64_t>(rng() % 60));
        } else {
            sim.advance_time(1000 + rng() % 5000);
        }
        if (step % 40 == 39)
            sim.drain();
    }
    sim.drain();
    CHECK(sim.audit());
    check_twin_exact(sim, posted);
}

void fallback_engagement() {
    // 5000 tracked levels overflow the 4096-entry map; aheads must stay exact.
    SimConfig cfg{};
    cfg.book.arena_capacity = 16384;
    cfg.book.ladder.lo_tick = 0;
    cfg.book.ladder.hi_tick = 20000;
    cfg.book.ladder.page_pool = 32;
    cfg.event_capacity = 8192;
    ExecutionSimulator sim(cfg);
    std::vector<std::uint64_t> refs;
    refs.reserve(5000);
    for (std::uint64_t i = 0; i < 5000; ++i) {
        const auto px = Price{1000 + static_cast<std::int64_t>(i)};
        static_cast<void>(sim.seed_external(OrderId{100 + i}, Side::buy, Qty{10}, px));
        const auto ty = SimOrderType::day_limit;
        static_cast<void>(
            sim.submit(SimInbound{OrderId{kExt + i}, Side::buy, Qty{5}, px, ty}, 10 * i));
        refs.push_back(kExt + i);
    }
    sim.drain();
    for (std::uint64_t i = 0; i < 5000; ++i)
        CHECK(sim.queue_ahead_of(OrderId{kExt + i}) == 10);
    CHECK(sim.audit());
    check_twin_exact(sim, refs);
    for (std::uint64_t i = 0; i < 5000; i += 2)
        static_cast<void>(sim.cancel_strategy(OrderId{kExt + i}));
    for (std::uint64_t i = 1; i < 5000; i += 2)
        CHECK(sim.queue_ahead_of(OrderId{kExt + i}) == 10);
    CHECK(sim.audit());
}

} // namespace

int main() {
    for (std::uint64_t seed = 1; seed <= 15; ++seed)
        differential_seed(seed);
    fallback_engagement();
    if (failures != 0) {
        std::fprintf(stderr, "ahead_e2e: %d failures\n", failures);
        return 1;
    }
    return 0;
}
