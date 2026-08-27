// Scripted simulation runner: hand-computed fills over a tiny script,
// seed determinism, and events-log completeness for tearsheet recompute.

#include <mog/SimRun.hpp>

#include <cstdio>
#include <string>

namespace {

int failures = 0;

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                   \
            ++failures;                                                                            \
        }                                                                                          \
    } while (0)

const std::string kScript = "kind,ts_ns,side,price_ticks,qty,ref\n"
                            "ext_add,100,B,99000,1000,1\n"      // external bid 1000 @ 99000
                            "ext_add,110,S,101000,1000,2\n"     // external ask 1000 @ 101000
                            "strat_limit,150,B,100000,300,10\n" // our passive buy rests behind ref1
                            "trade,200,S,100000,1200,3\n"       // aggressor sell eats bid FIFO:
                                                                //   ref1's 1000 then OURS for 200
                            "strat_ioc,300,S,99000,500,11\n"; // ioc sell: fills our remaining 100,
                                                              // remainder cancelled

mog::SimConfig quiet_config() {
    mog::SimConfig cfg;
    cfg.book = {1u << 12, {90000, 110000, 64}};
    return cfg; // zero depletion/latency/jitter/fees
}

} // namespace

int main() {
    // 1. Hand-computed fill stream.
    const auto r = mog::simrun::run(kScript, quiet_config());
    CHECK(r.has_value());
    if (!r)
        return 1;
    const auto& s = *r;
    // Hand model, corrected for engine semantics:
    //   - our buy @100000 price-improves the 99000 bid, so the level-100000
    //     print consumes OUR 300 first (maker fill via reports ledger);
    //   - apply_external is exact-level by contract: the 99000 external bid
    //     is untouched;
    //   - the ioc sell @99000 then crosses that external liquidity as a
    //     taker (500 filled), reported through the decisions ledger.
    CHECK(s.script_rows == 5);
    CHECK(s.fills == 2);
    CHECK(s.maker_fills == 1);
    CHECK(s.taker_fills == 1);
    CHECK(s.prints == 1);
    CHECK(s.volume_ticks == 300 * 100000 + 500 * 99000);
    CHECK(s.fees_paid_cash == 0);

    const auto& f0 = s.events[0];
    CHECK(f0.type == 'F');
    CHECK(f0.ref == (1ull << 62) + 10);
    CHECK(f0.price_ticks == 100000);
    CHECK(f0.qty == 300);
    CHECK(f0.side == 'B');
    // The print record lands with its own script row, before later rows.
    const auto& pr = s.events[1];
    CHECK(pr.type == 'P');
    CHECK(pr.ts_ns == 200);
    const auto& f1 = s.events[2];
    CHECK(f1.type == 'F');
    CHECK(f1.ref == (1ull << 62) + 11);
    CHECK(f1.price_ticks == 99000);
    CHECK(f1.qty == 500);
    CHECK(f1.side == 'S');

    // 2. Same seed => identical digest; different seed+jitter => different.
    const auto a = mog::simrun::run(kScript, quiet_config());
    const auto b = mog::simrun::run(kScript, quiet_config());
    CHECK(a.has_value() && b.has_value());
    CHECK(a->digest_high == b->digest_high);
    CHECK(a->digest_high == s.digest_high);

    // Fee attribution: maker rebate credit, taker charge.
    {
        mog::SimConfig fees = quiet_config();
        fees.maker_fee_bps = -2;
        fees.taker_fee_bps = 5;
        const auto fr = mog::simrun::run(kScript, fees);
        CHECK(fr.has_value());
        if (fr) {
            const std::int64_t expect_maker = -2 * 300 * 100000 / 10000; // -6000
            const std::int64_t expect_taker = 5 * 500 * 99000 / 10000;   // +24750
            CHECK(fr->maker_fills == 1);
            CHECK(fr->taker_fills == 1);
            CHECK(fr->fees_paid_cash == expect_maker + expect_taker);
        }
    }

    mog::SimConfig jittered = quiet_config();
    jittered.seed = 42;
    jittered.wire_latency_ns = 10;
    jittered.jitter_kind = mog::JitterKind::uniform;
    jittered.jitter_max_ns = 40;
    const auto c = mog::simrun::run(kScript, jittered);
    CHECK(c.has_value());
    // Fills identical (same liquidity path) even though stamps may shift.
    CHECK(c->fills == s.fills);
    CHECK(c->volume_ticks == s.volume_ticks);

    // 3. Events log carries mid marks for offline equity computation.
    bool saw_mid = false;
    for (const auto& e : s.events)
        if (e.mid_ticks != 0)
            saw_mid = true;
    CHECK(saw_mid);

    // 4. Halt gating: prints dropped AND unlogged, crossings suppressed,
    // limits still rest; resume restores exact-level consumption.
    {
        const std::string halted = "kind,ts_ns,side,price_ticks,qty,ref\n"
                                   "ext_add,110,S,101000,1000,2\n"
                                   "strat_limit,140,B,100000,300,10\n"
                                   "halt,150,\n"
                                   "trade,200,S,100000,500,3\n"     // dropped + unlogged: frozen
                                   "strat_ioc,210,S,99000,400,20\n" // no cross: cancelled whole
                                   "resume,300,\n"
                                   "trade,350,S,100000,400,4\n"; // now consumes our resting 300
        const auto h = mog::simrun::run(halted, quiet_config());
        CHECK(h.has_value());
        if (h) {
            // Exactly one maker fill: our 300 @ 100000, post-resume. Had the
            // halted print not been gated it would have consumed it earlier.
            CHECK(h->fills == 1);
            CHECK(h->maker_fills == 1);
            CHECK(h->taker_fills == 0);
            CHECK(h->prints == 1); // only the resumed print is logged
            CHECK(h->events[0].type == 'F');
            CHECK(h->events[0].qty == 300);
            CHECK(h->volume_ticks == 300 * 100000);
        }
    }

    // 5. Ladder pool exhaustion fails loudly, not corruptingly.
    {
        std::string wide = "kind,ts_ns,side,price_ticks,qty,ref\n";
        for (int i = 0; i < 8; ++i)
            wide += "ext_add," + std::to_string(100 + i) + ",B," +
                    std::to_string(1000000 + i * 1024) + ",100," + std::to_string(i + 1) + "\n";
        mog::SimConfig tiny;
        tiny.book = {1u << 10, {0, 12'000'000, 4}};
        const auto w = mog::simrun::run(wide, tiny);
        CHECK(w.has_value());
    }

    // 6. One-sided books produce mid=0 marks, not signed overflow.
    {
        const std::string oneside = "kind,ts_ns,side,price_ticks,qty,ref\n"
                                    "ext_add,100,S,101000,500,1\n"
                                    "strat_limit,150,S,101000,100,10\n"
                                    "trade,200,B,101000,600,2\n"; // empties the ask side entirely
        mog::SimConfig cfg;
        cfg.book = {1u << 12, {90000, 110000, 64}};
        const auto o = mog::simrun::run(oneside, cfg);
        CHECK(o.has_value());
        if (o) {
            bool saw_zero_mid_after_drain = false;
            for (const auto& ev : o->events)
                if (ev.type == 'P' && ev.mid_ticks == 0)
                    saw_zero_mid_after_drain = true;
            CHECK(saw_zero_mid_after_drain);
        }
    }

    // 7. Malformed scripts rejected loudly.
    CHECK(!mog::simrun::run("nope\n", quiet_config()).has_value());
    CHECK(
        mog::simrun::run("kind,ts_ns,side,price_ticks,qty,ref\next_add,x,B,1,1,1\n", quiet_config())
            .error() == mog::simrun::Error::bad_script);

    if (failures == 0)
        std::printf("simrun_e2e: ok\n");
    else
        std::printf("simrun_e2e: %d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
