// E2E test for PMU hardware performance counters.
#include <mog/Contracts.hpp>
#include <mog/ITCHParser.hpp>
#include <mog/OrderBook.hpp>
#include <mog/Pmu.hpp>

#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace {

#define CHECK(expr)                                                                                \
    do {                                                                                           \
        if (!(expr)) {                                                                             \
            std::fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, #expr);        \
            std::exit(1);                                                                          \
        }                                                                                          \
    } while (0)

void test_pmu_collector() {
    mog::pmu::PmuCollector collector;

    mog::pmu::PmuStats stats{};
    {
        mog::pmu::ScopedPmu pmu(collector, stats);

        mog::OrderBook<> book(mog::OrderBook<>::Config{
            .arena_capacity = 1024,
            .ladder = {.lo_tick = 0, .hi_tick = 1'000'000, .page_pool = 256},
        });

        for (std::uint64_t i = 1; i <= 100; ++i) {
            static_cast<void>(book.add(mog::OrderId{i}, mog::Side::buy, mog::Qty{100},
                                       mog::Price{static_cast<std::int64_t>(1000 + (i % 50))}));
        }
        for (std::uint64_t i = 1; i <= 100; ++i) {
            static_cast<void>(book.remove(mog::OrderId{i}));
        }
    }

    if (collector.is_available()) {
        CHECK(stats.instructions > 0);
        CHECK(stats.cycles > 0);
        CHECK(stats.ipc() > 0.0);
        std::printf("PMU active: %lu instructions, %lu cycles, IPC=%.2f, %lu L1D misses\n",
                    static_cast<unsigned long>(stats.instructions),
                    static_cast<unsigned long>(stats.cycles), stats.ipc(),
                    static_cast<unsigned long>(stats.l1d_misses));
    } else {
        std::printf("PMU hardware counters not available in current container/privilege mode\n");
    }
}

} // namespace

int main() {
    test_pmu_collector();
    return 0;
}
