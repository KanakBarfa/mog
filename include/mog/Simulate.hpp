// Execution simulator over OrderBook: order types, self-trade prevention,
// exact FIFO queue tracking under seeded depletion, latency pipeline.
// See docs/FILLMODEL.md for the queue arithmetic.
#pragma once

#include <mog/OrderBook.hpp>
#include <mog/Reflect.hpp>
#include <mog/Scheduler.hpp>
#include <mog/Sha256.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <random>
#include <tuple>
#include <vector>

namespace mog {

enum class SimOrderType : std::uint8_t { day_limit = 0, market = 1, ioc = 2, post_only = 3 };

// Outbound latency jitter shape. Exponential and normal modes use
// jitter_mean_ns / jitter_sigma_ns and are truncated at 8x the mean so
// hand-built expectations stay bounded.
enum class JitterKind : std::uint8_t { none = 0, uniform = 1, exponential = 2, normal = 3 };

// Self-trade prevention applied per potential match against a tracked victim.
enum class StpMode : std::uint8_t {
    none = 0,          // self-match allowed
    cancel_newest = 1, // incoming order stops matching; remainder cancelled
    cancel_oldest = 2, // resting victim removed; incoming keeps matching
    decrement = 3,     // both sides shrink by the would-be match
};

struct SimConfig {
    // Defaults here, not in the nested aggregate: an indeterminate config
    // once crashed innocent `SimConfig cfg;` lines.
    typename OrderBook<>::Config book{1u << 20, {0, 12'000'000, 4096}};
    // External depletion intensities in executed units per simulated
    // microsecond at each touch. Direction bias in [-1, 1] tilts the split:
    // bid_share = (1 + bias) / 2.
    double bid_depletion_per_us = 0.0;
    double ask_depletion_per_us = 0.0;
    std::uint64_t seed = 1;
    // Self-excitation (Hawkes-style): every executed consumption event adds
    // kappa to an excitement state that decays exponentially over tau ns.
    // 0 disables; intensities scale by (1 + excitement).
    double hawkes_kappa = 0.0;
    std::uint64_t hawkes_decay_ns = 1000000;
    // Momentum coupling: the direction of the last mid-price move within
    // momentum_memory_ns tilts the bid/ask intensity split by up to
    // momentum_gain (clamped so shares stay in [0,1]).
    double momentum_gain = 0.0;
    std::uint64_t momentum_memory_ns = 500000000;
    // Latency stages in nanoseconds (wire -> parse -> decision -> wire).
    std::uint32_t parse_latency_ns = 0;
    std::uint32_t decision_latency_ns = 0;
    std::uint32_t wire_latency_ns = 0;
    std::uint32_t jitter_max_ns = 0; // bound for the uniform shape
    JitterKind jitter_kind = JitterKind::uniform;
    double jitter_mean_ns = 0.0;  // exponential mean / normal center
    double jitter_sigma_ns = 1.0; // normal width
    // Refs below this value belong to external/synthetic liquidity.
    std::uint64_t external_ref_limit = std::uint64_t{1} << 62;
    // Scheduler capacity for in-flight decisions.
    std::size_t event_capacity = std::size_t{1} << 20;
    // Fee schedule in signed basis points of notional (docs/FILLMODEL.md).
    // Taker charges on the aggressive leg (typically >= 0); maker applies
    // per passive fill, typically negative for rebates. Zero rates are the
    // default and leave every number bit-identical to the fee-free engine.
    std::int64_t maker_fee_bps = 0;
    std::int64_t taker_fee_bps = 0;
};

// Signed basis points of notional, truncated toward zero (integer ticks;
// the rounding rule is part of the fee contract - see docs/FILLMODEL.md).
// A negative rate yields a negative result, i.e. a cash credit (rebate).
[[nodiscard]] inline std::int64_t fee_bps_of(std::uint64_t notional_ticks,
                                             std::int64_t bps) noexcept {
    const std::uint64_t mag =
        notional_ticks *
        (bps < 0 ? static_cast<std::uint64_t>(-bps) : static_cast<std::uint64_t>(bps)) / 10000ULL;
    return bps < 0 ? -static_cast<std::int64_t>(mag) : static_cast<std::int64_t>(mag);
}

struct SimFillReport {
    std::uint64_t visible_ts;
    std::uint64_t ref;
    std::int64_t price_ticks;
    std::uint32_t qty;
    std::uint64_t seq = 0; // global causal order; not part of the trace hash
    char side = 'B';       // resting side of the filled order
    std::int64_t fee = 0;  // maker settlement: signed cash impact of this fill
};

// One observed external execution against resting depth. Aggressor side is
// inferred from which side of the book lost quantity.
struct SimTrade {
    std::uint64_t ts;
    std::uint64_t seq = 0;
    std::int64_t px_ticks;
    std::uint32_t qty;
    std::uint8_t aggressor_buy;
    static constexpr auto mog_fields = std::tuple{
        field("ts", &SimTrade::ts), field("seq", &SimTrade::seq), field("px", &SimTrade::px_ticks),
        field("qty", &SimTrade::qty), field("aggressor_buy", &SimTrade::aggressor_buy)};
};

struct SimDecision {
    enum class Kind : std::uint8_t {
        rested = 0,
        filled = 1, // fully filled, nothing rests
        partial_then_rest = 2,
        partial_then_cancelled = 3, // ioc / market remainder
        rejected_would_cross = 4,   // post-only
        rejected_stp = 5,           // stp cancel-newest path consumed the order
        rejected_unknown = 6,
    };
    Kind kind = Kind::rejected_unknown;
    std::uint32_t filled_qty = 0;
    std::uint32_t cancelled_qty = 0;
    // Sum of price*qty over the aggressive leg; exact integer ticks so
    // accounting from acks alone loses nothing to averaging.
    std::uint64_t filled_notional = 0;
    std::uint64_t visible_ts = 0;
    std::uint64_t decision_ts = 0; // scheduler stamp before outbound latency
    std::uint64_t ref = 0;
    char side = 'B';       // resting/inbound side of the order
    std::uint64_t seq = 0; // global causal order; not part of the trace hash
    std::int64_t fee = 0;  // taker settlement on the aggressive leg (signed)
};

// Peg reference for non-displayed orders (G8). NASDAQ's midpoint and pegged
// facilities are non-displayable BY RULE, so a pegged order never enters the
// displayed book: it rests in the simulator's hidden ledger, reprices to its
// reference on every subsequent engine event (automatic repegging keeps time
// priority at the new price, matching exchange behavior), and executes only
// when flow crosses it.
enum class Peg : std::uint8_t { none = 0, mid = 1, bid = 2, ask = 3 };

struct SimInbound {
    OrderId ref{};
    Side side{};
    Qty qty{};
    Price price{}; // ignored for market and for pegged orders pre-resolution
    SimOrderType type = SimOrderType::day_limit;
    Peg peg = Peg::none;
    std::int64_t peg_offset_ticks = 0;
    bool non_displayed = false; // odd-lot style: static hidden order
};

// External iceberg registration: `display` rests in the book; when a display
// slice is consumed to empty and hidden quantity remains, a fresh slice of
// `display` re-enters at the tail of the level, exactly as exchange
// replenishment behaves under price-time priority.
struct IcebergSpec {
    OrderId ref{};
    Side side{};
    Price price{};
    std::int64_t display = 0;
    std::int64_t total = 0;
};

// One event kind in the scheduler: an inbound order reaching its decision
// point. Fills are stamped with outbound latency but processed inline so the
// pop stream stays a pure function of submission order.
struct SimEvent {
    std::uint64_t visible_ts;
    SimInbound order;
};

class ExecutionSimulator {
public:
    using BookT = OrderBook<>;

    explicit ExecutionSimulator(SimConfig cfg)
        : cfg_(cfg), book_(cfg.book), wheel_(cfg.event_capacity), tracked_(cfg.book.arena_capacity),
          rng_(cfg.seed) {}

    [[nodiscard]] const BookT& book() const noexcept { return book_; }
    [[nodiscard]] const SimConfig& config() const noexcept { return cfg_; }
    // Seeds one external resting order; refs below external_ref_limit.
    [[nodiscard]] BookTick seed_external(OrderId ref, Side s, Qty qty, Price price) noexcept {
        MOG_PRE(ref.value < cfg_.external_ref_limit);
        return book_.add(ref, s, qty, price);
    }

    // Seeds an external iceberg: display rests now, hidden reserves replenish
    // the tail as slices are consumed.
    [[nodiscard]] BookTick seed_iceberg(const IcebergSpec& spec) noexcept {
        MOG_PRE(spec.ref.value < cfg_.external_ref_limit);
        MOG_PRE(spec.display > 0 && spec.total >= spec.display);
        const Side s = spec.side;
        const BookTick t = book_.add(spec.ref, s, Qty{spec.display}, spec.price);
        if (!ok(t))
            return t;
        icebergs_.push_back(
            Iceberg{spec.ref.value, s, spec.price.ticks, spec.display, spec.total - spec.display});
        return t;
    }

    [[nodiscard]] std::int64_t iceberg_hidden(OrderId ref) const noexcept {
        const auto i = iceberg_index_of(ref.value);
        return i == kNullIndex ? -1 : icebergs_[i].hidden;
    }

    // Submits a strategy order; the decision lands after parse + decision +
    // uniform jitter latency. Returns the scheduled decision timestamp.
    std::uint64_t submit(const SimInbound& in, std::uint64_t arrival_ts) noexcept {
        MOG_PRE(in.ref.value >= cfg_.external_ref_limit);
        const std::uint64_t jitter = draw_jitter();
        const std::uint64_t decision_ts =
            arrival_ts + cfg_.parse_latency_ns + cfg_.decision_latency_ns + jitter;
        static_cast<void>(wheel_.push(decision_ts, SimEvent{decision_ts, in}));
        return decision_ts;
    }

    // Processes every decision scheduled at or before until_ts, in (ts, seq)
    // order. Returns the reports produced, stamped with wire-side latency.
    void run_until(std::uint64_t until_ts) {
        while (!wheel_.empty() && wheel_.peek_min_ts() <= until_ts) {
            const SimEvent ev = wheel_.pop_min();
            decide(ev.order, ev.visible_ts);
        }
    }

    // Drains everything scheduled.
    void drain() { run_until(~std::uint64_t{0}); }

    // Halt gating (G3): while halted, external prints are dropped, incoming
    // orders cannot cross (limits rest, IOC/market cancel in full), and the
    // statistical driver freezes - a halted book must not drift. Explicit
    // model choice: prints seen during halts are discarded, not queued for
    // replay at resume; auction mechanics are Session.hpp's cross records.
    void set_halted(bool h) noexcept { halted_ = h; }
    [[nodiscard]] bool halted() const noexcept { return halted_; }

    // Injects an observed external execution directly at an exact level;
    // the statistical driver advance_time() routes through here too. This is
    // what scenario suites use when expectations must be hand-computable.
    void apply_external(Side s, Price price, std::int64_t qty) {
        MOG_PRE(qty > 0);
        if (halted_)
            return;
        repeg_hidden();
        const std::int64_t avail = book_.qty_at(s, price);
        consume_level(s, price, std::min(qty, avail));
        // Exact-level semantics extend to non-displayed quantity: overflow
        // past the visible level consumes hidden orders at that same price.
        if (qty > avail) {
            std::uint64_t dummy_notional = 0;
            std::int64_t leftover = qty - avail;
            static_cast<void>(tap_hidden(s, price, leftover, dummy_notional));
        }
    }

    // Advances external time: draws Poisson depletion on both touches and
    // applies it through real FIFO-head executions. Deterministic in (seed,
    // call sequence).
    void advance_time(std::uint64_t dt_ns) {
        if (dt_ns == 0 || halted_)
            return; // frozen book: no depletion draw, no excitement decay
        decay_excitement(dt_ns);
        ext_now_ += dt_ns;
        now_hint_ = ext_now_;
        const double scale = 1.0 + excitement_;
        const auto total_now = [&](double base) {
            return base * scale * static_cast<double>(dt_ns) / 1000.0;
        };
        const double bias =
            clamp_bias(cfg_bias() + cfg_.momentum_gain * last_move_sign() * momentum_freshness());
        const double bid_share = (1.0 + bias) / 2.0;
        apply_depletion(
            Side::buy,
            poisson(total_now(cfg_.bid_depletion_per_us + cfg_.ask_depletion_per_us) * bid_share));
        apply_depletion(Side::sell,
                        poisson(total_now(cfg_.bid_depletion_per_us + cfg_.ask_depletion_per_us) *
                                (1.0 - bid_share)));
        static_cast<void>(check_conservation());
    }

    [[nodiscard]] double flow_excitement() const noexcept { return excitement_; }
    [[nodiscard]] double flow_momentum() const noexcept { return momentum_state_; }

    [[nodiscard]] bool cancel_strategy(OrderId ref) noexcept {
        const auto idx = tracked_index_of(ref);
        if (idx == kNullIndex) {
            // Hidden (non-displayed) orders live outside the book; their
            // cancel is a ledger removal.
            for (std::size_t i = 0; i < hidden_.size(); ++i)
                if (hidden_[i].ref == ref.value) {
                    hidden_.erase(hidden_.begin() + static_cast<std::ptrdiff_t>(i));
                    ++seq_counter_;
                    trace_hidden_cancel(ref.value);
                    return true;
                }
            return false;
        }
        const BookTick t = book_.remove(ref);
        if (!ok(t))
            return false;
        tracked_[idx].live = false;
        return true;
    }

    // Non-displayed resting quantity at an exact price (G8 queries).
    [[nodiscard]] std::int64_t hidden_qty_at(Side s, Price price) const noexcept {
        std::int64_t total = 0;
        for (const auto& h : hidden_)
            if (h.side == s && h.price_ticks == price.ticks)
                total += h.qty_remaining;
        return total;
    }
    [[nodiscard]] std::size_t hidden_count() const noexcept { return hidden_.size(); }

    [[nodiscard]] BookTick replace_strategy(OrderId orig, OrderId fresh, Qty qty,
                                            Price price) noexcept {
        MOG_PRE(fresh.value >= cfg_.external_ref_limit);
        const char side_c = book_.side_of(orig);
        const auto idx = tracked_index_of(orig);
        if (idx == kNullIndex) {
            for (std::size_t i = 0; i < hidden_.size(); ++i) {
                if (hidden_[i].ref == orig.value) {
                    const Side s = hidden_[i].side;
                    hidden_[i].ref = fresh.value;
                    hidden_[i].price_ticks = price.ticks;
                    hidden_[i].qty_remaining = qty.units;
                    ++seq_counter_;
                    trace_hidden_cancel(orig.value);
                    return BookTick{.price_ticks = price.ticks,
                                    .qty = static_cast<std::uint32_t>(qty.units),
                                    .kind = static_cast<std::uint8_t>(BookEventKind::replaced),
                                    .side = static_cast<std::uint8_t>(to_wire(s)),
                                    .error = 0,
                                    .reserved = 0};
                }
            }
            return BookTick{};
        }
        if (side_c != 'B' && side_c != 'S')
            return BookTick{};
        inplace_vector<LevelDelta, 3>* no_deltas = nullptr;
        const BookTick t = book_.replace(orig, fresh, qty, price, no_deltas);
        if (!ok(t))
            return t;
        tracked_[idx].live = false;
        // A replace re-enters at the tail of the new level's queue.
        track_resting(fresh, side_from_wire(side_c), qty, price);
        return t;
    }

    [[nodiscard]] std::size_t pending_decisions() const noexcept { return wheel_.size(); }

    // Exact modeled units queued ahead of a strategy order at its level;
    // -1 when the ref is not a live tracked order.
    [[nodiscard]] std::int64_t queue_ahead_of(OrderId ref) const noexcept {
        const auto idx = tracked_index_of(ref);
        return idx == kNullIndex ? -1 : tracked_[idx].qty_ahead;
    }

    [[nodiscard]] const std::vector<SimFillReport>& reports() const noexcept { return reports_; }
    [[nodiscard]] const std::vector<SimTrade>& trades() const noexcept { return trades_; }
    void clear_reports() noexcept { reports_.clear(); }
    // Clears every queued event (fills, decisions, trade prints); used by
    // StrategyRunner after pumping hooks.
    void clear_events() noexcept {
        reports_.clear();
        decisions_.clear();
        trades_.clear();
    }

    // Divergence accounting: naive assumes the whole order fills instantly at
    // the touch price; the model fills what queue dynamics allow.
    [[nodiscard]] std::uint64_t naive_filled_total() const noexcept { return naive_filled_; }
    [[nodiscard]] std::uint64_t model_filled_total() const noexcept { return model_filled_; }
    // Fills from active crossing only; the remainder of model_filled_ is
    // passive (queue reached by external flow).
    [[nodiscard]] std::uint64_t aggressive_filled_total() const noexcept {
        return aggressive_filled_;
    }

    [[nodiscard]] bool audit() const noexcept {
        return book_.audit() && wheel_.audit() && check_conservation();
    }

private:
    struct Tracked {
        std::uint64_t ref = 0;
        std::int64_t price_ticks = 0;
        std::int64_t qty_remaining = 0; // mirrors book state
        std::int64_t qty_ahead = 0;     // exact units queued before us
        char side = 'B';
        bool live = false;
    };
    struct Iceberg {
        std::uint64_t ref;
        Side side;
        std::int64_t price_ticks;
        std::int64_t display;
        std::int64_t hidden;
    };

    [[nodiscard]] std::uint32_t iceberg_index_of(std::uint64_t ref) const noexcept {
        for (std::uint32_t i = 0; i < icebergs_.size(); ++i)
            if (icebergs_[i].ref == ref)
                return i;
        return kNullIndex;
    }

    [[nodiscard]] std::uint32_t tracked_index_of(OrderId ref) const noexcept {
        const auto h = book_.find_handle(ref);
        if (h.index == kNullIndex)
            return kNullIndex;
        return tracked_[h.index].live ? h.index : kNullIndex;
    }

    void track_resting(OrderId ref, Side s, Qty qty, Price price) noexcept {
        const auto h = book_.find_handle(ref);
        MOG_CONTRACT_ASSERT(h.index != kNullIndex);
        Tracked& t = tracked_[h.index];
        t.ref = ref.value;
        t.side = to_wire(s);
        t.price_ticks = price.ticks;
        t.qty_remaining = qty.units;
        t.live = true;
        live_tracked_.push_back(static_cast<std::uint32_t>(h.index));
        recompute_positions(s, price);
    }

    [[nodiscard]] std::uint64_t draw_jitter() noexcept {
        switch (cfg_.jitter_kind) {
        case JitterKind::none:
            return 0;
        case JitterKind::exponential: {
            if (cfg_.jitter_mean_ns <= 0.0)
                return 0;
            std::exponential_distribution<double> d(1.0 / cfg_.jitter_mean_ns);
            return static_cast<std::uint64_t>(std::min(d(rng_), 8.0 * cfg_.jitter_mean_ns));
        }
        case JitterKind::normal: {
            const double m = cfg_.jitter_mean_ns;
            if (m <= 0.0)
                return 0;
            std::normal_distribution<double> d(m, cfg_.jitter_sigma_ns);
            return static_cast<std::uint64_t>(std::max(0.0, std::min(d(rng_), 8.0 * m)));
        }
        case JitterKind::uniform:
        default: {
            if (cfg_.jitter_max_ns == 0)
                return 0;
            std::uniform_int_distribution<std::uint64_t> d(0, cfg_.jitter_max_ns);
            return d(rng_);
        }
        }
    }

    [[nodiscard]] std::uint64_t poisson(double lambda) noexcept {
        if (lambda <= 0.0)
            return 0;
        if (lambda < 30.0) {
            std::uniform_real_distribution<double> u(0.0, 1.0);
            double l = std::exp(-lambda), p = 1.0;
            std::uint64_t k = 0;
            do {
                ++k;
                p *= u(rng_);
            } while (p > l && k < 1000000);
            return k - 1;
        }
        std::poisson_distribution<std::uint64_t> d(lambda);
        return d(rng_);
    }

    void apply_depletion(Side s, std::uint64_t qty) {
        if (qty == 0)
            return;
        const std::int64_t touch = s == Side::buy ? book_.best_bid() : book_.best_ask();
        if (touch == kNoTick)
            return;
        const std::int64_t avail = book_.qty_at(s, Price{touch});
        const std::int64_t applied = std::min<std::int64_t>(static_cast<std::int64_t>(qty), avail);
        if (applied <= 0)
            return;
        consume_level(s, Price{touch}, applied);
    }

    // Doubled mid (bid + ask) so one-tick moves on odd sums still register;
    // only sign comparisons use it.
    [[nodiscard]] std::int64_t mid_price() const noexcept {
        const std::int64_t b = book_.best_bid(), a = book_.best_ask();
        if (b == kNoTick)
            return a == kNoTick ? 0 : 2 * a;
        if (a == kNoTick)
            return 2 * b;
        return b + a;
    }

    void excite() noexcept { excitement_ += cfg_.hawkes_kappa; }

    void decay_excitement(std::uint64_t dt_ns) noexcept {
        if (excitement_ == 0.0 || cfg_.hawkes_decay_ns == 0)
            return;
        const double factor =
            std::exp(-static_cast<double>(dt_ns) / static_cast<double>(cfg_.hawkes_decay_ns));
        excitement_ *= factor;
        if (excitement_ < 1e-12)
            excitement_ = 0.0;
    }

    void update_momentum(std::int64_t mid_before) noexcept {
        const std::int64_t mid_after = mid_price();
        if (mid_after == mid_before)
            return;
        momentum_ts_ = ext_now_;
        momentum_state_ = mid_after > mid_before ? 1.0 : -1.0;
    }

    [[nodiscard]] double last_move_sign() const noexcept { return momentum_state_; }

    [[nodiscard]] double momentum_freshness() const noexcept {
        if (momentum_state_ == 0.0 || cfg_.momentum_memory_ns == 0 || ext_now_ <= momentum_ts_)
            return momentum_state_ == 0.0 ? 0.0 : 1.0;
        const std::uint64_t age = ext_now_ - momentum_ts_;
        if (age >= cfg_.momentum_memory_ns)
            return 0.0;
        return 1.0 - static_cast<double>(age) / static_cast<double>(cfg_.momentum_memory_ns);
    }

    [[nodiscard]] double cfg_bias() const noexcept {
        // Base directional tilt from asymmetric configured intensities.
        const double tb = cfg_.bid_depletion_per_us + cfg_.ask_depletion_per_us;
        if (tb <= 0.0)
            return 0.0;
        return (cfg_.bid_depletion_per_us - cfg_.ask_depletion_per_us) / tb;
    }

    [[nodiscard]] static double clamp_bias(double b) noexcept {
        return std::max(-0.999, std::min(0.999, b));
    }

    // Applies Q of external flow at an exact level. Real FIFO-head executions
    // are the sole source of fills; afterwards every tracked order at the
    // level has its queue estimate decayed by the units consumed, which never
    // invents quantity and cannot go negative.
    void consume_level(Side s, Price price, std::int64_t q) {
        const std::int64_t mid_before = mid_price();
        std::int64_t applied = 0;
        while (applied < q) {
            const auto head_ref = book_.front_ref(s, price);
            if (head_ref == 0)
                break;
            const OrderId head{head_ref};
            const std::int64_t take = std::min(q - applied, book_.remaining_of(head));
            MOG_CONTRACT_ASSERT(take > 0);
            apply_to_head(s, head, price, take);
            replenish_if_iceberg(head);
            applied += take;
        }
        // External executions excite the flow process and move the momentum
        // signal; strategy sweeps do not (they route via cross(), not here).
        if (applied > 0) {
            excite();
            update_momentum(mid_before);
            emit_trade(s, price, applied);
        }
        recompute_positions(s, price);
        static_cast<void>(check_conservation());
    }

    // Executes take against a specific head, mirrors the tracker, and reports
    // a visible fill when the victim is a strategy order. The tracker index
    // must be captured before the reduction: a fully-consumed order leaves
    // the id table during reduce(), so post-hoc lookups cannot see it.
    void reduce_and_sync(Side s, OrderId head, Price price, std::int64_t take,
                         bool via_execute_front) {
        const auto pre_idx = tracked_index_of(head);
        OrderId victim{};
        const BookTick tick = via_execute_front
                                  ? book_.execute_front(s, price, Qty{take}, nullptr, &victim)
                                  : book_.execute(head, Qty{take});
        MOG_CONTRACT_ASSERT(ok(tick));
        if (pre_idx == kNullIndex || victim.value != head.value)
            return;
        Tracked& t = tracked_[pre_idx];
        if (book_.find_handle(head).index == mog::kNullIndex) {
            t.live = false; // consumed to zero; the id entry is already gone
        } else {
            const std::int64_t rem = book_.remaining_of(head);
            t.qty_remaining = rem;
            t.qty_ahead = 0; // it was the FIFO head; nothing sits ahead of it
        }
        emit_fill(pre_idx, price.ticks, take, now_hint_);
    }

    // If the executed head was an iceberg display slice consumed to empty,
    // replenish min(display, hidden) at the tail of the same level. Cancelled
    // slices never replenish; only consumed ones do.
    void replenish_if_iceberg(OrderId head) {
        const auto idx = iceberg_index_of(head.value);
        if (idx == kNullIndex || !icebergs_[idx].hidden)
            return;
        Iceberg& ib = icebergs_[idx];
        if (book_.find_handle(head).index != mog::kNullIndex)
            return; // display not yet emptied; nothing to do
        const std::int64_t slice = std::min(ib.display, ib.hidden);
        MOG_CONTRACT_ASSERT(slice > 0);
        const BookTick t = book_.add(OrderId{ib.ref}, ib.side, Qty{slice}, Price{ib.price_ticks});
        MOG_CONTRACT_ASSERT(ok(t)); // ref free: previous slice was consumed
        ib.hidden -= slice;
        if (ib.hidden == 0)
            drop_iceberg(idx); // last slice resting as plain depth
    }

    void drop_iceberg(std::uint32_t idx) noexcept {
        icebergs_[idx] = icebergs_.back();
        icebergs_.pop_back();
    }

    void apply_to_head(Side s, OrderId head, Price price, std::int64_t take) {
        reduce_and_sync(s, head, price, take, /*via_execute_front*/ true);
    }

    // Recomputes exact queue positions for every tracked order at a level by
    // walking the real FIFO: units ahead of an order are precisely the
    // remaining quantities queued before it. No interleaving assumptions.
    void recompute_positions(Side s, Price price) noexcept {
        std::int64_t cum = 0;
        book_.for_each_in_level(s, price, [&](OrderId ref, std::int64_t rem) {
            const auto h = book_.find_handle(ref);
            if (h.index != mog::kNullIndex && tracked_[h.index].live) {
                tracked_[h.index].qty_ahead = cum;
                cum += rem;
            } else {
                cum += rem;
            }
        });
        // Tracked orders that left this level (fully filled) keep ahead == 0
        // semantics through sync; nothing else to do here.
    }

    // Marks a known-removed order's tracker dead without relying on id
    // lookup (the entry is gone by the time this runs).
    void sync_tracker_after_removal(OrderId victim) noexcept {
        const auto idx = tracked_index_of(victim);
        if (idx == kNullIndex)
            return;
        tracked_[idx].live = false;
    }

    void emit_fill(std::size_t tracked_idx, std::int64_t price, std::int64_t qty,
                   std::uint64_t base_ts) {
        const Tracked& t = tracked_[tracked_idx];
        const std::uint64_t vis = base_ts + cfg_.wire_latency_ns + draw_jitter();
        const auto notional = static_cast<std::uint64_t>(price) * static_cast<std::uint64_t>(qty);
        const std::int64_t fee = fee_bps_of(notional, cfg_.maker_fee_bps);
        reports_.push_back(SimFillReport{vis, t.ref, price, static_cast<std::uint32_t>(qty),
                                         ++seq_counter_, t.side, fee});
        model_filled_ += static_cast<std::uint64_t>(qty);
        trace_.update(&vis, sizeof(vis));
        const auto ref = t.ref;
        trace_.update(&ref, sizeof(ref));
        trace_.update(&price, sizeof(price));
        const auto q32 = static_cast<std::uint32_t>(qty);
        trace_.update(&q32, sizeof(q32));
    }

    [[nodiscard]] std::int64_t best_hidden_price(Side opp) const noexcept {
        std::int64_t best = kNoTick;
        for (const auto& h : hidden_) {
            if (h.side != opp || h.qty_remaining == 0)
                continue;
            if (best == kNoTick)
                best = h.price_ticks;
            else if (opp == Side::buy && h.price_ticks > best)
                best = h.price_ticks;
            else if (opp == Side::sell && h.price_ticks < best)
                best = h.price_ticks;
        }
        return best;
    }

    // Crosses an aggressive remainder against the opposite side, applying STP
    // per potential match. Returns units filled; sets stp_stop when the
    // incoming order must not continue (cancel-newest).
    std::int64_t cross(const SimInbound& in, std::int64_t remaining, bool& stp_stop,
                       std::uint64_t& notional_out) {
        stp_stop = false;
        std::int64_t filled = 0;
        std::uint64_t notional = 0;
        const bool buy = in.side == Side::buy;
        const Side opp = buy ? Side::sell : Side::buy;
        inplace_vector<Price, 8> touched{};
        while (remaining > 0) {
            const std::int64_t h_best = best_hidden_price(opp);
            const std::int64_t touch = buy ? book_.best_ask() : book_.best_bid();
            const bool h_better =
                h_best != kNoTick && (touch == kNoTick || (buy ? h_best < touch : h_best > touch));
            if (h_better) {
                if (in.type != SimOrderType::market &&
                    (buy ? h_best > in.price.ticks : h_best < in.price.ticks))
                    break;
                filled += tap_hidden(opp, Price{h_best}, remaining, notional);
                continue;
            }
            if (touch == kNoTick)
                break;
            if (in.type != SimOrderType::market &&
                (buy ? touch > in.price.ticks : touch < in.price.ticks))
                break; // price bound exhausted
            const auto head_ref = book_.front_ref(opp, Price{touch});
            if (head_ref == 0)
                break;
            const OrderId head{head_ref};
            const std::int64_t head_rem = book_.remaining_of(head);

            // STP applies per potential match against a tracked victim.
            if (tracked_index_of(head) != kNullIndex && stp_ != StpMode::none) {
                if (stp_ == StpMode::cancel_newest) {
                    stp_stop = true;
                    return filled;
                }
                if (stp_ == StpMode::cancel_oldest) {
                    static_cast<void>(book_.remove(head));
                    sync_tracker_after_removal(head);
                    recompute_positions(opp, Price{touch});
                    continue;
                }
                // decrement: both sides shrink by the mutual minimum.
                const std::int64_t d = std::min({head_rem, remaining});
                reduce_and_sync(opp, head, Price{touch}, d, /*via_execute_front*/ false);
                replenish_if_iceberg(head);
                notional += static_cast<std::uint64_t>(d) * static_cast<std::uint64_t>(touch);
                filled += d;
                remaining -= d;
                continue;
            }

            const std::int64_t take = std::min({remaining, head_rem});
            MOG_CONTRACT_ASSERT(take > 0);
            bool seen = false;
            for (const Price p : touched)
                if (p.ticks == touch)
                    seen = true;
            if (!seen && !touched.full())
                touched.push_back(Price{touch});
            apply_to_head(opp, head, Price{touch}, take);
            replenish_if_iceberg(head);
            notional += static_cast<std::uint64_t>(take) * static_cast<std::uint64_t>(touch);
            filled += take;
            remaining -= take;
            // Non-displayed liquidity at this exact price executes before
            // the sweep moves to a worse level; it never sets the touch.
            if (remaining > 0)
                filled += tap_hidden(opp, Price{touch}, remaining, notional);
        }
        // Exact queue arithmetic for every level this sweep touched.
        for (const Price p : touched)
            recompute_positions(opp, p);
        notional_out = notional;
        return filled;
    }

    void decide(const SimInbound& in0, std::uint64_t decision_ts) {
        now_hint_ = decision_ts;
        repeg_hidden();
        SimInbound in = in0;
        if (in.peg != Peg::none) {
            const std::int64_t px = hidden_reference(in.peg, in.peg_offset_ticks);
            if (px == kNoTick) { // reference side absent: reject cleanly
                const char wire_side0 = in.side == Side::buy ? 'B' : 'S';
                push_decision(SimDecision{SimDecision::Kind::rejected_unknown},
                              decision_ts + cfg_.wire_latency_ns + draw_jitter(), in.ref.value,
                              wire_side0);
                return;
            }
            in.price = Price{px};
        }
        const std::uint64_t vis = decision_ts + cfg_.wire_latency_ns + draw_jitter();
        const char wire_side = in.side == Side::buy ? 'B' : 'S';

        if (book_.find_handle(in.ref).index != kNullIndex) {
            push_decision(SimDecision{SimDecision::Kind::rejected_unknown}, vis, in.ref.value,
                          wire_side);
            return;
        }

        const bool buy = in.side == Side::buy;
        const std::int64_t touch = buy ? book_.best_ask() : book_.best_bid();
        // Market orders always sweep; every limit-flavored type crosses when
        // the touch is inside its bound - including post-only, whose crossing
        // means rejection rather than execution.
        const bool touches =
            touch != kNoTick && (buy ? touch <= in.price.ticks : touch >= in.price.ticks);
        const bool crosses =
            !halted_ &&
            (in.type == SimOrderType::market ||
             (touches && (in.type == SimOrderType::ioc || in.type == SimOrderType::day_limit ||
                          in.type == SimOrderType::post_only)));
        if (in.type == SimOrderType::post_only && crosses) {
            push_decision(SimDecision{SimDecision::Kind::rejected_would_cross}, vis, in.ref.value,
                          wire_side);
            return;
        }

        std::int64_t remaining = in.qty.units;
        SimDecision out{};
        out.decision_ts = decision_ts;
        if (crosses && remaining > 0) {
            bool stp_stop = false;
            std::uint64_t notional = 0;
            const std::int64_t got =
                cross(const_cast<SimInbound&>(in), remaining, stp_stop, notional);
            remaining -= got;
            out.filled_qty = static_cast<std::uint32_t>(got);
            out.filled_notional = notional;
            out.fee = fee_bps_of(notional, cfg_.taker_fee_bps);
            naive_filled_ += static_cast<std::uint64_t>(in.qty.units);
            model_filled_ += static_cast<std::uint64_t>(got);
            aggressive_filled_ += static_cast<std::uint64_t>(got);
            if (stp_stop) {
                out.kind = SimDecision::Kind::rejected_stp;
                out.cancelled_qty = static_cast<std::uint32_t>(remaining);
                push_decision(out, vis, in.ref.value, wire_side);
                return;
            }
        }

        const bool rests =
            (in.type == SimOrderType::day_limit || in.type == SimOrderType::post_only) &&
            remaining > 0;
        const bool goes_hidden = in.non_displayed || in.peg != Peg::none;
        if (rests && !goes_hidden) {
            const BookTick t = book_.add(in.ref, in.side, Qty{remaining}, in.price);
            if (!ok(t)) { // duplicate ref / band violation: exchange-style reject
                out.kind = SimDecision::Kind::rejected_unknown;
                push_decision(out, vis, in.ref.value, wire_side);
                return;
            }
            track_resting(in.ref, in.side, Qty{remaining}, in.price);
            out.kind =
                got_any(out) ? SimDecision::Kind::partial_then_rest : SimDecision::Kind::rested;
            push_decision(out, vis, in.ref.value, wire_side);
            return;
        }
        if (rests) { // non-displayed rest: hidden ledger, never the book
            hidden_.push_back(Hidden{in.ref.value, in.side, in.price.ticks, remaining, in.peg,
                                     in.peg_offset_ticks, in.peg != Peg::none});
            out.kind =
                got_any(out) ? SimDecision::Kind::partial_then_rest : SimDecision::Kind::rested;
            push_decision(out, vis, in.ref.value, wire_side);
            return;
        }
        if (remaining > 0)
            out.cancelled_qty = static_cast<std::uint32_t>(remaining);
        const bool fully = out.cancelled_qty == 0 && remaining == 0;
        out.kind = out.filled_qty > 0 && fully ? SimDecision::Kind::filled
                                               : SimDecision::Kind::partial_then_cancelled;
        push_decision(out, vis, in.ref.value, wire_side);
    }

    static bool got_any(const SimDecision& d) noexcept { return d.filled_qty > 0; }

    void push_decision(SimDecision d, std::uint64_t vis, std::uint64_t ref, char side) {
        d.ref = ref;
        d.visible_ts = vis;
        d.side = side;
        d.seq = ++seq_counter_;
        decisions_.push_back(d);
        trace_.update(&vis, sizeof(vis));
        trace_.update(&d.kind, sizeof(d.kind));
        trace_.update(&d.filled_qty, sizeof(d.filled_qty));
        trace_.update(&d.cancelled_qty, sizeof(d.cancelled_qty));
    }

    // Records an observed external execution for the telemetry layer. Pure
    // side output; the trace digest is untouched by design.
    void emit_trade(Side s, Price price, std::int64_t qty) {
        SimTrade t{now_hint_, ++seq_counter_, price.ticks, static_cast<std::uint32_t>(qty),
                   static_cast<std::uint8_t>(s == Side::sell ? 1 : 0)};
        trades_.push_back(t);
    }

    // Fill-conservation: tracked mirror must equal book truth for every live
    // tracked order. Returns falseness instead of asserting so test harnesses
    // can report context around the divergence.
    // Walks the compact live list rather than the arena-sized tracker array:
    // stale entries are pruned in passing, so cost tracks live orders, not
    // configured capacity. (The race harness caught the original O(arena)
    // scan - a 2.6 ms/op tax on every mutation.)
    [[nodiscard]] bool check_conservation() const noexcept {
        bool good = true;
        std::size_t w = 0;
        for (std::size_t r = 0; r < live_tracked_.size(); ++r) {
            const auto i = live_tracked_[r];
            const Tracked& t = tracked_[i];
            if (!t.live)
                continue;
            live_tracked_[w++] = i;
            const std::int64_t rem = book_.remaining_of(OrderId{t.ref});
            if (rem != t.qty_remaining || t.qty_ahead < 0) {
                good = false;
            }
        }
        live_tracked_.resize(w);
        return good;
    }

public:
    // STP policy is a run-wide setting; set before submitting.
    void set_stp_mode(StpMode m) noexcept { stp_ = m; }
    [[nodiscard]] StpMode stp_mode() const noexcept { return stp_; }
    [[nodiscard]] const std::vector<SimDecision>& decisions() const noexcept { return decisions_; }
    [[nodiscard]] std::array<unsigned char, 32> trace_digest() const noexcept {
        Sha256 copy = trace_;
        return copy.finish();
    }

private:
    SimConfig cfg_;
    BookT book_;
    Scheduler<SimEvent> wheel_;
    std::vector<Tracked> tracked_; // indexed by arena slot
    // Compact index of possibly-live tracker slots; pruned lazily by
    // check_conservation so audits never scan configured capacity.
    mutable std::vector<std::uint32_t> live_tracked_;
    std::vector<Iceberg> icebergs_;
    std::mt19937_64 rng_;
    bool halted_ = false;

    // ---- Non-displayed ledger (G8) --------------------------------------
    struct Hidden {
        std::uint64_t ref;
        Side side;
        std::int64_t price_ticks;
        std::int64_t qty_remaining;
        Peg peg;
        std::int64_t peg_offset;
        bool pegged; // peg!=none: reprices to reference on every event pass
    };
    std::vector<Hidden> hidden_;

    [[nodiscard]] std::int64_t hidden_reference(Peg peg, std::int64_t offset) const noexcept {
        const std::int64_t b = book_.best_bid();
        const std::int64_t a = book_.best_ask();
        switch (peg) {
        case Peg::bid:
            return b == kNoTick ? kNoTick : b + offset;
        case Peg::ask:
            return a == kNoTick ? kNoTick : a + offset;
        case Peg::mid:
            if (b == kNoTick || a == kNoTick)
                return kNoTick;
            return (b + a) / 2 + offset;
        case Peg::none:
            break;
        }
        return kNoTick;
    }

    // Repegs hidden orders to their current references. Preserves vector
    // order, i.e. time priority at the new price - matching exchange
    // automatic-repeg behavior.
    void repeg_hidden() noexcept {
        for (auto& h : hidden_) {
            if (!h.pegged)
                continue;
            const std::int64_t px = hidden_reference(h.peg, h.peg_offset);
            if (px != kNoTick)
                h.price_ticks = px;
        }
    }

    // Consumes hidden quantity on one side at one exact price, FIFO by
    // insertion. Returns quantity taken; updates notional and fills the
    // report ledger with maker-settled fills.
    std::int64_t tap_hidden(Side s, Price price, std::int64_t& remaining,
                            std::uint64_t& notional_out) {
        std::int64_t filled = 0;
        std::size_t i = 0;
        while (remaining > 0 && i < hidden_.size()) {
            Hidden& h = hidden_[i];
            if (h.side != s || h.price_ticks != price.ticks) {
                ++i;
                continue;
            }
            const std::int64_t take = std::min(remaining, h.qty_remaining);
            h.qty_remaining -= take;
            remaining -= take;
            filled += take;
            notional_out +=
                static_cast<std::uint64_t>(take) * static_cast<std::uint64_t>(price.ticks);
            trace_hidden_fill(h.ref, price.ticks, take);
            reports_.push_back(SimFillReport{now_hint_, h.ref, price.ticks,
                                             static_cast<std::uint32_t>(take), ++seq_counter_,
                                             s == Side::buy ? 'B' : 'S',
                                             fee_bps_of(static_cast<std::uint64_t>(take) *
                                                            static_cast<std::uint64_t>(price.ticks),
                                                        cfg_.maker_fee_bps)});
            model_filled_ += static_cast<std::uint64_t>(take);
            if (h.qty_remaining == 0)
                hidden_.erase(hidden_.begin() + static_cast<std::ptrdiff_t>(i));
            else
                ++i;
        }
        return filled;
    }

    void trace_hidden_fill(std::uint64_t ref, std::int64_t px, std::int64_t qty) noexcept {
        const std::uint64_t vis = now_hint_;
        trace_.update(&vis, sizeof(vis));
        trace_.update(&ref, sizeof(ref));
        trace_.update(&px, sizeof(px));
        trace_.update(&qty, sizeof(qty));
    }

    void trace_hidden_cancel(std::uint64_t ref) noexcept {
        const std::uint64_t vis = now_hint_;
        trace_.update(&vis, sizeof(vis));
        const char kind = static_cast<char>(0xC1); // hidden-cancel marker
        trace_.update(&kind, sizeof(kind));
        trace_.update(&ref, sizeof(ref));
    }

    std::vector<SimFillReport> reports_;
    std::vector<SimDecision> decisions_;
    Sha256 trace_;
    StpMode stp_ = StpMode::none;
    std::uint64_t naive_filled_ = 0;
    std::uint64_t model_filled_ = 0;
    std::uint64_t aggressive_filled_ = 0;
    std::uint64_t now_hint_ = 0;
    std::uint64_t ext_now_ = 0;
    std::uint64_t seq_counter_ = 0;
    std::vector<SimTrade> trades_;
    double excitement_ = 0.0;
    double momentum_state_ = 0.0;
    std::uint64_t momentum_ts_ = 0;
};

} // namespace mog
