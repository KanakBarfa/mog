// Session-level market state: System Event phases, per-stock halt windows,
// NOII imbalance snapshots, Reg SHO restrictions, and the close-cross model.
//
// These are the messages a naive order-book replay skips - and exactly where
// real days live: fills during halts are suspect, and the open/close crosses
// execute quantity that never appears in the continuous book. The cross model
// is explicit: at System Event 'M' (end of market hours) each stock with a
// NOII snapshot crosses at its near reference price for paired + |imbalance|
// shares, with direction carried through. Bias note: treating the full
// imbalance as executable assumes auction supply materializes; conservative
// users should size positions off `paired` alone.
#pragma once

#include <mog/Types.hpp>
#include <mog/Wire.hpp>

#include <algorithm>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace mog::session {

enum class Phase : std::uint8_t {
    idle = 0,         // no System Event seen yet
    messages_started, // 'O' start of messages
    system_hours,     // 'S' start of system hours
    market_open,      // 'Q' start of market hours (continuous trading)
    market_closed,    // 'M' end of market hours (close cross territory)
    system_closed,    // 'E' end of system hours
    ended,            // 'C' end of messages
};

struct HaltWindow {
    std::uint32_t locate = 0;
    std::uint64_t start_ns = 0;
    std::uint64_t end_ns = 0; // 0 while still halted
    char reason[5] = {};      // NUL-terminated copy of the 4-char code
};

struct NoiiSnapshot {
    std::uint64_t ts_ns = 0;
    std::uint32_t locate = 0;
    std::uint64_t paired_shares = 0;
    std::uint64_t imbalance_shares = 0;
    char direction = '0'; // 'B' buy imbalance, 'S' sell imbalance, '0' none
    std::int64_t near_price_ticks = 0;
    std::int64_t reference_price_ticks = 0;
};

enum class CrossType : char { opening = 'O', closing = 'C', halt_ipo = 'H' };

struct CrossRecord {
    std::uint32_t locate = 0;
    std::int64_t price_ticks = 0; // near reference price
    std::int64_t qty = 0;         // paired + |imbalance|
    char direction = '0';
    CrossType type = CrossType::closing;
};

struct RegShoState {
    std::uint32_t locate = 0;
    char action = '0'; // '0' none, '1' intra-day threshold, '2' pre-open
};

class Model {
public:
    // --- frame feeders (decoded from raw standard frames) ------------------
    void on_system_event(char code, std::uint64_t ts_ns) noexcept {
        switch (code) {
        case 'O':
            phase_ = Phase::messages_started;
            break;
        case 'S':
            phase_ = Phase::system_hours;
            break;
        case 'Q':
            phase_ = Phase::market_open;
            open_ts_ = ts_ns;
            compute_open_cross();
            break;
        case 'M':
            phase_ = Phase::market_closed;
            close_ts_ = ts_ns;
            compute_close_cross();
            break;
        case 'E':
            phase_ = Phase::system_closed;
            break;
        case 'C':
            phase_ = Phase::ended;
            break;
        default:
            break; // reserved codes ignored explicitly
        }
        ++system_events_;
    }

    void on_trading_action(std::uint32_t locate, char state, const char* reason,
                           std::uint64_t ts_ns) noexcept {
        if (state == 'H' || state == 'P') {
            if (!is_halted(locate)) {
                HaltWindow w;
                w.locate = locate;
                w.start_ns = ts_ns;
                for (int i = 0; i < 4; ++i)
                    w.reason[i] = reason[i];
                const std::size_t idx = windows_.size();
                windows_.push_back(w);
                locate_to_windows_[locate].push_back(idx);
            }
        } else if (state == 'T') {
            const auto it = locate_to_windows_.find(locate);
            if (it != locate_to_windows_.end()) {
                for (const std::size_t idx : it->second)
                    if (windows_[idx].end_ns == 0)
                        windows_[idx].end_ns = ts_ns;
                compute_halt_cross(locate);
            }
        }
        ++trading_actions_;
    }

    void on_noii(const NoiiSnapshot& snap) noexcept {
        latest_[snap.locate] = snap;
        // Zero-valued snapshots exist: NASDAQ clears imbalance state after
        // each auction. They must not erase the last informative one.
        if (snap.paired_shares > 0 || snap.imbalance_shares > 0)
            informative_[snap.locate] = snap;
        ++noii_updates_;
    }

    void on_reg_sho(std::uint32_t locate, char action) noexcept {
        sho_[locate] = action;
        ++reg_sho_events_;
    }

    // --- queries ------------------------------------------------------------
    [[nodiscard]] Phase phase() const noexcept { return phase_; }
    [[nodiscard]] std::size_t system_events() const noexcept { return system_events_; }
    [[nodiscard]] std::size_t trading_actions() const noexcept { return trading_actions_; }
    [[nodiscard]] std::size_t noii_updates() const noexcept { return noii_updates_; }
    [[nodiscard]] std::size_t reg_sho_events() const noexcept { return reg_sho_events_; }
    [[nodiscard]] std::uint64_t open_ts_ns() const noexcept { return open_ts_; }
    [[nodiscard]] std::uint64_t close_ts_ns() const noexcept { return close_ts_; }

    [[nodiscard]] bool is_halted(std::uint32_t locate) const noexcept {
        const auto it = locate_to_windows_.find(locate);
        if (it == locate_to_windows_.end())
            return false;
        for (const std::size_t idx : it->second)
            if (windows_[idx].end_ns == 0)
                return true;
        return false;
    }

    [[nodiscard]] bool was_halted_at(std::uint32_t locate, std::uint64_t ts_ns) const noexcept {
        const auto it = locate_to_windows_.find(locate);
        if (it == locate_to_windows_.end())
            return false;
        for (const std::size_t idx : it->second) {
            const auto& w = windows_[idx];
            if (w.start_ns <= ts_ns && (w.end_ns == 0 || ts_ns < w.end_ns))
                return true;
        }
        return false;
    }

    [[nodiscard]] const std::vector<HaltWindow>& halt_windows() const noexcept { return windows_; }
    [[nodiscard]] const NoiiSnapshot* latest_noii(std::uint32_t locate) const noexcept {
        const auto it = latest_.find(locate);
        return it == latest_.end() ? nullptr : &it->second;
    }
    [[nodiscard]] const std::vector<CrossRecord>& open_crosses() const noexcept {
        return open_crosses_;
    }
    [[nodiscard]] const std::vector<CrossRecord>& close_crosses() const noexcept {
        return close_crosses_;
    }
    [[nodiscard]] const std::vector<CrossRecord>& halt_crosses() const noexcept {
        return halt_crosses_;
    }

private:
    void compute_open_cross() noexcept {
        open_crosses_.clear();
        std::vector<std::uint32_t> keys;
        keys.reserve(informative_.size());
        for (const auto& [locate, snap] : informative_)
            keys.push_back(locate);
        std::sort(keys.begin(), keys.end());
        for (const std::uint32_t locate : keys) {
            const NoiiSnapshot& snap = informative_[locate];
            CrossRecord r;
            r.locate = locate;
            r.price_ticks = snap.near_price_ticks;
            r.qty = static_cast<std::int64_t>(snap.paired_shares + snap.imbalance_shares);
            r.direction = snap.direction;
            r.type = CrossType::opening;
            open_crosses_.push_back(r);
        }
    }

    void compute_close_cross() noexcept {
        close_crosses_.clear();
        std::vector<std::uint32_t> keys;
        keys.reserve(informative_.size());
        for (const auto& [locate, snap] : informative_)
            keys.push_back(locate);
        std::sort(keys.begin(), keys.end());
        for (const std::uint32_t locate : keys) {
            const NoiiSnapshot& snap = informative_[locate];
            CrossRecord r;
            r.locate = locate;
            r.price_ticks = snap.near_price_ticks;
            r.qty = static_cast<std::int64_t>(snap.paired_shares + snap.imbalance_shares);
            r.direction = snap.direction;
            r.type = CrossType::closing;
            close_crosses_.push_back(r);
        }
    }

    void compute_halt_cross(std::uint32_t locate) noexcept {
        const auto it = informative_.find(locate);
        if (it != informative_.end()) {
            const NoiiSnapshot& snap = it->second;
            CrossRecord r;
            r.locate = locate;
            r.price_ticks = snap.near_price_ticks;
            r.qty = static_cast<std::int64_t>(snap.paired_shares + snap.imbalance_shares);
            r.direction = snap.direction;
            r.type = CrossType::halt_ipo;
            halt_crosses_.push_back(r);
        }
    }

    Phase phase_ = Phase::idle;
    std::size_t system_events_ = 0;
    std::size_t trading_actions_ = 0;
    std::size_t noii_updates_ = 0;
    std::size_t reg_sho_events_ = 0;
    std::uint64_t open_ts_ = 0;
    std::uint64_t close_ts_ = 0;
    std::vector<HaltWindow> windows_;
    std::unordered_map<std::uint32_t, std::vector<std::size_t>> locate_to_windows_;
    std::vector<CrossRecord> open_crosses_;
    std::vector<CrossRecord> close_crosses_;
    std::vector<CrossRecord> halt_crosses_;
    std::unordered_map<std::uint32_t, NoiiSnapshot> latest_;
    std::unordered_map<std::uint32_t, NoiiSnapshot> informative_;
    std::unordered_map<std::uint32_t, char> sho_;
};

} // namespace mog::session
