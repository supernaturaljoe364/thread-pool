// 4 workers, 20 tasks, each task sleeps 100ms and logs which worker ran it.
// Tasks complete in waves of 4, so the whole batch takes ~500ms instead of
// the ~2000ms it would take on one thread.

#include "ThreadPool.hpp"

#include <chrono>
#include <future>
#include <iostream>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

namespace {

std::mutex g_out_mutex;  // std::cout is thread-safe per call, not per line
std::map<std::thread::id, int> g_worker_names;

// Map opaque thread ids to short labels (W0, W1, ...) in first-seen order.
int worker_label() {
    std::lock_guard<std::mutex> lock(g_out_mutex);
    auto [it, inserted] = g_worker_names.try_emplace(std::this_thread::get_id(),
                                                     static_cast<int>(g_worker_names.size()));
    return it->second;
}

long long ms_since(Clock::time_point start) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count();
}

}  // namespace

int main() {
    constexpr int kWorkers = 4;
    constexpr int kTasks = 20;

    ThreadPool pool(kWorkers);
    const auto start = Clock::now();

    std::vector<std::future<int>> results;
    for (int id = 0; id < kTasks; ++id) {
        results.push_back(pool.submit([id, start] {
            const int worker = worker_label();
            {
                std::lock_guard<std::mutex> lock(g_out_mutex);
                std::cout << "[" << ms_since(start) << "ms] task " << id << " started on W"
                          << worker << '\n';
            }
            std::this_thread::sleep_for(100ms);  // stand-in for real work
            return id * id;
        }));
    }
    std::cout << "submitted " << kTasks << " tasks to " << kWorkers << " workers\n";

    long long sum = 0;
    for (auto& f : results) {
        sum += f.get();  // blocks until that task is done
    }

    std::cout << "sum of squares = " << sum << '\n'
              << "elapsed: " << ms_since(start) << "ms (sequential would be ~"
              << kTasks * 100 << "ms)\n";
}
