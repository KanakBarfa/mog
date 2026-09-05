// Contract checking with three runtime modes and a determinism-preserving abort path.
#pragma once

#include <mog/Build.hpp>

#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <type_traits>
#include <unistd.h>

namespace mog::contracts {

enum class Mode : int { enforce = 0, observe = 1, ignore = 2 };

// Handler may return only in observe mode; in enforce mode it must not return.
using ViolationHandler = void (*)(const char* kind, const char* file, int line,
                                  const char* expr) noexcept;

Mode current_mode() noexcept;
Mode set_mode(Mode mode) noexcept;
ViolationHandler current_handler() noexcept;
ViolationHandler set_handler(ViolationHandler handler) noexcept;
bool active() noexcept;

// Called by contract macros; dispatches to the handler, defaulting to a
// fixed-format report on fd 2 followed by abort, with no allocation and no formatting.
void violate(const char* kind, const char* file, int line, const char* expr) noexcept;

namespace detail {

inline std::atomic<int>& mode_word() noexcept {
    static std::atomic<int> word{MOG_CONTRACTS_DEFAULT_MODE};
    return word;
}

inline std::atomic<ViolationHandler>& handler_word() noexcept {
    static std::atomic<ViolationHandler> word{nullptr};
    return word;
}

inline char* put_str(char* dst, const char* src) noexcept {
    while (*src != '\0')
        *dst++ = *src++;
    return dst;
}

inline char* put_u64(char* dst, std::uint64_t value) noexcept {
    char tmp[20];
    int n = 0;
    do {
        tmp[n++] = char('0' + value % 10);
        value /= 10;
    } while (value != 0);
    while (n > 0)
        *dst++ = tmp[--n];
    return dst;
}

inline void write_report(const char* kind, const char* file, int line, const char* expr) noexcept {
    char buf[512];
    char* p = buf;
    p = put_str(p, "MOG CONTRACT VIOLATION ");
    p = put_str(p, kind);
    *p++ = ' ';
    p = put_str(p, file);
    *p++ = ':';
    p = put_u64(p, static_cast<std::uint64_t>(line));
    *p++ = ' ';
    p = put_str(p, expr);
    *p++ = '\n';
    std::size_t remaining = static_cast<std::size_t>(p - buf);
    const char* cursor = buf;
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

} // namespace detail

inline Mode current_mode() noexcept {
    return static_cast<Mode>(detail::mode_word().load(std::memory_order_relaxed));
}

inline Mode set_mode(Mode mode) noexcept {
    return static_cast<Mode>(detail::mode_word().exchange(static_cast<int>(mode)));
}

inline ViolationHandler current_handler() noexcept {
    return detail::handler_word().load(std::memory_order_relaxed);
}

inline ViolationHandler set_handler(ViolationHandler handler) noexcept {
    return detail::handler_word().exchange(handler);
}

inline bool active() noexcept {
    return current_mode() != Mode::ignore;
}

inline void violate(const char* kind, const char* file, int line, const char* expr) noexcept {
    ViolationHandler handler = detail::handler_word().load();
    if (handler != nullptr) {
        handler(kind, file, line, expr);
        return;
    }
    detail::write_report(kind, file, line, expr);
    std::signal(SIGABRT, SIG_DFL);
    std::abort();
}

} // namespace mog::contracts

// Checks are a runtime mechanism: skipped under constant evaluation so that
// constexpr functions containing MOG_PRE/MOG_POST remain usable in constant
// expressions on strict frontends (AppleClang errors via -Winvalid-
// constexpr otherwise). violate() aborts, which is meaningless at compile
// time anyway.
// Default mode is only the default: set_mode() still flips at runtime, so
// the gate is one relaxed load plus a cold violation path.
#define MOG_CONTRACT_CHECK(cond, kind_str)                                                         \
    do {                                                                                           \
        if (!std::is_constant_evaluated()) {                                                       \
            if (::mog::contracts::active() && !(cond)) [[unlikely]] {                              \
                ::mog::contracts::violate(kind_str, __FILE__, __LINE__, #cond);                    \
            }                                                                                      \
        }                                                                                          \
    } while (false)

#define MOG_PRE(cond) MOG_CONTRACT_CHECK(cond, "pre")
#define MOG_POST(cond) MOG_CONTRACT_CHECK(cond, "post")
#define MOG_CONTRACT_ASSERT(cond) MOG_CONTRACT_CHECK(cond, "contract_assert")
