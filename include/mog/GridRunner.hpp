// Lock-free multi-threaded parameter grid backtest runner.
// Distributes independent simulation sweeps across worker threads with zero synchronization.
#pragma once

#include <mog/Contracts.hpp>
#include <mog/Simulate.hpp>
#include <mog/Types.hpp>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <thread>
#include <vector>

namespace mog {

struct GridTask {
    std::size_t task_id = 0;
    SimConfig config{};
};

struct GridResult {
    std::size_t task_id = 0;
    std::uint64_t digest = 0;
    std::size_t total_fills = 0;
    std::int64_t total_filled_shares = 0;
    std::int64_t total_filled_notional = 0;
    std::int64_t total_fees = 0;
};

class ParallelGridRunner {
public:
    explicit ParallelGridRunner(std::size_t num_threads = 0) noexcept
        : thread_count_(num_threads > 0 ? num_threads
                                        : std::max(1u, std::thread::hardware_concurrency())) {}

    [[nodiscard]] std::size_t concurrency() const noexcept { return thread_count_; }

    // Runs a collection of tasks against a simulator factory in parallel.
    // Results are deterministically mapped by task_id.
    template <typename TaskFn>
    [[nodiscard]] std::vector<GridResult> run_grid(const std::vector<GridTask>& tasks,
                                                   TaskFn&& task_fn) const {
        if (tasks.empty())
            return {};

        std::vector<GridResult> results(tasks.size());
        std::atomic<std::size_t> next_index{0};
        const std::size_t total_tasks = tasks.size();
        const std::size_t workers = std::min(thread_count_, total_tasks);

        auto worker_loop = [&]() {
            for (;;) {
                const std::size_t idx = next_index.fetch_add(1, std::memory_order_relaxed);
                if (idx >= total_tasks)
                    break;
                const auto& task = tasks[idx];
                results[idx] = task_fn(task);
                results[idx].task_id = task.task_id;
            }
        };

        if (workers <= 1) {
            worker_loop();
        } else {
            std::vector<std::thread> threads;
            threads.reserve(workers);
            for (std::size_t w = 0; w < workers; ++w) {
                threads.emplace_back(worker_loop);
            }
            for (auto& t : threads) {
                t.join();
            }
        }

        return results;
    }

private:
    std::size_t thread_count_;
};

} // namespace mog
