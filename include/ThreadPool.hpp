#pragma once

#include "BlockingQueue.hpp"

#include <concepts>
#include <cstddef>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

// A fixed-size pool of worker threads that execute tasks from a shared queue.
//
// Lifecycle:
//   construction  -> N workers start and block on the (empty) queue
//   submit()      -> task is queued, one sleeping worker is woken, caller
//                    gets a std::future for the result (or the exception)
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

    // Queues f(args...) for execution and returns a future for its result.
    // Works with lambdas, function pointers, function objects and member
    // function pointers (via std::invoke). Arguments are copied/moved into
    // the task, like std::thread; use std::ref to pass by reference.
    //
    // If the task throws, the exception is stored in the future and rethrown
    // by future.get(); the worker thread is unaffected.
    //
    // Throws std::runtime_error if the pool has been shut down.
    template <typename F, typename... Args>
        requires std::invocable<std::decay_t<F>, std::decay_t<Args>...>
    auto submit(F&& f, Args&&... args)
        -> std::future<std::invoke_result_t<std::decay_t<F>, std::decay_t<Args>...>>;

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
    // Type-erased, non-template half of submit(); lives in ThreadPool.cpp.
    void enqueue(std::function<void()> task);
    void worker_loop();

    // Declaration order matters: members are destroyed in reverse order, so
    // workers_ is torn down before queue_. A worker must never be able to
    // touch a destroyed queue.
    BlockingQueue<std::function<void()>> queue_;
    std::vector<std::jthread> workers_;
    std::mutex shutdown_mutex_;  // serialises concurrent shutdown() calls
};

template <typename F, typename... Args>
    requires std::invocable<std::decay_t<F>, std::decay_t<Args>...>
auto ThreadPool::submit(F&& f, Args&&... args)
    -> std::future<std::invoke_result_t<std::decay_t<F>, std::decay_t<Args>...>> {
    using R = std::invoke_result_t<std::decay_t<F>, std::decay_t<Args>...>;

    // 1. Bind the callable and its arguments into a zero-argument callable.
    //    Everything is stored by value inside the lambda, so the task can't
    //    dangle into the caller's stack frame after submit() returns.
    auto bound = [func = std::forward<F>(f),
                  ... captured = std::forward<Args>(args)]() mutable -> R {
        return std::invoke(std::move(func), std::move(captured)...);
    };

    // 2. packaged_task pairs the callable with a promise. Running it stores
    //    either the return value or the thrown exception in the shared state
    //    that the future reads from.
    //
    //    packaged_task is move-only but std::function requires a copyable
    //    target, so it's held through a shared_ptr. (C++23's
    //    std::move_only_function removes the need for this.)
    auto task = std::make_shared<std::packaged_task<R()>>(std::move(bound));
    std::future<R> result = task->get_future();

    // 3. Hand a type-erased void() wrapper to the queue. If the pool is shut
    //    down this throws and `task` (and the unreturned future) is discarded.
    enqueue([task] { (*task)(); });

    return result;
}
