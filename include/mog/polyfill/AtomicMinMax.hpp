// Atomic fetch_max/fetch_min per P0493; CAS-loop fallback.
#pragma once

#include <mog/BuildFeatures.hpp>

#include <atomic>

namespace mog::atomics {

template <class T>
inline T fetch_max(std::atomic<T>* atomic, T value,
                   std::memory_order order = std::memory_order_seq_cst) noexcept {
#if MOG_HAS_ATOMIC_MIN_MAX
    return atomic->fetch_max(value, order);
#else
    T prev = atomic->load(std::memory_order_relaxed);
    while (value > prev &&
           !atomic->compare_exchange_weak(prev, value, order, std::memory_order_relaxed)) {
    }
    return prev;
#endif
}

template <class T>
inline T fetch_min(std::atomic<T>* atomic, T value,
                   std::memory_order order = std::memory_order_seq_cst) noexcept {
#if MOG_HAS_ATOMIC_MIN_MAX
    return atomic->fetch_min(value, order);
#else
    T prev = atomic->load(std::memory_order_relaxed);
    while (value < prev &&
           !atomic->compare_exchange_weak(prev, value, order, std::memory_order_relaxed)) {
    }
    return prev;
#endif
}

} // namespace mog::atomics
