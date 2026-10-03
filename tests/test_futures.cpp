#include "ThreadPool.hpp"
#include "test_framework.hpp"

#include <chrono>
#include <functional>
#include <future>
#include <memory>
#include <numeric>
#include <stdexcept>
#include <string>
#include <vector>

using namespace std::chrono_literals;

namespace {

int answer() { return 42; }

struct Multiplier {
    int factor;
    int value;
    int operator()() const { return value * factor; }
};

struct Account {
    int balance = 100;
    int deposit(int amount) { return balance += amount; }
};

struct TaskError : std::runtime_error {
    int code;
    TaskError(const std::string& msg, int c) : std::runtime_error(msg), code(c) {}
};

}  // namespace

TEST(returns_value_from_lambda) {
    ThreadPool pool(2);
    auto f = pool.submit([] { return 6 * 7; });
    EXPECT_EQ(f.get(), 42);
}

TEST(returns_non_trivial_type) {
    ThreadPool pool(2);
    auto f = pool.submit([] { return std::string("hello from a worker"); });
    EXPECT_EQ(f.get(), std::string("hello from a worker"));
}

TEST(void_task_future_signals_completion) {
    ThreadPool pool(2);
    bool done = false;  // plain bool: future.get() provides the synchronisation
    std::future<void> f = pool.submit([&done] { done = true; });
    f.get();
    EXPECT_TRUE(done);
}

TEST(accepts_plain_function) {
    ThreadPool pool(2);
    auto f = pool.submit(answer);
    EXPECT_EQ(f.get(), 42);
}

TEST(accepts_function_object) {
    ThreadPool pool(2);
    auto f = pool.submit(Multiplier{3, 5});
    EXPECT_EQ(f.get(), 15);
}

TEST(lambda_can_call_member_function) {
    ThreadPool pool(1);
    Account acct;
    auto f = pool.submit([&acct] { return acct.deposit(50); });
    EXPECT_EQ(f.get(), 150);
    EXPECT_EQ(acct.balance, 150);
}

TEST(capture_by_value_is_copied_at_submit_time) {
    ThreadPool pool(1);
    int value = 1;
    auto f = pool.submit([value] { return value; });
    value = 999;  // must not affect the already-submitted task
    EXPECT_EQ(f.get(), 1);
}

TEST(capture_by_reference_writes_back) {
    ThreadPool pool(1);
    int value = 0;
    auto f = pool.submit([&value] { value = 123; });
    f.get();
    EXPECT_EQ(value, 123);
}

TEST(move_only_capture_and_result) {
    // packaged_task accepts move-only callables, so a lambda owning a
    // unique_ptr works, and so does returning one.
    ThreadPool pool(2);
    auto owned = std::make_unique<int>(10);
    auto f = pool.submit([p = std::move(owned)] { return std::make_unique<int>(*p + 5); });
    std::unique_ptr<int> result = f.get();
    EXPECT_EQ(*result, 15);
}

TEST(exception_propagates_through_future) {
    ThreadPool pool(2);
    auto f = pool.submit([]() -> int { throw TaskError("task failed", 7); });

    bool caught = false;
    try {
        f.get();
    } catch (const TaskError& e) {  // original dynamic type is preserved
        caught = true;
        EXPECT_EQ(std::string(e.what()), std::string("task failed"));
        EXPECT_EQ(e.code, 7);
    }
    EXPECT_TRUE(caught);
}

TEST(non_std_exception_propagates_through_future) {
    ThreadPool pool(1);
    auto f = pool.submit([] { throw 42; });
    int caught = 0;
    try {
        f.get();
    } catch (int v) {
        caught = v;
    }
    EXPECT_EQ(caught, 42);
}

TEST(worker_survives_exceptions_and_keeps_serving) {
    ThreadPool pool(1);  // a single worker: later tasks prove it is alive
    std::vector<std::future<int>> futures;
    for (int i = 0; i < 10; ++i) {
        futures.push_back(pool.submit([i]() -> int {
            if (i % 2 == 0) throw std::runtime_error("even");
            return i;
        }));
    }
    int ok = 0, failed = 0;
    for (auto& f : futures) {
        try {
            f.get();
            ++ok;
        } catch (const std::runtime_error&) {
            ++failed;
        }
    }
    EXPECT_EQ(ok, 5);
    EXPECT_EQ(failed, 5);
}

TEST(empty_std_function_reports_bad_function_call) {
    ThreadPool pool(1);
    auto f = pool.submit(std::function<int()>{});
    bool caught = false;
    try {
        f.get();
    } catch (const std::bad_function_call&) {
        caught = true;
    }
    EXPECT_TRUE(caught);
}

TEST(shutdown_from_worker_surfaces_logic_error_in_future) {
    ThreadPool pool(1);
    auto f = pool.submit([&pool] { pool.shutdown(); });
    bool caught = false;
    try {
        f.get();
    } catch (const std::logic_error&) {
        caught = true;
    }
    EXPECT_TRUE(caught);
}

TEST(task_can_submit_followup_task) {
    ThreadPool pool(1);
    auto outer = pool.submit([&pool] { return pool.submit([] { return 5; }); });
    std::future<int> inner = outer.get();
    EXPECT_EQ(inner.get(), 5);
}

TEST(future_get_publishes_non_atomic_writes) {
    // get() synchronises-with the worker that ran the task, so everything the
    // task wrote is visible afterwards without atomics. TSan verifies this.
    ThreadPool pool(4);
    std::vector<int> data(10'000);
    auto f = pool.submit([&data] { std::iota(data.begin(), data.end(), 0); });
    f.get();
    long long sum = std::accumulate(data.begin(), data.end(), 0LL);
    EXPECT_EQ(sum, 49'995'000LL);
}

TEST(many_futures_collected_in_submission_order) {
    ThreadPool pool(4);
    std::vector<std::future<long long>> futures;
    for (long long i = 1; i <= 200; ++i) {
        futures.push_back(pool.submit([i] { return i * i; }));
    }
    long long total = 0;
    for (auto& f : futures) total += f.get();
    EXPECT_EQ(total, 2'686'700LL);  // sum of squares 1..200
}

TEST(future_is_not_ready_until_task_finishes) {
    ThreadPool pool(1);
    std::promise<void> gate;
    std::shared_future<void> opened = gate.get_future().share();
    auto f = pool.submit([opened] { opened.wait(); return 1; });

    EXPECT_TRUE(f.wait_for(30ms) == std::future_status::timeout);
    gate.set_value();
    EXPECT_EQ(f.get(), 1);
}

TEST(submit_batch_returns_results_in_index_order) {
    ThreadPool pool(4);
    auto futures = pool.submit_batch(100, [](std::size_t i) { return i * i; });
    EXPECT_EQ(futures.size(), 100u);
    for (std::size_t i = 0; i < futures.size(); ++i) {
        EXPECT_EQ(futures[i].get(), i * i);
    }
}

TEST(submit_batch_isolates_exceptions_per_task) {
    ThreadPool pool(2);
    auto futures = pool.submit_batch(6, [](std::size_t i) -> std::size_t {
        if (i % 3 == 0) throw std::runtime_error("bad index");
        return i;
    });
    int failed = 0;
    std::size_t sum = 0;
    for (auto& f : futures) {
        try {
            sum += f.get();
        } catch (const std::runtime_error&) {
            ++failed;
        }
    }
    EXPECT_EQ(failed, 2);       // indices 0 and 3
    EXPECT_EQ(sum, 1u + 2 + 4 + 5);
}

TEST(submit_batch_of_zero_returns_no_futures) {
    ThreadPool pool(1);
    auto futures = pool.submit_batch(0, [](std::size_t) {});
    EXPECT_TRUE(futures.empty());
}

TEST(submit_batch_after_shutdown_throws_and_queues_nothing) {
    ThreadPool pool(2);
    pool.shutdown();
    bool threw = false;
    try {
        pool.submit_batch(10, [](std::size_t i) { return i; });
    } catch (const std::runtime_error&) {
        threw = true;
    }
    EXPECT_TRUE(threw);
    EXPECT_EQ(pool.pending(), 0u);
}

int main() { return RUN_ALL_TESTS(); }
