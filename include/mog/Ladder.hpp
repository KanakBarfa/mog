// Paged dense price ladder: O(1) level access by tick offset, lazily claimed
// preallocated pages, incremental L2 aggregates, and best-price refill scans.
#pragma once

#include <mog/Contracts.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>

namespace mog {

inline constexpr std::int64_t kNoTick = INT64_MAX;

struct alignas(32) Level {
    std::int64_t qty_total = 0;
    std::uint32_t head = kNullIndex;
    std::uint32_t tail = kNullIndex;
    std::uint32_t count = 0;
    std::uint32_t pad_ = 0;
    std::uint64_t reserved_ = 0;
};
static_assert(sizeof(Level) == 32);

template <std::size_t kPageShift = 10>
class PriceLadder {
public:
    static constexpr std::size_t kPageTicks = std::size_t{1} << kPageShift;
    static constexpr std::uint32_t kNoPage = UINT32_MAX;
    static_assert(kPageShift >= 4 && kPageShift <= 20);

    struct Config {
        std::int64_t lo_tick;
        std::int64_t hi_tick; // exclusive upper bound
        std::size_t page_pool;
    };

    explicit PriceLadder(Config cfg)
        : lo_(cfg.lo_tick), hi_(cfg.hi_tick), dir_len_(validated_dir_len(cfg)),
          pool_len_(cfg.page_pool),
          dir_(static_cast<std::uint32_t*>(::operator new(dir_len_ * sizeof(std::uint32_t)))),
          pages_(static_cast<Page*>(
              ::operator new(pool_len_ * sizeof(Page), std::align_val_t{alignof(Page)}))) {
        MOG_PRE(cfg.lo_tick < cfg.hi_tick);
        MOG_PRE(cfg.page_pool > 0 && cfg.page_pool <= kNoPage);
        // Zero-initialized pool; pages are sticky across drains and never re-cleared.
        for (std::size_t i = 0; i < dir_len_; ++i)
            dir_[i] = kNoPage;
        static constexpr Level kEmptyLevel{};
        for (std::size_t i = 0; i < pool_len_; ++i) {
            pages_[i].occupancy = 0;
            pages_[i].summary_mask = 0;
            std::fill(pages_[i].mask.begin(), pages_[i].mask.end(), std::uint64_t{0});
            std::fill(pages_[i].levels.begin(), pages_[i].levels.end(), kEmptyLevel);
        }
    }

    ~PriceLadder() {
        ::operator delete(pages_, std::align_val_t{alignof(Page)});
        ::operator delete(dir_);
    }

    PriceLadder(const PriceLadder&) = delete;
    PriceLadder& operator=(const PriceLadder&) = delete;

    [[nodiscard]] std::int64_t lo_tick() const noexcept { return lo_; }
    [[nodiscard]] std::int64_t hi_tick() const noexcept { return hi_; }
    [[nodiscard]] bool in_band(std::int64_t tick) const noexcept {
        return tick >= lo_ && tick < hi_;
    }
    [[nodiscard]] std::size_t live_pages() const noexcept { return pages_claimed_; }

    // Applies a signed quantity delta at tick, claiming or releasing pages across
    // the empty/non-empty transition; FIFO fields are left to the book layer.
    // True when tick resolves to an already-claimed page OR the pool has a
    // free page left. Callers MUST gate first-touch claims through this: with
    // contracts compiled out, apply()'s own precondition cannot catch
    // exhaustion, and silently indexing past the pool is corruption, not a
    // slow replay.
    [[nodiscard]] bool claimable(std::int64_t tick) const noexcept {
        const std::size_t d = static_cast<std::size_t>((tick - lo_) >> kPageShift);
        return dir_[d] != kNoPage || pages_claimed_ < pool_len_;
    }

    [[nodiscard]] Level* apply(std::int64_t tick, std::int64_t delta) noexcept {
        MOG_PRE(delta != 0);
        MOG_PRE(in_band(tick));
        const std::size_t d = static_cast<std::size_t>((tick - lo_) >> kPageShift);
        std::uint32_t ps = dir_[d];
        if (ps == kNoPage) {
            MOG_PRE(pages_claimed_ < pool_len_); // widen the page pool for this instrument
            ps = static_cast<std::uint32_t>(pages_claimed_);
            ++pages_claimed_;
            dir_[d] = ps;
        }
        const std::size_t offset = static_cast<std::size_t>(tick - lo_) & (kPageTicks - 1);
        Level& lv = pages_[ps].levels[offset];
        const bool was_nonzero = lv.qty_total != 0;
        lv.qty_total += delta;
        MOG_POST(lv.qty_total >= 0);
        const bool now_nonzero = lv.qty_total != 0;
        if (!was_nonzero && now_nonzero) {
            ++pages_[ps].occupancy;
            const std::size_t w = offset >> 6;
            pages_[ps].mask[w] |= (std::uint64_t{1} << (offset & 63));
            pages_[ps].summary_mask |= (std::uint64_t{1} << w);
        } else if (was_nonzero && !now_nonzero) {
            MOG_PRE(pages_[ps].occupancy > 0);
            --pages_[ps].occupancy;
            const std::size_t w = offset >> 6;
            pages_[ps].mask[w] &= ~(std::uint64_t{1} << (offset & 63));
            if (pages_[ps].mask[w] == 0)
                pages_[ps].summary_mask &= ~(std::uint64_t{1} << w);
        }
        return &lv;
    }

    [[nodiscard]] const Level* peek(std::int64_t tick) const noexcept {
        if (!in_band(tick))
            return nullptr;
        const std::uint32_t ps = dir_[static_cast<std::size_t>((tick - lo_) >> kPageShift)];
        if (ps == kNoPage)
            return nullptr;
        return &pages_[ps].levels[static_cast<std::size_t>(tick - lo_) & (kPageTicks - 1)];
    }

    [[nodiscard]] Level* peek(std::int64_t tick) noexcept {
        if (!in_band(tick))
            return nullptr;
        const std::uint32_t ps = dir_[static_cast<std::size_t>((tick - lo_) >> kPageShift)];
        if (ps == kNoPage)
            return nullptr;
        return &pages_[ps].levels[static_cast<std::size_t>(tick - lo_) & (kPageTicks - 1)];
    }

    // Nearest strictly-above tick with nonzero quantity; kNoTick when none.
    [[nodiscard]] std::int64_t first_above(std::int64_t from_exclusive) const noexcept {
        if (from_exclusive >= hi_ - 1)
            return kNoTick;
        const std::int64_t start = from_exclusive < lo_ ? lo_ : from_exclusive + 1;
        const std::size_t d0 = static_cast<std::size_t>((start - lo_) >> kPageShift);
        const std::size_t o0 = static_cast<std::size_t>(start - lo_) & (kPageTicks - 1);
        for (std::size_t d = d0; d < dir_len_; ++d) {
            const std::uint32_t ps = dir_[d];
            if (ps == kNoPage || pages_[ps].occupancy == 0)
                continue;
            const Page& p = pages_[ps];
            const std::size_t start_o = (d == d0) ? o0 : 0;
            const std::size_t w_start = start_o >> 6;

            std::uint64_t m = p.mask[w_start] & ~((std::uint64_t{1} << (start_o & 63)) - 1);
            if (m != 0) {
                const std::size_t bit = static_cast<std::size_t>(std::countr_zero(m));
                const std::size_t o = (w_start << 6) + bit;
                return lo_ + (static_cast<std::int64_t>(d) << kPageShift) +
                       static_cast<std::int64_t>(o);
            }

            if (w_start + 1 < kMaskWords) {
                const std::uint64_t sum_m =
                    p.summary_mask & ~((std::uint64_t{1} << (w_start + 1)) - 1);
                if (sum_m != 0) {
                    const std::size_t w = static_cast<std::size_t>(std::countr_zero(sum_m));
                    const std::size_t bit = static_cast<std::size_t>(std::countr_zero(p.mask[w]));
                    const std::size_t o = (w << 6) + bit;
                    return lo_ + (static_cast<std::int64_t>(d) << kPageShift) +
                           static_cast<std::int64_t>(o);
                }
            }
        }
        return kNoTick;
    }

    // Nearest strictly-below tick with nonzero quantity; kNoTick when none.
    [[nodiscard]] std::int64_t first_below(std::int64_t from_exclusive) const noexcept {
        if (from_exclusive <= lo_)
            return kNoTick;
        const std::int64_t start = from_exclusive > hi_ ? hi_ - 1 : from_exclusive - 1;
        const std::size_t d0 = static_cast<std::size_t>((start - lo_) >> kPageShift);
        const std::size_t o0 = static_cast<std::size_t>(start - lo_) & (kPageTicks - 1);
        std::size_t d = d0;
        for (;;) {
            const std::uint32_t ps = dir_[d];
            if (ps != kNoPage && pages_[ps].occupancy != 0) {
                const Page& p = pages_[ps];
                const std::size_t start_o = (d == d0) ? o0 : kPageTicks - 1;
                const std::size_t w_start = start_o >> 6;

                const std::size_t bit_pos = start_o & 63;
                const std::uint64_t top_mask =
                    (bit_pos == 63) ? ~std::uint64_t{0} : ((std::uint64_t{1} << (bit_pos + 1)) - 1);
                std::uint64_t m = p.mask[w_start] & top_mask;
                if (m != 0) {
                    const std::size_t bit = 63 - static_cast<std::size_t>(std::countl_zero(m));
                    const std::size_t o = (w_start << 6) + bit;
                    return lo_ + (static_cast<std::int64_t>(d) << kPageShift) +
                           static_cast<std::int64_t>(o);
                }

                if (w_start > 0) {
                    const std::uint64_t sum_m =
                        p.summary_mask & ((std::uint64_t{1} << w_start) - 1);
                    if (sum_m != 0) {
                        const std::size_t w =
                            63 - static_cast<std::size_t>(std::countl_zero(sum_m));
                        const std::size_t bit =
                            63 - static_cast<std::size_t>(std::countl_zero(p.mask[w]));
                        const std::size_t o = (w << 6) + bit;
                        return lo_ + (static_cast<std::int64_t>(d) << kPageShift) +
                               static_cast<std::int64_t>(o);
                    }
                }
            }
            if (d == 0)
                return kNoTick;
            --d;
        }
    }

    template <class Fn>
    void for_each_level(Fn&& fn) const noexcept {
        for (std::size_t d = 0; d < dir_len_; ++d) {
            const std::uint32_t ps = dir_[d];
            if (ps == kNoPage)
                continue;
            const Page& p = pages_[ps];
            for (std::size_t o = 0; o < kPageTicks; ++o)
                if (p.levels[o].qty_total != 0)
                    fn(lo_ + (static_cast<std::int64_t>(d) << kPageShift) +
                           static_cast<std::int64_t>(o),
                       p.levels[o]);
        }
    }

private:
    static constexpr std::size_t kMaskWords = (kPageTicks + 63) / 64;

    struct Page {
        std::uint32_t occupancy = 0;
        std::uint64_t summary_mask = 0;
        std::array<std::uint64_t, kMaskWords> mask{};
        std::array<Level, kPageTicks> levels{};
    };

    static std::size_t validated_dir_len(Config cfg) noexcept {
        MOG_PRE(cfg.lo_tick < cfg.hi_tick && cfg.page_pool > 0);
        return (static_cast<std::size_t>(cfg.hi_tick - cfg.lo_tick) + kPageTicks - 1) >> kPageShift;
    }

    std::int64_t lo_;
    std::int64_t hi_;
    std::size_t dir_len_;
    std::size_t pool_len_;
    std::size_t pages_claimed_ = 0;
    std::uint32_t* dir_ = nullptr;
    Page* pages_ = nullptr;
};

} // namespace mog
