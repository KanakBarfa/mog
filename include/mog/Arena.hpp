// Flat cacheline-aligned order arena with intrusive free list and
// generational handles, giving O(1) acquire/release with no ABA reuse.
#pragma once

#include <mog/Contracts.hpp>

#include <cstddef>
#include <cstdint>
#include <new>
#include <type_traits>
#include <utility>

namespace mog {

inline constexpr std::uint32_t kNullIndex = UINT32_MAX;

template <class Tag>
struct GenHandle {
    std::uint32_t index = kNullIndex;
    std::uint32_t generation = 0;

    [[nodiscard]] constexpr bool is_null() const noexcept { return index == kNullIndex; }
    friend constexpr bool operator==(const GenHandle&, const GenHandle&) = default;
};

// T must be trivially destructible; slots are recycled without destruction.
template <class T, class HandleTag>
class Arena {
public:
    using Handle = GenHandle<HandleTag>;
    static constexpr std::size_t kAlign = 64;

    struct alignas(kAlign) Slot {
        T value{};
        std::uint32_t generation{0};
        std::uint32_t free_next{kNullIndex};
        bool live{false};
    };
    static_assert(std::is_trivially_destructible_v<T>);

    explicit Arena(std::size_t capacity)
        : capacity_(capacity), slots_(static_cast<Slot*>(::operator new(
                                   capacity * sizeof(Slot), std::align_val_t{kAlign}))) {
        MOG_PRE(capacity > 0 && capacity < kNullIndex);
        // Lazy slots: built on first acquire in index order, matching the old order.
    }

    ~Arena() { ::operator delete(slots_, std::align_val_t{kAlign}); }

    Arena(const Arena&) = delete;
    Arena& operator=(const Arena&) = delete;

    [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }
    [[nodiscard]] std::size_t live_count() const noexcept { return live_; }

    // Returns kNullHandle when exhausted; caller maps that to a recoverable error.
    template <class... Args>
    [[nodiscard]] Handle acquire(Args&&... args) noexcept {
        std::uint32_t idx = free_head_;
        if (idx == kNullIndex) {
            if (watermark_ >= capacity_)
                return Handle{};
            idx = static_cast<std::uint32_t>(watermark_++);
            static_cast<void>(new (&slots_[idx]) Slot{});
        } else {
            free_head_ = slots_[idx].free_next;
        }
        Slot& s = slots_[idx];
        s.live = true;
        ++live_;
        T* placed = &s.value;
        *placed = T{std::forward<Args>(args)...};
        return Handle{idx, s.generation};
    }

    // Bumps the generation so handles captured before this call stop validating.
    void release(Handle h) noexcept {
        MOG_PRE(valid(h));
        Slot& s = slots_[h.index];
        s.live = false;
        ++s.generation;
        s.free_next = free_head_;
        free_head_ = h.index;
        --live_;
    }

    [[nodiscard]] bool valid(Handle h) const noexcept {
        if (h.index >= capacity_)
            return false;
        const Slot& s = slots_[h.index];
        return s.live && s.generation == h.generation;
    }

    [[nodiscard]] const T& at(Handle h) const noexcept {
        MOG_PRE(valid(h));
        return slots_[h.index].value;
    }
    [[nodiscard]] T& at(Handle h) noexcept {
        MOG_PRE(valid(h));
        return slots_[h.index].value;
    }

    [[nodiscard]] const T& at_index(std::uint32_t i) const noexcept {
        MOG_PRE(i < capacity_);
        return slots_[i].value;
    }
    [[nodiscard]] T& at_index(std::uint32_t i) noexcept {
        MOG_PRE(i < capacity_);
        return slots_[i].value;
    }

    [[nodiscard]] const Slot& slot(std::size_t i) const noexcept {
        MOG_PRE(i < capacity_);
        return slots_[i];
    }

private:
    std::size_t capacity_ = 0;
    std::size_t live_ = 0;
    std::size_t watermark_ = 0; // slots below this are constructed
    std::uint32_t free_head_ = kNullIndex;
    Slot* slots_ = nullptr;
};

} // namespace mog
