// Tearsheet (G7): every metric below is hand-derived from the events log
// text alone - the recompute-or-it-isn't-true contract, tested literally.

#include <mog/Tearsheet.hpp>

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
} // namespace

int main() {
    // Scenario: buy 100 @100000 (maker, rebate -40), sell 100 @100100
    // (taker, fee +30). Mids: 100050 after first fill, 100100 after second.
    // Prints at 101'000'001 and 101'100'000 drive markouts.
    const std::string log =
        "type,ts_ns,ref,side,price_ticks,qty,fee_cash,mid_ticks,ledger,submit_ns\n"
        "F,1000,7,B,100000,100,-40,100050,M\n"
        "P,2000,0,S,100060,50,0,100060,M\n"
        "F,3000,8,S,100100,100,30,100100,T\n"
        "P,101001000,0,S,100120,10,0,100120,M\n"; // +1ms after fill@3000? no:
                                                  // 3000+1e6=1003000; this is
                                                  // way later. Only print@2000
                                                  // is within reach of nothing.

    mog::tearsheet::Report rep;
    CHECK(mog::tearsheet::compute_from_csv(log, rep));
    CHECK(rep.fills == 2);
    CHECK(rep.prints == 2);
    CHECK(rep.volume_ticks == 100 * 100000 + 100 * 100100);
    CHECK(rep.fees_cash == -40 + 30);
    CHECK(rep.fees_maker_cash == -40);
    CHECK(rep.fees_taker_cash == 30);

    // Cash: buy pays 100*100000 = 10,000,000; sell receives 100*100100 =
    // 10,010,000 -> cash = +10,000. Inventory flat. Equity = cash.
    CHECK(rep.final_inventory == 0);
    CHECK(rep.cash_pnl_ticks == 10000);
    CHECK(rep.equity_pnl_ticks == 10000);
    CHECK(rep.final_mid_ticks == 100120); // last observed mark is the trailing print

    // Hit rate: buy had mid(100050) > px(100000) -> hit; sell had mid(100100)
    // == px -> not measurable; so 1 of 1 measurable.
    CHECK(rep.hit_rate == 1.0);

    // Markouts: fill@ts=1000 (+1ms window ends 1001000): next print ts=2000?
    // No - 2000 < 1001000, not in window; next is 101001000 -> measured.
    // Fill@3000 (+1ms -> 1003000): next print 101001000 -> measured.
    CHECK(rep.markouts.size() == 3);
    if (rep.markouts.size() == 3) {
        const auto& m1 = rep.markouts[0];
        CHECK(m1.horizon_ns == 1000000);
        CHECK(m1.measured == 2);
        // Buy@100000 vs later print 100120: +120/100000*1e4 = +12 bps.
        // Sell@100100 vs same print 100120: -(20)/100100*1e4 ~ -2.0 bps.
        const double expect_mean = (12.0 + (-20.0 / 100100.0 * 10000.0)) / 2.0;
        double diff = m1.mean_bps - expect_mean;
        if (diff < 0)
            diff = -diff;
        CHECK(diff < 0.01);
    }

    // Malformed logs are rejected.
    mog::tearsheet::Report junk;
    CHECK(!mog::tearsheet::compute_from_csv("garbage\n", junk));

    if (failures == 0)
        std::printf("tearsheet_e2e: ok\n");
    else
        std::printf("tearsheet_e2e: %d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
