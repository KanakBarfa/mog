// Overrides all operator new flavors; armed allocations and OOM abort deterministically.

#include <mog/AllocGuard.hpp>

#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <new>
#include <unistd.h>

namespace mog::alloc {

namespace detail {

inline std::atomic<bool>& armed_word() noexcept {
    static std::atomic<bool> word{false};
    return word;
}

inline void write_line(const char* msg, std::size_t len) noexcept {
    const char* cursor = msg;
    std::size_t remaining = len;
    while (remaining > 0) {
        ssize_t written = ::write(2, cursor, remaining);
        if (written < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        cursor += written;
        remaining -= static_cast<std::size_t>(written);
    }
}

[[noreturn]] inline void die(const char* msg) noexcept {
    write_line(msg, __builtin_strlen(msg));
    std::signal(SIGABRT, SIG_DFL);
    std::abort();
}

} // namespace detail

bool arm() noexcept {
    return detail::armed_word().exchange(true);
}

bool disarm() noexcept {
    return detail::armed_word().exchange(false);
}

bool is_armed() noexcept {
    return detail::armed_word().load();
}

} // namespace mog::alloc

namespace {

[[noreturn]] void trip_guard() noexcept {
    mog::alloc::detail::die("MOG ALLOCATION VIOLATION heap allocation in guarded region\n");
}

[[noreturn]] void trip_oom() noexcept {
    mog::alloc::detail::die("MOG ALLOCATION FAILURE out of memory\n");
}

void* allocate(std::size_t count) noexcept {
    if (mog::alloc::is_armed())
        trip_guard();
    void* p = std::malloc(count);
    if (p == nullptr)
        trip_oom();
    return p;
}

void* allocate_aligned(std::size_t count, std::size_t alignment) noexcept {
    if (mog::alloc::is_armed())
        trip_guard();
    if (alignment < sizeof(void*))
        alignment = sizeof(void*);
    const std::size_t padded = (count + alignment - 1) / alignment * alignment;
    void* p = std::aligned_alloc(alignment, padded == 0 ? alignment : padded);
    if (p == nullptr)
        trip_oom();
    return p;
}

} // namespace

void* operator new(std::size_t count) {
    return allocate(count);
}
void* operator new[](std::size_t count) {
    return ::operator new(count);
}

void* operator new(std::size_t count, const std::nothrow_t&) noexcept {
    return allocate(count);
}
void* operator new[](std::size_t count, const std::nothrow_t&) noexcept {
    return ::operator new(count, std::nothrow);
}

void* operator new(std::size_t count, std::align_val_t alignment) {
    return allocate_aligned(count, static_cast<std::size_t>(alignment));
}
void* operator new[](std::size_t count, std::align_val_t alignment) {
    return ::operator new(count, alignment);
}

void* operator new(std::size_t count, std::align_val_t alignment, const std::nothrow_t&) noexcept {
    return allocate_aligned(count, static_cast<std::size_t>(alignment));
}
void* operator new[](std::size_t count, std::align_val_t alignment,
                     const std::nothrow_t&) noexcept {
    return ::operator new(count, alignment, std::nothrow);
}

void operator delete(void* p) noexcept {
    std::free(p);
}
void operator delete[](void* p) noexcept {
    std::free(p);
}
void operator delete(void* p, std::size_t) noexcept {
    std::free(p);
}
void operator delete[](void* p, std::size_t) noexcept {
    std::free(p);
}
void operator delete(void* p, const std::nothrow_t&) noexcept {
    std::free(p);
}
void operator delete[](void* p, const std::nothrow_t&) noexcept {
    std::free(p);
}

void operator delete(void* p, std::align_val_t) noexcept {
    std::free(p);
}
void operator delete[](void* p, std::align_val_t) noexcept {
    std::free(p);
}
void operator delete(void* p, std::size_t, std::align_val_t) noexcept {
    std::free(p);
}
void operator delete[](void* p, std::size_t, std::align_val_t) noexcept {
    std::free(p);
}
void operator delete(void* p, std::align_val_t, const std::nothrow_t&) noexcept {
    std::free(p);
}
void operator delete[](void* p, std::align_val_t, const std::nothrow_t&) noexcept {
    std::free(p);
}
