// Scenarios: graceful shutdown with queued tasks, shutdown while tasks are
// executing, submitting after shutdown, and shutdown racing with producers.

#include "ThreadPool.hpp"
#include "test_framework.hpp"
#include "test_helpers.hpp"

#include <atomic>
#include <chrono>
#include <future>
#include <latch>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

namespace {

// True once the pool has started shutting down (i.e. the queue is closed).
// Probing with submit() is safe: before close() the no-op is simply accepted.
bool submit_rejected(ThreadPool& pool, std::atomic<int>& accepted_probes) {
    try {
        pool.submit([] {});
        accepted_probes.fetch_add(1);
        return false;
    } catch (const std::runtime_error&) {
        return true;
    }
}

}  // namespace

// --- Scenario 8: graceful shutdown drains queued tasks ---------------------

TEST(shutdown_runs_every_queued_task) {
    constexpr int kQueued = 100;
    ThreadPool pool(1);
    Gate gate;
    std::latch started(1);
    std::atomic<int> ran{0};

    // Occupy the only worker so everything else piles up in the queue.
    auto blocker = pool.submit([&] { started.count_down(); gate.wait(); });
    started.wait();

    std::vector<std::future<int>> futures;
    for (int i = 0; i < kQueued; ++i) {
        futures.push_back(pool.submit([&ran, i] { ran.fetch_add(1); return i; }));
    }
    EXPECT_EQ(pool.pending(), static_cast<std::size_t>(kQueued));

    auto shutting_down = std::async(std::launch::async, [&pool] { pool.shutdown(); });

    // Deterministically wait until shutdown has actually closed the queue.
    std::atomic<int> probes{0};
    EXPECT_TRUE(eventually([&] { return submit_rejected(pool, probes); }));

    // The backlog is still there and shutdown is blocked behind it.
    EXPECT_TRUE(shutting_down.wait_for(50ms) == std::future_status::timeout);
    EXPECT_TRUE(ran.load() == 0);

    gate.open();
    shutting_down.get();

    // Nothing that was accepted got dropped.
    EXPECT_EQ(ran.load(), kQueued);
    for (int i = 0; i < kQueued; ++i) {
        EXPECT_EQ(futures[i].get(), i);
    }
    EXPECT_EQ(pool.pending(), 0u);
}

TEST(destructor_drains_queue) {
    std::atomic<int> ran{0};
    {
        ThreadPool pool(2);
        for (int i = 0; i < 500; ++i) {
            pool.submit([&ran] { ran.fetch_add(1); });
        }
    }
    EXPECT_EQ(ran.load(), 500);
}

// --- Scenario 9: shutdown while workers are executing ----------------------

TEST(shutdown_waits_for_in_flight_tasks) {
    constexpr int kWorkers = 4;
    ThreadPool pool(kWorkers);
    Gate gate;
    std::latch all_started(kWorkers);
    std::atomic<int> finished{0};

    std::vector<std::future<int>> futures;
    for (int i = 0; i < kWorkers; ++i) {
        futures.push_back(pool.submit([&, i] {
            all_started.count_down();
            gate.wait();
            finished.fetch_add(1);
            return i;
        }));
    }
    all_started.wait();  // every worker is mid-task

    auto shutting_down = std::async(std::launch::async, [&pool] { pool.shutdown(); });

    // Running tasks are never interrupted, so shutdown must still be waiting.
    EXPECT_TRUE(shutting_down.wait_for(100ms) == std::future_status::timeout);
    EXPECT_EQ(finished.load(), 0);

    gate.open();
    shutting_down.get();

    EXPECT_EQ(finished.load(), kWorkers);
    for (int i = 0; i < kWorkers; ++i) {
        EXPECT_EQ(futures[i].get(), i);  // values, not exceptions
    }
}

TEST(running_task_cannot_submit_once_shutdown_began) {
    ThreadPool pool(1);
    Gate gate;
    std::latch started(1);

    auto f = pool.submit([&] {
        started.count_down();
        gate.wait();
        try {
            pool.submit([] {});
            return false;
        } catch (const std::runtime_error&) {
            return true;  // follow-up work is rejected during shutdown
        }
    });
    started.wait();

    auto shutting_down = std::async(std::launch::async, [&pool] { pool.shutdown(); });
    std::atomic<int> probes{0};
    EXPECT_TRUE(eventually([&] { return submit_rejected(pool, probes); }));

    gate.open();
    shutting_down.get();
    EXPECT_TRUE(f.get());
}

// --- Scenario 10: submitting after shutdown --------------------------------

TEST(submit_after_shutdown_throws_runtime_error) {
    ThreadPool pool(2);
    pool.shutdown();

    int rejected = 0;
    for (int i = 0; i < 3; ++i) {
        try {
            pool.submit([] { return 1; });
        } catch (const std::runtime_error&) {
            ++rejected;
        }
    }
    try {
        pool.submit([](int a, int b) { return a + b; }, 1, 2);
    } catch (const std::runtime_error&) {
        ++rejected;
    }
    EXPECT_EQ(rejected, 4);
    EXPECT_EQ(pool.pending(), 0u);
}

// --- Shutdown racing with other threads ------------------------------------

TEST(every_accepted_task_runs_even_when_racing_shutdown) {
    // Producers hammer submit() while the main thread shuts the pool down.
    // Invariant: a task is either rejected (submit throws) or it runs.
    // There must be no "accepted, then silently dropped" outcome.
    constexpr int kProducers = 4;
    ThreadPool pool(4);
    std::atomic<int> accepted{0};
    std::atomic<int> rejected{0};
    std::atomic<int> executed{0};
    std::latch producers_running(kProducers);

    std::vector<std::thread> producers;
    for (int p = 0; p < kProducers; ++p) {
        producers.emplace_back([&] {
            producers_running.count_down();
            while (true) {
                try {
                    pool.submit([&executed] { executed.fetch_add(1); });
                    accepted.fetch_add(1);
                } catch (const std::runtime_error&) {
                    rejected.fetch_add(1);
                    return;
                }
            }
        });
    }
    producers_running.wait();
    EXPECT_TRUE(eventually([&] { return accepted.load() > 1000; }));  // real backlog
    pool.shutdown();
    for (auto& t : producers) t.join();

    EXPECT_TRUE(accepted.load() > 0);
    EXPECT_EQ(rejected.load(), kProducers);
    EXPECT_EQ(executed.load(), accepted.load());
}

TEST(concurrent_shutdown_calls_all_block_until_done) {
    constexpr int kCallers = 4;
    ThreadPool pool(2);
    std::atomic<int> ran{0};
    for (int i = 0; i < 200; ++i) {
        pool.submit([&ran] {
            std::this_thread::sleep_for(100us);
            ran.fetch_add(1);
        });
    }

    std::latch go(kCallers);
    std::vector<std::future<int>> callers;
    for (int i = 0; i < kCallers; ++i) {
        callers.push_back(std::async(std::launch::async, [&] {
            go.arrive_and_wait();
            pool.shutdown();
            return ran.load();  // must see the fully drained count
        }));
    }
    for (auto& c : callers) {
        EXPECT_EQ(c.get(), 200);
    }
}

#ifdef __linux__
TEST(destructor_leaves_no_worker_threads_running) {
    const long before = os_thread_count();
    {
        ThreadPool pool(6);
        EXPECT_EQ(os_thread_count(), before + 6);
        pool.submit([] {}).get();
    }
    EXPECT_EQ(os_thread_count(), before);
}
#endif

int main() { return RUN_ALL_TESTS(); }
