#pragma once

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>
#include <optional>
#include <utility>

// A thread-safe, unbounded FIFO queue supporting multiple producers and
// multiple consumers (MPMC).
//
// Invariant: every access to `items_` and `closed_` happens while holding
// `mutex_`. That single rule is what makes the queue correct -- the
// condition variable only exists so consumers can sleep instead of spin.
//
// Close semantics:
//   - After close(), push() is rejected (returns false).
//   - Items already in the queue are NOT discarded; pop() keeps returning
//     them until the queue is empty, and only then returns std::nullopt.
//   - close() wakes every blocked consumer so nobody waits forever.
template <typename T>
class BlockingQueue {
public:
    BlockingQueue() = default;

    // The queue owns a mutex and a condition variable, neither of which can
    // be copied or moved. Sharing a queue means sharing a reference to it.
    BlockingQueue(const BlockingQueue&) = delete;
    BlockingQueue& operator=(const BlockingQueue&) = delete;

    // Returns false if the queue has been closed (the item is not enqueued).
    bool push(T item) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (closed_) {
                return false;
            }
            items_.push_back(std::move(item));
        }
        // Notify after releasing the lock: the woken consumer can grab the
        // mutex immediately instead of waking up only to block on it again.
        not_empty_.notify_one();
        return true;
    }

    // Blocks until an item is available or the queue is closed and drained.
    // Returns std::nullopt only in the latter case.
    std::optional<T> pop() {
        std::unique_lock<std::mutex> lock(mutex_);

        // wait() atomically releases the lock and puts this thread to sleep.
        // When woken it re-acquires the lock and re-checks the predicate, so
        // spurious wakeups and "another consumer got there first" are both
        // handled by looping back to sleep.
        not_empty_.wait(lock, [this] { return closed_ || !items_.empty(); });

        if (items_.empty()) {
            return std::nullopt;  // closed and fully drained
        }

        // Still holding the lock here, so no other consumer can observe or
        // remove this same element.
        T item = std::move(items_.front());
        items_.pop_front();
        return item;
    }

    // Non-blocking variant: returns std::nullopt immediately if empty.
    std::optional<T> try_pop() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (items_.empty()) {
            return std::nullopt;
        }
        T item = std::move(items_.front());
        items_.pop_front();
        return item;
    }

    void close() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            closed_ = true;
        }
        // Every sleeping consumer must re-check the predicate, otherwise a
        // consumer blocked on an empty queue would sleep forever.
        not_empty_.notify_all();
    }

    bool closed() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return closed_;
    }

    // Snapshot only: the value may be stale by the time the caller uses it.
    std::size_t size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return items_.size();
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable not_empty_;
    std::deque<T> items_;
    bool closed_ = false;  // guarded by mutex_, deliberately not atomic
};
