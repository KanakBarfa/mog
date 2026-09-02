// Hardware Performance Monitoring Unit (PMU) counter interface via Linux perf_event_open.
// Collects instructions retired, CPU cycles, branch mispredictions, and L1 cache misses with zero
// overhead.
#pragma once

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <unistd.h>

#if defined(__linux__)
#include <linux/perf_event.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#endif

namespace mog::pmu {

struct PmuStats {
    std::uint64_t instructions = 0;
    std::uint64_t cycles = 0;
    std::uint64_t branches = 0;
    std::uint64_t branch_misses = 0;
    std::uint64_t l1d_misses = 0;

    [[nodiscard]] double ipc() const noexcept {
        return cycles > 0 ? static_cast<double>(instructions) / static_cast<double>(cycles) : 0.0;
    }
    [[nodiscard]] double branch_miss_rate_pct() const noexcept {
        return branches > 0
                   ? (100.0 * static_cast<double>(branch_misses) / static_cast<double>(branches))
                   : 0.0;
    }
};

class PmuCollector {
public:
    PmuCollector() noexcept {
#if defined(__linux__) && defined(__NR_perf_event_open)
        fd_instructions_ = open_hw_counter(PERF_COUNT_HW_INSTRUCTIONS);
        fd_cycles_ = open_hw_counter(PERF_COUNT_HW_CPU_CYCLES);
        fd_branches_ = open_hw_counter(PERF_COUNT_HW_BRANCH_INSTRUCTIONS);
        fd_branch_misses_ = open_hw_counter(PERF_COUNT_HW_BRANCH_MISSES);
        fd_l1d_misses_ = open_cache_counter(PERF_COUNT_HW_CACHE_L1D, PERF_COUNT_HW_CACHE_OP_READ,
                                            PERF_COUNT_HW_CACHE_RESULT_MISS);

        available_ = (fd_instructions_ >= 0 && fd_cycles_ >= 0);
#endif
    }

    ~PmuCollector() noexcept {
        close_fd(fd_instructions_);
        close_fd(fd_cycles_);
        close_fd(fd_branches_);
        close_fd(fd_branch_misses_);
        close_fd(fd_l1d_misses_);
    }

    PmuCollector(const PmuCollector&) = delete;
    PmuCollector& operator=(const PmuCollector&) = delete;

    [[nodiscard]] bool is_available() const noexcept { return available_; }

    void start() noexcept {
#if defined(__linux__)
        if (!available_)
            return;
        reset_and_enable(fd_instructions_);
        reset_and_enable(fd_cycles_);
        reset_and_enable(fd_branches_);
        reset_and_enable(fd_branch_misses_);
        reset_and_enable(fd_l1d_misses_);
#endif
    }

    [[nodiscard]] PmuStats stop() noexcept {
        PmuStats stats{};
#if defined(__linux__)
        if (!available_)
            return stats;
        disable_counter(fd_instructions_);
        disable_counter(fd_cycles_);
        disable_counter(fd_branches_);
        disable_counter(fd_branch_misses_);
        disable_counter(fd_l1d_misses_);

        stats.instructions = read_counter(fd_instructions_);
        stats.cycles = read_counter(fd_cycles_);
        stats.branches = read_counter(fd_branches_);
        stats.branch_misses = read_counter(fd_branch_misses_);
        stats.l1d_misses = read_counter(fd_l1d_misses_);
#endif
        return stats;
    }

private:
#if defined(__linux__) && defined(__NR_perf_event_open)
    static int open_hw_counter(std::uint64_t config) noexcept {
        struct perf_event_attr pe{};
        pe.type = PERF_TYPE_HARDWARE;
        pe.size = sizeof(struct perf_event_attr);
        pe.config = config;
        pe.disabled = 1;
        pe.exclude_kernel = 1;
        pe.exclude_hv = 1;
        return static_cast<int>(::syscall(__NR_perf_event_open, &pe, 0, -1, -1, 0));
    }

    static int open_cache_counter(std::uint64_t cache_id, std::uint64_t op_id,
                                  std::uint64_t result_id) noexcept {
        struct perf_event_attr pe{};
        pe.type = PERF_TYPE_HW_CACHE;
        pe.size = sizeof(struct perf_event_attr);
        pe.config = cache_id | (op_id << 8) | (result_id << 16);
        pe.disabled = 1;
        pe.exclude_kernel = 1;
        pe.exclude_hv = 1;
        return static_cast<int>(::syscall(__NR_perf_event_open, &pe, 0, -1, -1, 0));
    }

    static void reset_and_enable(int fd) noexcept {
        if (fd < 0)
            return;
        static_cast<void>(::ioctl(fd, PERF_EVENT_IOC_RESET, 0));
        static_cast<void>(::ioctl(fd, PERF_EVENT_IOC_ENABLE, 0));
    }

    static void disable_counter(int fd) noexcept {
        if (fd < 0)
            return;
        static_cast<void>(::ioctl(fd, PERF_EVENT_IOC_DISABLE, 0));
    }

    static std::uint64_t read_counter(int fd) noexcept {
        if (fd < 0)
            return 0;
        std::uint64_t val = 0;
        const auto n = ::read(fd, &val, sizeof(val));
        return (n == sizeof(val)) ? val : 0;
    }
#endif

    static void close_fd(int& fd) noexcept {
#if defined(__linux__)
        if (fd >= 0) {
            ::close(fd);
            fd = -1;
        }
#else
        fd = -1;
#endif
    }

    bool available_ = false;
    int fd_instructions_ = -1;
    int fd_cycles_ = -1;
    int fd_branches_ = -1;
    int fd_branch_misses_ = -1;
    int fd_l1d_misses_ = -1;
};

class ScopedPmu {
public:
    explicit ScopedPmu(PmuCollector& col, PmuStats& out) noexcept : col_(col), out_(out) {
        col_.start();
    }
    ~ScopedPmu() noexcept { out_ = col_.stop(); }

    ScopedPmu(const ScopedPmu&) = delete;
    ScopedPmu& operator=(const ScopedPmu&) = delete;

private:
    PmuCollector& col_;
    PmuStats& out_;
};

} // namespace mog::pmu
