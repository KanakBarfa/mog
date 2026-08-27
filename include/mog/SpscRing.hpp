// Bounded wait-free SPSC ring for offloading telemetry from the
// single-threaded deterministic core to a background logger. Power-of-two
// capacity, cache-line-padded indices, trivially copyable slots.
//
// The ring itself never blocks: try_push/try_pop report fullness and the
// caller picks a policy (spin, drop, or synchronous fallback). A spin_push
// helper covers the common backpressure case.
#pragma once

#include <mog/Contracts.hpp>

#include <atomic>
#include <concepts>
#include <cstdint>
#include <new>
#include <thread>

namespace mog {

template <class T>
concept RingSlot = std::is_trivially_copyable_v<T> && std::is_nothrow_move_assignable_v<T>;

template <RingSlot T, std::size_t kCapacity>
class SpscRing {
    static_assert((kCapacity & (kCapacity - 1)) == 0, "capacity must be a power of two");

public:
    SpscRing() noexcept = default;
    SpscRing(const SpscRing&) = delete;
    SpscRing& operator=(const SpscRing&) = delete;

    [[nodiscard]] constexpr std::size_t capacity() const noexcept { return kCapacity; }

    // Producer side only.
    [[nodiscard]] bool try_push(const T& v) noexcept {
        const std::uint64_t t = tail_.load(std::memory_order_relaxed);
        if (t - head_cache_ >= kCapacity) {
            head_cache_ = head_.load(std::memory_order_acquire);
            if (t - head_cache_ >= kCapacity)
                return false;
        }
        slots_[t & kMask] = v;
        tail_.store(t + 1, std::memory_order_release);
        return true;
    }

    void push_spin(const T& v) noexcept {
        while (!try_push(v))
            std::this_thread::yield();
    }

    // Consumer side only.
    [[nodiscard]] bool try_pop(T& out) noexcept {
        const std::uint64_t h = head_.load(std::memory_order_relaxed);
        if (h >= tail_cache_) {
            tail_cache_ = tail_.load(std::memory_order_acquire);
            if (h >= tail_cache_)
                return false;
        }
        out = slots_[h & kMask];
        head_.store(h + 1, std::memory_order_release);
        return true;
    }

    [[nodiscard]] std::size_t size() const noexcept {
        const auto t = tail_.load(std::memory_order_acquire);
        const auto h = head_.load(std::memory_order_acquire);
        return static_cast<std::size_t>(t - h);
    }

private:
    static constexpr std::uint64_t kMask = kCapacity - 1;
    struct alignas(64) Pad {
        char c[64];
    };

    alignas(64) std::atomic<std::uint64_t> head_{0}; // consumer cursor
    alignas(64) Pad pad1_;
    alignas(64) std::atomic<std::uint64_t> tail_{0}; // producer cursor
    alignas(64) Pad pad2_;
    // Producer-private / consumer-private caches avoid sharing the peer's
    // line on every operation.
    std::uint64_t head_cache_ = 0; // written by producer
    std::uint64_t tail_cache_ = 0; // written by consumer
    alignas(64) T slots_[kCapacity]{};
};

} // namespace mog
