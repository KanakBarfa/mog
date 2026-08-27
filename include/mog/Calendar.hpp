// NASDAQ trading calendar: session bounds, early closes, timestamp sanity.
//
// Multi-day corpora silently produce wrong session lengths when half-days or
// boundaries go unnoticed; a replay that reports which session its timestamps
// fall into (and whether they leak outside one) makes that visible. Default
// sessions are 09:30-16:00 ET on weekdays; a dated early-close set covers
// half-days. Holidays are intentionally NOT modeled - a capture on a holiday
// is data worth looking at twice, not silently accepted.
#pragma once

#include <mog/Contracts.hpp>

#include <cstdint>
#include <string_view>
#include <vector>

namespace mog::calendar {

// Session bounds in nanoseconds from midnight ET.
inline constexpr std::int64_t kOpenNs = 9 * 3'600'000'000'000LL + 30 * 60'000'000'000LL;
inline constexpr std::int64_t kCloseNs = 16 * 3'600'000'000'000LL;
inline constexpr std::int64_t kEarlyCloseNs = 13 * 3'600'000'000'000LL;

// Civil-date days since 1970-01-01 (UTC-independent day count).
[[nodiscard]] constexpr std::int64_t epoch_day(std::int64_t y, std::int64_t m,
                                               std::int64_t d) noexcept {
    // Howard Hinnant's days_from_civil algorithm.
    const std::int64_t yy = m <= 2 ? y - 1 : y;
    const std::int64_t era = (yy >= 0 ? yy : yy - 399) / 400;
    const std::uint64_t yoe = static_cast<std::uint64_t>(yy - era * 400);
    const std::uint64_t doy = (153u * static_cast<std::uint64_t>(m + (m > 2 ? -3 : 9)) + 2u) / 5u +
                              static_cast<std::uint64_t>(d) - 1u;
    const std::uint64_t doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
    return era * 146'097 + static_cast<std::int64_t>(doe) - 719'468;
}

// Weekday of an epoch day: 0=Sunday .. 6=Saturday.
[[nodiscard]] constexpr int weekday(std::int64_t day) noexcept {
    // 1970-01-01 was a Thursday (=4).
    return static_cast<int>(((day % 7) + 7 + 4) % 7);
}

struct Calendar {
    std::int64_t first_day = 0; // inclusive epoch-day of coverage
    std::int64_t last_day = 0;  // inclusive
    std::vector<bool> early_closes{};

    [[nodiscard]] static Calendar make(std::int64_t y0, std::int64_t m0, std::int64_t d0,
                                       std::int64_t y1, std::int64_t m1, std::int64_t d1) {
        const std::int64_t f = epoch_day(y0, m0, d0);
        const std::int64_t l = epoch_day(y1, m1, d1);
        const std::size_t span = l >= f ? static_cast<std::size_t>(l - f + 1) : 0;
        Calendar c{f, l, std::vector<bool>(span, false)};
        return c;
    }

    // Marks a dated early close (e.g. 2026-07-03) inside the covered range.
    void add_early_close(std::int64_t y, std::int64_t m, std::int64_t d) noexcept {
        const std::int64_t day = epoch_day(y, m, d);
        if (day >= first_day && day <= last_day) {
            const std::size_t idx = static_cast<std::size_t>(day - first_day);
            if (idx < early_closes.size())
                early_closes[idx] = true;
        }
    }

    [[nodiscard]] constexpr bool trading_day(std::int64_t day) const noexcept {
        return weekday(day) != 0 && weekday(day) != 6;
    }

    // Session close for a given trading day; kCloseNs or kEarlyCloseNs.
    [[nodiscard]] std::int64_t close_ns(std::int64_t day) const noexcept {
        if (day < first_day || day > last_day)
            return kCloseNs;
        const std::size_t idx = static_cast<std::size_t>(day - first_day);
        if (idx >= early_closes.size())
            return kCloseNs;
        return early_closes[idx] ? kEarlyCloseNs : kCloseNs;
    }
};

// Where a nanosecond timestamp falls relative to its day's session.
enum class SessionState : std::uint8_t {
    before_open = 0,
    in_session = 1,
    after_close = 2,
    non_trading_day = 3,
};

[[nodiscard]] inline SessionState classify(const Calendar& cal, std::int64_t day,
                                           std::int64_t ns_of_day_et) noexcept {
    if (!cal.trading_day(day))
        return SessionState::non_trading_day;
    if (ns_of_day_et < kOpenNs)
        return SessionState::before_open;
    if (ns_of_day_et > cal.close_ns(day))
        return SessionState::after_close;
    return SessionState::in_session;
}

} // namespace mog::calendar
