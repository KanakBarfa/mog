// Book throughput benchmarks over a closed op script: the script starts and
// ends with an empty book, so iterations replay cleanly without rebuilding.

#include <mog/OrderBook.hpp>

#include <support/CorpusGen.hpp>

#include <benchmark/benchmark.h>
#include <cstdint>
#include <expected>
#include <unordered_map>
#include <vector>

namespace {

using mog::BookTick;
using mog::kTickError;
using mog::OrderId;
using mog::Price;
using mog::Qty;
using mog::Side;

constexpr std::size_t kArenaCapacity = 1 << 18;
constexpr std::int64_t kBandLo = 1'000'000;
constexpr std::int64_t kBandHi = 1'100'000;

enum class OpKind : std::uint8_t { add, execute, cancel, remove, replace };
struct Op {
    OpKind kind;
    std::uint64_t ref = 0;
    std::uint64_t ref2 = 0;
    std::int64_t qty = 0;
    std::int64_t price = 0;
};

[[nodiscard]] std::int64_t draw_price(std::uint64_t& rng) noexcept {
    if (mog::testing::next_splitmix(rng) % 10 < 7)
        return kBandLo + 49'744 + static_cast<std::int64_t>(mog::testing::next_splitmix(rng) % 512);
    return kBandLo +
           static_cast<std::int64_t>(mog::testing::next_splitmix(rng) % (kBandHi - kBandLo));
}

// Random walk under a live-set cap, drained back to empty by an exact
// mini-simulation of engine semantics so the script stays closed.
[[nodiscard]] std::vector<Op> make_closed_script(std::uint64_t seed, std::size_t target_ops) {
    std::vector<Op> script;
    std::vector<std::uint64_t> pool;
    pool.reserve(32'000);
    std::uint64_t rng = seed;
    std::uint64_t fresh = 1;

    mog::OrderBook<> sim_book{{
        .arena_capacity = kArenaCapacity,
        .ladder = {.lo_tick = kBandLo, .hi_tick = kBandHi, .page_pool = 256},
    }};

    const auto pick_known = [&]() -> std::uint64_t {
        return !pool.empty() && mog::testing::next_splitmix(rng) % 100 < 70
                   ? pool[mog::testing::next_splitmix(rng) % pool.size()]
                   : mog::testing::next_splitmix(rng);
    };
    const auto draw_qty = [&]() -> std::int64_t {
        return static_cast<std::int64_t>(mog::testing::next_splitmix(rng) % 900) + 1;
    };

    while (script.size() < target_ops) {
        Op op{};
        const bool force_remove =
            pool.size() > 24'000 || mog::testing::next_splitmix(rng) % 10'000 == 0;
        if (force_remove && !pool.empty()) {
            op.kind = OpKind::remove;
            const std::size_t idx = mog::testing::next_splitmix(rng) % pool.size();
            op.ref = pool[idx];
        } else {
            const std::uint64_t sel = mog::testing::next_splitmix(rng) % 100;
            if (sel < 35) {
                op.kind = OpKind::add;
                op.ref = fresh++;
                op.qty = draw_qty();
                op.price = draw_price(rng);
            } else if (sel < 55) {
                op.kind = OpKind::execute;
                op.ref = pick_known();
                op.qty = draw_qty();
            } else if (sel < 65) {
                op.kind = OpKind::cancel;
                op.ref = pick_known();
                op.qty = draw_qty();
            } else if (sel < 80) {
                op.kind = OpKind::remove;
                op.ref = pick_known();
            } else {
                op.kind = OpKind::replace;
                op.ref = pick_known();
                op.ref2 = fresh++;
                op.qty = draw_qty();
                op.price = draw_price(rng);
            }
        }
        script.push_back(op);

        switch (op.kind) {
        case OpKind::add: {
            const auto r = sim_book.add(OrderId{op.ref}, (op.ref & 1) != 0 ? Side::buy : Side::sell,
                                        Qty{op.qty}, Price{op.price});
            if (r.kind != kTickError)
                pool.push_back(op.ref);
            break;
        }
        case OpKind::execute: {
            const auto r = sim_book.execute(OrderId{op.ref}, Qty{op.qty});
            if (r.kind == static_cast<std::uint8_t>(mog::BookEventKind::removed)) {
                if (auto it = std::find(pool.begin(), pool.end(), op.ref); it != pool.end()) {
                    *it = pool.back();
                    pool.pop_back();
                }
            }
            break;
        }
        case OpKind::cancel: {
            const auto r = sim_book.cancel(OrderId{op.ref}, Qty{op.qty});
            if (r.kind == static_cast<std::uint8_t>(mog::BookEventKind::removed)) {
                if (auto it = std::find(pool.begin(), pool.end(), op.ref); it != pool.end()) {
                    *it = pool.back();
                    pool.pop_back();
                }
            }
            break;
        }
        case OpKind::remove: {
            const auto r = sim_book.remove(OrderId{op.ref});
            if (r.kind != kTickError) {
                if (auto it = std::find(pool.begin(), pool.end(), op.ref); it != pool.end()) {
                    *it = pool.back();
                    pool.pop_back();
                }
            }
            break;
        }
        case OpKind::replace: {
            const auto r =
                sim_book.replace(OrderId{op.ref}, OrderId{op.ref2}, Qty{op.qty}, Price{op.price});
            if (r.kind != kTickError) {
                if (auto it = std::find(pool.begin(), pool.end(), op.ref); it != pool.end()) {
                    *it = op.ref2;
                } else {
                    pool.push_back(op.ref2);
                }
            }
            break;
        }
        }
    }

    std::vector<std::uint64_t> live_refs;
    sim_book.for_each_l2([&](Side side, std::int64_t tick, std::int64_t) {
        sim_book.for_each_in_level(
            side, Price{tick}, [&](OrderId ref, std::int64_t) { live_refs.push_back(ref.value); });
    });
    for (std::uint64_t r : live_refs) {
        static_cast<void>(sim_book.remove(OrderId{r}));
        Op op{};
        op.kind = OpKind::remove;
        op.ref = r;
        script.push_back(op);
    }
    return script;
}

struct World {
    std::vector<Op> script = make_closed_script(0xC1055ULL, 2u << 20);
    mog::OrderBook<> book{{
        .arena_capacity = kArenaCapacity,
        .ladder = {.lo_tick = kBandLo, .hi_tick = kBandHi, .page_pool = 256},
    }};
};

[[nodiscard]] bool apply_script(mog::OrderBook<>& book, const std::vector<Op>& script) noexcept {
    std::uint64_t sink = 0;
    for (const Op& op : script) {
        BookTick r;
        switch (op.kind) {
        case OpKind::add:
            r = book.add(OrderId{op.ref}, (op.ref & 1) != 0 ? Side::buy : Side::sell, Qty{op.qty},
                         Price{op.price});
            break;
        case OpKind::execute:
            r = book.execute(OrderId{op.ref}, Qty{op.qty});
            break;
        case OpKind::cancel:
            r = book.cancel(OrderId{op.ref}, Qty{op.qty});
            break;
        case OpKind::remove:
            r = book.remove(OrderId{op.ref});
            break;
        case OpKind::replace:
            r = book.replace(OrderId{op.ref}, OrderId{op.ref2}, Qty{op.qty}, Price{op.price});
            break;
        }
        sink += r.kind != kTickError ? static_cast<std::uint64_t>(r.kind) : 0xEE;
        benchmark::DoNotOptimize(sink);
    }
    return book.live_orders() == 0 && book.audit();
}

void BM_BookMixedFeed(benchmark::State& state) {
    World world;
    if (!apply_script(world.book, world.script)) {
        state.SkipWithError("script replay failed");
        return;
    }
    for (auto _ : state) {
        if (!apply_script(world.book, world.script)) {
            state.SkipWithError("script replay failed");
            return;
        }
        benchmark::DoNotOptimize(world.book.live_orders());
    }
    state.SetItemsProcessed(state.iterations() * static_cast<long long>(world.script.size()));
}
BENCHMARK(BM_BookMixedFeed);

void BM_BookAddRemoveCycle(benchmark::State& state) {
    mog::OrderBook<> book({
        .arena_capacity = 1024,
        .ladder = {.lo_tick = kBandLo, .hi_tick = kBandHi, .page_pool = 8},
    });
    std::uint64_t rng = 0xFACEULL;
    std::uint64_t ref = 1;
    std::size_t i = 0;
    for (auto _ : state) {
        const std::int64_t price =
            kBandLo + 49'744 + static_cast<std::int64_t>(mog::testing::next_splitmix(rng) % 512);
        const auto added =
            book.add(OrderId{ref}, (i & 1) != 0 ? Side::buy : Side::sell, Qty{37}, Price{price});
        if (!mog::ok(added)) {
            state.SkipWithError("add failed");
            return;
        }
        const auto gone = book.remove(OrderId{ref});
        if (!mog::ok(gone)) {
            state.SkipWithError("remove failed");
            return;
        }
        ++ref;
        ++i;
    }
    state.SetItemsProcessed(state.iterations() * 2);
}
BENCHMARK(BM_BookAddRemoveCycle);

void BM_BookBestQuery(benchmark::State& state) {
    World world;
    if (!apply_script(world.book, world.script)) {
        state.SkipWithError("script replay failed");
        return;
    }
    for (auto _ : state) {
        std::int64_t bb = world.book.best_bid();
        std::int64_t ba = world.book.best_ask();
        benchmark::DoNotOptimize(bb);
        benchmark::DoNotOptimize(ba);
    }
}
BENCHMARK(BM_BookBestQuery);

} // namespace

BENCHMARK_MAIN();
