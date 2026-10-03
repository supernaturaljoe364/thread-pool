#pragma once

// Synchronisation helpers that make concurrency tests deterministic: instead
// of sleeping and hoping the threads interleaved the way we wanted, tests
// block on explicit events.

#include <chrono>
#include <future>
#include <latch>
#include <thread>

#ifdef __linux__
#include <filesystem>
#endif

// A one-shot gate: tasks wait() on it until the test calls open().
// Built from a promise/shared_future pair, so it can be waited on from any
// number of threads.
class Gate {
public:
    void open() { promise_.set_value(); }
    void wait() const { opened_.wait(); }

private:
    std::promise<void> promise_;
    std::shared_future<void> opened_ = promise_.get_future().share();
};

// Arrive at a std::latch and wait for everyone else, but give up after
// `timeout` instead of hanging the test forever. Returns true only if all
// participants arrived, which proves they were running at the same time.
inline bool rendezvous(std::latch& latch,
                       std::chrono::milliseconds timeout = std::chrono::seconds(5)) {
    latch.count_down();
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!latch.try_wait()) {
        if (std::chrono::steady_clock::now() > deadline) {
            return false;
        }
        std::this_thread::yield();
    }
    return true;
}

// Polls `condition` until it is true or the timeout expires.
template <typename Pred>
bool eventually(Pred condition, std::chrono::milliseconds timeout = std::chrono::seconds(5)) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!condition()) {
        if (std::chrono::steady_clock::now() > deadline) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

#ifdef __linux__
// Number of OS threads in this process (each is an entry in /proc/self/task).
inline long os_thread_count() {
    long n = 0;
    for ([[maybe_unused]] const auto& entry :
         std::filesystem::directory_iterator("/proc/self/task")) {
        ++n;
    }
    return n;
}
#endif
