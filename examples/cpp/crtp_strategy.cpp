// Minimal CRTP market-maker: quote around the mid through scripted flow.
#include <mog/Metrics.hpp>
#include <mog/Simulate.hpp>
#include <mog/Strategy.hpp>

#include <cstdio>

namespace {
using namespace mog;

class MarketMaker final : public Strategy<MarketMaker> {
public:
    static constexpr std::int64_t kHalfSpread = 2;
    static constexpr std::int64_t kSize = 10;

    void bind(StrategyRunner<MarketMaker>& r) {
        runner_ = &r;
        next_ref_ = r.sim().config().external_ref_limit + 1000;
    }

    void on_order_book_update(const BookUpdate& u) {
        if (u.bid_px == kNoTick || u.ask_px == kNoTick)
            return;
        const std::int64_t mid = (u.bid_px + u.ask_px) / 2;
        requote(mid - kHalfSpread, mid + kHalfSpread);
    }
    void on_order_ack(const OrderAck& a) {
        // Aggressive executions report exact notional through acks.
        if (a.filled > 0 && (a.kind == 1 || a.kind == 2))
            acct_.on_aggressive_fill(a.notional, a.buy ? static_cast<std::int64_t>(a.filled)
                                                       : -static_cast<std::int64_t>(a.filled));
    }
    void on_order_fill(const OrderFill& f) {
        acct_.on_fill(f.px,
                      f.buy ? static_cast<std::int64_t>(f.qty) : -static_cast<std::int64_t>(f.qty));
        ++fills_;
    }
    // Where our bid currently rests; the scenario aims flow at this level.
    [[nodiscard]] std::int64_t last_bid_px() const noexcept { return last_bid_px_; }
    [[nodiscard]] const Account& account() const noexcept { return acct_; }
    [[nodiscard]] std::uint64_t fill_count() const noexcept { return fills_; }

private:
    void requote(std::int64_t bid_px, std::int64_t ask_px) {
        for (const std::uint64_t ref : {bid_ref_, ask_ref_})
            if (ref != 0)
                static_cast<void>(runner_->sim().cancel_strategy(OrderId{ref}));
        SimInbound b{};
        b.ref = OrderId{next_ref_++};
        b.side = Side::buy;
        b.qty = Qty{kSize};
        b.price = Price{bid_px};
        bid_ref_ = b.ref.value;
        last_bid_px_ = bid_px;
        static_cast<void>(runner_->submit(b));

        SimInbound a{};
        a.ref = OrderId{next_ref_++};
        a.side = Side::sell;
        a.qty = Qty{kSize};
        a.price = Price{ask_px};
        ask_ref_ = a.ref.value;
        static_cast<void>(runner_->submit(a));
    }

    StrategyRunner<MarketMaker>* runner_ = nullptr;
    Account acct_{};
    std::uint64_t next_ref_ = 0;
    std::uint64_t bid_ref_ = 0;
    std::uint64_t ask_ref_ = 0;
    std::uint64_t fills_ = 0;
    std::int64_t last_bid_px_ = 0;
};

} // namespace

int main() {
    SimConfig cfg{};
    cfg.book.arena_capacity = 4096;
    cfg.book.ladder = {0, 4000, 8};
    cfg.event_capacity = 4096;
    cfg.parse_latency_ns = 200;
    cfg.decision_latency_ns = 300;
    cfg.wire_latency_ns = 150;
    cfg.jitter_kind = JitterKind::none;

    StrategyRunner<MarketMaker> runner(cfg, MarketMaker{});
    MarketMaker& mm = runner.strategy();
    mm.bind(runner);
    ExecutionSimulator& sim = runner.sim();

    // Seed: bids 990x50, 985x50; asks 1010x50, 1016x50.
    static_cast<void>(sim.seed_external(OrderId{100}, Side::buy, Qty{50}, Price{990}));
    static_cast<void>(sim.seed_external(OrderId{101}, Side::buy, Qty{50}, Price{985}));
    static_cast<void>(sim.seed_external(OrderId{200}, Side::sell, Qty{50}, Price{1010}));
    static_cast<void>(sim.seed_external(OrderId{201}, Side::sell, Qty{50}, Price{1016}));

    // Phase A: aggressive market buy crosses the 1010 offer.
    SimInbound mkt{};
    mkt.ref = OrderId{sim.config().external_ref_limit + 7};
    mkt.side = Side::buy;
    mkt.qty = Qty{30};
    mkt.type = SimOrderType::market;
    static_cast<void>(runner.submit(mkt));
    runner.advance_time(500);

    // Phase B: more sellers rest on 1010.
    sim.apply_external(Side::sell, Price{1010}, 20);
    runner.advance_time(500);

    // Settle window: the requotes triggered along the way finish resting
    // here (decision 300ns + wire 150ns stack behind each book update).
    runner.advance_time(500);

    // Phase C: selling pressure works the bid side where our quote rests.
    sim.apply_external(Side::buy, Price{mm.last_bid_px()}, 10);
    runner.advance_time(500);

    // Settle and report.
    sim.drain();
    char hex[65];
    const auto dg = Sha256::hex(sim.trace_digest(), hex);
    std::printf("inventory=%lld cash=%lld fills=%llu trace_digest=%.*s\n",
                static_cast<long long>(mm.account().inventory()),
                static_cast<long long>(mm.account().cash()),
                static_cast<unsigned long long>(mm.fill_count()), static_cast<int>(dg.size()),
                dg.data());
    return 0;
}
