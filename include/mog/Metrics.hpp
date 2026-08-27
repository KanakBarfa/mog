// Strategy accounting and latency instrumentation.
//
// Accounting is deliberately dumb arithmetic over an explicit fill stream:
// every number it produces can be recomputed by a third party from the same
// logged fills, which is exactly what the M5 exit criterion demands.
//
// The histogram measures with rdtsc where available and exposes per-bucket
// watermarks through atomic_ref::fetch_max, so a monitor thread can observe
// maxima without locks or false sharing with the measuring core.
#pragma once

#include <mog/Contracts.hpp>
#include <mog/polyfill/AtomicMinMax.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cstdint>
#include <vector>

#if defined(__x86_64__)
#include <x86intrin.h>
#endif

namespace mog {

[[nodiscard]] inline std::uint64_t tick_now() noexcept {
#if defined(__x86_64__)
    return __builtin_ia32_rdtsc();
#else
    return static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
#endif
}

// Atomic max watermark using P0493 polyfill.
[[nodiscard]] inline std::uint64_t atomic_fetch_max(std::atomic<std::uint64_t>& cell,
                                                    std::uint64_t v) noexcept {
    return mog::atomics::fetch_max(&cell, v, std::memory_order_release);
}

// Log-spaced latency histogram. record() is single-writer; watermark() may be
// called from any thread.
template <std::size_t kBuckets = 32>
class TickHistogram {
public:
    void record(std::uint64_t ticks) noexcept {
        const std::size_t b = bucket_of(ticks);
        counts_[b].fetch_add(1, std::memory_order_relaxed);
        static_cast<void>(atomic_fetch_max(maxima_[b], ticks));
    }

    [[nodiscard]] std::uint64_t bucket_max(std::size_t b) const noexcept {
        return maxima_[b].load(std::memory_order_acquire);
    }
    [[nodiscard]] std::uint64_t bucket_count(std::size_t b) const noexcept {
        return counts_[b].load(std::memory_order_relaxed);
    }
    [[nodiscard]] constexpr std::size_t buckets() const noexcept { return kBuckets; }

    // Bucket bounds are powers of two: [1<<(b+4), 1<<(b+5)) ns-equivalents in
    // whatever unit the caller's clock ticks; callers convert.
    [[nodiscard]] static constexpr std::uint64_t bucket_lo(std::size_t b) noexcept {
        return std::uint64_t{16} << b;
    }

private:
    [[nodiscard]] std::size_t bucket_of(std::uint64_t t) const noexcept {
        if (t < 16)
            return 0;
        const auto lz = static_cast<std::size_t>(std::countl_zero(t));
        const std::size_t b = 63 - lz - 4; // floor_log2(t) - 4
        return std::min(b, kBuckets - 1);
    }

    alignas(64) std::array<std::atomic<std::uint64_t>, kBuckets> counts_{};
    alignas(64) std::array<std::atomic<std::uint64_t>, kBuckets> maxima_{};
};

// Cash/inventory accounting over fills at known marks.
class Account {
public:
    // qty signed: positive buy, negative sell. price in ticks. fee is the
    // signed cash impact of the fill's settlement (maker rebate < 0);
    // default 0 keeps fee-free accounting identical to before.
    void on_fill(std::int64_t price_ticks, std::int64_t signed_qty, std::int64_t fee = 0) noexcept {
        cash_ -= signed_qty * price_ticks;
        cash_ -= fee;
        fees_ += fee;
        inventory_ += signed_qty;
    }

    // Aggressive legs arrive as acks with an exact integer notional instead
    // of a single price; the notional already contains the quantity, so only
    // its sign is applied here. fee is the taker charge (signed, >= 0 for a
    // conventional schedule).
    void on_aggressive_fill(std::uint64_t notional_ticks, std::int64_t signed_qty,
                            std::int64_t fee = 0) noexcept {
        const std::int64_t sign = signed_qty < 0 ? -1 : 1;
        cash_ -= sign * static_cast<std::int64_t>(notional_ticks);
        cash_ -= fee;
        fees_ += fee;
        inventory_ += signed_qty;
    }

    // Marks the position at mid; equity = cash + inventory * mark.
    [[nodiscard]] double equity(double mark_price_ticks) const noexcept {
        return static_cast<double>(cash_) + static_cast<double>(inventory_) * mark_price_ticks;
    }

    void observe_equity(double eq) noexcept {
        peak_ = std::max(peak_, eq);
        drawdown_ = std::max(drawdown_, peak_ - eq);
    }

    [[nodiscard]] std::int64_t inventory() const noexcept { return inventory_; }
    [[nodiscard]] std::int64_t cash() const noexcept { return cash_; }
    // Cumulative signed fee cash impact: positive = net paid, negative =
    // net rebated. Equity already includes it via cash.
    [[nodiscard]] std::int64_t net_fees() const noexcept { return fees_; }
    [[nodiscard]] double drawdown() const noexcept { return drawdown_; }

private:
    std::int64_t cash_ = 0;
    std::int64_t inventory_ = 0;
    std::int64_t fees_ = 0;
    double peak_ = 0.0;
    double drawdown_ = 0.0;
};

// Markouts of fill price against future mid at fixed horizons. Mid samples
// are (ts, doubled_mid) pairs appended monotonically; markout() interpolates
// no state beyond the first sample at or after the horizon.
class Markouts {
public:
    static constexpr std::array<std::uint64_t, 3> kHorizons{1000, 10000, 100000};

    void add_mid(std::uint64_t ts_ns, std::int64_t doubled_mid) noexcept {
        mids_.push_back(Sample{ts_ns, doubled_mid});
    }

    void add_fill(std::uint64_t ts_ns, std::int64_t price_ticks, char side) noexcept {
        for (std::size_t i = 0; i < kHorizons.size(); ++i) {
            const auto* s = sample_at(ts_ns + kHorizons[i]);
            if (!s)
                continue;
            const double mo = side == 'B' ? static_cast<double>(s->mid2 / 2 - price_ticks)
                                          : static_cast<double>(price_ticks - s->mid2 / 2);
            sum_[i] += mo;
            ++n_[i];
        }
    }

    [[nodiscard]] double mean_markout(std::size_t h_idx) const noexcept {
        return n_[h_idx] ? sum_[h_idx] / static_cast<double>(n_[h_idx]) : 0.0;
    }
    [[nodiscard]] std::uint64_t count(std::size_t h_idx) const noexcept { return n_[h_idx]; }

private:
    struct Sample {
        std::uint64_t ts;
        std::int64_t mid2;
    };

    [[nodiscard]] const Sample* sample_at(std::uint64_t ts) const noexcept {
        auto it = std::lower_bound(mids_.begin(), mids_.end(), ts,
                                   [](const Sample& s, std::uint64_t t) { return s.ts < t; });
        return it == mids_.end() ? nullptr : &*it;
    }

    std::vector<Sample> mids_;
    std::array<double, 3> sum_{};
    std::array<std::uint64_t, 3> n_{};
};

} // namespace mog
