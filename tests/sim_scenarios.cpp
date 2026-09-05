// M4 scenario suite: hand-built expectations for order types, STP modes,
// queue-position dynamics, latency ordering, replace semantics, and
// conservation contracts. Every quantity below is computed by hand.
#include <mog/Simulate.hpp>

#include <cstdint>
#include <cstdio>

namespace {

using mog::ExecutionSimulator;
using mog::IcebergSpec;
using mog::JitterKind;
using mog::ok;
using mog::OrderId;
using mog::Price;
using mog::Qty;
using mog::Side;
using mog::SimConfig;
using mog::SimDecision;
using mog::SimInbound;
using mog::SimOrderType;
using mog::StpMode;

constexpr std::uint64_t kExt = std::uint64_t{1} << 62;

SimConfig base_config() noexcept {
    SimConfig cfg{};
    cfg.book.arena_capacity = 256;
    cfg.book.ladder.lo_tick = 0;
    cfg.book.ladder.hi_tick = 4000; // exclusive
    cfg.book.ladder.page_pool = 8;
    cfg.event_capacity = 1024;
    return cfg;
}

int failures = 0;

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            std::fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #cond);                    \
            ++failures;                                                                            \
        }                                                                                          \
    } while (false)

SimInbound inbound(std::uint64_t ref, Side s, std::uint64_t qty, std::int64_t price,
                   SimOrderType type) {
    return SimInbound{OrderId{ref}, s, Qty{static_cast<std::int64_t>(qty)}, Price{price}, type};
}

} // namespace

int main() {
    // S1: queue-position dynamics with exact depletion arithmetic.
    {
        auto cfg = base_config();
        cfg.bid_depletion_per_us = 0; // statistical driver off; direct injection only
        ExecutionSimulator sim(cfg);
        CHECK(ok(sim.seed_external(OrderId{1}, Side::buy, Qty{50}, Price{998})));
        sim.submit(inbound(kExt + 1, Side::buy, 20, 998, SimOrderType::day_limit), 100);
        sim.drain();
        CHECK(sim.book().qty_at(Side::buy, Price{998}) == 70);

        // 30 units of external flow: eats 30 of the 50 ahead, we do not fill.
        sim.apply_external(Side::buy, Price{998}, 30);
        const auto h = sim.book().find_handle(OrderId{kExt + 1});
        (void)h;
        // Our remaining must be untouched; level total dropped by 30.
        CHECK(sim.book().qty_at(Side::buy, Price{998}) == 40);
        // 25 more: clears the last 20 ahead, then fills 5 of our 20.
        sim.apply_external(Side::buy, Price{998}, 25);
        CHECK(sim.book().remaining_of(OrderId{kExt + 1}) == 15);
        CHECK(sim.reports().size() >= 1);
        if (!sim.reports().empty()) {
            CHECK(sim.reports().back().ref == kExt + 1);
            CHECK(sim.reports().back().price_ticks == 998);
            CHECK(sim.reports().back().qty == 5);
        }
        // Final 25: fills the remaining 15 and removes the order.
        sim.apply_external(Side::buy, Price{998}, 25);
        CHECK(sim.book().find_handle(OrderId{kExt + 1}).index == mog::kNullIndex);
    }

    // S2: IOC partial fill cancels the remainder; full fill cancels nothing.
    {
        ExecutionSimulator sim(base_config());
        CHECK(ok(sim.seed_external(OrderId{2}, Side::sell, Qty{60}, Price{1000})));
        CHECK(ok(sim.seed_external(OrderId{3}, Side::sell, Qty{40}, Price{1001})));
        sim.submit(inbound(kExt + 10, Side::buy, 80, 1001, SimOrderType::ioc), 0);
        sim.submit(inbound(kExt + 11, Side::buy, 150, 1001, SimOrderType::ioc), 1);
        sim.drain();
        const auto& ds = sim.decisions();
        CHECK(ds.size() == 2);
        if (ds.size() == 2) {
            // First IOC: 60 at 1000 plus 20 of the 40 at 1001.
            CHECK(ds[0].kind == SimDecision::Kind::filled);
            CHECK(ds[0].filled_qty == 80);
            CHECK(ds[0].cancelled_qty == 0);
            // Second IOC sees only the 20 left at 1001.
            CHECK(ds[1].kind == SimDecision::Kind::partial_then_cancelled);
            CHECK(ds[1].filled_qty == 20);
            CHECK(ds[1].cancelled_qty == 130);
        }
        CHECK(sim.book().remaining_of(OrderId{2}) == 0);
        CHECK(sim.book().remaining_of(OrderId{3}) == 0); // both IOCs consume it fully
        CHECK(sim.book().live_orders() == 0);
    }

    // S3: market order sweeps every level until the side is empty.
    {
        ExecutionSimulator sim(base_config());
        CHECK(ok(sim.seed_external(OrderId{4}, Side::sell, Qty{30}, Price{1000})));
        CHECK(ok(sim.seed_external(OrderId{5}, Side::sell, Qty{30}, Price{1002})));
        sim.submit(inbound(kExt + 12, Side::buy, 100, 9999, SimOrderType::market), 0);
        sim.drain();
        const auto& ds = sim.decisions();
        CHECK(ds.size() == 1);
        if (ds.size() == 1) {
            CHECK(ds[0].filled_qty == 60);
            CHECK(ds[0].cancelled_qty == 40);
        }
        CHECK(sim.book().best_ask() == mog::kNoTick);
    }

    // S4: post-only rejects when crossing, rests when it would not.
    {
        ExecutionSimulator sim(base_config());
        CHECK(ok(sim.seed_external(OrderId{6}, Side::sell, Qty{10}, Price{1000})));
        sim.submit(inbound(kExt + 13, Side::buy, 20, 1001, SimOrderType::post_only), 0);
        sim.submit(inbound(kExt + 14, Side::buy, 20, 999, SimOrderType::post_only), 1);
        sim.drain();
        const auto& ds = sim.decisions();
        CHECK(ds.size() == 2);
        if (ds.size() == 2) {
            CHECK(ds[0].kind == SimDecision::Kind::rejected_would_cross);
            CHECK(ds[1].kind == SimDecision::Kind::rested);
        }
        CHECK(sim.book().live_orders() == 2); // external + our rested bid
    }

    // S5: self-trade prevention across all four modes, hand-computed.
    {
        // none: self-match executes against our own resting sell.
        {
            ExecutionSimulator sim(base_config());
            sim.set_stp_mode(StpMode::none);
            sim.submit(inbound(kExt + 20, Side::sell, 50, 1000, SimOrderType::day_limit), 0);
            sim.drain();
            sim.clear_reports();
            sim.submit(inbound(kExt + 21, Side::buy, 30, 1000, SimOrderType::ioc), 10);
            sim.drain();
            CHECK(sim.book().remaining_of(OrderId{kExt + 20}) == 20);
            const auto& ds = sim.decisions();
            CHECK(ds.back().kind == SimDecision::Kind::filled && ds.back().filled_qty == 30);
        }
        // cancel_newest: incoming dies untouched-book.
        {
            ExecutionSimulator sim(base_config());
            sim.set_stp_mode(StpMode::cancel_newest);
            sim.submit(inbound(kExt + 22, Side::sell, 50, 1000, SimOrderType::day_limit), 0);
            sim.drain();
            sim.submit(inbound(kExt + 23, Side::buy, 30, 1000, SimOrderType::ioc), 10);
            sim.drain();
            CHECK(sim.book().remaining_of(OrderId{kExt + 22}) == 50);
            const auto& ds = sim.decisions();
            CHECK(ds.back().kind == SimDecision::Kind::rejected_stp);
            CHECK(ds.back().cancelled_qty == 30);
        }
        // cancel_oldest: resting victim removed, incoming keeps going, then rests.
        {
            ExecutionSimulator sim(base_config());
            sim.set_stp_mode(StpMode::cancel_oldest);
            sim.submit(inbound(kExt + 24, Side::sell, 50, 1000, SimOrderType::day_limit), 0);
            sim.drain();
            sim.submit(inbound(kExt + 25, Side::buy, 30, 1000, SimOrderType::day_limit), 10);
            sim.drain();
            CHECK(sim.book().find_handle(OrderId{kExt + 24}).index == mog::kNullIndex);
            // Incoming had nothing left to hit after removing the victim; it rests in full.
            CHECK(sim.book().remaining_of(OrderId{kExt + 25}) == 30);
        }
        // decrement: both sides shrink by the mutual minimum.
        {
            ExecutionSimulator sim(base_config());
            sim.set_stp_mode(StpMode::decrement);
            sim.submit(inbound(kExt + 26, Side::sell, 50, 1000, SimOrderType::day_limit), 0);
            sim.drain();
            sim.submit(inbound(kExt + 27, Side::buy, 30, 1000, SimOrderType::ioc), 10);
            sim.drain();
            CHECK(sim.book().remaining_of(OrderId{kExt + 26}) == 20);
            const auto& ds = sim.decisions();
            CHECK(ds.back().filled_qty == 30);
        }
    }

    // S15: STP decrement syncs the resting mirror and emits a maker report.
    {
        ExecutionSimulator sim(base_config());
        sim.set_stp_mode(StpMode::decrement);
        sim.submit(inbound(kExt + 80, Side::sell, 50, 1000, SimOrderType::day_limit), 0);
        sim.drain();
        sim.submit(inbound(kExt + 81, Side::buy, 30, 1000, SimOrderType::ioc), 10);
        sim.drain();
        CHECK(sim.book().remaining_of(OrderId{kExt + 80}) == 20);
        CHECK(sim.audit());
        CHECK(sim.reports().size() == 1);
        if (!sim.reports().empty()) {
            CHECK(sim.reports().front().ref == kExt + 80);
            CHECK(sim.reports().front().qty == 30);
        }
        CHECK(sim.decisions().back().filled_qty == 30);
    }

    // S6: replace moves to the tail of the new level; old tracker dies.
    {
        ExecutionSimulator sim(base_config());
        CHECK(ok(sim.seed_external(OrderId{7}, Side::buy, Qty{40}, Price{995})));
        sim.submit(inbound(kExt + 30, Side::buy, 10, 996, SimOrderType::day_limit), 0);
        sim.drain();
        CHECK(
            ok(sim.replace_strategy(OrderId{kExt + 30}, OrderId{kExt + 31}, Qty{10}, Price{995})));
        // After replace our order sits behind 40 at 995 and joins as the tail.
        CHECK(sim.book().qty_at(Side::buy, Price{995}) == 50);
        CHECK(sim.book().find_handle(OrderId{kExt + 30}).index == mog::kNullIndex);
        CHECK(sim.book().remaining_of(OrderId{kExt + 31}) == 10);
        // Depleting 45 leaves our 5.
        sim.apply_external(Side::buy, Price{995}, 45);
        CHECK(sim.book().remaining_of(OrderId{kExt + 31}) == 5);
    }

    // S7: latency pipeline orders decisions by scheduled decision time.
    {
        auto cfg = base_config();
        cfg.parse_latency_ns = 100;
        cfg.decision_latency_ns = 200;
        cfg.wire_latency_ns = 50;
        cfg.jitter_max_ns = 0;
        ExecutionSimulator sim(cfg);
        // The resting side must be a tracked strategy order so its fill shows
        // up in the report channel.
        const auto t0 =
            sim.submit(inbound(kExt + 39, Side::sell, 9, 905, SimOrderType::day_limit), 900);
        const auto t1 =
            sim.submit(inbound(kExt + 40, Side::buy, 5, 900, SimOrderType::day_limit), 1000);
        const auto t2 = sim.submit(inbound(kExt + 41, Side::buy, 7, 906, SimOrderType::ioc), 1100);
        CHECK(t0 == 1200);
        CHECK(t1 == 1300);
        CHECK(t2 == 1400);
        sim.run_until(1299);
        CHECK(sim.pending_decisions() == 2); // t0 already decided
        sim.run_until(1300);
        CHECK(sim.pending_decisions() == 1);
        sim.drain();
        // The IOC crosses the tracked resting sell: one visible report.
        CHECK(sim.reports().size() == 1);
        if (!sim.reports().empty()) {
            const auto& r = sim.reports().front();
            CHECK(r.ref == kExt + 39);
            CHECK(r.qty == 7);
            CHECK(r.price_ticks == 905);
            CHECK(r.visible_ts >= t2 + cfg.wire_latency_ns);
            CHECK(sim.book().remaining_of(OrderId{kExt + 39}) == 2);
        }
    }

    // S9: exact queue positions with externals interleaved between tracked
    // orders. FIFO: A(30) T1(20) B(40) T2(10) C(50) at 995 on the bid side.
    {
        ExecutionSimulator sim(base_config());
        CHECK(ok(sim.seed_external(OrderId{11}, Side::buy, Qty{30}, Price{995})));      // A
        sim.submit(inbound(kExt + 60, Side::buy, 20, 995, SimOrderType::day_limit), 0); // T1
        sim.drain();
        CHECK(ok(sim.seed_external(OrderId{12}, Side::buy, Qty{40}, Price{995})));       // B
        sim.submit(inbound(kExt + 61, Side::buy, 10, 995, SimOrderType::day_limit), 10); // T2
        sim.drain();
        CHECK(ok(sim.seed_external(OrderId{13}, Side::buy, Qty{50}, Price{995}))); // C

        CHECK(sim.queue_ahead_of(OrderId{kExt + 60}) == 30);
        CHECK(sim.queue_ahead_of(OrderId{kExt + 61}) == 90);
        CHECK(sim.book().qty_at(Side::buy, Price{995}) == 150);

        // Flow 35 consumes A fully plus 5 of T1.
        sim.apply_external(Side::buy, Price{995}, 35);
        CHECK(sim.book().remaining_of(OrderId{kExt + 60}) == 15);
        CHECK(sim.queue_ahead_of(OrderId{kExt + 60}) == 0);
        CHECK(sim.queue_ahead_of(OrderId{kExt + 61}) == 55); // T1(15) + B(40)
        // Flow 60 consumes T1(15) + B(40) + 5 of T2.
        sim.apply_external(Side::buy, Price{995}, 60);
        CHECK(sim.book().find_handle(OrderId{kExt + 60}).index == mog::kNullIndex);
        CHECK(sim.book().remaining_of(OrderId{kExt + 61}) == 5);
        CHECK(sim.queue_ahead_of(OrderId{kExt + 61}) == 0);

        const auto& rs = sim.reports();
        std::uint32_t t1_fills = 0, t2_fills = 0;
        for (const auto& r : rs) {
            if (r.ref == kExt + 60)
                t1_fills += r.qty;
            if (r.ref == kExt + 61)
                t2_fills += r.qty;
        }
        CHECK(t1_fills == 20);
        CHECK(t2_fills == 5);
    }

    // S14: cancels and replace-away settle mates left behind (T1/T2/T3 at 995).
    {
        ExecutionSimulator sim(base_config());
        sim.submit(inbound(kExt + 70, Side::buy, 10, 995, SimOrderType::day_limit), 0);
        sim.submit(inbound(kExt + 71, Side::buy, 20, 995, SimOrderType::day_limit), 1);
        sim.submit(inbound(kExt + 72, Side::buy, 30, 995, SimOrderType::day_limit), 2);
        sim.drain();
        CHECK(sim.queue_ahead_of(OrderId{kExt + 70}) == 0);
        CHECK(sim.queue_ahead_of(OrderId{kExt + 71}) == 10);
        CHECK(sim.queue_ahead_of(OrderId{kExt + 72}) == 30);
        CHECK(sim.cancel_strategy(OrderId{kExt + 70}));
        CHECK(sim.queue_ahead_of(OrderId{kExt + 70}) == -1);
        CHECK(sim.queue_ahead_of(OrderId{kExt + 71}) == 0);
        CHECK(sim.queue_ahead_of(OrderId{kExt + 72}) == 20);
        CHECK(
            ok(sim.replace_strategy(OrderId{kExt + 71}, OrderId{kExt + 73}, Qty{20}, Price{996})));
        CHECK(sim.queue_ahead_of(OrderId{kExt + 71}) == -1);
        CHECK(sim.queue_ahead_of(OrderId{kExt + 72}) == 0);
        CHECK(sim.queue_ahead_of(OrderId{kExt + 73}) == 0);
    }

    // S11: jitter shapes stay deterministic, non-negative, and bounded.
    // A single tracked resting seller is swept by twenty IOCs so every fill
    // produces an observable visibility stamp.
    {
        auto mk = [&](JitterKind k) {
            auto cfg = base_config();
            cfg.jitter_kind = k;
            cfg.wire_latency_ns = 100;
            cfg.jitter_mean_ns = 500.0;
            cfg.jitter_sigma_ns = 120.0;
            cfg.jitter_max_ns = 400;
            return ExecutionSimulator(cfg);
        };
        auto pump = [](ExecutionSimulator& sim) {
            sim.submit(inbound(kExt + 90, Side::sell, 20, 1500, SimOrderType::day_limit), 0);
            sim.drain();
            CHECK(sim.queue_ahead_of(OrderId{kExt + 90}) == 0);
            for (int i = 0; i < 20; ++i)
                sim.submit(inbound(kExt + 100 + static_cast<std::uint64_t>(i), Side::buy, 1, 1500,
                                   SimOrderType::ioc),
                           static_cast<std::uint64_t>(i));
            sim.drain();
            CHECK(sim.reports().size() == 20);
            std::uint64_t min_vis = ~std::uint64_t{0}, max_vis = 0;
            for (const auto& r : sim.reports()) {
                min_vis = std::min(min_vis, r.visible_ts);
                max_vis = std::max(max_vis, r.visible_ts);
                CHECK(r.ref == kExt + 90);
                CHECK(r.qty == 1);
            }
            return std::make_pair(min_vis, max_vis);
        };
        auto e1 = mk(JitterKind::exponential);
        const auto [lo1, hi1] = pump(e1);
        auto e2 = mk(JitterKind::exponential);
        const auto [lo2, hi2] = pump(e2);
        CHECK(lo1 == lo2 && hi1 == hi2);          // reproducible under the same seed
        CHECK(lo1 >= 100 && hi1 <= 119u + 4000u); // wire floor, 8x-mean cap
        auto n1 = mk(JitterKind::none);
        const auto [lon, hin] = pump(n1);
        CHECK(lon == 100 && hin == 119); // zero-jitter stamps are exact
    }

    // S12: correlated flow. Self-excitation makes consumption cluster after
    // activity; momentum tilts the split after a mid-price move.
    {
        auto mk = [&](double kappa, double gain) {
            auto cfg = base_config();
            cfg.book.arena_capacity = 4096;
            cfg.bid_depletion_per_us = 1.0;
            cfg.ask_depletion_per_us = 1.0;
            cfg.seed = 7;
            cfg.hawkes_kappa = kappa;
            cfg.momentum_gain = gain;
            return ExecutionSimulator(cfg);
        };
        // Deep two-sided book: 400 units per side across many levels.
        auto seed_book = [](ExecutionSimulator& sim) {
            for (std::uint64_t i = 0; i < 8; ++i) {
                CHECK(ok(sim.seed_external(OrderId{500 + i}, Side::buy, Qty{50},
                                           Price{990 - static_cast<std::int64_t>(i)})));
                CHECK(ok(sim.seed_external(OrderId{600 + i}, Side::sell, Qty{50},
                                           Price{1010 + static_cast<std::int64_t>(i)})));
            }
        };
        // Baseline vs excited: same seed, same draw stream, different lambda.
        auto consume_total = [&](ExecutionSimulator& sim) {
            seed_book(sim);
            std::uint64_t before = sim.book().live_orders();
            for (int i = 0; i < 10; ++i)
                sim.advance_time(10000);
            // Count consumed units by level totals.
            std::int64_t left = 0;
            const auto bb = sim.book().best_bid();
            const auto ba = sim.book().best_ask();
            for (std::int64_t p = 983; p <= 997; ++p)
                if (bb != mog::kNoTick && p <= bb)
                    left += sim.book().qty_at(Side::buy, Price{p});
            for (std::int64_t p = 1003; p <= 1017; ++p)
                if (ba != mog::kNoTick && p >= ba)
                    left += sim.book().qty_at(Side::sell, Price{p});
            static_cast<void>(before);
            return left;
        };
        auto base_sim = mk(0.0, 0.0);
        auto excite_sim = mk(2.0, 0.0);
        const auto base_left = consume_total(base_sim);
        const auto excite_left = consume_total(excite_sim);
        CHECK(excite_left < base_left); // excitement consumes more overall
        CHECK(mk(0.0, 0.0).flow_excitement() == 0.0);

        // Momentum: thin the ask until touch moves up, then check signal.
        {
            auto sim = mk(0.0, 5.0);
            seed_book(sim);
            sim.apply_external(Side::sell, Price{1010}, 50); // clear best ask
            CHECK(sim.flow_momentum() == 1.0);               // mid moved up
            sim.apply_external(Side::buy, Price{990}, 50);
            CHECK(sim.flow_momentum() == -1.0);
        }
    }

    // S10: iceberg replenishment joins the tail; hidden reserves exhaust.
    {
        ExecutionSimulator sim(base_config());
        IcebergSpec spec{};
        spec.ref = OrderId{21};
        spec.side = Side::sell;
        spec.price = Price{1000};
        spec.display = 20;
        spec.total = 70; // 50 hidden
        CHECK(ok(sim.seed_iceberg(spec)));
        CHECK(sim.book().qty_at(Side::sell, Price{1000}) == 20);
        CHECK(sim.iceberg_hidden(OrderId{21}) == 50);

        // IOC 55 sweeps three slices' worth of demand against one display.
        sim.submit(inbound(kExt + 70, Side::buy, 55, 1000, SimOrderType::ioc), 0);
        sim.drain();
        // Slice1 (20) emptied -> replenish 20 from hidden(50->30).
        // Slice2 (20) emptied -> replenish 20 from hidden(30->10).
        // Slice3: only 15 of the displayed 20 consumed -> no replenish.
        CHECK(sim.book().qty_at(Side::sell, Price{1000}) == 5);
        CHECK(sim.book().remaining_of(OrderId{21}) == 5);
        CHECK(sim.iceberg_hidden(OrderId{21}) == 10);
        const auto& ds = sim.decisions();
        CHECK(ds.size() == 1 && ds[0].filled_qty == 55);

        // Flow consumes the last 5 displayed -> replenish final 10.
        sim.apply_external(Side::sell, Price{1000}, 5);
        CHECK(sim.book().qty_at(Side::sell, Price{1000}) == 10);
        CHECK(sim.iceberg_hidden(OrderId{21}) == -1); // registry entry dropped
        // Flow drains everything; hidden exhausted so nothing re-enters.
        sim.apply_external(Side::sell, Price{1000}, 12);
        CHECK(sim.book().qty_at(Side::sell, Price{1000}) == 0);
        CHECK(sim.book().front_ref(Side::sell, Price{1000}) == 0);
    }

    // S8: determinism - identical scripts produce identical trace digests.
    {
        auto run_once = [](ExecutionSimulator&& sim) {
            CHECK(ok(sim.seed_external(OrderId{9}, Side::sell, Qty{70}, Price{1000})));
            sim.submit(inbound(kExt + 50, Side::buy, 25, 1001, SimOrderType::day_limit), 5);
            sim.advance_time(500);
            sim.submit(inbound(kExt + 51, Side::buy, 45, 1002, SimOrderType::ioc), 600);
            sim.advance_time(700);
            sim.drain();
            sim.apply_external(Side::sell, Price{1000}, 30);
            return sim.trace_digest();
        };
        auto a = run_once(ExecutionSimulator(base_config()));
        auto b = run_once(ExecutionSimulator(base_config()));
        (void)a;
        (void)b;
        char hex_a[65], hex_b[65];
        const auto va = mog::Sha256::hex(a, hex_a);
        const auto vb = mog::Sha256::hex(b, hex_b);
        CHECK(va == vb);
        std::printf("sim trace: %.*s\n", static_cast<int>(va.size()), va.data());
    }

    // S11: Midpoint pegged order matches with superior price priority over displayed touch.
    {
        ExecutionSimulator sim(base_config());
        CHECK(ok(sim.seed_external(OrderId{1}, Side::buy, Qty{50}, Price{990})));
        CHECK(ok(sim.seed_external(OrderId{2}, Side::sell, Qty{50}, Price{1010})));
        SimInbound mid_order{OrderId{kExt + 80},      Side::buy,     Qty{20}, Price{0},
                             SimOrderType::day_limit, mog::Peg::mid, 0,       true};
        sim.submit(mid_order, 0);
        sim.drain();
        CHECK(sim.hidden_qty_at(Side::buy, Price{1000}) == 20);

        // Aggressive sell crosses: must fill against midpoint peg (1000) before touch (990).
        SimInbound sell_ioc{OrderId{kExt + 81}, Side::sell, Qty{15}, Price{990}, SimOrderType::ioc};
        sim.submit(sell_ioc, 10);
        sim.drain();
        CHECK(sim.hidden_qty_at(Side::buy, Price{1000}) == 5);
        CHECK(sim.book().qty_at(Side::buy, Price{990}) == 50); // displayed touch untouched
        CHECK(sim.reports().size() >= 1);
        if (!sim.reports().empty()) {
            CHECK(sim.reports().back().price_ticks == 1000);
            CHECK(sim.reports().back().qty == 15);
        }
    }

    // S12: replace_strategy on non-displayed / pegged orders.
    {
        ExecutionSimulator sim(base_config());
        SimInbound nd_order{OrderId{kExt + 90},      Side::buy,      Qty{30}, Price{995},
                            SimOrderType::day_limit, mog::Peg::none, 0,       true};
        sim.submit(nd_order, 0);
        sim.drain();
        CHECK(sim.hidden_qty_at(Side::buy, Price{995}) == 30);
        const auto rep_tick =
            sim.replace_strategy(OrderId{kExt + 90}, OrderId{kExt + 91}, Qty{25}, Price{996});
        CHECK(ok(rep_tick));
        CHECK(sim.hidden_qty_at(Side::buy, Price{995}) == 0);
        CHECK(sim.hidden_qty_at(Side::buy, Price{996}) == 25);
    }

    // S13: InplaceVector non-trivial type lifecycle.
    {
        mog::inplace_vector<std::string, 4> v;
        v.emplace_back("alpha");
        v.emplace_back("beta");
        CHECK(v.size() == 2);
        CHECK(v[0] == "alpha" && v[1] == "beta");
        v.pop_back();
        CHECK(v.size() == 1);
        v.clear();
        CHECK(v.empty());
    }

    if (failures == 0)
        std::puts("sim scenarios OK");
    return failures == 0 ? 0 : 1;
}
