// Queue inspection: watch position in the book evolve as flow works a level.
#include <mog/Simulate.hpp>

#include <cstdio>

namespace {
using namespace mog;

void dump_l2(const OrderBook<>& b) {
    std::printf("  L2:");
    b.for_each_l2([&](Side s, std::int64_t tick, std::int64_t qty) {
        std::printf(" %c%lld x %lld", s == Side::buy ? 'B' : 'S', static_cast<long long>(tick),
                    static_cast<long long>(qty));
    });
    std::printf("\n");
}

} // namespace

int main() {
    SimConfig cfg{};
    cfg.book.arena_capacity = 4096;
    cfg.book.ladder = {0, 4000, 8};
    cfg.event_capacity = 4096;

    ExecutionSimulator sim(cfg);

    // External liquidity: bid side stacked at 998/996, offer at 1010.
    static_cast<void>(sim.seed_external(OrderId{1}, Side::buy, Qty{100}, Price{998}));
    static_cast<void>(sim.seed_external(OrderId{2}, Side::buy, Qty{200}, Price{996}));
    static_cast<void>(sim.seed_external(OrderId{3}, Side::sell, Qty{150}, Price{1010}));

    // Our passive bid joins the BACK of the 998 level.
    const std::uint64_t ref = (std::uint64_t{1} << 62) + 500;
    const SimInbound order{OrderId{ref}, Side::buy, Qty{10}, Price{998}};
    static_cast<void>(sim.submit(order, /*arrival_ts=*/100));
    sim.drain();
    std::printf("after join: queue_ahead_of=%lld\n",
                static_cast<long long>(sim.queue_ahead_of(OrderId{ref})));
    dump_l2(sim.book());

    // Selling pressure consumes 60 of the 100 shares queued ahead of us.
    sim.apply_external(Side::buy, Price{998}, 60);
    sim.drain();
    std::printf("after first sweep: queue_ahead_of=%lld\n",
                static_cast<long long>(sim.queue_ahead_of(OrderId{ref})));
    dump_l2(sim.book());

    // The next sweep takes the rest of the level, including our order.
    sim.apply_external(Side::buy, Price{998}, 60);
    sim.drain();
    for (const auto& f : sim.reports())
        std::printf("fill: ref=%llu qty=%lld px=%lld visible_ts=%llu\n",
                    static_cast<unsigned long long>(f.ref), static_cast<long long>(f.qty),
                    static_cast<long long>(f.price_ticks),
                    static_cast<unsigned long long>(f.visible_ts));
    return 0;
}
