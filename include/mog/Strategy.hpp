// Strategy API: CRTP hooks with static dispatch, driven by StrategyRunner.
//
// A strategy derives from Strategy<Derived> and overrides any subset of:
//   on_order_book_update(const BookUpdate&)
//   on_trade(const TradePrint&)
//   on_order_ack(const OrderAck&)
//   on_order_fill(const OrderFill&)
// Unoverridden hooks fall back to no-ops. There is no virtual dispatch on
// the hot path; the runner reaches the override through the derived object.
#pragma once

#include <mog/Contracts.hpp>
#include <mog/Reflect.hpp>
#include <mog/Simulate.hpp>

#include <algorithm>
#include <cstdint>
#include <vector>

namespace mog {

// Telemetry record types. Every one is Reflected, so the same declaration
// feeds the hooks and the columnar log schema. Trades reuse the simulator's
// own reflected SimTrade record - one type end to end.

struct BookUpdate {
    std::uint64_t ts = 0;
    std::int64_t bid_px = 0;
    std::uint64_t bid_qty = 0;
    std::int64_t ask_px = 0;
    std::uint64_t ask_qty = 0;
    static constexpr auto mog_fields =
        std::tuple{field("ts", &BookUpdate::ts), field("bid_px", &BookUpdate::bid_px),
                   field("bid_qty", &BookUpdate::bid_qty), field("ask_px", &BookUpdate::ask_px),
                   field("ask_qty", &BookUpdate::ask_qty)};
};

// Ack of a submitted order's decision outcome.
struct OrderAck {
    std::uint64_t ts = 0;
    std::uint64_t seq = 0;
    std::uint64_t ref = 0;
    std::uint32_t filled = 0;
    std::uint32_t cancelled = 0;
    std::uint64_t notional = 0; // exact price*qty over the aggressive leg
    std::int64_t fee = 0;       // taker settlement, signed cash impact
    std::uint8_t kind = 0;      // SimDecision::Kind
    std::uint8_t buy = 0;
    static constexpr auto mog_fields = std::tuple{field("ts", &OrderAck::ts),
                                                  field("seq", &OrderAck::seq),
                                                  field("ref", &OrderAck::ref),
                                                  field("filled", &OrderAck::filled),
                                                  field("cancelled", &OrderAck::cancelled),
                                                  field("notional", &OrderAck::notional),
                                                  field("fee", &OrderAck::fee),
                                                  field("kind", &OrderAck::kind),
                                                  field("buy", &OrderAck::buy)};
};

struct OrderFill {
    std::uint64_t ts = 0;
    std::uint64_t seq = 0;
    std::uint64_t ref = 0;
    std::int64_t px = 0;
    std::uint32_t qty = 0;
    std::int64_t fee = 0; // maker settlement, signed cash impact (rebate < 0)
    std::uint8_t buy = 0; // 1 when the filled order was a buy
    static constexpr auto mog_fields = std::tuple{
        field("ts", &OrderFill::ts),  field("seq", &OrderFill::seq), field("ref", &OrderFill::ref),
        field("px", &OrderFill::px),  field("qty", &OrderFill::qty), field("fee", &OrderFill::fee),
        field("buy", &OrderFill::buy)};
};

using TradePrint = SimTrade;

template <class Derived>
class Strategy {
public:
    void on_order_book_update(const BookUpdate&) noexcept {}
    void on_trade(const TradePrint&) noexcept {}
    void on_order_ack(const OrderAck&) noexcept {}
    void on_order_fill(const OrderFill&) noexcept {}

protected:
    [[nodiscard]] Derived& self() noexcept { return static_cast<Derived&>(*this); }
    [[nodiscard]] const Derived& self() const noexcept {
        return static_cast<const Derived&>(*this);
    }
};

// Drives an ExecutionSimulator on behalf of a strategy and delivers events
// through the CRTP hooks in exact causal order. The simulator queues raw
// occurrences stamped with a global sequence counter; the runner merges them
// by sequence after each step, so hook delivery is a deterministic function
// of the simulation regardless of batching granularity.
template <class S>
class StrategyRunner {
public:
    StrategyRunner(SimConfig cfg, S strat) : sim_(cfg), strat_(std::move(strat)) {}

    [[nodiscard]] ExecutionSimulator& sim() noexcept { return sim_; }
    [[nodiscard]] const ExecutionSimulator& sim() const noexcept { return sim_; }
    [[nodiscard]] S& strategy() noexcept { return strat_; }
    [[nodiscard]] const S& strategy() const noexcept { return strat_; }
    [[nodiscard]] std::uint64_t now() const noexcept { return clock_; }

    // Submits an order at the current clock.
    std::uint64_t submit(const SimInbound& in) noexcept { return sim_.submit(in, clock_); }

    // Advances simulated time, drains decisions due within the window, and
    // pumps hooks in causal order.
    void advance_time(std::uint64_t dt_ns) {
        sim_.advance_time(dt_ns);
        clock_ += dt_ns;
        sim_.run_until(clock_);
        emit_book_update();
        drain_pump();
    }

    // Drains pending decisions without advancing external time.
    void run_until(std::uint64_t until_ts) {
        sim_.run_until(until_ts);
        emit_book_update();
        drain_pump();
    }

private:
    void emit_book_update() {
        const std::int64_t bid_px = sim_.book().best_bid();
        const std::int64_t ask_px = sim_.book().best_ask();
        const std::uint64_t bid_qty =
            bid_px == kNoTick
                ? 0
                : static_cast<std::uint64_t>(sim_.book().qty_at(Side::buy, Price{bid_px}));
        const std::uint64_t ask_qty =
            ask_px == kNoTick
                ? 0
                : static_cast<std::uint64_t>(sim_.book().qty_at(Side::sell, Price{ask_px}));
        if (bid_px == last_bid_ && ask_px == last_ask_ && bid_qty == last_bid_qty_ &&
            ask_qty == last_ask_qty_)
            return; // top of book unchanged: nothing to report
        last_bid_ = bid_px;
        last_ask_ = ask_px;
        last_bid_qty_ = bid_qty;
        last_ask_qty_ = ask_qty;
        BookUpdate u{};
        u.ts = clock_;
        u.bid_px = last_bid_;
        u.ask_px = last_ask_;
        u.bid_qty = last_bid_qty_;
        u.ask_qty = last_ask_qty_;
        strat_.on_order_book_update(u);
    }

    void drain_pump() {
        const auto& trades = sim_.trades();
        const auto& reports = sim_.reports();
        const auto& decisions = sim_.decisions();
        std::size_t it = 0, ir = 0, id = 0;
        const std::size_t nt = trades.size();
        const std::size_t nr = reports.size();
        const std::size_t nd = decisions.size();

        while (it < nt || ir < nr || id < nd) {
            const std::uint64_t st = it < nt ? trades[it].seq : UINT64_MAX;
            const std::uint64_t sr = ir < nr ? reports[ir].seq : UINT64_MAX;
            const std::uint64_t sd = id < nd ? decisions[id].seq : UINT64_MAX;

            if (st <= sr && st <= sd) {
                strat_.on_trade(trades[it]);
                ++it;
            } else if (sr <= sd) {
                const auto& r = reports[ir];
                strat_.on_order_fill(OrderFill{r.visible_ts, r.seq, r.ref, r.price_ticks, r.qty,
                                               r.fee,
                                               static_cast<std::uint8_t>(r.side == 'B' ? 1 : 0)});
                ++ir;
            } else {
                const auto& d = decisions[id];
                strat_.on_order_ack(OrderAck{d.visible_ts, d.seq, d.ref, d.filled_qty,
                                             d.cancelled_qty, d.filled_notional, d.fee,
                                             static_cast<std::uint8_t>(d.kind),
                                             static_cast<std::uint8_t>(d.side == 'B' ? 1 : 0)});
                ++id;
            }
        }
        sim_.clear_events();
    }

    ExecutionSimulator sim_;
    S strat_;
    std::uint64_t clock_ = 0;
    std::int64_t last_bid_ = kNoTick;
    std::int64_t last_ask_ = kNoTick;
    std::uint64_t last_bid_qty_ = 0;
    std::uint64_t last_ask_qty_ = 0;
};

} // namespace mog
