// Task-granularity sweep: the same total work split into tasks of different
// sizes. Every task pays a fixed cost: two allocations, the queue lock, a
// future, and -- when workers drain the queue faster than it's filled -- a
// condvar wakeup of a sleeping worker (a futex syscall; ~20us under WSL2,
// a few us on bare metal). When a task does less work than that, the pool is
// slower than just running the loop on one thread.

#include "ThreadPool.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <future>
#include <iomanip>
#include <iostream>
#include <vector>

using Clock = std::chrono::steady_clock;

namespace {

constexpr std::uint64_t kTotalUnits = 1ull << 22;  // ~4M units of work
constexpr int kRepeats = 3;

// One "unit" = one xorshift step (~1ns). The chain can't be optimised away
// or vectorised, so cost scales linearly with `units`.
std::uint64_t spin(std::uint64_t seed, std::uint64_t units) {
    std::uint64_t x = seed | 1;
    for (std::uint64_t i = 0; i < units; ++i) {
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
    }
    return x;
}

std::uint64_t run_sequential(std::uint64_t task_size) {
    std::uint64_t acc = 0;
    for (std::uint64_t t = 0; t < kTotalUnits / task_size; ++t) acc += spin(t, task_size);
    return acc;
}

std::uint64_t run_pool(ThreadPool& pool, std::uint64_t task_size) {
    const std::uint64_t tasks = kTotalUnits / task_size;
    std::vector<std::future<std::uint64_t>> futures;
    futures.reserve(tasks);
    for (std::uint64_t t = 0; t < tasks; ++t) {
        futures.push_back(pool.submit([t, task_size] { return spin(t, task_size); }));
    }
    std::uint64_t acc = 0;
    for (auto& f : futures) acc += f.get();
    return acc;
}

template <typename Fn>
double best_ms(Fn fn, std::uint64_t& result) {
    double best = 1e18;
    for (int r = 0; r < kRepeats; ++r) {
        const auto start = Clock::now();
        result = fn();
        best = std::min(best, std::chrono::duration<double, std::milli>(Clock::now() - start).count());
    }
    return best;
}

}  // namespace

int main() {
    // One pool reused for every row, so thread start-up cost isn't measured.
    ThreadPool pool;

    std::cout << "total work: " << kTotalUnits << " units, workers: " << pool.size()
              << ", best of " << kRepeats << "\n\n"
              << std::setw(12) << "units/task" << std::setw(10) << "tasks" << std::setw(11)
              << "seq (ms)" << std::setw(12) << "pool (ms)" << std::setw(10) << "speedup"
              << std::setw(16) << "us/task (pool)" << '\n'
              << std::fixed;

    for (std::uint64_t size = 16; size <= (1u << 16); size *= 4) {
        std::uint64_t seq_result = 0, pool_result = 0;
        const double seq = best_ms([size] { return run_sequential(size); }, seq_result);
        const double par = best_ms([&pool, size] { return run_pool(pool, size); }, pool_result);
        if (seq_result != pool_result) {
            std::cerr << "result mismatch at size " << size << '\n';
            return 1;
        }
        const std::uint64_t tasks = kTotalUnits / size;
        std::cout << std::setprecision(1) << std::setw(12) << size << std::setw(10) << tasks
                  << std::setw(11) << seq << std::setw(12) << par << std::setw(9)
                  << std::setprecision(2) << seq / par << 'x' << std::setw(16)
                  << std::setprecision(3) << 1000.0 * par / static_cast<double>(tasks) << '\n';
    }
}
