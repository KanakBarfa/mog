// Tearsheet (G7): performance summary recomputed from the simrun events log
// alone. Nothing here may consult engine state - if a number cannot be
// derived from the log file, the log is wrong, not the tearsheet.
//
// Conventions: side 'B' rows acquire inventory, 'S' rows dispose of it; cash
// flow is -px*qty for 'B', +px*qty for 'S'; fees are signed cash impacts.
// Equity at each event = cash + inventory * mid. Hit rate is the fraction of
// fills with positive instantaneous edge against the post-fill mid ('B':
// mid > px, 'S': mid < px). Markouts compare each fill price to the next
// print at or after fill_ts + horizon, signed by direction.
#pragma once

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace mog::tearsheet {

struct Markout {
    std::int64_t horizon_ns = 0;
    std::size_t measured = 0;
    double mean_bps = 0.0;
};

struct Report {
    std::size_t fills = 0;
    std::size_t prints = 0;
    std::size_t buy_qty = 0;
    std::size_t sell_qty = 0;
    std::int64_t volume_ticks = 0;
    std::int64_t fees_cash = 0;
    std::int64_t fees_maker_cash = 0;
    std::int64_t fees_taker_cash = 0;
    std::int64_t final_inventory = 0;
    std::int64_t final_mid_ticks = 0;
    std::int64_t cash_pnl_ticks = 0;     // realized cash from fills only
    std::int64_t equity_pnl_ticks = 0;   // cash + inventory marked at last mid
    std::int64_t max_drawdown_ticks = 0; // absolute peak-to-trough drop in ticks
    double max_drawdown_bps = 0.0;       // on the equity curve vs running peak
    double hit_rate = 0.0;               // see header convention
    std::vector<Markout> markouts;
};

namespace detail {

struct Row {
    char type = 'F';
    std::uint64_t ts_ns = 0;
    char side = 'B';
    std::int64_t price_ticks = 0;
    std::uint32_t qty = 0;
    std::int64_t fee_cash = 0;
    std::int64_t mid_ticks = 0;
    char ledger = 'M';
};

[[nodiscard]] inline bool parse_log(const std::string& text, std::vector<Row>& out) {
    std::size_t pos = 0;
    bool first = true;
    while (pos <= text.size()) {
        const std::size_t eol = text.find('\n', pos);
        const std::size_t len = eol == std::string_view::npos ? text.size() - pos : eol - pos;
        std::string_view line{text.data() + pos, len};
        pos = eol == std::string_view::npos ? text.size() + 1 : eol + 1;
        if (!line.empty() && line.back() == '\r')
            line.remove_suffix(1);
        if (line.empty())
            continue;
        if (first) {
            // Positional fields through `ledger`; trailing columns (e.g.
            // submit_ns for calibration) are tolerated additions.
            constexpr std::string_view kPrefix =
                "type,ts_ns,ref,side,price_ticks,qty,fee_cash,mid_ticks";
            if (line.substr(0, kPrefix.size()) != kPrefix)
                return false;
            first = false;
            continue;
        }
        Row r;
        auto field = [&line](std::size_t& p) -> std::string_view {
            const std::size_t c = line.find(',', p);
            const std::string_view f =
                line.substr(p, c == std::string_view::npos ? line.size() - p : c - p);
            p = c == std::string_view::npos ? line.size() + 1 : c + 1;
            return f;
        };
        auto num = [](std::string_view v) -> std::int64_t {
            if (v.empty())
                return -1;
            const bool neg = v[0] == '-';
            std::string_view digits = neg ? v.substr(1) : v;
            std::int64_t val = 0;
            for (const char ch : digits) {
                if (ch < '0' || ch > '9')
                    return -1;
                val = val * 10 + (ch - '0');
            }
            return neg ? -val : val;
        };
        std::size_t p = 0;
        const auto t = field(p);
        if (t.size() != 1)
            return false;
        r.type = t[0];
        const std::int64_t ts = num(field(p));
        static_cast<void>(field(p)); // ref unused by metrics
        const auto side = field(p);
        r.price_ticks = num(field(p));
        const std::int64_t qty = num(field(p));
        r.fee_cash = num(field(p));
        r.mid_ticks = num(field(p));
        const auto led = field(p);
        if (ts < 0 || r.price_ticks < 0 || qty <= 0 || r.mid_ticks < 0 || side.size() != 1 ||
            led.size() != 1)
            return false;
        r.ts_ns = static_cast<std::uint64_t>(ts);
        r.side = side[0];
        r.qty = static_cast<std::uint32_t>(qty);
        r.ledger = led[0];
        out.push_back(r);
    }
    return !first;
}

} // namespace detail

[[nodiscard]] inline Report compute(const std::vector<detail::Row>& rows) {
    using detail::Row;
    Report rep;
    std::int64_t cash = 0, inv = 0, peak = 0;
    std::size_t hits = 0, edge_measurable = 0;

    for (const Row& r : rows) {
        if (r.type == 'P') {
            ++rep.prints;
            continue;
        }
        if (r.type != 'F')
            continue;
        ++rep.fills;
        const std::int64_t signed_qty =
            r.side == 'B' ? static_cast<std::int64_t>(r.qty) : -static_cast<std::int64_t>(r.qty);
        const std::int64_t flow = -signed_qty * r.price_ticks; // B pays, S receives
        cash += flow;
        inv += signed_qty;
        rep.volume_ticks += r.price_ticks * static_cast<std::int64_t>(r.qty);
        rep.fees_cash += r.fee_cash;
        if (r.ledger == 'M')
            rep.fees_maker_cash += r.fee_cash;
        else if (r.ledger == 'T')
            rep.fees_taker_cash += r.fee_cash;
        rep.buy_qty += r.side == 'B' ? r.qty : 0;
        rep.sell_qty += r.side == 'S' ? r.qty : 0;
        if (r.side == 'B' ? r.mid_ticks > r.price_ticks : r.mid_ticks < r.price_ticks)
            ++hits;
        if (r.mid_ticks > 0 && r.mid_ticks != r.price_ticks)
            ++edge_measurable;
    }

    rep.final_inventory = inv;
    rep.cash_pnl_ticks = cash;
    // Second pass for equity curve drawdown and terminal marking.
    if (!rows.empty()) {
        std::int64_t last_mid = 0;
        for (const Row& r : rows)
            if (r.mid_ticks > 0)
                last_mid = r.mid_ticks;
        rep.final_mid_ticks = last_mid;
        std::int64_t c = 0, i = 0;
        for (const Row& r : rows) {
            if (r.type == 'F') {
                const std::int64_t sq = r.side == 'B' ? static_cast<std::int64_t>(r.qty)
                                                      : -static_cast<std::int64_t>(r.qty);
                c += -sq * r.price_ticks;
                i += sq;
            }
            const std::int64_t mid = r.mid_ticks > 0 ? r.mid_ticks : last_mid;
            const std::int64_t eq = c + i * mid;
            if (eq > peak)
                peak = eq;
            if (peak - eq > rep.max_drawdown_ticks)
                rep.max_drawdown_ticks = peak - eq;
            if (peak > 0) {
                const double dd = static_cast<double>(peak - eq) / static_cast<double>(peak);
                if (dd > rep.max_drawdown_bps)
                    rep.max_drawdown_bps = dd;
            }
        }
        rep.equity_pnl_ticks = c + i * rep.final_mid_ticks;
    }
    rep.max_drawdown_bps *= 10000.0;
    rep.hit_rate = edge_measurable > 0
                       ? static_cast<double>(static_cast<unsigned long long>(hits)) /
                             static_cast<double>(static_cast<unsigned long long>(edge_measurable))
                       : 0.0;

    // Markouts vs the next print at or past each horizon (binary search over sorted prints).
    std::vector<const Row*> prints;
    prints.reserve(rows.size());
    for (const Row& r : rows)
        if (r.type == 'P' && r.price_ticks > 0)
            prints.push_back(&r);

    constexpr std::int64_t kHorizons[] = {1'000'000, 10'000'000, 100'000'000};
    for (const std::int64_t h : kHorizons) {
        Markout m;
        m.horizon_ns = h;
        double acc = 0.0;
        auto it = prints.begin();
        for (const Row& r : rows) {
            if (r.type != 'F' || r.price_ticks <= 0)
                continue;
            const std::uint64_t target = r.ts_ns + static_cast<std::uint64_t>(h);
            it = std::lower_bound(it, prints.end(), target,
                                  [](const Row* p, std::uint64_t t) { return p->ts_ns < t; });
            if (it == prints.end())
                continue;
            const Row* next = *it;
            const double dir = r.side == 'B' ? 1.0 : -1.0;
            acc += dir * static_cast<double>(next->price_ticks - r.price_ticks) /
                   static_cast<double>(r.price_ticks) * 10000.0;
            ++m.measured;
        }
        m.mean_bps = m.measured > 0
                         ? acc / static_cast<double>(static_cast<unsigned long long>(m.measured))
                         : 0.0;
        rep.markouts.push_back(m);
    }
    return rep;
}

[[nodiscard]] inline bool compute_from_csv(const std::string& text, Report& out) {
    std::vector<detail::Row> rows;
    if (!detail::parse_log(text, rows))
        return false;
    out = compute(rows);
    return true;
}

} // namespace mog::tearsheet
