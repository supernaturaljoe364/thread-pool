#pragma once

#include "BlockingQueue.hpp"

#include <cstddef>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

// A fixed-size pool of worker threads that execute tasks from a shared queue.
//
// Lifecycle:
//   construction  -> N workers start and block on the (empty) queue
//   submit()      -> task is queued, one sleeping worker is woken
//   shutdown()    -> queue stops accepting work, workers drain what's left,
//                    then exit and are joined
//   destruction   -> calls shutdown(); no worker outlives the pool
class ThreadPool {
public:
    explicit ThreadPool(std::size_t num_threads = default_thread_count());
    ~ThreadPool();

    // Workers hold a pointer to this pool, so it must never move in memory.
    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;
    ThreadPool(ThreadPool&&) = delete;
    ThreadPool& operator=(ThreadPool&&) = delete;

    // Queues a task. Throws std::runtime_error if the pool has been shut down,
    // std::invalid_argument if the task is empty.
    void submit(std::function<void()> task);

    // Stops accepting new tasks, lets workers finish everything already queued,
    // then joins them. Blocks until all workers have exited.
    // Idempotent and safe to call concurrently from several threads.
    // Throws std::logic_error if called from one of this pool's own workers
    // (a thread cannot join itself).
    void shutdown();

    std::size_t size() const noexcept { return workers_.size(); }

    // Snapshot of queued (not yet started) tasks; may be stale immediately.
    std::size_t pending() const { return queue_.size(); }

    static std::size_t default_thread_count() noexcept;

private:
    void worker_loop();

    // Declaration order matters: members are destroyed in reverse order, so
    // workers_ is torn down before queue_. A worker must never be able to
    // touch a destroyed queue.
    BlockingQueue<std::function<void()>> queue_;
    std::vector<std::jthread> workers_;
    std::mutex shutdown_mutex_;  // serialises concurrent shutdown() calls
};
