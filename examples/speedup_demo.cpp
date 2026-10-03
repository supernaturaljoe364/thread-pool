// Same CPU-bound workload run twice: once on the calling thread, once split
// across the pool. Shows why a thread pool exists -- independent work spread
// over all cores finishes roughly cores-times faster.

#include "ThreadPool.hpp"

#include <chrono>
#include <cstdint>
#include <future>
#include <iomanip>
#include <iostream>
#include <vector>

using Clock = std::chrono::steady_clock;

namespace {

bool is_prime(std::uint64_t n) {
    if (n < 2) return false;
    if (n % 2 == 0) return n == 2;
    for (std::uint64_t d = 3; d * d <= n; d += 2) {
        if (n % d == 0) return false;
    }
    return true;
}

// The "expensive task": count primes in [lo, hi). Pure function, no shared
// state, so chunks can run in any order on any thread.
std::uint64_t count_primes(std::uint64_t lo, std::uint64_t hi) {
    std::uint64_t count = 0;
    for (std::uint64_t n = lo; n < hi; ++n) {
        count += is_prime(n);
    }
    return count;
}

double ms_since(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

}  // namespace

int main() {
    constexpr std::uint64_t kChunks = 32;
    constexpr std::uint64_t kChunkSize = 200'000;
    const std::size_t workers = ThreadPool::default_thread_count();

    // Sequential baseline.
    auto start = Clock::now();
    std::uint64_t sequential_total = 0;
    for (std::uint64_t i = 0; i < kChunks; ++i) {
        sequential_total += count_primes(i * kChunkSize, (i + 1) * kChunkSize);
    }
    const double sequential_ms = ms_since(start);

    // Same chunks through the pool. Timing includes pool startup and
    // shutdown, so the comparison is honest.
    start = Clock::now();
    std::uint64_t pool_total = 0;
    {
        ThreadPool pool(workers);
        std::vector<std::future<std::uint64_t>> parts;
        for (std::uint64_t i = 0; i < kChunks; ++i) {
            parts.push_back(
                pool.submit([i] { return count_primes(i * kChunkSize, (i + 1) * kChunkSize); }));
        }
        for (auto& p : parts) pool_total += p.get();
    }
    const double pool_ms = ms_since(start);

    std::cout << std::fixed << std::setprecision(1)
              << "primes below " << kChunks * kChunkSize << ", " << kChunks << " chunks\n"
              << "  sequential : " << sequential_ms << " ms  (" << sequential_total << " primes)\n"
              << "  pool (" << workers << " workers): " << pool_ms << " ms  (" << pool_total
              << " primes)\n"
              << "  speedup    : " << std::setprecision(2) << sequential_ms / pool_ms << "x\n";

    return sequential_total == pool_total ? 0 : 1;
}
