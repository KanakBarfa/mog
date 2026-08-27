// Snapshot-ring groundwork: fixed-capacity ring of level-transition deltas
// with monotonic sequence numbers, foldable to reconstruct L2 state.
#pragma once

#include <mog/Contracts.hpp>

#include <array>
#include <cstddef>
#include <cstdint>

namespace mog {

struct LevelDelta {
    std::int64_t price_ticks = 0;
    std::int32_t qty_delta = 0; // signed change applied at that level
    std::uint8_t side = 0;      // Side as char ('B'/'S')
};

struct Transition {
    std::uint64_t seq = 0;
    LevelDelta delta{};
};

// Overwrites the oldest entry when full; dropped_count exposes that fact.
template <std::size_t kCapacity>
class DeltaRing {
public:
    static_assert(kCapacity > 0 && (kCapacity & (kCapacity - 1)) == 0,
                  "capacity must be a power of two");

    void push(const LevelDelta& d) noexcept {
        MOG_PRE(d.qty_delta != 0);
        slots_[head_].seq = next_seq_;
        slots_[head_].delta = d;
        head_ = (head_ + 1) & kMask;
        if (size_ < kCapacity)
            ++size_;
        else
            ++dropped_;
        ++next_seq_;
    }

    [[nodiscard]] std::size_t size() const noexcept { return size_; }
    [[nodiscard]] std::uint64_t dropped_count() const noexcept { return dropped_; }
    [[nodiscard]] std::uint64_t next_seq() const noexcept { return next_seq_; }

    // Oldest-to-newest traversal of live entries.
    template <class Fn>
    void for_each(Fn&& fn) const noexcept {
        const std::size_t base = size_ < kCapacity ? 0 : head_;
        for (std::size_t i = 0; i < size_; ++i)
            fn(slots_[(base + i) & kMask]);
    }

private:
    static constexpr std::size_t kMask = kCapacity - 1;
    std::array<Transition, kCapacity> slots_{};
    std::size_t head_ = 0;
    std::size_t size_ = 0;
    std::uint64_t next_seq_ = 0;
    std::uint64_t dropped_ = 0;
};

} // namespace mog
