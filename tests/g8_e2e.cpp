// G8 NASDAQ-native order types: odd-lot execute-not-display, midpoint
// pegging, bid/ask pegs with offsets - every expectation hand-derived.

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

mog::SimConfig cfg() {
    mog::SimConfig c;
    c.book = {1u << 12, {90000, 110000, 64}};
    return c;
}

mog::simrun::Summary run(const std::string& script) {
    auto r = mog::simrun::run(script, cfg());
    CHECK(r.has_value());
    return r.has_value() ? *r : mog::simrun::Summary{};
}

} // namespace

int main() {
    // 1. Odd lot: "nd" rests hidden - invisible to displayed aggregates,
    // executes at its exact price only after visible quantity drains.
    {
        const std::string s = "kind,ts_ns,side,price_ticks,qty,ref,flags\n"
                              "ext_add,100,B,100000,500,1\n"         // visible bid 500
                              "strat_limit,110,B,100000,300,10,nd\n" // hidden bid 300
                              "trade,120,S,100000,600,2\n"; // eats 500 visible + 100 hidden
        const auto r = run(s);
        CHECK(r.fills == 1);
        if (r.fills == 1) {
            CHECK(r.events[0].type == 'F');
            CHECK(r.events[0].qty == 100);
            CHECK(r.volume_ticks == 100 * 100000);
        }
    }

    // 2. Midpoint peg with automatic repegging: rests at the initial mid,
    // migrates when the reference moves (a tighter bid arrives), and a
    // print at the NEW mid consumes it there - time priority preserved.
    {
        const std::string s = "kind,ts_ns,side,price_ticks,qty,ref,flags\n"
                              "ext_add,100,B,99000,1000,1\n"     // bid 99000
                              "ext_add,110,S,101000,1000,2\n"    // ask 101000 -> mid 100000
                              "strat_limit,120,B,0,400,10,mid\n" // pegged buy @ 100000
                              "ext_add,130,B,99500,100,5\n"      // better bid -> mid 100250
                              "trade,140,S,100250,50,6\n";       // print at the new mid
        const auto r = run(s);
        CHECK(r.maker_fills == 1);
        if (r.maker_fills == 1) {
            bool found = false;
            for (const auto& e : r.events)
                if (e.type == 'F' && e.ref == (1ull << 62) + 10) {
                    found = true;
                    CHECK(e.price_ticks == 100250); // repegged, not 100000
                    CHECK(e.qty == 50);
                }
            CHECK(found);
        }
    }

    // 3. Peg above the far touch: "ask:+10" resolves outside the spread,
    // rests without crossing, and stays unfilled while untouched.
    {
        const std::string s = "kind,ts_ns,side,price_ticks,qty,ref,flags\n"
                              "ext_add,100,B,100000,500,1\n"
                              "ext_add,110,S,100200,500,2\n"
                              "strat_limit,120,S,0,200,10,ask:+10\n"; // sell pegged at 100210
        const auto r = run(s);
        CHECK(r.fills == 0);
    }

    // 4. Cancelling a hidden order works through the same strategy ref.
    {
        const std::string s = "kind,ts_ns,side,price_ticks,qty,ref,flags\n"
                              "ext_add,100,B,99000,1000,1\n"
                              "ext_add,110,S,101000,1000,2\n"
                              "strat_limit,120,B,0,400,10,mid\n"
                              "strat_cancel,130,S,0,1,10\n"
                              "trade,140,S,100250,50,4\n";
        const auto r = run(s);
        CHECK(r.fills == 0); // cancelled before the cross could reach it
    }

    // 5. Determinism: identical scripts produce identical digests.
    {
        const std::string s = "kind,ts_ns,side,price_ticks,qty,ref,flags\n"
                              "ext_add,100,B,99000,1000,1\n"
                              "ext_add,110,S,101000,1000,2\n"
                              "strat_limit,120,B,0,400,10,mid:+5\n";
        const auto a = mog::simrun::run(s, cfg());
        const auto b = mog::simrun::run(s, cfg());
        CHECK(a.has_value() && b.has_value());
        CHECK(a->digest_high == b->digest_high);
    }

    // 6. Unknown flag tokens are script errors.
    {
        const std::string bad = "kind,ts_ns,side,price_ticks,qty,ref,flags\n"
                                "strat_limit,100,B,100000,100,10,wat\n";
        CHECK(!mog::simrun::run(bad, cfg()).has_value());
    }

    if (failures == 0)
        std::printf("g8_e2e: ok\n");
    else
        std::printf("g8_e2e: %d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
