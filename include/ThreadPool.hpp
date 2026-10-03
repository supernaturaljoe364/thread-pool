#pragma once

#include "BlockingQueue.hpp"

#include <cstddef>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <thread>
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

    // Queues f() for execution and returns a future for its result. f is
    // any callable that takes no arguments (lambda, function, functor).
    // To pass arguments, capture them: pool.submit([x] { return work(x); }).
    //
    // If the task throws, the exception is stored in the future and rethrown
    // by future.get(); the worker thread is unaffected.
    //
    // Throws std::runtime_error if the pool has been shut down.
    template <typename F>
    auto submit(F f);

    // Runs f(0), f(1), ..., f(count - 1) as separate tasks and returns their
    // futures in order. Queues them all with one lock + one notify_all
    // instead of one per task, which is much cheaper for lots of tiny tasks.
    // Throws std::runtime_error (and queues nothing) after shutdown.
    template <typename F>
    auto submit_batch(std::size_t count, F f);

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
    void enqueue_all(std::vector<std::function<void()>> tasks);
    void worker_loop();

    // Declaration order matters: members are destroyed in reverse order, so
    // workers_ is torn down before queue_. A worker must never be able to
    // touch a destroyed queue.
    BlockingQueue<std::function<void()>> queue_;
    std::vector<std::jthread> workers_;
    std::mutex shutdown_mutex_;  // serialises concurrent shutdown() calls
};

template <typename F>
auto ThreadPool::submit(F f) {
    using R = decltype(f());  // whatever f returns

    // packaged_task pairs the callable with a promise: running it stores the
    // return value (or the thrown exception) where the future can read it.
    //
    // packaged_task is move-only but std::function needs a copyable target,
    // so it's held through a shared_ptr.
    auto task = std::make_shared<std::packaged_task<R()>>(std::move(f));
    std::future<R> result = task->get_future();

    // Hand a type-erased void() wrapper to the queue. If the pool is shut
    // down this throws and the task is discarded.
    enqueue([task] { (*task)(); });
    return result;
}

template <typename F>
auto ThreadPool::submit_batch(std::size_t count, F f) {
    using R = decltype(f(std::size_t{0}));  // whatever f returns

    std::vector<std::future<R>> futures;
    std::vector<std::function<void()>> tasks;

    // Package each task exactly like submit() does, but collect them locally
    // so the queue lock is taken once for the whole batch.
    for (std::size_t i = 0; i < count; ++i) {
        auto task = std::make_shared<std::packaged_task<R()>>([f, i] { return f(i); });
        futures.push_back(task->get_future());
        tasks.push_back([task] { (*task)(); });
    }

    enqueue_all(std::move(tasks));
    return futures;
}
