// Trading calendar (G11): civil-date math, weekend rules, early closes,
// session classification - all against externally known calendar facts.

#include <mog/Calendar.hpp>

#include <cstdio>

namespace {

int failures = 0;

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                   \
            ++failures;                                                                            \
        }                                                                                          \
    } while (0)

using namespace mog::calendar;

} // namespace

int main() {
    // Civil-date anchors checked against known values.
    CHECK(epoch_day(1970, 1, 1) == 0);
    CHECK(epoch_day(2000, 3, 1) == 11017); // Hinnant's documented example
    CHECK(epoch_day(2026, 8, 25) == epoch_day(1970, 1, 1) + 20690);
    CHECK(weekday(0) == 4); // 1970-01-01 was a Thursday

    // 2026-08-25 is a Tuesday; the Saturday of that week is not a trading day.
    const Calendar week = Calendar::make(2026, 8, 24, 2026, 8, 30);
    CHECK(weekday(epoch_day(2026, 8, 25)) == 2);
    CHECK(week.trading_day(epoch_day(2026, 8, 25)));
    CHECK(!week.trading_day(epoch_day(2026, 8, 29))); // Saturday
    CHECK(!week.trading_day(epoch_day(2026, 8, 30))); // Sunday
    for (std::int64_t d = epoch_day(2026, 8, 24); d <= epoch_day(2026, 8, 28); ++d)
        CHECK(week.trading_day(d));

    // Early close: July 3, 2026 (Friday before Independence Day observed).
    Calendar half = Calendar::make(2026, 7, 1, 2026, 7, 10);
    half.add_early_close(2026, 7, 3);
    CHECK(half.close_ns(epoch_day(2026, 7, 2)) == kCloseNs);
    CHECK(half.close_ns(epoch_day(2026, 7, 3)) == kEarlyCloseNs);
    CHECK(half.close_ns(epoch_day(2026, 7, 6)) == kCloseNs);

    // Classification across one full session.
    const Calendar day = Calendar::make(2026, 8, 25, 2026, 8, 25);
    const std::int64_t tue = epoch_day(2026, 8, 25);
    CHECK(classify(day, tue, kOpenNs - 1) == SessionState::before_open);
    CHECK(classify(day, tue, kOpenNs) == SessionState::in_session);
    CHECK(classify(day, tue, kOpenNs + 1) == SessionState::in_session);
    CHECK(classify(day, tue, kCloseNs) == SessionState::in_session);
    CHECK(classify(day, tue, kCloseNs + 1) == SessionState::after_close);
    // An early close flips afternoon timestamps to after_close on that day.
    half.add_early_close(2026, 7, 3);
    CHECK(classify(half, epoch_day(2026, 7, 3), kEarlyCloseNs + 1) == SessionState::after_close);
    CHECK(classify(half, epoch_day(2026, 7, 4), kOpenNs + 1) ==
          SessionState::non_trading_day); // Saturday

    // Multi-year coverage span (>64 days) regression test.
    Calendar multi_year = Calendar::make(2025, 1, 1, 2027, 12, 31);
    multi_year.add_early_close(2026, 11, 27); // Black Friday (day index > 650)
    CHECK(multi_year.close_ns(epoch_day(2026, 11, 26)) == kCloseNs);
    CHECK(multi_year.close_ns(epoch_day(2026, 11, 27)) == kEarlyCloseNs);
    CHECK(multi_year.close_ns(epoch_day(2026, 11, 30)) == kCloseNs);

    // Out-of-range days fall back to full-session close (visible default).
    CHECK(day.close_ns(epoch_day(2027, 1, 1)) == kCloseNs);

    if (failures == 0)
        std::printf("calendar_e2e: ok\n");
    else
        std::printf("calendar_e2e: %d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
