#include "ThreadPool.hpp"

#include <stdexcept>

namespace {

// Which pool (if any) the current thread is a worker of. Lets shutdown()
// detect the self-join deadlock instead of hanging or crashing.
thread_local const ThreadPool* t_owning_pool = nullptr;

}  // namespace

std::size_t ThreadPool::default_thread_count() noexcept {
    // hardware_concurrency() is allowed to return 0 when it can't tell.
    const unsigned n = std::thread::hardware_concurrency();
    return n == 0 ? 1 : n;
}

ThreadPool::ThreadPool(std::size_t num_threads) {
    if (num_threads == 0) {
        throw std::invalid_argument("ThreadPool requires at least one worker thread");
    }

    workers_.reserve(num_threads);
    try {
        for (std::size_t i = 0; i < num_threads; ++i) {
            // Capturing `this` before the constructor finishes is safe here:
            // queue_ is fully constructed (members are initialised before the
            // body runs) and worker_loop() touches nothing else.
            workers_.emplace_back([this] { worker_loop(); });
        }
    } catch (...) {
        // Creating thread k failed (std::system_error). Threads 0..k-1 are
        // already blocked in queue_.pop(); close the queue so they exit, then
        // let the jthread destructors join them before rethrowing. Without
        // the close() this would deadlock; with plain std::thread and no
        // join it would call std::terminate().
        queue_.close();
        workers_.clear();
        throw;
    }
}

ThreadPool::~ThreadPool() {
    // RAII: whoever owns the pool doesn't have to remember to stop it.
    shutdown();
}

void ThreadPool::enqueue(std::function<void()> task) {
    // push() checks `closed` under the queue's lock, so a task is either
    // accepted before shutdown (and guaranteed to run) or rejected -- there is
    // no window where it is accepted but silently dropped.
    if (!queue_.push(std::move(task))) {
        throw std::runtime_error("ThreadPool::submit() called after shutdown");
    }
}

void ThreadPool::shutdown() {
    if (t_owning_pool == this) {
        throw std::logic_error("ThreadPool::shutdown() called from one of its own worker threads");
    }

    // Held for the whole shutdown so a second caller blocks until workers
    // are fully joined, rather than returning while they're still running.
    // Workers never take this mutex, so holding it while joining can't deadlock.
    std::lock_guard<std::mutex> lock(shutdown_mutex_);

    queue_.close();  // reject new work + wake every sleeping worker

    for (auto& worker : workers_) {
        if (worker.joinable()) {  // false on the 2nd+ call: already joined
            worker.join();
        }
    }
}

void ThreadPool::worker_loop() {
    t_owning_pool = this;

    // pop() blocks while the queue is empty and returns nullopt only once the
    // queue is closed AND drained -- that is the worker's exit condition.
    while (auto task = queue_.pop()) {
        try {
            (*task)();
        } catch (...) {
            // Defensive only. Tasks arrive wrapped in std::packaged_task, which
            // catches whatever the user's callable throws and stores it in the
            // future. This catch guarantees that even if something slips
            // through, it can't escape the thread function and std::terminate()
            // the process.
        }
    }
}
