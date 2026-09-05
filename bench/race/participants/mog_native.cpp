// mog native participant: replays the canonical stream through
// ExecutionSimulator directly in C++ - no binding overhead - so the
// engine's own throughput is comparable to hftbacktest's Rust loop.
//
// Build (from repo root):
//   g++-15 -std=c++23 -O2 -march=native -I include \
//     bench/race/participants/mog_native.cpp -o bench/race/out/mog_native
// Run:
//   bench/race/out/mog_native --feed bench/race/out/feed.csv \
//       --runs 5 --out bench/race/out/mog_native.json \
//       [--digest golden|fast] [--contracts enforce|ignore]
// Defaults are the record config; MOG_RACE_CALLGRIND scopes callgrind.

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include "mog/Contracts.hpp"
#include "mog/Simulate.hpp"

#ifdef MOG_RACE_CALLGRIND
#include <valgrind/callgrind.h>
#endif

namespace {

struct Row {
    std::string kind;
    std::uint64_t ts;
    char side;
    std::int64_t price;
    std::int64_t qty;
    std::uint64_t ref;
};

std::vector<Row> load(const std::string& path) {
    std::vector<Row> rows;
    std::ifstream f(path);
    std::string line;
    std::getline(f, line); // header
    while (std::getline(f, line)) {
        Row r{};
        int field = 0;
        std::size_t pos = 0;
        while (pos <= line.size()) {
            auto comma = line.find(',', pos);
            const std::string tok =
                comma == std::string::npos ? line.substr(pos) : line.substr(pos, comma - pos);
            switch (field++) {
            case 0:
                r.kind = tok;
                break;
            case 1:
                r.ts = std::strtoull(tok.c_str(), nullptr, 10);
                break;
            case 2:
                r.side = tok.empty() ? ' ' : tok[0];
                break;
            case 3:
                r.price = std::strtoll(tok.c_str(), nullptr, 10);
                break;
            case 4:
                r.qty = std::strtoll(tok.c_str(), nullptr, 10);
                break;
            case 5:
                r.ref = std::strtoull(tok.c_str(), nullptr, 10);
                break;
            default:
                break;
            }
            if (comma == std::string::npos)
                break;
            pos = comma + 1;
        }
        rows.push_back(std::move(r));
    }
    return rows;
}

} // namespace

int main(int argc, char** argv) {
    const std::string feed = [&] {
        for (int i = 1; i + 1 < argc; ++i)
            if (std::strcmp(argv[i], "--feed") == 0)
                return std::string(argv[i + 1]);
        return std::string("bench/race/out/feed.csv");
    }();
    const int runs = [&] {
        for (int i = 1; i + 1 < argc; ++i)
            if (std::strcmp(argv[i], "--runs") == 0)
                return std::atoi(argv[i + 1]);
        return 5;
    }();
    const std::string out = [&] {
        for (int i = 1; i + 1 < argc; ++i)
            if (std::strcmp(argv[i], "--out") == 0)
                return std::string(argv[i + 1]);
        return std::string("bench/race/out/mog_native.json");
    }();
    std::string digest_name = "golden";
    for (int i = 1; i + 1 < argc; ++i)
        if (std::strcmp(argv[i], "--digest") == 0)
            digest_name = argv[i + 1];
    const mog::DigestMode digest_mode =
        digest_name == "fast" ? mog::DigestMode::fast : mog::DigestMode::golden;
    std::string contracts_name = "enforce";
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::strcmp(argv[i], "--contracts") == 0) {
            contracts_name = argv[i + 1];
            const bool ignore = contracts_name == "ignore";
            static_cast<void>(mog::contracts::set_mode(ignore ? mog::contracts::Mode::ignore
                                                              : mog::contracts::Mode::enforce));
        }
    }

    const auto all = load(feed);

    // Schedule mirrors protocol.py: fire after N market rows consumed.
    struct Sched {
        std::size_t trigger;
        const Row* op;
    };
    std::vector<Sched> sched;
    std::size_t market_count = 0;
    for (const auto& r : all) {
        if (r.kind.rfind("strat", 0) == 0)
            sched.push_back({market_count, &r});
        else
            ++market_count;
    }

    std::vector<double> walls;
    std::vector<std::string> digests;
    std::size_t fills = 0;
    struct AggRow {
        std::string ref;
        std::string side;
        double vwap;
        long long qty;
    };
    std::vector<AggRow> last_agg;
    for (int run = 0; run < runs; ++run) {
        mog::SimConfig cfg{};
        cfg.digest_mode = digest_mode;
        cfg.book.arena_capacity = 1u << 21;
        cfg.book.ladder.lo_tick = 900;
        cfg.book.ladder.hi_tick = 1100;
        cfg.book.ladder.page_pool = 512;
        mog::ExecutionSimulator sim{cfg};

        const auto t0 = std::chrono::steady_clock::now();
#ifdef MOG_RACE_CALLGRIND
        CALLGRIND_TOGGLE_COLLECT;
#endif
        // Single pass in stream order; strategy fires when market cursor reaches trigger.
        std::size_t market_seen = 0;
        std::size_t si = 0;
        for (const auto& r : all) {
            if (r.kind == "ext_add") {
                const auto t = sim.seed_external(mog::OrderId{r.ref},
                                                 r.side == 'B' ? mog::Side::buy : mog::Side::sell,
                                                 mog::Qty{r.qty}, mog::Price{r.price});
                if (!ok(t)) {
                    std::fprintf(stderr, "seed rejected\n");
                    return 1;
                }
                ++market_seen;
            } else if (r.kind == "trade") {
                // feed side is the AGGRESSOR; apply_external consumes the
                // resting side it hits.
                const mog::Side hit = r.side == 'B' ? mog::Side::sell : mog::Side::buy;
                sim.apply_external(hit, mog::Price{r.price}, r.qty);
                ++market_seen;
            } else {
                while (si < static_cast<std::size_t>(sched.size()) &&
                       sched[si].trigger <= market_seen) {
                    const Row& s = *sched[si].op;
                    const mog::OrderId ref{s.ref + (1ull << 62)};
                    if (s.kind == "strat_cancel") {
                        static_cast<void>(sim.cancel_strategy(ref));
                    } else {
                        const mog::SimOrderType ty = s.kind == "strat_limit"
                                                         ? mog::SimOrderType::day_limit
                                                         : mog::SimOrderType::ioc;
                        static_cast<void>(sim.submit(
                            mog::SimInbound{ref, s.side == 'B' ? mog::Side::buy : mog::Side::sell,
                                            mog::Qty{s.qty}, mog::Price{s.price}, ty},
                            s.ts));
                    }
                    // Per-action drain: a quote posted at row k is hittable at row k+1.
                    sim.drain();
                    ++si;
                }
            }
        }
        while (si < sched.size()) {
            const Row& s = *sched[si].op;
            const mog::OrderId ref{s.ref + (1ull << 62)};
            if (s.kind == "strat_cancel") {
                static_cast<void>(sim.cancel_strategy(ref));
            } else {
                static_cast<void>(sim.submit(
                    mog::SimInbound{ref, s.side == 'B' ? mog::Side::buy : mog::Side::sell,
                                    mog::Qty{s.qty}, mog::Price{s.price},
                                    s.kind == "strat_limit" ? mog::SimOrderType::day_limit
                                                            : mog::SimOrderType::ioc},
                    s.ts));
            }
            sim.drain();
            ++si;
        }
        sim.drain();
        const auto t1 = std::chrono::steady_clock::now();
#ifdef MOG_RACE_CALLGRIND
        CALLGRIND_TOGGLE_COLLECT;
#endif

        walls.push_back(std::chrono::duration<double>(t1 - t0).count());
        fills = sim.reports().size();
        if (digest_mode == mog::DigestMode::golden) {
            const auto dg = sim.trace_digest();
            digests.emplace_back(reinterpret_cast<const char*>(dg.data()), dg.size());
        } else {
            const auto dg = sim.fast_trace_digest();
            digests.emplace_back(reinterpret_cast<const char*>(dg.data()), dg.size());
        }

        // Per-order aggregates [ref, side, vwap_price, total_qty] for agreement.
        std::map<std::uint64_t, std::pair<std::pair<double, double>, char>> agg;
        for (const auto& r : sim.reports()) {
            auto& a = agg[r.ref];
            const double q = r.qty;
            a.first.first += r.price_ticks * q;
            a.first.second += q;
            a.second = r.side;
        }
        last_agg.clear();
        for (const auto& [ref, a] : agg)
            last_agg.push_back({std::to_string(ref), std::string(1, a.second),
                                a.first.first / a.first.second,
                                static_cast<long long>(a.first.second)});
    }

    // median wall
    std::vector<double> sorted = walls;
    std::sort(sorted.begin(), sorted.end());
    const double med = sorted[sorted.size() / 2];
    const double ops = static_cast<double>(all.size());

    std::ofstream o(out);
    o << "{\n"
      << "  \"participant\": \"mog-native\",\n"
      << "  \"fills_by_order\": [";
    for (std::size_t i = 0; i < last_agg.size(); ++i) {
        const auto& r = last_agg[i];
        o << "[\"" << r.ref << "\",\"" << r.side << "\"," << r.vwap << "," << r.qty << "]"
          << (i + 1 < last_agg.size() ? "," : "");
    }
    o << "],\n"
      << "  \"ops\": " << all.size() << ",\n"
      << "  \"runs\": " << runs << ",\n"
      << "  \"digest_mode\": \"" << digest_name << "\",\n"
      << "  \"contracts\": \"" << contracts_name << "\",\n"
      << "  \"wall_s_median\": " << med << ",\n"
      << "  \"events_per_sec\": " << ops / med << ",\n"
      << "  \"fills\": " << fills << ",\n"
      << "  \"deterministic\": "
      << (std::all_of(digests.begin(), digests.end(),
                      [&](const std::string& d) { return d == digests[0]; }))
      << "\n}\n";

    std::printf("mog-native[%s,%s]: %.0f ops/s (%zu fills, deterministic=%d)\n",
                digest_name.c_str(), contracts_name.c_str(), ops / med, fills,
                std::all_of(digests.begin(), digests.end(),
                            [&](const std::string& d) { return d == digests[0]; }));
    return 0;
}
