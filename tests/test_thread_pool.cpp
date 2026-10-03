#include "ThreadPool.hpp"
#include "test_framework.hpp"

#include <atomic>
#include <chrono>
#include <mutex>
#include <set>
#include <stdexcept>
#include <thread>

using namespace std::chrono_literals;

TEST(creates_requested_number_of_workers) {
    ThreadPool pool(4);
    EXPECT_EQ(pool.size(), 4u);
}

TEST(zero_workers_is_rejected) {
    bool threw = false;
    try {
        ThreadPool pool(0);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    EXPECT_TRUE(threw);
}

TEST(default_thread_count_is_at_least_one) {
    EXPECT_TRUE(ThreadPool::default_thread_count() >= 1);
    ThreadPool pool;
    EXPECT_EQ(pool.size(), ThreadPool::default_thread_count());
}

TEST(runs_every_submitted_task) {
    std::atomic<int> counter{0};
    {
        ThreadPool pool(4);
        for (int i = 0; i < 1000; ++i) {
            pool.submit([&counter] { counter.fetch_add(1, std::memory_order_relaxed); });
        }
    }  // destructor drains the queue and joins workers
    EXPECT_EQ(counter.load(), 1000);
}

TEST(tasks_run_on_worker_threads_not_the_caller) {
    constexpr std::size_t kWorkers = 3;
    std::mutex m;
    std::set<std::thread::id> ids;
    {
        ThreadPool pool(kWorkers);
        for (int i = 0; i < 300; ++i) {
            pool.submit([&] {
                std::lock_guard<std::mutex> lock(m);
                ids.insert(std::this_thread::get_id());
            });
        }
    }
    EXPECT_TRUE(!ids.empty());
    EXPECT_TRUE(ids.size() <= kWorkers);
    EXPECT_TRUE(ids.count(std::this_thread::get_id()) == 0);
}

TEST(throwing_task_does_not_kill_worker) {
    // With exactly one worker, the follow-up task can only run if that worker
    // survived the exception.
    std::atomic<int> ran_after{0};
    {
        ThreadPool pool(1);
        pool.submit([] { throw std::runtime_error("boom"); });
        pool.submit([] { throw 42; });  // non-std exceptions too
        pool.submit([&] { ran_after.fetch_add(1); });
    }
    EXPECT_EQ(ran_after.load(), 1);
}

TEST(submit_after_shutdown_throws) {
    ThreadPool pool(2);
    pool.shutdown();
    bool threw = false;
    try {
        pool.submit([] {});
    } catch (const std::runtime_error&) {
        threw = true;
    }
    EXPECT_TRUE(threw);
}

TEST(shutdown_is_idempotent) {
    ThreadPool pool(2);
    pool.shutdown();
    pool.shutdown();
    // destructor calls shutdown() a third time; must not throw or hang
}

TEST(shutdown_from_worker_is_rejected_not_deadlocked) {
    std::atomic<bool> got_logic_error{false};
    {
        ThreadPool pool(1);
        pool.submit([&] {
            try {
                pool.shutdown();
            } catch (const std::logic_error&) {
                got_logic_error = true;
            }
        });
    }
    EXPECT_TRUE(got_logic_error.load());
}

TEST(destructor_waits_for_running_and_queued_tasks) {
    std::atomic<int> finished{0};
    {
        ThreadPool pool(2);
        for (int i = 0; i < 6; ++i) {
            pool.submit([&] {
                std::this_thread::sleep_for(20ms);
                finished.fetch_add(1);
            });
        }
    }
    EXPECT_EQ(finished.load(), 6);
}

int main() { return RUN_ALL_TESTS(); }
