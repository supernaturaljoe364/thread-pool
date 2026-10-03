#include "BlockingQueue.hpp"
#include "test_framework.hpp"

#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <numeric>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

TEST(fifo_order_single_thread) {
    BlockingQueue<int> q;
    for (int i = 0; i < 5; ++i) {
        EXPECT_TRUE(q.push(i));
    }
    EXPECT_EQ(q.size(), 5u);
    for (int i = 0; i < 5; ++i) {
        auto v = q.pop();
        EXPECT_TRUE(v.has_value());
        EXPECT_EQ(*v, i);
    }
    EXPECT_EQ(q.size(), 0u);
}

TEST(try_pop_on_empty_returns_nullopt) {
    BlockingQueue<int> q;
    EXPECT_TRUE(!q.try_pop().has_value());
    q.push(42);
    auto v = q.try_pop();
    EXPECT_TRUE(v.has_value());
    EXPECT_EQ(*v, 42);
}

TEST(move_only_items_are_supported) {
    // Tasks will later be move-only (std::packaged_task), so the queue must
    // never copy its elements.
    BlockingQueue<std::unique_ptr<int>> q;
    q.push(std::make_unique<int>(7));
    auto v = q.pop();
    EXPECT_TRUE(v.has_value());
    EXPECT_EQ(**v, 7);
}

TEST(pop_blocks_until_item_is_pushed) {
    BlockingQueue<int> q;
    auto consumer = std::async(std::launch::async, [&q] { return q.pop(); });

    // Consumer has nothing to take, so it must still be waiting.
    EXPECT_TRUE(consumer.wait_for(50ms) == std::future_status::timeout);

    q.push(99);
    auto v = consumer.get();
    EXPECT_TRUE(v.has_value());
    EXPECT_EQ(*v, 99);
}

TEST(close_wakes_all_blocked_consumers) {
    BlockingQueue<int> q;
    constexpr int kConsumers = 4;
    std::vector<std::future<std::optional<int>>> consumers;
    for (int i = 0; i < kConsumers; ++i) {
        consumers.push_back(std::async(std::launch::async, [&q] { return q.pop(); }));
    }
    for (auto& c : consumers) {
        EXPECT_TRUE(c.wait_for(20ms) == std::future_status::timeout);
    }

    q.close();

    // Without notify_all() in close() these would hang forever.
    for (auto& c : consumers) {
        EXPECT_TRUE(c.wait_for(2s) == std::future_status::ready);
        EXPECT_TRUE(!c.get().has_value());
    }
}

TEST(push_after_close_is_rejected) {
    BlockingQueue<int> q;
    q.close();
    EXPECT_TRUE(q.closed());
    EXPECT_TRUE(!q.push(1));
    EXPECT_EQ(q.size(), 0u);
}

TEST(close_drains_remaining_items_before_signalling_end) {
    BlockingQueue<int> q;
    q.push(1);
    q.push(2);
    q.close();

    EXPECT_EQ(*q.pop(), 1);
    EXPECT_EQ(*q.pop(), 2);
    EXPECT_TRUE(!q.pop().has_value());
    EXPECT_TRUE(!q.pop().has_value());  // stays at end-of-stream
}

TEST(mpmc_every_item_delivered_exactly_once) {
    BlockingQueue<int> q;
    constexpr int kProducers = 4;
    constexpr int kConsumers = 4;
    constexpr int kPerProducer = 25'000;
    constexpr int kTotal = kProducers * kPerProducer;

    // seen[v] counts how many times value v was popped. If two consumers
    // ever took the same element, some entry would end up as 2.
    std::vector<std::atomic<int>> seen(kTotal);

    std::vector<std::thread> consumers;
    for (int c = 0; c < kConsumers; ++c) {
        consumers.emplace_back([&] {
            while (auto v = q.pop()) {
                seen[*v].fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    std::vector<std::thread> producers;
    for (int p = 0; p < kProducers; ++p) {
        producers.emplace_back([&q, p] {
            for (int i = 0; i < kPerProducer; ++i) {
                q.push(p * kPerProducer + i);
            }
        });
    }

    for (auto& t : producers) t.join();
    q.close();  // consumers drain the rest, then exit their loop
    for (auto& t : consumers) t.join();

    int duplicates = 0;
    int missing = 0;
    for (auto& s : seen) {
        int n = s.load();
        if (n == 0) ++missing;
        if (n > 1) ++duplicates;
    }
    EXPECT_EQ(missing, 0);
    EXPECT_EQ(duplicates, 0);
    EXPECT_EQ(q.size(), 0u);
}

TEST(push_all_appends_in_order) {
    BlockingQueue<int> q;
    q.push(0);
    EXPECT_TRUE(q.push_all({1, 2, 3}));
    for (int i = 0; i < 4; ++i) EXPECT_EQ(*q.pop(), i);
}

TEST(push_all_wakes_every_blocked_consumer) {
    BlockingQueue<int> q;
    constexpr int kConsumers = 4;
    std::vector<std::future<std::optional<int>>> consumers;
    for (int i = 0; i < kConsumers; ++i) {
        consumers.push_back(std::async(std::launch::async, [&q] { return q.pop(); }));
    }
    for (auto& c : consumers) EXPECT_TRUE(c.wait_for(20ms) == std::future_status::timeout);

    q.push_all({10, 20, 30, 40});  // single notify_all must reach all four

    int sum = 0;
    for (auto& c : consumers) {
        EXPECT_TRUE(c.wait_for(2s) == std::future_status::ready);
        sum += *c.get();
    }
    EXPECT_EQ(sum, 100);
}

TEST(push_all_after_close_enqueues_nothing) {
    BlockingQueue<int> q;
    q.close();
    EXPECT_TRUE(!q.push_all({1, 2, 3}));
    EXPECT_TRUE(!q.push_all({}));
    EXPECT_EQ(q.size(), 0u);
}

int main() { return RUN_ALL_TESTS(); }
