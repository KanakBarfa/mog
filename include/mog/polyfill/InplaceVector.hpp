// Fixed-capacity vector per P0843 subset; contract abort instead of throw (no exceptions).
#pragma once

#include <mog/Build.hpp>
#include <mog/Contracts.hpp>

#if MOG_HAS_INPLACE_VECTOR
#include <inplace_vector>

namespace mog {
template <class T, std::size_t N>
using inplace_vector = std::inplace_vector<T, N>;
}

#else

#include <cstddef>
#include <new>
#include <type_traits>
#include <utility>

namespace mog {

template <class T, std::size_t N>
class inplace_vector {
public:
    using value_type = T;

    constexpr inplace_vector() noexcept = default;
    constexpr ~inplace_vector() { clear(); }

    constexpr inplace_vector(const inplace_vector& other)
        requires(std::is_copy_constructible_v<T>)
        : inplace_vector() {
        for (std::size_t i = 0; i < other.size_; ++i)
            emplace_back(other.data()[i]);
    }
    constexpr inplace_vector& operator=(const inplace_vector& other)
        requires(std::is_copy_assignable_v<T> && std::is_copy_constructible_v<T>)
    {
        if (this != &other) {
            clear();
            for (std::size_t i = 0; i < other.size_; ++i)
                emplace_back(other.data()[i]);
        }
        return *this;
    }

    static constexpr std::size_t capacity() noexcept { return N; }
    constexpr std::size_t size() const noexcept { return size_; }
    constexpr bool empty() const noexcept { return size_ == 0; }
    constexpr bool full() const noexcept { return size_ == N; }

    constexpr T* begin() noexcept { return data(); }
    constexpr const T* begin() const noexcept { return data(); }
    constexpr T* end() noexcept { return data() + size_; }
    constexpr const T* end() const noexcept { return data() + size_; }

    constexpr T& operator[](std::size_t i) noexcept {
        MOG_PRE(i < size_);
        return data()[i];
    }
    constexpr const T& operator[](std::size_t i) const noexcept {
        MOG_PRE(i < size_);
        return data()[i];
    }

    template <class... Args>
    constexpr T& emplace_back(Args&&... args) {
        MOG_PRE(size_ < N);
        if constexpr (std::is_trivially_copyable_v<T>) {
            // Plain slot assignment keeps the hot path to a couple of stores;
            // byte-storage placement new hides aliasing facts from optimizers.
            T& slot = slots_[size_];
            slot = T{static_cast<Args&&>(args)...};
            ++size_;
            return slot;
        } else {
            T* slot = ::new (static_cast<void*>(data() + size_)) T(static_cast<Args&&>(args)...);
            ++size_;
            return *slot;
        }
    }

    constexpr void push_back(const T& v) { emplace_back(v); }
    constexpr void push_back(T&& v) { emplace_back(static_cast<T&&>(v)); }

    constexpr void pop_back() noexcept {
        MOG_PRE(size_ > 0);
        if constexpr (!std::is_trivially_destructible_v<T>)
            data()[--size_].~T();
        else
            --size_;
    }

    constexpr void clear() noexcept {
        if constexpr (!std::is_trivially_destructible_v<T>)
            for (std::size_t i = 0; i < size_; ++i)
                data()[i].~T();
        size_ = 0;
    }

private:
    constexpr T* data() noexcept {
        if constexpr (std::is_trivially_copyable_v<T>)
            return slots_.data();
        else
            return reinterpret_cast<T*>(storage_);
    }
    constexpr const T* data() const noexcept {
        if constexpr (std::is_trivially_copyable_v<T>)
            return slots_.data();
        else
            return reinterpret_cast<const T*>(storage_);
    }

    union {
        alignas(T) unsigned char storage_[sizeof(T) * N]; // non-trivial payloads
        std::array<T, N> slots_{};                        // trivially copyable payloads
    };
    std::size_t size_ = 0;
};

} // namespace mog

#endif
