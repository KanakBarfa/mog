// M8 orchestration-layer e2e: multi-instrument fan-out with deterministic
// broadcast ordering, per-instrument isolation, independent ref spaces,
// and replay-identical portfolio digests.
#include <mog/Orchestrate.hpp>

#include <cstdint>
#include <cstdio>
#include <string>

namespace {

using mog::ExecutionSimulator;
using mog::ok;
using mog::Orchestrator;
using mog::OrderId;
using mog::Price;
using mog::Qty;
using mog::Side;
using mog::SimConfig;
using mog::SimInbound;
using mog::StpMode;

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

// One instrument's script, parameterized by its own refs so the same ref
// values may legally repeat across instruments (no shared id space).
void drive(Orchestrator& o, std::size_t idx) {
    const OrderId ext{idx * 100 + 1};          // below external_ref_limit
    const OrderId strat{kExt + idx * 100 + 2}; // strategy range
    static_cast<void>(o.seed_external(idx, ext, Side::sell, Qty{50}, Price{1000}));
    static_cast<void>(o.submit(
        idx, SimInbound{strat, Side::buy, Qty{30}, Price{999}, mog::SimOrderType::day_limit}, 10));
    o.drain(idx);
    o.advance_time(idx, 500);
    // External flow lifts the offer: ask level 1000 drops 50 -> 30 units.
    o.apply_external(idx, Side::sell, Price{1000}, 20);
}

} // namespace

int main() {
    // --- replay determinism at portfolio scale ---------------------------------
    {
        Orchestrator a, b;
        const std::size_t ia = a.add_instrument(base_config(), "ES");
        const std::size_t ib = b.add_instrument(base_config(), "ES");
        CHECK(ia == ib); // routing indices are allocation-order stable
        drive(a, ia);
        drive(b, ib);
        CHECK(a.global_digest() == b.global_digest());
        CHECK(a.audit());

        // Adding an instrument changes the fold even if untouched - the
        // portfolio identity covers every member.
        static_cast<void>(b.add_instrument(base_config(), "NQ"));
        CHECK(a.global_digest() != b.global_digest());
    }

    // --- per-instrument isolation ------------------------------------------------
    {
        // Ops routed to instrument 0 must leave instrument 1 bit-identical
        // to a solo simulator fed only instrument 1's script.
        Orchestrator o;
        static_cast<void>(o.add_instrument(base_config(), "A"));
        const std::size_t i1 = o.add_instrument(base_config(), "B");
        drive(o, 0);
        drive(o, i1);

        ExecutionSimulator solo(base_config());
        static_cast<void>(
            solo.seed_external(OrderId{i1 * 100 + 1}, Side::sell, Qty{50}, Price{1000}));
        static_cast<void>(solo.submit(SimInbound{OrderId{kExt + i1 * 100 + 2}, Side::buy, Qty{30},
                                                 Price{999}, mog::SimOrderType::day_limit},
                                      10));
        solo.drain();
        solo.advance_time(500);
        solo.apply_external(Side::sell, Price{1000}, 20);
        CHECK(o.sim(i1).trace_digest() == solo.trace_digest());
        CHECK(o.sim(i1).book().best_bid() == 999);
        CHECK(o.sim(i1).book().best_ask() == 1000);
        CHECK(o.sim(i1).book().qty_at(Side::sell, Price{1000}) == 30);
    }

    // --- independent ref spaces -----------------------------------------------------
    {
        Orchestrator o;
        const std::size_t i0 = o.add_instrument(base_config(), "A");
        const std::size_t i1 = o.add_instrument(base_config(), "B");
        const OrderId same{kExt + 7};
        static_cast<void>(o.submit(
            i0, SimInbound{same, Side::buy, Qty{10}, Price{1000}, mog::SimOrderType::day_limit},
            1));
        static_cast<void>(o.submit(
            i1, SimInbound{same, Side::buy, Qty{11}, Price{1001}, mog::SimOrderType::day_limit},
            1));
        o.drain_all();
        CHECK(o.sim(i0).book().remaining_of(same) == 10);
        CHECK(o.sim(i1).book().remaining_of(same) == 11);
        CHECK(o.name(i0) == "A");
        CHECK(o.audit());
    }

    // --- broadcast ordering is the determinism contract ------------------------------
    {
        Orchestrator a, b;
        for (int k = 0; k < 3; ++k) {
            static_cast<void>(a.add_instrument(base_config()));
            static_cast<void>(b.add_instrument(base_config()));
        }
        for (std::size_t i = 0; i < 3; ++i) {
            drive(a, i);
            drive(b, i);
        }
        // Interleaved broadcasts must replay identically.
        a.drain_all();
        a.advance_all(250);
        a.drain_all();
        b.drain_all();
        b.advance_all(250);
        b.drain_all();
        CHECK(a.global_digest() == b.global_digest());

        // advance_all(dt) equals per-instrument advance_time(dt) in index order.
        Orchestrator c;
        for (int k = 0; k < 3; ++k)
            static_cast<void>(c.add_instrument(base_config()));
        for (std::size_t i = 0; i < 3; ++i)
            drive(c, i);
        for (std::size_t i = 0; i < 3; ++i)
            c.advance_time(i, 250);
        for (std::size_t i = 0; i < 3; ++i) {
            c.drain(i);
            a.drain(i);
        }
        CHECK(c.global_digest() == a.global_digest());
    }

    if (failures == 0)
        std::printf("orchestrate_e2e: all checks passed\n");
    else
        std::printf("orchestrate_e2e: %d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
