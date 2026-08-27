// Tick-to-book latency percentiles over a closed-loop mixed op stream.
// Gate (DESIGN.md): median < 15 ns, p99.9 < 60 ns on the bench machine.
// TSC overhead inflates these numbers slightly, so they read conservative.

#include <mog/OrderBook.hpp>

#include <support/CorpusGen.hpp>

#ifdef MOG_IR_METER
#include <valgrind/callgrind.h>
#include <valgrind/valgrind.h>
#endif

#include <x86intrin.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <expected>
#include <vector>

namespace {

using mog::BookTick;
using mog::kTickError;
using mog::OrderId;
using mog::Price;
using mog::Qty;
using mog::Side;

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

} // namespace

int main(int argc, char** argv) {
    std::size_t samples = 3'000'000;
    if (argc > 1)
        samples = static_cast<std::size_t>(std::strtoull(argv[1], nullptr, 10));
    // Default models one liquid instrument's live set; scale via argv to probe
    // the cache-bound regime.
    std::size_t arena_capacity = 1 << 15;
    std::size_t live_soft_cap = 24'000;
    if (argc > 2)
        arena_capacity = static_cast<std::size_t>(std::strtoull(argv[2], nullptr, 10));
    if (argc > 3)
        live_soft_cap = static_cast<std::size_t>(std::strtoull(argv[3], nullptr, 10));

    mog::OrderBook<> book({
        .arena_capacity = arena_capacity,
        .ladder = {.lo_tick = kBandLo, .hi_tick = kBandHi, .page_pool = 256},
    });

    // Contracts mode is a replay configuration knob: CI/fuzz gates run
    // enforce; release replays run observe/ignore (contracts doctrine in DESIGN.md).
    const char* cm = std::getenv("MOG_BOOK_CONTRACTS");
    const mog::contracts::Mode cmode =
        (cm != nullptr && std::strcmp(cm, "ignore") == 0)    ? mog::contracts::Mode::ignore
        : (cm != nullptr && std::strcmp(cm, "observe") == 0) ? mog::contracts::Mode::observe
                                                             : mog::contracts::Mode::enforce;
    mog::contracts::set_mode(cmode);

    std::uint64_t rng = 0x7A7ULL;
    std::uint64_t fresh = 1;
    // Exact live pool with zero heap churn: flat open-addressing map into a
    // swap-pop vector; recency-biased picks mimic real feed locality.
    const std::size_t kMapCap = std::size_t{1} << 19;
    std::vector<std::uint64_t> map_keys(kMapCap, 0);
    std::vector<std::uint32_t> map_vals(kMapCap, 0);
    std::vector<std::uint64_t> pool;
    pool.reserve(arena_capacity + 16);
    const auto hash_ref = [](std::uint64_t x) noexcept {
        x ^= x >> 33;
        x *= 0xff51afd7ed558ccdULL;
        x ^= x >> 33;
        x *= 0xc4ceb9fe1a85ec53ULL;
        x ^= x >> 33;
        return x;
    };
    const auto map_erase = [&](std::uint64_t r) noexcept {
        std::size_t i = hash_ref(r) & (kMapCap - 1);
        while (map_keys[i] != r)
            i = (i + 1) & (kMapCap - 1);
        // backward shift to keep probe chains intact
        std::size_t j = i;
        for (;;) {
            j = (j + 1) & (kMapCap - 1);
            if (map_keys[j] == 0)
                break;
            const std::size_t home = hash_ref(map_keys[j]) & (kMapCap - 1);
            const bool in_gap = i < j ? (home > i && home <= j) : (home > i || home <= j);
            if (!in_gap) {
                map_keys[i] = map_keys[j];
                map_vals[i] = map_vals[j];
                i = j;
            }
        }
        map_keys[i] = 0;
    };
    const auto pool_insert = [&](std::uint64_t r) {
        pool.push_back(r);
        std::size_t i = hash_ref(r) & (kMapCap - 1);
        while (map_keys[i] != 0)
            i = (i + 1) & (kMapCap - 1);
        map_keys[i] = r;
        map_vals[i] = static_cast<std::uint32_t>(pool.size() - 1);
    };
    const auto pool_erase = [&](std::uint64_t r) {
        std::size_t i = hash_ref(r) & (kMapCap - 1);
        while (map_keys[i] != r)
            i = (i + 1) & (kMapCap - 1);
        const std::uint32_t idx = map_vals[i];
        map_erase(r);
        const std::uint64_t moved = pool.back();
        pool[idx] = moved;
        pool.pop_back();
        if (moved != r) {
            std::size_t k = hash_ref(moved) & (kMapCap - 1);
            while (map_keys[k] != moved)
                k = (k + 1) & (kMapCap - 1);
            map_vals[k] = idx;
        }
    };
    const auto pick_known = [&]() -> std::uint64_t {
        if (pool.empty() || mog::testing::next_splitmix(rng) % 100 >= 70)
            return mog::testing::next_splitmix(rng);
        const std::size_t n = pool.size();
        const std::size_t window = n < 512 ? n : 512;
        return n - window + mog::testing::next_splitmix(rng) % window < n
                   ? pool[n - window + mog::testing::next_splitmix(rng) % window]
                   : pool[mog::testing::next_splitmix(rng) % n];
    };

    // TSC calibration against steady clock.
    const auto t0 = std::chrono::steady_clock::now();
    const std::uint64_t c0 = __rdtsc();
    std::uint64_t spin = 1;
    do {
        for (int i = 0; i < 1000; ++i)
            spin *= 0x100000001b3ULL;
    } while (std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(200));
    const std::uint64_t c1 = __rdtsc();
    const std::uint64_t ns_total = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0)
            .count());
    const std::uint64_t cycles_per_ns_x1024 = ((c1 - c0) * 1024) / ns_total;
    std::uint64_t sink = spin;
    asm volatile("" : : "r"(sink) : "memory");

    std::vector<std::uint32_t> lat;
    lat.resize(samples);

#ifdef MOG_IR_METER
    // Collect only around the engine call; toggling collection never flushes
    // the translation cache, unlike start/stop instrumentation.
    CALLGRIND_ZERO_STATS;
#endif

    std::size_t recorded = 0;
    std::size_t attempted = 0;
    while (recorded < samples) {
        ++attempted;
        Op op{};
        if (book.live_orders() > live_soft_cap || mog::testing::next_splitmix(rng) % 10'000 == 0) {
            if (!pool.empty()) {
                op.kind = OpKind::remove;
                op.ref = pool[mog::testing::next_splitmix(rng) % pool.size()];
            }
        }
        if (op.kind != OpKind::remove) {
            const std::uint64_t sel = mog::testing::next_splitmix(rng) % 100;
            if (sel < 35) {
                op.kind = OpKind::add;
                op.ref = fresh++;
                op.qty = static_cast<std::int64_t>(mog::testing::next_splitmix(rng) % 900) + 1;
                op.price = draw_price(rng);
            } else if (sel < 55) {
                op.kind = OpKind::execute;
                op.ref = pick_known();
                op.qty = static_cast<std::int64_t>(mog::testing::next_splitmix(rng) % 900) + 1;
            } else if (sel < 65) {
                op.kind = OpKind::cancel;
                op.ref = pick_known();
                op.qty = static_cast<std::int64_t>(mog::testing::next_splitmix(rng) % 900) + 1;
            } else if (sel < 80) {
                op.kind = OpKind::remove;
                op.ref = pick_known();
            } else {
                op.kind = OpKind::replace;
                op.ref = pick_known();
                op.ref2 = fresh++;
                op.qty = static_cast<std::int64_t>(mog::testing::next_splitmix(rng) % 900) + 1;
                op.price = draw_price(rng);
            }
        }

        std::uint64_t begin = __rdtsc();
        BookTick r;
#ifdef MOG_IR_METER
        CALLGRIND_TOGGLE_COLLECT;
#endif
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
#ifdef MOG_IR_METER
        CALLGRIND_TOGGLE_COLLECT;
#endif
        const std::uint64_t end = __rdtsc();
        asm volatile("" : : "g"(mog::ok(r)), "g"(r.error) : "cc");

        if (ok(r)) {
            const std::uint64_t cyc = end - begin;
            lat[recorded++] = static_cast<std::uint32_t>(cyc > 0xFFFFFFFFULL ? 0xFFFFFFFFULL : cyc);
        }

        // Ring maintenance mirrors accepted outcomes; stale entries are fine.
        using K = mog::BookEventKind;
        if (mog::ok(r)) {
            switch (static_cast<mog::BookEventKind>(r.kind)) {
            case K::added:
            case K::replaced:
                pool_insert(op.kind == OpKind::replace ? op.ref2 : op.ref);
                break;
            case K::removed:
                pool_erase(op.ref);
                break;
            case K::reduced:
                break;
            }
        }
    }

    std::sort(lat.begin(), lat.end());
    const auto ns = [&](std::size_t idx) {
        return static_cast<double>(lat[idx]) * 1024.0 / static_cast<double>(cycles_per_ns_x1024);
    };
    std::printf(
        "book-percentile OK (%zu successful of %zu attempted ops, live=%zu, contracts=%s)\n",
        recorded, attempted, book.live_orders(),
        cmode == mog::contracts::Mode::enforce   ? "enforce"
        : cmode == mog::contracts::Mode::observe ? "observe"
                                                 : "ignore");
    std::printf("p50=%.1fns p90=%.1fns p99=%.1fns p99.9=%.1fns p99.99=%.1fns max=%.1fns\n",
                ns(recorded / 2), ns(recorded * 9 / 10), ns(recorded * 99 / 100),
                ns(recorded * 999 / 1000), ns(recorded * 9999 / 10000), ns(recorded - 1));
    return 0;
}
