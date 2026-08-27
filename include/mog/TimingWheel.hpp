// Hierarchical timing-wheel scheduler: 11 levels of 64 buckets, one level per
// base-64 digit of the nanosecond timestamp. Routing is O(1) via the highest
// digit that differs from the watermark; cancellation is O(1) through
// intrusive doubly-linked buckets; drains walk time forward bucket by bucket.
//
// Ordering semantics match Scheduler<T>: lexicographic (ts, seq), stable on
// equal timestamps. The structural invariant that makes this exact: an event
// sits at the highest digit where it differs from the watermark, so as the
// watermark advances, events demote strictly downward until level 0's current
// bucket holds precisely the events due at that nanosecond.
//
// Invariants (checked by audit()):
//   I1  every pending ts satisfies ts >= watermark
//   I2  an event in level li > 0 shares all digits above li with the
//       watermark and sits in bucket (ts_digit_li), which is strictly greater
//       than the watermark's digit there
//   I3  a level 0 event lies inside the current 64 ns window at
//       [watermark & ~63, (watermark & ~63) + 64)
//   I4  occupancy masks mirror bucket chains; backlinks close; counts add up
//
// One restriction against Scheduler<T>: pushes must not go back in time
// (ts >= watermark), the usual discrete-event simulation contract.
#pragma once

#include <mog/Arena.hpp>
#include <mog/Contracts.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <new>

namespace mog {

// Shares EventTag handles with Scheduler<T>: engines are interchangeable.
template <typename T>
class TimingWheel {
public:
    using value_type = T;

    static constexpr std::uint32_t kBitsPerLevel = 6;
    static constexpr std::uint32_t kBuckets = 64;
    static constexpr std::uint32_t kLevels = 11; // 66 bits of span over u64 ns

private:
    struct Slot {
        T value{};
        std::uint64_t ts{0};
        std::uint64_t seq{0};
        std::uint32_t generation{0};
        std::uint32_t bucket{kNullIndex}; // level * 64 + slot
        std::uint32_t prev{kNullIndex};
        std::uint32_t next{kNullIndex};
        std::uint32_t free_next{kNullIndex};
        bool live{false};
    };
    struct Level {
        std::array<std::uint32_t, kBuckets> head{};
        std::array<std::uint32_t, kBuckets> tail{};
        std::uint64_t occupied{0}; // bit s set <=> buckets[s] non-empty
        // Level 0 only: the current-tick bucket emits from the head whenever
        // inserts arrived in increasing-seq order (the common feed case);
        // out-of-order inserts clear the flag and force a linear min-scan.
        std::array<std::uint64_t, kBuckets> bucket_last_seq{};
        std::array<bool, kBuckets> bucket_sorted{};
    };

public:
    explicit TimingWheel(std::size_t capacity)
        : capacity_(capacity), slots_(static_cast<Slot*>(::operator new(capacity * sizeof(Slot)))) {
        MOG_PRE(capacity > 0 && capacity < kNullIndex);
        for (std::size_t i = 0; i < capacity_; ++i)
            static_cast<void>(new (&slots_[i]) Slot{});
        for (Level& l : levels_)
            l.head.fill(kNullIndex), l.tail.fill(kNullIndex);
        relink_free_list();
    }

    ~TimingWheel() { ::operator delete(slots_); }

    TimingWheel(const TimingWheel&) = delete;
    TimingWheel& operator=(const TimingWheel&) = delete;

    [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }
    [[nodiscard]] std::size_t size() const noexcept { return live_; }
    [[nodiscard]] bool empty() const noexcept { return live_ == 0; }
    [[nodiscard]] std::uint64_t watermark() const noexcept { return now_; }

    [[nodiscard]] EventHandle push(std::uint64_t ts, const T& value) noexcept {
        MOG_PRE(live_ < capacity_);
        MOG_PRE(ts >= now_); // no scheduling into the past
        const std::uint32_t idx = free_head_;
        MOG_PRE(idx != kNullIndex);
        Slot& s = slots_[idx];
        free_head_ = s.free_next;
        s.value = value;
        s.ts = ts;
        s.seq = next_seq_;
        s.live = true;
        attach(idx);
        ++live_;
        ++next_seq_;
        return EventHandle{idx, s.generation};
    }

    bool cancel(EventHandle h) noexcept {
        if (h.index >= capacity_)
            return false;
        Slot& s = slots_[h.index];
        if (!s.live || s.generation != h.generation)
            return false;
        detach(h.index);
        free_slot(h.index);
        --live_;
        return true;
    }

    // Both peek variants advance the watermark lazily; that movement is part
    // of settling and does not affect the pop sequence.
    [[nodiscard]] std::uint64_t peek_min_ts() noexcept {
        MOG_PRE(!empty());
        settle();
        return now_;
    }

    [[nodiscard]] const T& peek_min() noexcept {
        MOG_PRE(!empty());
        settle();
        return slots_[front_of_tick()].value;
    }

    [[nodiscard]] T pop_min() noexcept {
        MOG_PRE(!empty());
        settle();
        const std::uint32_t idx = take_front_of_tick();
        const T out = slots_[idx].value;
        free_slot(idx);
        --live_;
        return out;
    }

    void clear() noexcept {
        for (std::size_t i = 0; i < capacity_; ++i) {
            Slot& s = slots_[i];
            if (s.live)
                ++s.generation;
            s.live = false;
            s.bucket = kNullIndex;
        }
        for (Level& l : levels_) {
            l.head.fill(kNullIndex);
            l.tail.fill(kNullIndex);
            l.occupied = 0;
        }
        relink_free_list();
        live_ = 0;
        now_ = 0;
        next_seq_ = 0;
    }

    [[nodiscard]] bool audit() const noexcept {
        std::size_t seen = 0;
        for (std::size_t li = 0; li < kLevels; ++li) {
            const Level& l = levels_[li];
            for (std::uint32_t b = 0; b < kBuckets; ++b) {
                const bool has_bit = ((l.occupied >> b) & 1ULL) != 0;
                const std::uint32_t h = l.head[b];
                if (has_bit != (h != kNullIndex))
                    return false;
                if (has_bit && l.tail[b] == kNullIndex)
                    return false;
                std::size_t chain = 0;
                std::uint32_t prev_idx = kNullIndex;
                std::uint32_t idx = h;
                while (idx != kNullIndex) {
                    if (++chain > capacity_)
                        return false;
                    const Slot& s = slots_[idx];
                    if (!s.live || s.bucket != li * kBuckets + b || s.prev != prev_idx)
                        return false;
                    if (s.ts < now_)
                        return false; // I1
                    const std::uint64_t shift = kBitsPerLevel * (li + 1);
                    if (li > 0) {
                        if (shift < 64 && ((s.ts ^ now_) >> shift) != 0)
                            return false; // I2 digits above agree
                        if (((s.ts >> (kBitsPerLevel * li)) & 63U) != b ||
                            b <= ((now_ >> (kBitsPerLevel * li)) & 63U))
                            return false; // I2 strictly ahead in own digit
                    } else {
                        if ((s.ts >> kBitsPerLevel) != (now_ >> kBitsPerLevel))
                            return false; // I3 inside current window
                    }
                    prev_idx = idx;
                    idx = s.next;
                }
                if (has_bit && l.tail[b] != prev_idx)
                    return false;
                seen += chain;
            }
        }
        if (seen != live_)
            return false;
        std::size_t free_len = 0;
        std::uint32_t cursor = free_head_;
        while (cursor != kNullIndex) {
            if (cursor >= capacity_ || slots_[cursor].live)
                return false;
            cursor = slots_[cursor].free_next;
            ++free_len;
        }
        return free_len + seen == capacity_;
    }

private:
    // Highest differing base-64 digit decides the level; equal timestamps
    // land directly in level 0's current bucket.
    std::uint32_t route_bucket(std::uint64_t ts) const noexcept {
        const std::uint64_t diff = ts ^ now_;
        if (diff == 0)
            return static_cast<std::uint32_t>(now_) & 63U;
        const std::uint32_t top = 63u - static_cast<std::uint32_t>(std::countl_zero(diff));
        const std::uint32_t level = top / kBitsPerLevel;
        return level * kBuckets + ((ts >> (kBitsPerLevel * level)) & 63U);
    }

    void attach(std::uint32_t idx) noexcept {
        Slot& s = slots_[idx];
        const std::uint32_t b = route_bucket(s.ts);
        const std::uint32_t li = b / kBuckets;
        const std::uint32_t slot = b % kBuckets;
        Level& l = levels_[li];
        s.bucket = b;
        s.next = kNullIndex;
        if (l.head[slot] == kNullIndex) {
            s.prev = kNullIndex;
            l.head[slot] = idx;
        } else {
            s.prev = l.tail[slot];
            slots_[l.tail[slot]].next = idx;
        }
        l.tail[slot] = idx;
        l.occupied |= std::uint64_t{1} << slot;
        if (li == 0) {
            if (l.head[slot] == idx)
                l.bucket_sorted[slot] = true; // first element starts sorted
            else
                l.bucket_sorted[slot] = l.bucket_sorted[slot] && s.seq > l.bucket_last_seq[slot];
            l.bucket_last_seq[slot] = s.seq;
        }
    }

    void detach(std::uint32_t idx) noexcept {
        Slot& s = slots_[idx];
        const std::uint32_t li = s.bucket / kBuckets;
        const std::uint32_t slot = s.bucket % kBuckets;
        Level& l = levels_[li];
        if (s.prev != kNullIndex)
            slots_[s.prev].next = s.next;
        else
            l.head[slot] = s.next;
        if (s.next != kNullIndex)
            slots_[s.next].prev = s.prev;
        else
            l.tail[slot] = s.prev;
        if (l.head[slot] == kNullIndex) {
            l.occupied &= ~(std::uint64_t{1} << slot);
            if (li == 0)
                l.bucket_sorted[slot] = false;
        }
        s.bucket = kNullIndex;
    }

    void free_slot(std::uint32_t idx) noexcept {
        Slot& s = slots_[idx];
        s.live = false;
        ++s.generation;
        s.free_next = free_head_;
        free_head_ = idx;
    }

    void relink_free_list() noexcept {
        for (std::size_t i = 0; i < capacity_; ++i)
            slots_[i].free_next = static_cast<std::uint32_t>(i) + 1 < capacity_
                                      ? static_cast<std::uint32_t>(i) + 1
                                      : kNullIndex;
        free_head_ = 0;
    }

    [[nodiscard]] static constexpr std::uint64_t mask_low_bits(std::uint32_t bits) noexcept {
        return bits >= 64 ? ~std::uint64_t{0} : (std::uint64_t{1} << bits) - 1;
    }

    // Min-seq entry of the current tick under the sorted fast path, else a
    // linear scan of the chain.
    [[nodiscard]] std::uint32_t front_of_tick() noexcept {
        const std::uint32_t slot = static_cast<std::uint32_t>(now_) & 63U;
        Level& l = levels_[0];
        const std::uint32_t head = l.head[slot];
        MOG_PRE(head != kNullIndex);
        if (l.bucket_sorted[slot])
            return head;
        std::uint32_t best = head;
        for (std::uint32_t it = slots_[head].next; it != kNullIndex; it = slots_[it].next)
            if (slots_[it].seq < slots_[best].seq)
                best = it;
        return best;
    }

    [[nodiscard]] std::uint32_t take_front_of_tick() noexcept {
        const std::uint32_t idx = front_of_tick();
        detach(idx);
        return idx;
    }

    // Advances the watermark to the next pending timestamp, demoting along
    // the way, so the current level 0 bucket ends up holding exactly the
    // events due at that nanosecond. Each demotion sends an event strictly
    // deeper, bounding total work by kLevels per event over its lifetime.
    void settle() noexcept {
        for (;;) {
            if (live_ == 0)
                return;
            const std::uint32_t slot0 = static_cast<std::uint32_t>(now_) & 63U;
            const std::uint64_t occ0 = levels_[0].occupied;
            if (((occ0 >> slot0) & 1ULL) != 0)
                return; // events due exactly now exist
            // Later slots inside the current 64 ns window always win over
            // coarse boundaries: they are less than 64 ns ahead while any
            // coarse step is at least 64 ns.
            const std::uint64_t later = occ0 & ~mask_low_bits(slot0 + 1);
            if (later != 0) {
                now_ = (now_ & ~std::uint64_t{63}) +
                       static_cast<std::uint32_t>(std::countr_zero(later));
                continue;
            }
            // Nearest coarse boundary across levels 1..kLevels-1. By I2 all
            // occupied slots are strictly ahead of the watermark's digit, so
            // the lowest set bit is the nearest.
            std::uint64_t best = ~std::uint64_t{0};
            for (std::uint32_t li = 1; li < kLevels; ++li) {
                const std::uint64_t occ = levels_[li].occupied;
                if (occ == 0)
                    continue;
                const std::uint32_t j = static_cast<std::uint32_t>(std::countr_zero(occ));
                const std::uint64_t bound = (now_ & ~mask_low_bits(kBitsPerLevel * (li + 1))) |
                                            (static_cast<std::uint64_t>(j) << (kBitsPerLevel * li));
                best = std::min(best, bound);
            }
            if (best == ~std::uint64_t{0})
                return; // unreachable while live_ > 0; kept defensive
            now_ = best;
            // Demote every bucket at or behind the new watermark's digit; all
            // of them re-route strictly deeper.
            for (std::uint32_t li = 1; li < kLevels; ++li) {
                const std::uint32_t nd =
                    static_cast<std::uint32_t>((now_ >> (kBitsPerLevel * li)) & 63U);
                std::uint64_t to_demote = levels_[li].occupied & mask_low_bits(nd + 1);
                while (to_demote != 0) {
                    const std::uint32_t slot =
                        static_cast<std::uint32_t>(std::countr_zero(to_demote));
                    to_demote &= to_demote - 1;
                    std::uint32_t idx = levels_[li].head[slot];
                    while (idx != kNullIndex) {
                        const std::uint32_t nxt = slots_[idx].next;
                        detach(idx);
                        attach(idx);
                        idx = nxt;
                    }
                }
            }
        }
    }

    std::size_t capacity_;
    Slot* slots_;
    std::uint32_t free_head_{kNullIndex};
    std::size_t live_{0};
    std::uint64_t now_{0};
    std::uint64_t next_seq_{0};
    std::array<Level, kLevels> levels_{};
};

} // namespace mog
