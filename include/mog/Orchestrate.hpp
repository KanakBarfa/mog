// Multi-instrument orchestration: fan-out across N independent single-
// instrument ExecutionSimulators behind one deterministic facade.
//
// The core stays single-instrument by design (DESIGN.md);
// this layer adds portfolio-scale plumbing without polluting either the
// hot path or the per-book semantics:
//
//   - Every instrument-scoped op names its target index explicitly, so
//     routing is never inferred and cannot be ambiguous.
//   - Time ops (run_until / drain / advance_time) broadcast to all
//     instruments in ascending index order. Combined with per-instrument
//     determinism, that makes the whole portfolio a pure function of
//     (per-instrument configs, op sequence) - replay it and you get
//     bit-identical traces.
//   - Refs are the user's responsibility: instruments do not share an id
//     space, and cross-instrument strategy logic is out of scope for v1.
//
// Instruments are heap-held because ExecutionSimulator is neither movable
// nor assignable; the array itself is fixed-capacity and pointer-stable.
#pragma once

#include <mog/Contracts.hpp>
#include <mog/Simulate.hpp>

#include <memory>
#include <string>
#include <vector>

namespace mog {

inline constexpr std::size_t kMaxInstruments = 256;

class Orchestrator {
public:
    // Explicit: vector<Instrument> declares its copy constructor even though
    // Instrument cannot be copied, which makes std::is_copy_constructible
    // lie until instantiation. Binding layers query these traits.
    Orchestrator() = default;
    Orchestrator(const Orchestrator&) = delete;
    Orchestrator& operator=(const Orchestrator&) = delete;

    // Adds an instrument and returns its routing index. Names are labels
    // for humans; indices are the addressing scheme.
    [[nodiscard]] std::size_t add_instrument(const SimConfig& cfg, std::string name = {}) {
        MOG_PRE(instruments_.size() < kMaxInstruments);
        const std::size_t idx = instruments_.size();
        instruments_.push_back(
            Instrument{std::make_unique<ExecutionSimulator>(cfg), std::move(name)});
        return idx;
    }

    [[nodiscard]] std::size_t count() const noexcept { return instruments_.size(); }
    [[nodiscard]] const std::string& name(std::size_t idx) const noexcept {
        return instruments_[at(idx)].name;
    }

    // ---- routed ops -----------------------------------------------------------
    [[nodiscard]] BookTick seed_external(std::size_t idx, OrderId ref, Side s, Qty qty,
                                         Price price) {
        return instruments_[at(idx)].sim->seed_external(ref, s, qty, price);
    }

    [[nodiscard]] BookTick seed_iceberg(std::size_t idx, const IcebergSpec& spec) {
        return instruments_[at(idx)].sim->seed_iceberg(spec);
    }

    void set_stp_mode(std::size_t idx, StpMode m) noexcept {
        instruments_[at(idx)].sim->set_stp_mode(m);
    }

    void run_until(std::size_t idx, std::uint64_t until_ts) {
        instruments_[at(idx)].sim->run_until(until_ts);
    }

    void drain(std::size_t idx) { instruments_[at(idx)].sim->drain(); }

    [[nodiscard]] std::uint64_t submit(std::size_t idx, const SimInbound& in,
                                       std::uint64_t arrival_ts) {
        return instruments_[at(idx)].sim->submit(in, arrival_ts);
    }

    void apply_external(std::size_t idx, Side s, Price price, std::int64_t qty) {
        instruments_[at(idx)].sim->apply_external(s, price, qty);
    }

    void advance_time(std::size_t idx, std::uint64_t dt_ns) {
        instruments_[at(idx)].sim->advance_time(dt_ns);
    }

    [[nodiscard]] bool cancel_strategy(std::size_t idx, OrderId ref) noexcept {
        return instruments_[at(idx)].sim->cancel_strategy(ref);
    }

    [[nodiscard]] BookTick replace_strategy(std::size_t idx, OrderId orig, OrderId fresh, Qty qty,
                                            Price price) noexcept {
        return instruments_[at(idx)].sim->replace_strategy(orig, fresh, qty, price);
    }

    [[nodiscard]] std::int64_t queue_ahead_of(std::size_t idx, OrderId ref) const noexcept {
        return instruments_[at(idx)].sim->queue_ahead_of(ref);
    }

    [[nodiscard]] std::size_t pending_decisions(std::size_t idx) const noexcept {
        return instruments_[at(idx)].sim->pending_decisions();
    }

    // ---- broadcast ops ----------------------------------------------------------
    // Ascending-index order is the determinism contract: identical op
    // sequences produce identical per-instrument RNG consumption regardless
    // of any other factor.
    void drain_all() {
        for (auto& inst : instruments_)
            inst.sim->drain();
    }

    void advance_all(std::uint64_t dt_ns) {
        for (auto& inst : instruments_)
            inst.sim->advance_time(dt_ns);
    }

    void run_until_all(std::uint64_t until_ts) {
        for (auto& inst : instruments_)
            inst.sim->run_until(until_ts);
    }

    // ---- top of book & pricing --------------------------------------------------
    [[nodiscard]] std::int64_t best_bid(std::size_t idx) const noexcept {
        return instruments_[at(idx)].sim->book().best_bid();
    }

    [[nodiscard]] std::int64_t best_ask(std::size_t idx) const noexcept {
        return instruments_[at(idx)].sim->book().best_ask();
    }

    [[nodiscard]] double mid_price(std::size_t idx) const noexcept {
        const auto b = best_bid(idx);
        const auto a = best_ask(idx);
        if (b == kNoTick || a == kNoTick)
            return 0.0;
        return static_cast<double>(b + a) / 2.0;
    }

    // ---- portfolio analytics ----------------------------------------------------
    [[nodiscard]] std::uint64_t total_filled_notional() const noexcept {
        std::uint64_t sum = 0;
        for (const auto& inst : instruments_)
            sum += inst.sim->total_filled_notional();
        return sum;
    }

    [[nodiscard]] std::int64_t total_fees() const noexcept {
        std::int64_t sum = 0;
        for (const auto& inst : instruments_)
            sum += inst.sim->total_fees();
        return sum;
    }

    [[nodiscard]] std::size_t total_fill_count() const noexcept {
        std::size_t sum = 0;
        for (const auto& inst : instruments_)
            sum += inst.sim->total_fill_count();
        return sum;
    }

    // ---- inspection ---------------------------------------------------------------
    [[nodiscard]] const ExecutionSimulator& sim(std::size_t idx) const noexcept {
        return *instruments_[at(idx)].sim;
    }

    [[nodiscard]] ExecutionSimulator& sim(std::size_t idx) noexcept {
        return *instruments_[at(idx)].sim;
    }

    // Folds per-instrument trace digests in ascending index order. Empty
    // portfolios fold to the SHA-256 of nothing, which is stable and useless.
    [[nodiscard]] std::array<unsigned char, 32> global_digest() const noexcept {
        Sha256 fold{};
        for (const auto& inst : instruments_) {
            const auto d = inst.sim->trace_digest();
            fold.update(d.data(), d.size());
        }
        return fold.finish();
    }

    [[nodiscard]] bool audit() const noexcept {
        for (const auto& inst : instruments_)
            if (!inst.sim->audit())
                return false;
        return true;
    }

private:
    struct Instrument {
        std::unique_ptr<ExecutionSimulator> sim;
        std::string name;
    };

    [[nodiscard]] std::size_t at(std::size_t idx) const noexcept {
        MOG_PRE(idx < instruments_.size());
        return idx;
    }

    std::vector<Instrument> instruments_;
};

} // namespace mog
