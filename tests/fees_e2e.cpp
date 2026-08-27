// Fee settlement e2e (PLAN open question 5): maker/taker tiers in signed
// basis points of notional, truncated toward zero, applied at exactly two
// points - taker on the aggressive leg's decision, maker per passive fill.
// Zero rates are the default and must leave every observable number
// identical to the fee-free engine, which the untouched determinism
// anchors already prove elsewhere.
#include <mog/Metrics.hpp>
#include <mog/Simulate.hpp>

#include <cstdint>
#include <cstdio>

namespace {

using mog::Account;
using mog::ExecutionSimulator;
using mog::fee_bps_of;
using mog::ok;
using mog::OrderId;
using mog::Price;
using mog::Qty;
using mog::Side;
using mog::SimConfig;
using mog::SimDecision;
using mog::SimFillReport;
using mog::SimInbound;
using mog::SimOrderType;

constexpr std::uint64_t kExt = std::uint64_t{1} << 62;

int failures = 0;

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                   \
            ++failures;                                                                            \
        }                                                                                          \
    } while (false)

SimConfig base_config() noexcept {
    SimConfig cfg{};
    cfg.book.arena_capacity = 256;
    cfg.book.ladder.lo_tick = 0;
    cfg.book.ladder.hi_tick = 4000; // exclusive
    cfg.book.ladder.page_pool = 8;
    cfg.event_capacity = 1024;
    return cfg;
}

} // namespace

int main() {
    // --- rounding rule: truncation toward zero ---------------------------------
    CHECK(fee_bps_of(20'000, 25) == 50); // exact: 20000*25/10000
    CHECK(fee_bps_of(7'021, 3) == 2);    // 21063/10000 -> 2
    CHECK(fee_bps_of(7'021, -3) == -2);  // rebate magnitude truncates too
    CHECK(fee_bps_of(9'999, 1) == 0);    // below one tick of fee
    CHECK(fee_bps_of(0, 100) == 0);
    CHECK(fee_bps_of(10'000, -150) == -150); // full rebate sign passthrough

    // --- taker charge on the aggressive leg -------------------------------------
    {
        SimConfig cfg = base_config();
        cfg.taker_fee_bps = 25; // 0.25% of notional
        ExecutionSimulator sim(cfg);
        CHECK(ok(sim.seed_external(OrderId{1}, Side::sell, Qty{50}, Price{1000})));
        static_cast<void>(sim.submit(
            SimInbound{OrderId{kExt + 1}, Side::buy, Qty{20}, Price{1000}, SimOrderType::market},
            10));
        sim.drain();
        const std::vector<SimDecision>& d = sim.decisions();
        CHECK(d.size() == 1);
        CHECK(d[0].filled_qty == 20);
        CHECK(d[0].filled_notional == 20'000); // 20 * 1000
        CHECK(d[0].fee == 50);                 // 20000 * 25 / 10000
        CHECK(sim.reports().empty());          // no passive fills yet

        // Account integration: cash impact includes the charge exactly once.
        Account acct;
        acct.on_aggressive_fill(d[0].filled_notional, d[0].filled_qty, d[0].fee);
        CHECK(acct.cash() == -(20'000 + 50));
        CHECK(acct.net_fees() == 50);
        CHECK(acct.inventory() == 20);
    }

    // --- maker rebate per passive fill -------------------------------------------
    {
        SimConfig cfg = base_config();
        cfg.maker_fee_bps = -2; // -0.02%: rebate
        ExecutionSimulator sim(cfg);
        // Our resting bid joins external depth at the same level.
        CHECK(ok(sim.seed_external(OrderId{1}, Side::buy, Qty{50}, Price{998})));
        static_cast<void>(sim.submit(
            SimInbound{OrderId{kExt + 1}, Side::buy, Qty{20}, Price{998}, SimOrderType::day_limit},
            100));
        sim.drain();
        sim.apply_external(Side::buy, Price{998}, 60); // eats all 50 + 10 of ours
        const std::vector<SimFillReport>& r = sim.reports();
        CHECK(r.size() == 1);
        CHECK(r[0].qty == 10);
        CHECK(r[0].price_ticks == 998);
        // Notional 9'980; rebate 9980*2/10000 = 1.996 -> 1 tick truncated,
        // credited.
        CHECK(r[0].fee == -1);

        Account acct;
        acct.on_fill(r[0].price_ticks, r[0].qty, r[0].fee);
        CHECK(acct.cash() == -9'980 + 1); // cost reduced by the rebate
        CHECK(acct.net_fees() == -1);
        CHECK(acct.inventory() == 10);
    }

    // --- mixed lifecycle: taker partial then maker remainder ----------------------
    {
        SimConfig cfg = base_config();
        cfg.taker_fee_bps = 10;
        cfg.maker_fee_bps = -2;
        ExecutionSimulator sim(cfg);
        CHECK(ok(sim.seed_external(OrderId{1}, Side::sell, Qty{30}, Price{1000})));
        // Buy 50 limit @1000: sweeps 30 (taker), rests 20 (maker later).
        static_cast<void>(sim.submit(
            SimInbound{OrderId{kExt + 1}, Side::buy, Qty{50}, Price{1000}, SimOrderType::day_limit},
            10));
        sim.drain();
        CHECK(sim.decisions().size() == 1);
        const SimDecision& ack = sim.decisions()[0];
        CHECK(ack.kind == SimDecision::Kind::partial_then_rest);
        CHECK(ack.filled_qty == 30);
        CHECK(ack.filled_notional == 30'000);
        CHECK(ack.fee == 30); // 30000 * 10 / 10000

        sim.apply_external(Side::buy, Price{1000}, 20); // lift our remainder
        CHECK(sim.reports().size() == 1);
        const SimFillReport& fill = sim.reports()[0];
        CHECK(fill.qty == 20);
        CHECK(fill.fee == -4); // 20000 * 2 / 10000 rebate

        // Net fees across both legs: paid 30, rebated 4 -> net 26.
        Account acct;
        acct.on_aggressive_fill(ack.filled_notional, ack.filled_qty, ack.fee);
        acct.on_fill(fill.price_ticks, fill.qty, fill.fee);
        CHECK(acct.net_fees() == 26);
        CHECK(acct.inventory() == 50);
        CHECK(acct.cash() == -(30'000 + 30) - (20'000 + -4));
    }

    // --- zero-rate default is bit-identical to the fee-free engine ---------------
    {
        ExecutionSimulator plain(base_config());
        SimConfig with_zero = base_config();
        with_zero.maker_fee_bps = 0;
        with_zero.taker_fee_bps = 0;
        ExecutionSimulator zeroed(with_zero);

        for (ExecutionSimulator* s : {&plain, &zeroed}) {
            CHECK(ok(s->seed_external(OrderId{1}, Side::sell, Qty{40}, Price{1002})));
            // Buy 55 through the touch: sweeps all 40 external, rests 15,
            // so the later depletion produces a passive fill on both runs.
            static_cast<void>(s->submit(SimInbound{OrderId{kExt + 7}, Side::buy, Qty{55},
                                                   Price{1002}, SimOrderType::day_limit},
                                        5));
            s->drain();
            s->apply_external(Side::buy, Price{1002}, 45);
        }
        CHECK(plain.trace_digest() == zeroed.trace_digest());
        CHECK(plain.decisions().size() == 1);
        CHECK(plain.reports().size() == 1);
        CHECK(plain.decisions()[0].fee == 0);
        CHECK(plain.reports()[0].fee == 0);
    }

    if (failures == 0)
        std::printf("fees_e2e: all checks passed\n");
    else
        std::printf("fees_e2e: %d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
