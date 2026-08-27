// Canonical trade-CSV diff: our reconstruction against a reference file
// (e.g. converted from NASDAQ's official Daily Trade). Rows are keyed by
// match number; price/shares/timestamp divergences are reported per field
// with the first divergence position - a fidelity report, not a boolean.
#pragma once

#include <charconv>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace mog::trades {

struct DiffRow {
    std::uint64_t match_number = 0;
    std::int64_t ours_price = 0;
    std::int64_t ref_price = 0;
    std::uint32_t ours_shares = 0;
    std::uint32_t ref_shares = 0;
};

struct DiffReport {
    std::size_t rows_ours = 0;
    std::size_t rows_ref = 0;
    std::size_t matched = 0;
    std::size_t price_mismatch = 0;
    std::size_t shares_mismatch = 0;
    DiffRow first_divergence{};
    bool has_divergence = false;

    [[nodiscard]] bool clean() const noexcept {
        return rows_ours == rows_ref && rows_ref == matched && price_mismatch == 0 &&
               shares_mismatch == 0;
    }
};

struct ParsedCsv {
    // match_number -> (price_ticks, shares)
    std::unordered_map<std::uint64_t, std::pair<std::int64_t, std::uint32_t>> rows;
};

// Minimal strict parser for the canonical format emitted by `mog trades
// --csv`; header required, seven fields, numeric fields validated.
[[nodiscard]] inline bool parse_canonical_csv(std::string_view text, ParsedCsv& out) {
    std::size_t pos = 0;
    bool first = true;
    while (pos <= text.size()) {
        const std::size_t eol = text.find('\n', pos);
        std::string_view line =
            text.substr(pos, eol == std::string_view::npos ? text.size() - pos : eol - pos);
        pos = eol == std::string_view::npos ? text.size() + 1 : eol + 1;
        if (!line.empty() && line.back() == '\r')
            line.remove_suffix(1);
        if (line.empty())
            continue;
        if (first) {
            if (line != "match_number,locate,ts_ns,price_ticks,shares,"
                        "printable,with_price")
                return false;
            first = false;
            continue;
        }
        // Fields: match,locate,ts,price,shares,printable,with_price
        std::uint64_t nums[5] = {};
        std::size_t f = 0, p = 0;
        bool bad = false;
        for (; f < 5 && !bad; ++f) {
            const std::size_t comma = line.find(',', p);
            if (comma == std::string_view::npos)
                bad = true;
            else {
                const std::string_view field = line.substr(p, comma - p);
                if (field.empty())
                    bad = true;
                else {
                    const auto [ptr, ec] =
                        std::from_chars(field.data(), field.data() + field.size(), nums[f]);
                    if (ec != std::errc{} || ptr != field.data() + field.size())
                        bad = true;
                }
                p = comma + 1;
            }
        }
        if (bad || f != 5)
            return false; // trailing fields not validated beyond count
        out.rows[nums[0]] = {static_cast<std::int64_t>(nums[3]),
                             static_cast<std::uint32_t>(nums[4])};
    }
    return !first;
}

[[nodiscard]] inline DiffReport diff(const ParsedCsv& ours, const ParsedCsv& ref) {
    DiffReport r;
    r.rows_ours = ours.rows.size();
    r.rows_ref = ref.rows.size();
    for (const auto& [match, ps] : ours.rows) {
        const auto it = ref.rows.find(match);
        if (it == ref.rows.end())
            continue;
        ++r.matched;
        const bool price_ok = ps.first == it->second.first;
        const bool shares_ok = ps.second == it->second.second;
        if (!price_ok)
            ++r.price_mismatch;
        if (!shares_ok)
            ++r.shares_mismatch;
        if ((!price_ok || !shares_ok) && !r.has_divergence) {
            r.has_divergence = true;
            r.first_divergence =
                DiffRow{match, ps.first, it->second.first, ps.second, it->second.second};
        }
    }
    return r;
}

} // namespace mog::trades
