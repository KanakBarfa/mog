// E2E test for ParallelGridRunner multi-threaded sweeps and pegged order types.
#include <mog/Contracts.hpp>
#include <mog/GridRunner.hpp>
#include <mog/Simulate.hpp>

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {

#define CHECK(expr)                                                                                \
    do {                                                                                           \
        if (!(expr)) {                                                                             \
            std::fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, #expr);        \
            std::exit(1);                                                                          \
        }                                                                                          \
    } while (0)

void test_pegged_order_types() {
    mog::SimConfig cfg{};
    cfg.book.ladder.lo_tick = 0;
    cfg.book.ladder.hi_tick = 1'000'000;
    cfg.external_ref_limit = 1'000'000;
    mog::ExecutionSimulator sim(cfg);

    // Seed market depth: Best Bid = 10000, Best Ask = 10020 (Mid = 10010)
    static_cast<void>(
        sim.seed_external(mog::OrderId{100}, mog::Side::buy, mog::Qty{100}, mog::Price{10000}));
    static_cast<void>(
        sim.seed_external(mog::OrderId{101}, mog::Side::sell, mog::Qty{100}, mog::Price{10020}));

    // Submit Midpoint Peg buy order
    mog::SimInbound mid_order{
        .ref = mog::OrderId{2'000'001},
        .side = mog::Side::buy,
        .qty = mog::Qty{50},
        .type = mog::SimOrderType::midpoint_peg,
    };
    static_cast<void>(sim.submit(mid_order, 1000));
    sim.drain();

    const auto& decs = sim.decisions();
    CHECK(decs.size() == 1);
    CHECK(decs[0].kind == mog::SimDecision::Kind::rested);
    CHECK(sim.hidden_count() == 1);
    CHECK(sim.hidden_qty_at(mog::Side::buy, mog::Price{10010}) == 50);

    // External seller hits bid at 10010 (crosses midpoint peg)
    sim.apply_external(mog::Side::buy, mog::Price{10010}, 50);
    CHECK(sim.hidden_qty_at(mog::Side::buy, mog::Price{10010}) == 0);
}

void test_parallel_grid_runner() {
    mog::ParallelGridRunner runner(4);
    CHECK(runner.concurrency() >= 1);

    std::vector<mog::GridTask> tasks;
    for (std::size_t i = 0; i < 16; ++i) {
        mog::SimConfig cfg{};
        cfg.book.ladder.lo_tick = 0;
        cfg.book.ladder.hi_tick = 1'000'000;
        cfg.external_ref_limit = 1'000'000;
        cfg.maker_fee_bps = static_cast<std::int64_t>(i);
        tasks.push_back(mog::GridTask{i, cfg});
    }

    const auto results = runner.run_grid(tasks, [](const mog::GridTask& task) {
        mog::ExecutionSimulator sim(task.config);
        static_cast<void>(
            sim.seed_external(mog::OrderId{10}, mog::Side::buy, mog::Qty{100}, mog::Price{1000}));
        static_cast<void>(
            sim.seed_external(mog::OrderId{11}, mog::Side::sell, mog::Qty{100}, mog::Price{1010}));

        mog::SimInbound in{
            .ref = mog::OrderId{2'000'000 + task.task_id},
            .side = mog::Side::buy,
            .qty = mog::Qty{50},
            .price = mog::Price{1010},
            .type = mog::SimOrderType::day_limit,
        };
        static_cast<void>(sim.submit(in, 100));
        sim.drain();

        const auto fast_dig = sim.fast_trace_digest();
        return mog::GridResult{
            .task_id = task.task_id,
            .digest = mog::wire::load_be64(fast_dig.data()),
            .total_fills = sim.total_fill_count(),
            .total_filled_shares = 50,
            .total_filled_notional = static_cast<std::int64_t>(sim.total_filled_notional()),
            .total_fees = sim.total_fees(),
        };
    });

    CHECK(results.size() == 16);
    for (std::size_t i = 0; i < 16; ++i) {
        CHECK(results[i].task_id == i);
        CHECK(results[i].total_fills == 1);
        CHECK(results[i].total_filled_shares == 50);
        CHECK(results[i].total_filled_notional == 50500);
    }
}

} // namespace

int main() {
    test_pegged_order_types();
    test_parallel_grid_runner();
    return 0;
}
