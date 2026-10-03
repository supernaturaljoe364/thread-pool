// Scenarios: tasks run concurrently, more tasks than workers, multiple
// producers, idle workers sleep, sleeping workers wake for new work.

#include "ThreadPool.hpp"
#include "test_framework.hpp"
#include "test_helpers.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <ctime>
#include <latch>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

namespace {

// Tracks how many tasks are inside the "running" section at once.
struct ConcurrencyMeter {
    std::atomic<int> current{0};
    std::atomic<int> peak{0};

    void enter() {
        int now = current.fetch_add(1) + 1;
        int prev = peak.load();
        while (now > prev && !peak.compare_exchange_weak(prev, now)) {
        }
    }
    void leave() { current.fetch_sub(1); }
};

}  // namespace

// --- Scenario 1: multiple tasks execute concurrently -----------------------

TEST(n_workers_run_n_tasks_simultaneously) {
    // Each task waits at a latch until all kWorkers tasks have arrived. If the
    // pool ran fewer than kWorkers tasks at a time, nobody could get past the
    // latch and every rendezvous would time out.
    constexpr int kWorkers = 4;
    ThreadPool pool(kWorkers);
    std::latch all_running(kWorkers);

    std::vector<std::future<bool>> futures;
    for (int i = 0; i < kWorkers; ++i) {
        futures.push_back(pool.submit([&all_running] { return rendezvous(all_running); }));
    }
    for (auto& f : futures) {
        EXPECT_TRUE(f.get());
    }
}

TEST(concurrent_tasks_use_distinct_threads) {
    constexpr int kWorkers = 4;
    ThreadPool pool(kWorkers);
    std::latch all_running(kWorkers);

    std::vector<std::future<std::thread::id>> futures;
    for (int i = 0; i < kWorkers; ++i) {
        futures.push_back(pool.submit([&all_running] {
            rendezvous(all_running);  // hold every worker so none runs two tasks
            return std::this_thread::get_id();
        }));
    }
    std::set<std::thread::id> ids;
    for (auto& f : futures) ids.insert(f.get());
    EXPECT_EQ(ids.size(), static_cast<std::size_t>(kWorkers));
}

// --- Scenario 2: more tasks than worker threads ----------------------------

TEST(more_tasks_than_workers_all_complete) {
    constexpr int kWorkers = 2;
    constexpr int kTasks = 1000;
    ThreadPool pool(kWorkers);

    std::vector<std::future<int>> futures;
    for (int i = 0; i < kTasks; ++i) {
        futures.push_back(pool.submit([i] { return i * 2; }));
    }
    long long sum = 0;
    for (auto& f : futures) sum += f.get();
    EXPECT_EQ(sum, 2LL * (kTasks - 1) * kTasks / 2);
}

TEST(concurrency_never_exceeds_worker_count) {
    constexpr int kWorkers = 3;
    ThreadPool pool(kWorkers);
    ConcurrencyMeter meter;
    std::mutex m;
    std::set<std::thread::id> ids;

    std::vector<std::future<void>> futures;
    for (int i = 0; i < 60; ++i) {
        futures.push_back(pool.submit([&] {
            meter.enter();
            {
                std::lock_guard<std::mutex> lock(m);
                ids.insert(std::this_thread::get_id());
            }
            std::this_thread::sleep_for(1ms);
            meter.leave();
        }));
    }
    for (auto& f : futures) f.get();

    EXPECT_TRUE(meter.peak.load() <= kWorkers);
    EXPECT_TRUE(ids.size() <= static_cast<std::size_t>(kWorkers));
}

TEST(backlog_is_visible_while_workers_are_busy) {
    ThreadPool pool(1);
    Gate gate;
    std::latch started(1);
    auto blocker = pool.submit([&] { started.count_down(); gate.wait(); });
    started.wait();  // the only worker is now occupied

    std::vector<std::future<void>> queued;
    for (int i = 0; i < 10; ++i) queued.push_back(pool.submit([] {}));
    EXPECT_EQ(pool.pending(), 10u);

    gate.open();
    for (auto& f : queued) f.get();
    EXPECT_EQ(pool.pending(), 0u);
}

// --- Scenario 5: tasks submitted from multiple producer threads ------------

TEST(multiple_producers_every_task_runs_exactly_once) {
    constexpr int kProducers = 8;
    constexpr int kPerProducer = 2000;
    constexpr int kTotal = kProducers * kPerProducer;

    ThreadPool pool(4);
    std::vector<std::atomic<int>> runs(kTotal);
    std::vector<long long> producer_sums(kProducers, 0);
    std::latch start_together(kProducers);

    std::vector<std::thread> producers;
    for (int p = 0; p < kProducers; ++p) {
        producers.emplace_back([&, p] {
            start_together.arrive_and_wait();  // maximise contention on submit()
            std::vector<std::future<int>> futures;
            futures.reserve(kPerProducer);
            for (int i = 0; i < kPerProducer; ++i) {
                const int id = p * kPerProducer + i;
                futures.push_back(pool.submit([&runs, id] {
                    runs[id].fetch_add(1, std::memory_order_relaxed);
                    return id;
                }));
            }
            for (auto& f : futures) producer_sums[p] += f.get();
        });
    }
    for (auto& t : producers) t.join();

    long long total = 0;
    for (long long s : producer_sums) total += s;
    EXPECT_EQ(total, static_cast<long long>(kTotal - 1) * kTotal / 2);
    EXPECT_TRUE(std::all_of(runs.begin(), runs.end(), [](auto& r) { return r.load() == 1; }));
}

// --- Scenario 6: workers sleep when the queue is empty ---------------------

TEST(idle_workers_consume_no_cpu) {
    // std::clock() measures CPU time used by the whole process. If 4 idle
    // workers were spinning on the queue, 300ms of wall time would burn about
    // 1200ms of CPU. Blocked in condition_variable::wait() they use ~0.
    constexpr int kWorkers = 4;
    ThreadPool pool(kWorkers);
    pool.submit([] {}).get();  // make sure workers have started and gone idle

    const std::clock_t cpu_before = std::clock();
    std::this_thread::sleep_for(300ms);
    const std::clock_t cpu_after = std::clock();

    const double cpu_ms = 1000.0 * static_cast<double>(cpu_after - cpu_before) / CLOCKS_PER_SEC;
    EXPECT_TRUE(cpu_ms < 100.0);
}

// --- Scenario 7: workers wake when new tasks arrive ------------------------

TEST(sleeping_worker_wakes_for_new_task) {
    ThreadPool pool(2);
    std::this_thread::sleep_for(100ms);  // every worker is now blocked in wait()

    auto f = pool.submit([] { return 7; });
    EXPECT_TRUE(f.wait_for(2s) == std::future_status::ready);
    EXPECT_EQ(f.get(), 7);
}

TEST(all_sleeping_workers_wake_for_a_burst) {
    // After an idle period, a burst of kWorkers rendezvous tasks can only
    // complete if every sleeping worker was woken (one notify_one per push).
    constexpr int kWorkers = 4;
    ThreadPool pool(kWorkers);

    for (int round = 0; round < 3; ++round) {
        std::this_thread::sleep_for(50ms);  // idle -> all workers asleep
        std::latch all_running(kWorkers);
        std::vector<std::future<bool>> futures;
        for (int i = 0; i < kWorkers; ++i) {
            futures.push_back(pool.submit([&all_running] { return rendezvous(all_running); }));
        }
        for (auto& f : futures) EXPECT_TRUE(f.get());
    }
}

TEST(one_batch_wakes_all_sleeping_workers) {
    // submit_batch issues a single notify_all; all kWorkers rendezvous tasks
    // can only finish if every sleeping worker was woken by it.
    constexpr int kWorkers = 4;
    ThreadPool pool(kWorkers);
    std::this_thread::sleep_for(50ms);  // idle -> all workers asleep

    std::latch all_running(kWorkers);
    auto futures = pool.submit_batch(kWorkers, [&all_running](std::size_t) {
        return rendezvous(all_running);
    });
    for (auto& f : futures) EXPECT_TRUE(f.get());
}

int main() { return RUN_ALL_TESTS(); }
