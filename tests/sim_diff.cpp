// M4 fuzz: random op streams under contract enforcement. Checks tracker/book
// mirror conservation continuously, audits both structures, verifies
// determinism by double-run SHA-256 equality, and reports model-vs-naive
// divergence for the whitepaper's numbers.
#include <mog/Sha256.hpp>
#include <mog/Simulate.hpp>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <random>

namespace {

using mog::ExecutionSimulator;
using mog::ok;
using mog::OrderId;
using mog::Price;
using mog::Qty;
using mog::Side;
using mog::SimConfig;
using mog::SimInbound;
using mog::SimOrderType;

constexpr std::uint64_t kExt = std::uint64_t{1} << 62;

SimConfig fuzz_config(std::uint64_t seed) noexcept {
    SimConfig cfg{};
    cfg.book.arena_capacity = 4096;
    cfg.book.ladder.lo_tick = 0;
    cfg.book.ladder.hi_tick = 4000;
    cfg.book.ladder.page_pool = 8;
    cfg.event_capacity = 1 << 16;
    cfg.bid_depletion_per_us = 0.4;
    cfg.ask_depletion_per_us = 0.4;
    cfg.seed = seed;
    cfg.parse_latency_ns = 40;
    cfg.decision_latency_ns = 120;
    cfg.wire_latency_ns = 60;
    cfg.jitter_max_ns = 30;
    return cfg;
}

struct RunStats {
    std::uint64_t submits = 0, cancels = 0, replaces = 0, external_seeds = 0, advances = 0;
    std::array<unsigned char, 32> digest{};
};

RunStats run_script(std::uint64_t seed, std::uint64_t n_ops) {
    ExecutionSimulator sim(fuzz_config(seed));
    std::mt19937_64 rng(seed);
    RunStats st{};
    std::uint64_t next_strategy_ref = kExt + 1;
    std::vector<OrderId> live_strategy;

    for (std::uint64_t step = 0; step < n_ops; ++step) {
        const int roll = static_cast<int>(rng() % 100);
        if (roll < 25) { // seed external depth
            const std::uint64_t ref = rng() % (kExt / 2);
            const Side s = (rng() & 1) ? Side::buy : Side::sell;
            const std::int64_t price = 1500 + static_cast<std::int64_t>(rng() % 1000);
            const std::int64_t qty = static_cast<std::int64_t>(1 + rng() % 500);
            if (sim.book().find_handle(OrderId{ref}).index == mog::kNullIndex)
                if (ok(sim.seed_external(OrderId{ref}, s, Qty{qty}, Price{price})))
                    ++st.external_seeds;
        } else if (roll < 65) { // submit strategy order of a random type
            SimOrderType types[4] = {SimOrderType::day_limit, SimOrderType::market,
                                     SimOrderType::ioc, SimOrderType::post_only};
            SimInbound in{};
            in.ref = OrderId{next_strategy_ref};
            in.side = (rng() & 1) ? Side::buy : Side::sell;
            in.qty = Qty{static_cast<std::int64_t>(1 + rng() % 300)};
            in.price = Price{1500 + static_cast<std::int64_t>(rng() % 1000)};
            in.type = types[rng() % 4];
            sim.submit(in, rng() % 100000);
            live_strategy.push_back(in.ref);
            ++next_strategy_ref;
            ++st.submits;
        } else if (roll < 80) { // cancel a random prior submission attempt
            ++st.cancels;
            // Refs may be gone; cancel_strategy reports honestly either way.
            if (!live_strategy.empty()) {
                const std::size_t idx = rng() % live_strategy.size();
                const OrderId victim = live_strategy[idx];
                static_cast<void>(sim.cancel_strategy(victim));
                live_strategy[idx] = live_strategy.back();
                live_strategy.pop_back();
            }
        } else if (roll < 90) { // time advances drive statistical depletion
            sim.advance_time(1 + rng() % 2000);
            ++st.advances;
        } else { // drain decisions so the wheel stays bounded
            sim.run_until(rng() % 200000);
        }
        if ((rng() & 1) && next_strategy_ref > kExt + 1)
            --next_strategy_ref; // reuse pressure: refs can collide with resting
        if (step % 997 == 0) {
            if (!sim.audit()) {
                std::fprintf(stderr, "audit failed at step %llu\n",
                             static_cast<unsigned long long>(step));
                std::exit(2);
            }
        }
    }
    sim.drain();
    if (!sim.audit()) {
        std::fputs("final audit failed\n", stderr);
        std::exit(2);
    }
    st.digest = sim.trace_digest();

    const auto nf = sim.naive_filled_total();
    const auto mf = sim.model_filled_total();
    {
        const auto af = sim.aggressive_filled_total();
        std::printf("seed=%llu naive=%llu aggressive=%llu passive=%lld\n",
                    static_cast<unsigned long long>(seed), static_cast<unsigned long long>(nf),
                    static_cast<unsigned long long>(af), static_cast<long long>(mf - af));
    }
    return st;
}

} // namespace

int main() {
    const std::uint64_t n_ops = 200000;
    const auto a = run_script(20260825, n_ops);
    const auto b = run_script(20260825, n_ops);
    if (a.digest != b.digest) {
        std::fputs("FAIL determinism across runs\n", stderr);
        return 1;
    }
    char hex[65];
    const auto v = mog::Sha256::hex(a.digest, hex);
    std::printf("sha256 trace: %.*s\n", static_cast<int>(v.size()), v.data());
    std::puts("sim fuzz OK");
    return 0;
}
