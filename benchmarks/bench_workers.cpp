// Worker-count sweep: fixed CPU-bound workload, pool size 1..2x cores.
// Speedup should climb until physical cores run out, then flatten (or dip)
// -- more threads than cores just adds context switching.

#include "ThreadPool.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <future>
#include <iomanip>
#include <iostream>
#include <vector>

using Clock = std::chrono::steady_clock;

namespace {

constexpr std::uint64_t kChunks = 64;
constexpr std::uint64_t kChunkSize = 100'000;
constexpr int kRepeats = 3;

bool is_prime(std::uint64_t n) {
    if (n < 2) return false;
    if (n % 2 == 0) return n == 2;
    for (std::uint64_t d = 3; d * d <= n; d += 2) {
        if (n % d == 0) return false;
    }
    return true;
}

std::uint64_t count_primes(std::uint64_t lo, std::uint64_t hi) {
    std::uint64_t count = 0;
    for (std::uint64_t n = lo; n < hi; ++n) count += is_prime(n);
    return count;
}

std::uint64_t run_sequential() {
    std::uint64_t total = 0;
    for (std::uint64_t i = 0; i < kChunks; ++i) {
        total += count_primes(i * kChunkSize, (i + 1) * kChunkSize);
    }
    return total;
}

std::uint64_t run_pool(std::size_t workers) {
    ThreadPool pool(workers);
    std::vector<std::future<std::uint64_t>> parts;
    for (std::uint64_t i = 0; i < kChunks; ++i) {
        parts.push_back(pool.submit(count_primes, i * kChunkSize, (i + 1) * kChunkSize));
    }
    std::uint64_t total = 0;
    for (auto& p : parts) total += p.get();
    return total;
}

// Best of kRepeats: the minimum is the run least disturbed by other processes.
template <typename Fn>
double best_ms(Fn&& fn, std::uint64_t expected) {
    double best = 1e18;
    for (int r = 0; r < kRepeats; ++r) {
        const auto start = Clock::now();
        const std::uint64_t result = fn();
        best = std::min(best, std::chrono::duration<double, std::milli>(Clock::now() - start).count());
        if (result != expected) {
            std::cerr << "wrong result: " << result << " != " << expected << '\n';
            std::exit(1);
        }
    }
    return best;
}

}  // namespace

int main() {
    const std::size_t cores = ThreadPool::default_thread_count();
    const std::uint64_t expected = run_sequential();
    const double seq_ms = best_ms(run_sequential, expected);

    std::cout << "workload: " << kChunks << " chunks, primes below " << kChunks * kChunkSize
              << ", best of " << kRepeats << ", hardware threads: " << cores << "\n\n"
              << std::fixed << std::setprecision(1)
              << std::setw(10) << "workers" << std::setw(12) << "time (ms)" << std::setw(10)
              << "speedup" << std::setw(12) << "efficiency" << '\n'
              << std::setw(10) << "seq" << std::setw(12) << seq_ms << std::setw(10) << "1.00x"
              << std::setw(12) << "-" << '\n';

    for (std::size_t w = 1; w <= cores * 2; w *= 2) {
        const double ms = best_ms([w] { return run_pool(w); }, expected);
        const double speedup = seq_ms / ms;
        std::cout << std::setw(10) << w << std::setw(12) << ms << std::setw(9)
                  << std::setprecision(2) << speedup << 'x' << std::setw(11)
                  << std::setprecision(0) << 100.0 * speedup / static_cast<double>(w) << '%'
                  << std::setprecision(1) << '\n';
    }
}
