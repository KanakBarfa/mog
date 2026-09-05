// Scripted simulation runner (G6/G7/G10 enabler): drives ExecutionSimulator
// over a bench/race-protocol script CSV with externally configurable
// SimConfig parameters, emitting a machine summary plus an events log that
// the tearsheet can recompute from alone.
#pragma once

#include <mog/Simulate.hpp>

#include <cstdio>
#include <cstring>
#include <expected>
#include <string>
#include <string_view>
#include <vector>

namespace mog::simrun {

struct EventRecord {
    char type = 'F'; // 'F' fill, 'P' external print
    std::uint64_t ts_ns = 0;
    std::uint64_t ref = 0;
    char side = 'B'; // fills: resting side of the filled order
    std::int64_t price_ticks = 0;
    std::uint32_t qty = 0;
    std::int64_t fee_cash = 0;
    std::int64_t mid_ticks = 0;  // book mid after the event, 0 if unknown
    char ledger = 'M';           // 'M' maker settlement, 'T' taker charge
    std::uint64_t submit_ns = 0; // decision submit stamp (latency fitting)
};

struct Summary {
    std::size_t script_rows = 0;
    std::size_t fills = 0; // maker + taker fill events
    std::size_t maker_fills = 0;
    std::size_t taker_fills = 0;
    std::size_t prints = 0;
    std::int64_t volume_ticks = 0;               // sum price*qty over all fills
    std::int64_t fees_paid_cash = 0;             // signed sum (rebates are credits)
    std::uint64_t digest_high = 0;               // first 64 bits of the mode-appropriate digest
    DigestMode digest_mode = DigestMode::golden; // tier that produced digest_high
    std::vector<EventRecord> events;
};

enum class Error : std::uint8_t { bad_script = 1 };

namespace detail {

// Defined below; declared here because parse_script validates flag tokens.
[[nodiscard]] inline bool apply_flag_column(const std::string& flags, SimInbound& in);

struct Row {
    std::string kind;
    std::uint64_t ts = 0;
    char side = 'B';
    std::int64_t price = 0;
    std::uint32_t qty = 0;
    std::uint64_t ref = 0;
    // Optional 7th field: "" | "nd" | "mid" | "mid:+5" | "bid:-10" | "ask:3"
    std::string flags;
};

[[nodiscard]] inline bool parse_script(const std::string& text, std::vector<Row>& out) {
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
            if (line != "kind,ts_ns,side,price_ticks,qty,ref" &&
                line != "kind,ts_ns,side,price_ticks,qty,ref,flags")
                return false;
            first = false;
            continue;
        }
        Row r;
        auto next_field = [&line](std::size_t& p) -> std::string_view {
            if (p > line.size())
                return {};
            const std::size_t c = line.find(',', p);
            const std::string_view f =
                line.substr(p, c == std::string_view::npos ? line.size() - p : c - p);
            p = c == std::string_view::npos ? line.size() + 1 : c + 1;
            return f;
        };
        std::size_t p = 0;
        r.kind = std::string(next_field(p));
        const auto ts_f = next_field(p);
        const auto side_f = next_field(p);
        const auto px_f = next_field(p);
        const auto qty_f = next_field(p);
        const auto ref_f = next_field(p);
        auto num = [](std::string_view v) -> std::int64_t {
            if (v.empty())
                return -1;
            std::int64_t val = 0;
            for (const char ch : v) {
                if (ch < '0' || ch > '9')
                    return -1;
                val = val * 10 + (ch - '0');
            }
            return val;
        };
        const std::int64_t ts = num(ts_f);
        // Control rows (halt/resume) carry only a timestamp; liquidity rows
        // validate every field.
        const bool control = r.kind == "halt" || r.kind == "resume";
        if (!control && r.kind != "ext_add" && r.kind != "trade" && r.kind != "strat_limit" &&
            r.kind != "strat_ioc" && r.kind != "strat_cancel")
            return false;
        if (ts < 0)
            return false;
        const std::int64_t px = control ? 0 : num(px_f);
        const std::int64_t qty = control ? 0 : num(qty_f);
        const std::int64_t ref = control ? 0 : num(ref_f);
        if (!control) {
            if (px < 0 || qty <= 0 || ref < 0 || side_f.size() != 1)
                return false;
            if (side_f[0] != 'B' && side_f[0] != 'S')
                return false;
        }

        r.ts = static_cast<std::uint64_t>(ts);
        r.price = px;
        r.qty = static_cast<std::uint32_t>(qty);
        r.ref = static_cast<std::uint64_t>(ref);
        r.side = side_f.empty() ? 'B' : side_f[0];
        if (p <= line.size()) {
            r.flags = std::string(next_field(p));
            // Validate now: unknown tokens are script errors, not silent
            // displayed orders.
            SimInbound probe{};
            if (!apply_flag_column(r.flags, probe))
                return false;
        }
        out.push_back(r);
    }
    return !first;
}

// Parses the optional flags column: "nd" | "mid[:off]" | "bid[:off]" |
// "ask[:off]". Unknown tokens are a script error.
[[nodiscard]] inline bool apply_flag_column(const std::string& flags, SimInbound& in) {
    if (flags.empty() || flags == "nd") {
        in.non_displayed = flags == "nd";
        return true;
    }
    const std::size_t colon = flags.find(':');
    const std::string kind = colon == std::string::npos ? flags : flags.substr(0, colon);
    std::int64_t off = 0;
    if (colon != std::string::npos) {
        if (flags.empty())
            return false;
        std::string num = flags.substr(colon + 1);
        bool neg = !num.empty() && num[0] == '-';
        // Explicit '+' is accepted and means positive, as in "mid:+5".
        if (!num.empty() && (num[0] == '-' || num[0] == '+'))
            num = num.substr(1);
        if (num.empty())
            return false;
        for (const char c : num) {
            if (c < '0' || c > '9')
                return false;
            off = off * 10 + (c - '0');
        }
        if (neg)
            off = -off;
    }
    if (kind == "mid")
        in.peg = Peg::mid;
    else if (kind == "bid")
        in.peg = Peg::bid;
    else if (kind == "ask")
        in.peg = Peg::ask;
    else if (kind == "nd")
        in.non_displayed = true;
    else
        return false;
    in.peg_offset_ticks = off;
    if (in.peg != Peg::none) {
        in.non_displayed = true;           // pegged facilities are non-displayable by rule
        in.type = SimOrderType::day_limit; // pegging implies resting semantics
    }
    return true;
}

[[nodiscard]] inline std::int64_t mid_of(const ExecutionSimulator& sim) noexcept {
    const std::int64_t b = sim.book().best_bid();
    const std::int64_t a = sim.book().best_ask();
    // kNoTick (INT64_MAX) means "that side is empty"; adding it to anything
    // overflows - caught by CI's sanitizer on a one-sided book.
    if (b == kNoTick || a == kNoTick || b <= 0 || a <= 0)
        return 0;
    return (b + a) / 2;
}

} // namespace detail

// Inline processing in file order preserves the script's own sequencing:
// liquidity is seeded, prints consume it, strategy actions submit and the
// engine drains up to the current stream position after every row.
[[nodiscard]] inline std::expected<Summary, Error> run(const std::string& script_text,
                                                       const SimConfig& cfg) {
    std::vector<detail::Row> rows;
    if (!detail::parse_script(script_text, rows))
        return std::unexpected(Error::bad_script);

    ExecutionSimulator sim(cfg);
    Summary out;
    constexpr std::uint64_t kStratBase = std::uint64_t{1} << 62;

    std::uint64_t prev_ts = 0;
    for (const detail::Row& r : rows) {
        ++out.script_rows;
        // Advance external time first so depletion / Hawkes / momentum act on
        // the gaps between scripted events; zero-depletion configs are exact.
        if (r.ts > prev_ts)
            sim.advance_time(r.ts - prev_ts);
        prev_ts = r.ts;
        if (r.kind == "halt") {
            sim.set_halted(true);
            continue;
        }
        if (r.kind == "resume") {
            sim.set_halted(false);
            continue;
        }
        if (r.kind == "ext_add") {
            static_cast<void>(
                sim.seed_external(OrderId{r.ref}, r.side == 'B' ? Side::buy : Side::sell,
                                  Qty{static_cast<std::int64_t>(r.qty)}, Price{r.price}));
        } else if (r.kind == "trade") {
            // Script side is the AGGRESSOR; apply_external consumes the hit side.
            const Side hit = r.side == 'B' ? Side::sell : Side::buy;
            sim.apply_external(hit, Price{r.price}, static_cast<std::int64_t>(r.qty));
            if (!sim.halted())
                ++out.prints;
        } else if (r.kind == "strat_cancel") {
            static_cast<void>(sim.cancel_strategy(OrderId{r.ref + kStratBase}));
        } else {
            const SimOrderType ty =
                r.kind == "strat_limit" ? SimOrderType::day_limit : SimOrderType::ioc;
            SimInbound inbound{OrderId{r.ref + kStratBase},
                               r.side == 'B' ? Side::buy : Side::sell,
                               Qty{static_cast<std::int64_t>(r.qty)},
                               Price{r.price},
                               ty,
                               Peg::none,
                               0,
                               false};
            static_cast<void>(detail::apply_flag_column(r.flags, inbound));
            static_cast<void>(sim.submit(inbound, r.ts));
        }
        const std::size_t seen_decisions = sim.decisions().size();
        sim.drain();
        // Event records carry the SCRIPT row timestamp: advance_time() and
        // decide() drive separate internal clocks, so log consumers get one
        // authoritative timeline. Engine stamps remain in the digest.
        const std::uint64_t row_ts = r.ts;
        // Passive ledger: reports() carries fills RECEIVED by our resting
        // orders (maker settlement already applied inside).
        for (const auto& f : sim.reports()) {
            EventRecord e;
            e.type = 'F';
            e.ts_ns = row_ts;
            e.ref = f.ref;
            e.side = f.side;
            e.price_ticks = f.price_ticks;
            e.qty = f.qty;
            e.fee_cash = f.fee;
            e.mid_ticks = detail::mid_of(sim);
            e.ledger = 'M';
            out.events.push_back(e); // submit_ns unknown for passive fills
            out.volume_ticks += f.price_ticks * static_cast<std::int64_t>(f.qty);
            out.fees_paid_cash += f.fee;
            ++out.fills;
            ++out.maker_fills;
        }
        sim.clear_reports();
        // Aggressive ledger: our own crossing fills live in decisions(),
        // priced by filled_notional, settled at the taker schedule.
        for (std::size_t i = seen_decisions; i < sim.decisions().size(); ++i) {
            const auto& d = sim.decisions()[i];
            if (d.filled_qty == 0)
                continue;
            EventRecord e;
            e.type = 'F';
            e.ts_ns = row_ts;
            e.ref = d.ref;
            e.side = d.side;
            e.price_ticks = static_cast<std::int64_t>(
                (d.filled_notional + static_cast<std::uint64_t>(d.filled_qty) / 2ULL) /
                static_cast<std::uint64_t>(d.filled_qty));
            e.qty = d.filled_qty;
            e.fee_cash = mog::fee_bps_of(d.filled_notional, cfg.taker_fee_bps);
            e.mid_ticks = detail::mid_of(sim);
            e.ledger = 'T';
            e.submit_ns = d.decision_ts;
            out.events.push_back(e);
            out.volume_ticks += static_cast<std::int64_t>(d.filled_notional);
            out.fees_paid_cash += e.fee_cash;
            ++out.fills;
            ++out.taker_fills;
        }
        if (r.kind == "trade" && !sim.halted()) {
            EventRecord e;
            e.type = 'P';
            e.ts_ns = r.ts;
            e.side = r.side; // aggressor side as scripted
            e.price_ticks = r.price;
            e.qty = r.qty;
            e.mid_ticks = detail::mid_of(sim);
            out.events.push_back(e);
        }
    }

    out.digest_mode = sim.digest_mode();
    if (out.digest_mode == DigestMode::fast) {
        const auto dg = sim.fast_trace_digest();
        std::memcpy(&out.digest_high, dg.data(), sizeof(out.digest_high));
    } else {
        const auto dg = sim.trace_digest();
        std::memcpy(&out.digest_high, dg.data(), sizeof(out.digest_high));
    }
    return out;
}

[[nodiscard]] inline std::string_view events_header() {
    return "type,ts_ns,ref,side,price_ticks,qty,fee_cash,mid_ticks,ledger,"
           "submit_ns";
}

[[nodiscard]] inline std::string to_csv(const EventRecord& e) {
    char buf[160];
    std::snprintf(buf, sizeof(buf), "%c,%llu,%llu,%c,%lld,%u,%lld,%lld,%c,%llu", e.type,
                  static_cast<unsigned long long>(e.ts_ns), static_cast<unsigned long long>(e.ref),
                  e.side, static_cast<long long>(e.price_ticks), e.qty,
                  static_cast<long long>(e.fee_cash), static_cast<long long>(e.mid_ticks), e.ledger,
                  static_cast<unsigned long long>(e.submit_ns));
    return std::string(buf);
}

} // namespace mog::simrun
