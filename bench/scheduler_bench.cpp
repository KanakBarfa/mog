// Scheduler throughput suite plus the M3 exit criterion runner: a numeric
// first argument streams that many events through a fixed live-window and
// prints the allocation-free scheduling rate.
#include <mog/Scheduler.hpp>
#include <mog/TimingWheel.hpp>

#include <benchmark/benchmark.h>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <random>
#include <string>
#include <vector>

namespace {

struct Payload {
    std::uint64_t a;
    std::uint32_t b;
};

constexpr std::size_t kWindow = 1 << 20;

void fill(std::mt19937_64& rng, Payload& p) noexcept {
    p.a = rng();
    p.b = static_cast<std::uint32_t>(rng());
}

void BM_DrainSorted(benchmark::State& state) {
    const auto n = static_cast<std::size_t>(state.range(0));
    std::mt19937_64 rng(42);
    for (auto _ : state) {
        mog::Scheduler<Payload> s(kWindow);
        for (std::size_t i = 0; i < n; ++i) {
            Payload p{};
            fill(rng, p);
            static_cast<void>(s.push(rng() % 1000000, p));
        }
        std::uint64_t sink = 0;
        while (!s.empty())
            sink += s.pop_min().a;
        benchmark::DoNotOptimize(sink);
    }
    state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(n));
}
BENCHMARK(BM_DrainSorted)->Arg(1 << 16)->Arg(1 << 18);

void BM_PushPopSteady(benchmark::State& state) {
    const auto n = static_cast<std::size_t>(state.range(0));
    const std::size_t warm = kWindow / 2;
    std::mt19937_64 rng(43);
    mog::Scheduler<Payload> s(kWindow);
    for (std::size_t i = 0; i < warm; ++i) {
        Payload p{};
        fill(rng, p);
        static_cast<void>(s.push(rng() % 1000000, p));
    }
    for (auto _ : state) {
        std::uint64_t sink = 0;
        for (std::size_t i = 0; i < n; ++i) {
            Payload p{};
            fill(rng, p);
            static_cast<void>(s.push(rng() % 1000000, p));
            sink += s.pop_min().a;
        }
        benchmark::DoNotOptimize(sink);
    }
    state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(n));
}
BENCHMARK(BM_PushPopSteady)->Arg(1 << 16);

void BM_CancelHeavy(benchmark::State& state) {
    const auto n = static_cast<std::size_t>(state.range(0)) / 2;
    std::mt19937_64 rng(44);
    for (auto _ : state) {
        mog::Scheduler<Payload> s(kWindow);
        std::vector<mog::EventHandle> handles;
        handles.reserve(n);
        for (std::size_t i = 0; i < n; ++i) {
            Payload p{};
            fill(rng, p);
            handles.push_back(s.push(rng() % 1000000, p));
        }
        // Cancel the even-indexed half, then drain what remains.
        for (std::size_t i = 0; i < handles.size(); i += 2)
            static_cast<void>(s.cancel(handles[i]));
        std::uint64_t sink = 0;
        while (!s.empty())
            sink += s.pop_min().a;
        benchmark::DoNotOptimize(sink);
    }
    state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(n + n / 2));
}
BENCHMARK(BM_CancelHeavy)->Arg(1 << 15);

void BM_WorstCaseSift(benchmark::State& state) {
    const auto n = static_cast<std::size_t>(state.range(0));
    std::mt19937_64 rng(45);
    for (auto _ : state) {
        mog::Scheduler<Payload> s(kWindow);
        std::uint64_t ts = n;
        for (std::size_t i = 0; i < n; ++i, --ts) { // strictly decreasing ts
            Payload p{};
            fill(rng, p);
            static_cast<void>(s.push(ts, p));
        }
        std::uint64_t sink = 0;
        while (!s.empty())
            sink += s.pop_min().a;
        benchmark::DoNotOptimize(sink);
    }
    state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(n));
}
BENCHMARK(BM_WorstCaseSift)->Arg(1 << 15);

template <class Eng>
void near_sorted_run(Eng& s, std::mt19937_64& rng, std::size_t n) {
    std::uint64_t ts = 0, sink = 0;
    for (std::size_t i = 0; i < n; ++i) {
        ts += rng() % 64; // realistic feed: nearly ordered with jitter
        Payload p{rng(), static_cast<std::uint32_t>(rng())};
        static_cast<void>(s.push(ts, p));
        if (s.size() > 1 << 14)
            sink += s.pop_min().a;
    }
    while (!s.empty())
        sink += s.pop_min().a;
    benchmark::DoNotOptimize(sink);
}

void BM_NearSorted_Heap(benchmark::State& state) {
    const auto n = static_cast<std::size_t>(state.range(0));
    std::mt19937_64 rng(47);
    for (auto _ : state) {
        mog::Scheduler<Payload> s(kWindow);
        near_sorted_run(s, rng, n);
    }
    state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(n));
}
BENCHMARK(BM_NearSorted_Heap)->Arg(1 << 18);

void BM_NearSorted_Wheel(benchmark::State& state) {
    const auto n = static_cast<std::size_t>(state.range(0));
    std::mt19937_64 rng(47);
    for (auto _ : state) {
        mog::TimingWheel<Payload> s(kWindow);
        near_sorted_run(s, rng, n);
    }
    state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(n));
}
BENCHMARK(BM_NearSorted_Wheel)->Arg(1 << 18);

template <class Eng>
void shuffled_drain_run(Eng& s, std::mt19937_64& rng, std::size_t n) {
    for (std::size_t i = 0; i < n; ++i) {
        Payload p{rng(), static_cast<std::uint32_t>(rng())};
        static_cast<void>(s.push(rng() % 100000000ULL, p)); // adversarial shuffle
    }
    std::uint64_t sink = 0;
    while (!s.empty())
        sink += s.pop_min().a;
    benchmark::DoNotOptimize(sink);
}

void BM_ShuffledDrain_Heap(benchmark::State& state) {
    const auto n = static_cast<std::size_t>(state.range(0));
    std::mt19937_64 rng(48);
    for (auto _ : state) {
        mog::Scheduler<Payload> s(kWindow);
        shuffled_drain_run(s, rng, n);
    }
    state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(n));
}
BENCHMARK(BM_ShuffledDrain_Heap)->Arg(1 << 17);

void BM_ShuffledDrain_Wheel(benchmark::State& state) {
    const auto n = static_cast<std::size_t>(state.range(0));
    std::mt19937_64 rng(48);
    for (auto _ : state) {
        mog::TimingWheel<Payload> s(kWindow);
        shuffled_drain_run(s, rng, n);
    }
    state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(n));
}
BENCHMARK(BM_ShuffledDrain_Wheel)->Arg(1 << 17);

} // namespace

// M3 exit criterion: stream `argv[1]` events through a fixed live window.
// Every push and pop runs against preallocated storage only.
int main(int argc, char** argv) {
    if (argc > 1 && std::isdigit(static_cast<unsigned char>(argv[1][0]))) {
        const std::uint64_t total = std::strtoull(argv[1], nullptr, 10);
        // Live-window size is tunable so cache-resident and thrashed regimes
        // are both measurable.
        std::size_t kLive = 1 << 22;
        if (const char* env = std::getenv("MOG_SCHED_LIVE"))
            kLive = static_cast<std::size_t>(std::strtoull(env, nullptr, 10));
        bool use_wheel = false;
        if (const char* env = std::getenv("MOG_SCHED_ENGINE"))
            use_wheel = std::string_view(env) == "wheel";
        std::mt19937_64 rng(46);
        std::uint64_t sink = 0, done = 0;
        const auto t0 = std::chrono::steady_clock::now();
        std::uint64_t ts_cursor = 0;
        auto step = [&](auto& s) {
            while (done < total) {
                const std::size_t burst = static_cast<std::size_t>(total - done) < kLive / 2
                                              ? static_cast<std::size_t>(total - done)
                                              : kLive / 2;
                for (std::size_t i = 0; i < burst && s.size() < kLive; ++i) {
                    Payload p{rng(), static_cast<std::uint32_t>(rng())};
                    // Non-decreasing feed: valid for both engines (the wheel
                    // forbids scheduling into the past by contract).
                    ts_cursor += rng() % 512;
                    static_cast<void>(s.push(ts_cursor, p));
                }
                const std::size_t drain = s.size() - kLive / 2;
                for (std::size_t i = 0; i < drain; ++i) {
                    sink += s.pop_min().a;
                    ++done;
                }
            }
        };
        if (use_wheel) {
            mog::TimingWheel<Payload> s(kLive);
            step(s);
        } else {
            mog::Scheduler<Payload> s(kLive);
            step(s);
        }
        const std::chrono::duration<double> dt = std::chrono::steady_clock::now() - t0;
        std::printf("scheduled=%llu live_cap=%zu seconds=%.3f events_per_sec=%.3g sink=%llu\n",
                    static_cast<unsigned long long>(done), kLive, dt.count(),
                    static_cast<double>(done) / dt.count(), static_cast<unsigned long long>(sink));
        return 0;
    }
    benchmark::Initialize(&argc, argv);
    if (::benchmark::ReportUnrecognizedArguments(argc, argv))
        return 1;
    benchmark::RunSpecifiedBenchmarks();
    benchmark::Shutdown();
    return 0;
}
