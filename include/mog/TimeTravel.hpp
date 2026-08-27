// Time-travel debugger: record/replay session over ExecutionSimulator.
//
// Every causal mutator call is journaled as a SimOp. Because the simulator
// is a pure function of (config, op sequence) - the seeded RNG is consumed
// strictly in call order - rewinding needs no state snapshots: seek(k)
// rebuilds a fresh simulator and replays ops[0..k). fork_at(k) returns an
// independent branch sharing the prefix, which is exactly counterfactual
// replay: apply different history after k, then compare trace digests and
// decision streams against the base branch.
//
// This layer is tooling, not hot path; it allocates freely. The wrapped
// simulator's zero-allocation replay discipline is untouched.
#pragma once

#include <mog/Contracts.hpp>
#include <mog/Simulate.hpp>

#include <cstdint>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace mog {

enum class SimOpKind : std::uint8_t {
    seed_external = 0,
    seed_iceberg = 1,
    set_stp = 2,
    submit = 3,
    run_until = 4,
    drain = 5,
    apply_external = 6,
    advance_time = 7,
    cancel_strategy = 8,
    replace_strategy = 9,
};

// One journaled mutator call. Fields are union-by-convention per kind:
//   seed_external   ref, side, qty, price
//   seed_iceberg    ref, side, price, num=display, num2=total
//   set_stp         stp
//   submit          inbound (ref/side/qty/price/type), ts=arrival_ts
//   run_until       ts
//   drain           -
//   apply_external  side, price, num (signed units)
//   advance_time    ts (=dt_ns)
//   cancel_strategy ref
//   replace_strategy ref, ref2=fresh, qty, price
struct SimOp {
    SimOpKind kind{};
    OrderId ref{};
    OrderId ref2{};
    Side side = Side::buy;
    Qty qty{};
    Price price{};
    std::int64_t num = 0;
    std::int64_t num2 = 0;
    std::uint64_t ts = 0;
    SimOrderType type = SimOrderType::day_limit;
    StpMode stp = StpMode::none;
};

[[nodiscard]] inline std::string_view op_kind_name(SimOpKind k) noexcept {
    switch (k) {
    case SimOpKind::seed_external:
        return "seed_external";
    case SimOpKind::seed_iceberg:
        return "seed_iceberg";
    case SimOpKind::set_stp:
        return "set_stp";
    case SimOpKind::submit:
        return "submit";
    case SimOpKind::run_until:
        return "run_until";
    case SimOpKind::drain:
        return "drain";
    case SimOpKind::apply_external:
        return "apply_external";
    case SimOpKind::advance_time:
        return "advance_time";
    case SimOpKind::cancel_strategy:
        return "cancel_strategy";
    case SimOpKind::replace_strategy:
        return "replace_strategy";
    }
    return "?";
}

struct L2Row {
    char side = 'B';
    std::int64_t tick = 0;
    std::int64_t qty = 0;
};

// Index into decisions()/reports() when no divergence exists.
inline constexpr std::size_t kNoDivergence = static_cast<std::size_t>(-1);

class TimeTravelSession {
public:
    explicit TimeTravelSession(SimConfig cfg) : cfg_(cfg) { sim_.emplace(cfg_); }

    // Non-copyable and immovable: ExecutionSimulator itself is neither, and
    // fork_at() takes an out-parameter instead of returning by value.
    TimeTravelSession(const TimeTravelSession&) = delete;
    TimeTravelSession& operator=(const TimeTravelSession&) = delete;

    // ---- recorded forward ops -------------------------------------------------
    [[nodiscard]] BookTick seed_external(OrderId ref, Side s, Qty qty, Price price) {
        const BookTick t = sim_->seed_external(ref, s, qty, price);
        if (ok(t))
            record(SimOp{SimOpKind::seed_external, ref, OrderId{}, s, qty, price});
        return t;
    }

    [[nodiscard]] BookTick seed_iceberg(const IcebergSpec& spec) {
        const BookTick t = sim_->seed_iceberg(spec);
        if (ok(t)) {
            SimOp op{SimOpKind::seed_iceberg, spec.ref, OrderId{}, spec.side, Qty{}, spec.price};
            op.num = spec.display;
            op.num2 = spec.total;
            record(op);
        }
        return t;
    }

    void set_stp_mode(StpMode m) {
        sim_->set_stp_mode(m);
        record(SimOp{SimOpKind::set_stp, OrderId{}, OrderId{}, Side::buy, Qty{}, Price{}, 0, 0, 0,
                     SimOrderType::day_limit, m});
    }

    [[nodiscard]] std::uint64_t submit(const SimInbound& in, std::uint64_t arrival_ts) {
        const std::uint64_t decision_ts = sim_->submit(in, arrival_ts);
        SimOp op{SimOpKind::submit, in.ref, OrderId{}, in.side, in.qty, in.price};
        op.ts = arrival_ts;
        op.type = in.type;
        record(op);
        return decision_ts;
    }

    void run_until(std::uint64_t until_ts) {
        sim_->run_until(until_ts);
        SimOp op{SimOpKind::run_until};
        op.ts = until_ts;
        record(op);
    }

    void drain() {
        sim_->drain();
        record(SimOp{SimOpKind::drain});
    }

    void apply_external(Side s, Price price, std::int64_t qty) {
        sim_->apply_external(s, price, qty);
        SimOp op{SimOpKind::apply_external};
        op.side = s;
        op.price = price;
        op.num = qty;
        record(op);
    }

    void advance_time(std::uint64_t dt_ns) {
        sim_->advance_time(dt_ns);
        SimOp op{SimOpKind::advance_time};
        op.ts = dt_ns;
        record(op);
    }

    [[nodiscard]] bool cancel_strategy(OrderId ref) {
        const bool removed = sim_->cancel_strategy(ref);
        if (removed)
            record(SimOp{SimOpKind::cancel_strategy, ref});
        return removed;
    }

    [[nodiscard]] BookTick replace_strategy(OrderId orig, OrderId fresh, Qty qty, Price price) {
        const BookTick t = sim_->replace_strategy(orig, fresh, qty, price);
        if (ok(t))
            record(SimOp{SimOpKind::replace_strategy, orig, fresh, Side::buy, qty, price});
        return t;
    }

    // ---- travel ---------------------------------------------------------------
    // Moves the cursor to k. Rewinding rebuilds from scratch; fast-forwarding
    // applies the tail incrementally. Either way the postcondition is:
    // state == sequential application of ops[0..k).
    void seek(std::size_t k) {
        MOG_PRE(k <= log_.size());
        if (k < cursor_) {
            sim_.reset();
            sim_.emplace(cfg_);
            cursor_ = 0;
        }
        while (cursor_ < k)
            apply(log_[cursor_++]);
    }

    [[nodiscard]] std::size_t cursor() const noexcept { return cursor_; }
    [[nodiscard]] std::size_t op_count() const noexcept { return log_.size(); }
    [[nodiscard]] const std::vector<SimOp>& log() const noexcept { return log_; }

    // Independent branch over the shared prefix [0..k). Divergence after the
    // fork point never touches the base branch. The branch must be a fresh
    // session built from the same config.
    void fork_at(std::size_t k, TimeTravelSession& branch) const {
        MOG_PRE(k <= log_.size());
        MOG_PRE(branch.log_.empty() && branch.cursor_ == 0);
        branch.log_.assign(log_.begin(), log_.begin() + static_cast<std::ptrdiff_t>(k));
        while (branch.cursor_ < k)
            branch.apply(branch.log_[branch.cursor_++]);
    }

    // ---- inspection -----------------------------------------------------------
    [[nodiscard]] const ExecutionSimulator& sim() const noexcept { return *sim_; }
    [[nodiscard]] const SimConfig& config() const noexcept { return cfg_; }
    [[nodiscard]] bool audit() const { return sim_->audit(); }

    [[nodiscard]] std::vector<L2Row> l2_rows() const {
        std::vector<L2Row> rows;
        sim_->book().for_each_l2([&](Side s, std::int64_t tick, std::int64_t qty) {
            rows.push_back(L2Row{to_wire(s), tick, qty});
        });
        return rows;
    }

    [[nodiscard]] std::int64_t queue_ahead_of(OrderId ref) const noexcept {
        return sim_->queue_ahead_of(ref);
    }

    // ---- counterfactual diff ----------------------------------------------------
    // First index where the two branches' decision streams disagree, or
    // kNoDivergence. Streams are compared field-for-field including seq, so
    // identical prefixes short-circuit cheaply and any behavioral divergence
    // downstream of the fork shows up at its first visible effect.
    [[nodiscard]] static std::size_t
    first_decision_divergence(const TimeTravelSession& a, const TimeTravelSession& b) noexcept {
        const auto& da = a.sim().decisions();
        const auto& db = b.sim().decisions();
        const std::size_t n = std::min(da.size(), db.size());
        for (std::size_t i = 0; i < n; ++i) {
            const SimDecision& x = da[i];
            const SimDecision& y = db[i];
            if (x.kind != y.kind || x.filled_qty != y.filled_qty ||
                x.cancelled_qty != y.cancelled_qty || x.filled_notional != y.filled_notional ||
                x.visible_ts != y.visible_ts || x.ref != y.ref || x.side != y.side ||
                x.seq != y.seq)
                return i;
        }
        return da.size() == db.size() ? kNoDivergence : n;
    }

    // ---- reporting --------------------------------------------------------------
    // Deterministic text: two sessions with identical logs always render
    // byte-identical reports, so exports are golden-testable.
    [[nodiscard]] std::string export_report() const {
        std::ostringstream out;
        out << "# mog time-travel report\n";
        out << "ops: " << log_.size() << "\n";
        out << "cursor: " << cursor_ << "\n";
        out << "seed: " << cfg_.seed << "\n";
        out << "stp: " << static_cast<int>(sim_->stp_mode()) << "\n";
        const auto digest = sim_->trace_digest();
        out << "trace: ";
        for (const unsigned char b : digest)
            out << hex_digit(b >> 4) << hex_digit(b & 0xF);
        out << "\n";

        out << "## timeline\n";
        for (std::size_t i = 0; i < log_.size(); ++i)
            out << i << ' ' << render_op(log_[i]) << '\n';

        out << "## decisions\n";
        out << "seq kind filled cancelled notional ts ref side\n";
        for (const SimDecision& d : sim_->decisions())
            out << d.seq << ' ' << decision_kind_name(d.kind) << ' ' << d.filled_qty << ' '
                << d.cancelled_qty << ' ' << d.filled_notional << ' ' << d.visible_ts << ' '
                << d.ref << ' ' << d.side << '\n';

        out << "## fills\n";
        out << "ts ref px qty seq side\n";
        for (const SimFillReport& r : sim_->reports())
            out << r.visible_ts << ' ' << r.ref << ' ' << r.price_ticks << ' ' << r.qty << ' '
                << r.seq << ' ' << r.side << '\n';

        out << "## trades\n";
        out << "ts seq px qty aggressor_buy\n";
        for (const SimTrade& t : sim_->trades())
            out << t.ts << ' ' << t.seq << ' ' << t.px_ticks << ' ' << t.qty << ' '
                << static_cast<int>(t.aggressor_buy) << '\n';

        out << "## book L2\n";
        out << "side tick qty\n";
        for (const L2Row& r : l2_rows())
            out << r.side << ' ' << r.tick << ' ' << r.qty << '\n';
        return out.str();
    }

private:
    void record(const SimOp& op) {
        if (cursor_ < log_.size())
            log_.erase(log_.begin() + static_cast<std::ptrdiff_t>(cursor_), log_.end());
        log_.push_back(op);
        ++cursor_;
    }

    void apply(const SimOp& op) {
        switch (op.kind) {
        case SimOpKind::seed_external:
            static_cast<void>(sim_->seed_external(op.ref, op.side, op.qty, op.price));
            break;
        case SimOpKind::seed_iceberg:
            static_cast<void>(
                sim_->seed_iceberg(IcebergSpec{op.ref, op.side, op.price, op.num, op.num2}));
            break;
        case SimOpKind::set_stp:
            sim_->set_stp_mode(op.stp);
            break;
        case SimOpKind::submit:
            static_cast<void>(
                sim_->submit(SimInbound{op.ref, op.side, op.qty, op.price, op.type}, op.ts));
            break;
        case SimOpKind::run_until:
            sim_->run_until(op.ts);
            break;
        case SimOpKind::drain:
            sim_->drain();
            break;
        case SimOpKind::apply_external:
            sim_->apply_external(op.side, op.price, op.num);
            break;
        case SimOpKind::advance_time:
            sim_->advance_time(op.ts);
            break;
        case SimOpKind::cancel_strategy:
            static_cast<void>(sim_->cancel_strategy(op.ref));
            break;
        case SimOpKind::replace_strategy:
            static_cast<void>(sim_->replace_strategy(op.ref, op.ref2, op.qty, op.price));
            break;
        }
    }

    [[nodiscard]] static char hex_digit(unsigned v) noexcept {
        return v < 10 ? static_cast<char>('0' + v) : static_cast<char>('a' + v - 10);
    }

    [[nodiscard]] static std::string_view decision_kind_name(SimDecision::Kind k) noexcept {
        using K = SimDecision::Kind;
        switch (k) {
        case K::rested:
            return "rested";
        case K::filled:
            return "filled";
        case K::partial_then_rest:
            return "partial_then_rest";
        case K::partial_then_cancelled:
            return "partial_then_cancelled";
        case K::rejected_would_cross:
            return "rejected_would_cross";
        case K::rejected_stp:
            return "rejected_stp";
        case K::rejected_unknown:
            return "rejected_unknown";
        }
        return "?";
    }

    [[nodiscard]] std::string render_op(const SimOp& op) const {
        std::ostringstream s;
        s << op_kind_name(op.kind);
        switch (op.kind) {
        case SimOpKind::seed_external:
            s << " ref=" << op.ref.value << ' ' << to_wire(op.side) << " qty=" << op.qty.units
              << " @" << op.price.ticks;
            break;
        case SimOpKind::seed_iceberg:
            s << " ref=" << op.ref.value << ' ' << to_wire(op.side) << " display=" << op.num
              << " total=" << op.num2 << " @" << op.price.ticks;
            break;
        case SimOpKind::set_stp:
            s << " mode=" << static_cast<int>(op.stp);
            break;
        case SimOpKind::submit:
            s << " ref=" << op.ref.value << ' ' << to_wire(op.side) << " qty=" << op.qty.units
              << " @" << op.price.ticks << " type=" << static_cast<int>(op.type)
              << " arrival=" << op.ts;
            break;
        case SimOpKind::run_until:
            s << " ts=" << op.ts;
            break;
        case SimOpKind::apply_external:
            s << ' ' << to_wire(op.side) << " @" << op.price.ticks << " qty=" << op.num;
            break;
        case SimOpKind::advance_time:
            s << " dt=" << op.ts;
            break;
        case SimOpKind::cancel_strategy:
            s << " ref=" << op.ref.value;
            break;
        case SimOpKind::replace_strategy:
            s << " orig=" << op.ref.value << " fresh=" << op.ref2.value << " qty=" << op.qty.units
              << " @" << op.price.ticks;
            break;
        case SimOpKind::drain:
            break;
        }
        return s.str();
    }

    SimConfig cfg_;
    // Engaged for the lifetime of the session; disengaged only inside
    // seek()'s rewind rebuild, because ExecutionSimulator is neither
    // movable nor assignable.
    std::optional<ExecutionSimulator> sim_;
    std::vector<SimOp> log_;
    std::size_t cursor_ = 0;
};

} // namespace mog
