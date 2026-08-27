// Discrete-event scheduler: fixed-capacity binary heap over an arena of
// generational slots. Total order is (timestamp, insertion sequence), so the
// pop stream is a pure function of the op sequence and fully deterministic.
#pragma once

#include <mog/Arena.hpp>
#include <mog/Contracts.hpp>

#include <cstddef>
#include <cstdint>
#include <new>
#include <type_traits>
#include <vector>

namespace mog {

struct EventTag {};
using EventHandle = GenHandle<EventTag>;

// T must be trivially copyable: pop returns by value and slots are recycled
// without destruction, mirroring Arena's discipline.
template <typename T>
class Scheduler {
public:
    using value_type = T;
    static_assert(std::is_trivially_copyable_v<T>, "Scheduler stores trivially copyable events");

    struct HeapEntry {
        std::uint64_t ts;
        std::uint64_t seq;
        std::uint32_t slot;
    };
    struct Slot {
        T value{};
        std::uint64_t seq{0};
        std::uint32_t generation{0};
        std::uint32_t heap_pos{0};
        std::uint32_t free_next{kNullIndex};
        bool live{false};
    };

    explicit Scheduler(std::size_t capacity)
        : capacity_(capacity), slots_(static_cast<Slot*>(::operator new(capacity * sizeof(Slot)))) {
        MOG_PRE(capacity > 0 && capacity < kNullIndex);
        // Construct every slot so free-slot metadata is defined; slots are
        // recycled afterwards without further construction.
        for (std::size_t i = 0; i < capacity_; ++i)
            static_cast<void>(new (&slots_[i]) Slot{});
        for (std::size_t i = 0; i < capacity_; ++i)
            slots_[i].free_next = static_cast<std::uint32_t>(i) + 1 < capacity_
                                      ? static_cast<std::uint32_t>(i) + 1
                                      : kNullIndex;
        free_head_ = 0;
        heap_.reserve(capacity_);
    }

    ~Scheduler() { ::operator delete(slots_); }

    Scheduler(const Scheduler&) = delete;
    Scheduler& operator=(const Scheduler&) = delete;

    [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }
    [[nodiscard]] std::size_t size() const noexcept { return heap_.size(); }
    [[nodiscard]] bool empty() const noexcept { return heap_.empty(); }

    // Schedules at an arbitrary timestamp; monotonic push order is not required.
    // Equal timestamps pop in push order via the unique sequence number.
    [[nodiscard]] EventHandle push(std::uint64_t ts, const T& value) noexcept {
        MOG_PRE(heap_.size() < capacity_);
        const std::uint32_t idx = free_head_;
        MOG_PRE(idx != kNullIndex);
        Slot& s = slots_[idx];
        free_head_ = s.free_next;
        s.value = value;
        s.seq = next_seq_++;
        s.live = true;
        s.heap_pos = static_cast<std::uint32_t>(heap_.size());
        heap_.push_back(HeapEntry{ts, s.seq, idx});
        sift_up(s.heap_pos);
        return EventHandle{idx, s.generation};
    }

    // Returns false for stale handles (already popped/cancelled) or bad indices.
    bool cancel(EventHandle h) noexcept {
        if (h.index >= capacity_)
            return false;
        Slot& s = slots_[h.index];
        if (!s.live || s.generation != h.generation)
            return false;
        remove_at(s.heap_pos);
        free_slot(h.index);
        return true;
    }

    [[nodiscard]] const T& peek_min() const noexcept {
        MOG_PRE(!heap_.empty());
        return slots_[heap_.front().slot].value;
    }
    [[nodiscard]] std::uint64_t peek_min_ts() const noexcept {
        MOG_PRE(!heap_.empty());
        return heap_.front().ts;
    }

    [[nodiscard]] T pop_min() noexcept {
        MOG_PRE(!heap_.empty());
        const HeapEntry root = heap_.front();
        Slot& s = slots_[root.slot];
        T out = s.value;
        remove_at(0);
        free_slot(root.slot);
        // Every remaining event must sort no earlier than the one popped.
        MOG_POST(heap_.empty() || !entry_less(heap_.front(), root));
        return out;
    }

    void clear() noexcept {
        heap_.clear();
        for (std::size_t i = 0; i < capacity_; ++i) {
            Slot& s = slots_[i];
            if (s.live)
                ++s.generation; // stale live handles must not resurrect
            s.live = false;
            s.free_next = static_cast<std::uint32_t>(i) + 1 < capacity_
                              ? static_cast<std::uint32_t>(i) + 1
                              : kNullIndex;
        }
        free_head_ = 0;
        next_seq_ = 0;
    }

    // Full structural check: heap ordering, position backlinks, seq agreement,
    // and free-list length. O(n); intended for tests and debug tooling.
    [[nodiscard]] bool audit() const noexcept {
        if (heap_.size() > capacity_)
            return false;
        for (std::size_t i = 0; i < heap_.size(); ++i) {
            const HeapEntry& e = heap_[i];
            if (e.slot >= capacity_)
                return false;
            const Slot& s = slots_[e.slot];
            if (!s.live || s.heap_pos != i || s.seq != e.seq)
                return false;
            if (i > 0) {
                const HeapEntry& p = heap_[(i - 1) / 2];
                if (entry_less(e, p)) // child must never sort before its parent
                    return false;
            }
        }
        std::size_t free_len = 0;
        std::uint32_t cursor = free_head_;
        while (cursor != kNullIndex) {
            if (cursor >= capacity_ || slots_[cursor].live)
                return false;
            cursor = slots_[cursor].free_next;
            ++free_len;
        }
        return free_len == capacity_ - heap_.size();
    }

private:
    // Strict total order: timestamp first, unique insertion sequence second.
    static bool entry_less(const HeapEntry& a, const HeapEntry& b) noexcept {
        return a.ts < b.ts || (a.ts == b.ts && a.seq < b.seq);
    }

    void swap_entries(std::uint32_t a, std::uint32_t b) noexcept {
        HeapEntry tmp = heap_[a];
        heap_[a] = heap_[b];
        heap_[b] = tmp;
        slots_[heap_[a].slot].heap_pos = a;
        slots_[heap_[b].slot].heap_pos = b;
    }

    void sift_up(std::uint32_t i) noexcept {
        while (i > 0 && entry_less(heap_[i], heap_[(i - 1) / 2])) {
            swap_entries(i, (i - 1) / 2);
            i = (i - 1) / 2;
        }
    }

    void sift_down(std::uint32_t i) noexcept {
        const std::uint32_t n = static_cast<std::uint32_t>(heap_.size());
        for (;;) {
            const std::uint32_t l = 2 * i + 1;
            const std::uint32_t r = 2 * i + 2;
            std::uint32_t best = i;
            if (l < n && entry_less(heap_[l], heap_[best]))
                best = l;
            if (r < n && entry_less(heap_[r], heap_[best]))
                best = r;
            if (best == i)
                return;
            swap_entries(i, best);
            i = best;
        }
    }

    // Detaches the entry at heap position pos, restoring the invariant by
    // moving the last entry there and sifting both directions as needed.
    void remove_at(std::uint32_t pos) noexcept {
        const std::uint32_t last = static_cast<std::uint32_t>(heap_.size() - 1);
        swap_entries(pos, last);
        heap_.pop_back();
        if (pos >= heap_.size())
            return;
        if (pos > 0 && entry_less(heap_[pos], heap_[(pos - 1) / 2]))
            sift_up(pos);
        else
            sift_down(pos);
    }

    void free_slot(std::uint32_t idx) noexcept {
        Slot& s = slots_[idx];
        s.live = false;
        ++s.generation;
        s.free_next = free_head_;
        free_head_ = idx;
    }

    std::size_t capacity_;
    Slot* slots_;
    std::uint32_t free_head_{kNullIndex};
    std::uint64_t next_seq_{0};
    std::vector<HeapEntry> heap_;
};

} // namespace mog
