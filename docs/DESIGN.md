# Design Notes

Working notes on the design decisions behind the thread pool, written as each
component was built.

---

## 1. The task queue (`include/BlockingQueue.hpp`)

### The problem

The pool has one or more producers (threads calling `submit`) and N consumers
(the workers), all touching one container. `std::deque` is not thread-safe.
`push_back` and `pop_front` update several internal fields (block pointers,
start index, size), so two threads interleaving inside them can corrupt the
container, lose elements, or hand the same element to two threads.

### Why a mutex?

A mutex makes each *compound* operation atomic as a whole. Consider:

```cpp
if (!items_.empty()) {          // 1. check
    T item = items_.front();    // 2. read
    items_.pop_front();         // 3. remove
}
```

Steps 1–3 must happen as one unit. Two workers could both pass step 1 while one
element remains, and then both read the same element, or one pops from an
empty deque (undefined behaviour). Holding the mutex across all three steps
gives us mutual exclusion. Only one thread is ever between `lock` and
`unlock`, so whatever it observes stays true until it releases the lock.

**This is also why two workers can never take the same task:** the
`front()` + `pop_front()` pair runs entirely under the lock.

### Why isn't an atomic enough?

`std::atomic<T>` makes *one* load/store/RMW on *one* memory location
indivisible. A queue operation touches several locations and makes a decision
based on what it read (`if not empty then remove`). You can't wrap a
`std::deque` in `std::atomic`. It isn't trivially copyable, and even if it
were, an atomic `deque` would only let you swap whole deques, not "pop if
non-empty".

Lock-free queues exist (Michael-Scott queue, ring buffers with atomic indices),
but they need careful memory reclamation (hazard pointers, epochs) and are much
harder to get right. A thread pool's tasks usually take microseconds to
milliseconds, while an uncontended mutex lock costs tens of nanoseconds. The
lock isn't the bottleneck, so the simpler design wins.

### Why a condition variable instead of polling?

Without one, an idle worker has two options:

- **Spin:** `while (queue.empty()) {}`. This burns 100% of a core doing nothing
  and competes for the mutex with the producers trying to push.
- **Sleep-poll:** `while (queue.empty()) sleep(1ms);`. This wastes less CPU,
  but every task now waits up to 1 ms before anyone notices it, and you're
  still waking up 1000×/s per worker for nothing.

A condition variable lets the thread block in the kernel (on Linux it's a
futex) and be woken *exactly* when a producer signals. The thread uses zero CPU
while idle and wakes with minimal latency.

### What happens inside `cv.wait(lock, pred)`?

The predicate overload is exactly this loop:

```cpp
while (!pred()) {
    cv.wait(lock);
}
```

and a single `cv.wait(lock)` does:

1. **Atomically** release `lock` and add this thread to the CV's wait list.
2. Sleep until notified (or a spurious wakeup).
3. Re-acquire `lock` before returning.

The loop matters for two reasons:

- **Spurious wakeups:** the standard allows `wait` to return without a notify.
- **Stolen wakeups:** producer pushes 1 item and calls `notify_one`. Worker A
  wakes, but before it re-acquires the mutex, worker B (which was awake anyway)
  grabs the lock and takes the item. When A finally gets the lock the queue is
  empty again, so it must go back to sleep, not pop from an empty deque.

### Why does `wait()` need a `unique_lock`, not a `lock_guard`?

`wait()` has to *unlock the mutex while sleeping and re-lock it on wake*.
`std::lock_guard` can only lock in its constructor and unlock in its
destructor; it has no `unlock()`/`lock()` members. `std::unique_lock` is a
movable lock owner with explicit `lock()`/`unlock()`, which is the interface
`condition_variable::wait` needs.

Elsewhere (`push`, `close`, `size`) we never unlock early, so the lighter
`lock_guard` is used.

### Why must "unlock + sleep" be atomic? (the lost-wakeup problem)

If the CV released the mutex and *then* registered as a waiter, there'd be a
window:

```
worker:   sees queue empty → unlocks mutex →         → registers as waiter → sleeps forever
producer:                                   push, notify (no one waiting yet!)
```

The notification is lost and the worker sleeps even though work exists. The
CV closes this window by doing both steps atomically with respect to the mutex.
For the same reason, **the state the predicate reads must be modified under the
same mutex**. This is why `closed_` is a plain `bool` guarded by `mutex_`, not
a `std::atomic<bool>` written without the lock:

```
worker:   lock; pred(): closed_ == false, queue empty → (about to sleep)
shutdown: closed_.store(true); notify_all()          ← no lock taken
worker:   ...atomically unlock + sleep               ← missed the notify, sleeps forever
```

An atomic flag gives you a race-free *read*. It doesn't make "check, then
sleep" atomic with "set, then notify". Only taking the mutex does.

### Notify inside or outside the lock?

Both are correct. Notifying *after* releasing the lock avoids a "hurry up and
wait" where the woken thread immediately blocks on the mutex the notifier still
holds. It's safe here because the state change itself happened under the lock,
so no waiter can miss it.

### What happens if items are pushed while consumers are sleeping?

`push` adds the item under the lock and then calls `notify_one`, which wakes
one sleeper. That sleeper re-acquires the lock, sees `!items_.empty()`, and
pops. If no-one is sleeping (all workers busy), `notify_one` is a no-op, which
is fine. The item stays in the queue, and the next worker to call `pop()` sees
it in the predicate check *before* ever sleeping.

### Close semantics

`close()` sets `closed_` under the lock and calls `notify_all()` so *every*
sleeper wakes, re-checks the predicate, and sees `closed_ == true`.

- `push` after close returns `false`, so the caller learns the item was rejected.
- `pop` returns remaining items first and only returns `std::nullopt` once the
  queue is **closed *and* empty**. That gives the pool a "drain" shutdown for
  free: `while (auto task = queue.pop()) run(*task);` exits on its own when
  shutdown begins and the backlog is finished.

### Races / pitfalls identified in this component

| Concern | How it's handled |
|---|---|
| Two consumers take the same item | `front()`+`pop_front()` happen under one lock |
| Pop from empty deque after wakeup | predicate re-checked under the lock after every wake |
| Lost wakeup on shutdown | `closed_` written under `mutex_` before `notify_all` |
| Consumer sleeps forever on shutdown | `close()` uses `notify_all`, not `notify_one` |
| `size()` used for decisions | Documented as a snapshot; never used for control flow |
| Copying a mutex | Copy ctor/assignment deleted |
| Move-only payloads (`packaged_task`) | Elements are only ever moved |

### Contention

Every push and pop goes through one mutex. With very short tasks and many
workers, that lock becomes the bottleneck. Work-stealing pools (per-worker
deques) exist to fix this. For tasks that do real work the critical section
(a deque push/pop) is tiny compared to the task, so a single queue is fine.

### Verification

- `tests/test_blocking_queue.cpp`: FIFO order, blocking `pop`, `close()` waking
  every blocked consumer, rejecting pushes after close, draining on close, and
  an MPMC stress test (4 producers × 4 consumers × 100k items) asserting
  every item is delivered **exactly once**.
- Run under ThreadSanitizer with no reports. As a sanity check, removing the
  lock from `push()` makes TSan report data races immediately.

---

## 2. Workers and lifecycle (`include/ThreadPool.hpp`, `src/ThreadPool.cpp`)

### The worker loop

```cpp
while (auto task = queue_.pop()) {
    try { (*task)(); } catch (...) {}
}
```

All of the worker's states map onto `BlockingQueue::pop()`:

| Worker state | What's happening |
|---|---|
| **Idle / sleeping** | blocked inside `pop()` → `cv.wait()`, using no CPU |
| **Woken** | a `push()` did `notify_one`, or `close()` did `notify_all` |
| **Executing** | `pop()` returned a task; the queue lock is **not** held while it runs |
| **Exiting** | `pop()` returned `nullopt` = queue closed *and* empty |

The task runs **outside** the queue's lock. If it ran under the lock, the pool
would execute one task at a time, and a task calling `submit()` would
self-deadlock on the same mutex.

### `std::thread` vs `std::jthread`

A `std::thread` that's still *joinable* when destroyed calls
`std::terminate()`. You must `join()` (wait for it) or `detach()` (abandon it)
on every path, including exception paths. `std::jthread` (C++20) joins
automatically in its destructor (after calling `request_stop()`).

Workers are stored as `std::jthread` as a safety net. The pool still joins
explicitly in `shutdown()`, because joining alone isn't enough. A worker
blocked in `pop()` on an open queue never exits, so a join without a prior
`close()` deadlocks. jthread's `request_stop()` doesn't help either, because
our workers wait on the queue's condition variable, not on a `stop_token`.
**The queue must be closed first; then joining is safe.**

Where jthread pays off is the constructor's failure path. If creating worker
#3 throws `std::system_error` (e.g. the OS is out of threads), workers #0–#2
already exist. The catch block closes the queue and `clear()`s the vector, and
the jthread destructors join them. With `std::thread` and no explicit join,
that would be `std::terminate()`.

### Why must worker threads be joined? (Q10)

- **Lifetime:** workers access `queue_` through `this`. If the pool were
  destroyed while a worker was still inside `pop()` or running a task, the
  worker would touch freed memory (use-after-free).
- **Completion:** `join()` is the only way to know the thread has *finished*,
  and it also establishes a *happens-before* edge. Everything the worker wrote
  is visible to the thread that joined it.
- **Resources:** an unjoined, undetached thread is never cleaned up, and the
  standard turns that into `std::terminate()`.

`detach()` would avoid the terminate but leaves the first problem: detached
workers outliving the pool and reading a dead queue.

### Why should the destructor handle cleanup? (Q11, RAII)

The pool *owns* threads, a resource whose release must happen exactly once on
every exit path: normal return, early return, exceptions. Tying it to the
destructor means a `ThreadPool` on the stack can't leak running threads, even
if the code that owns it throws. `shutdown()` is also public, so callers can
stop the pool early (and catch errors) without waiting for scope exit.

### Member declaration order

```cpp
BlockingQueue<std::function<void()>> queue_;   // constructed 1st, destroyed last
std::vector<std::jthread> workers_;            // constructed 2nd, destroyed 1st
```

Members are constructed in declaration order and destroyed in reverse. The
queue must exist before any worker starts, and outlive every worker. Swapping
these two lines would let a worker start before its queue exists.

Capturing `this` in the worker lambda during the constructor is fine for the
same reason: by the time the constructor body runs, `queue_` is fully built.

### The shutdown sequence (Q8, Q9)

```cpp
void ThreadPool::shutdown() {
    if (t_owning_pool == this) throw std::logic_error(...);   // 1
    std::lock_guard<std::mutex> lock(shutdown_mutex_);        // 2
    queue_.close();                                           // 3
    for (auto& w : workers_) if (w.joinable()) w.join();      // 4
}
```

1. **Self-join guard.** A task calling `pool.shutdown()` would make a worker
   wait on itself forever. A `thread_local` pointer records which pool the
   current thread belongs to, so this is turned into an exception.
2. **Concurrent callers.** Two threads calling `join()` on the same thread
   object is a data race. The mutex serialises callers. The second caller
   blocks until the first has finished joining, then finds nothing joinable
   and returns, so *every* caller returns only after the workers are gone.
   Workers never take this mutex, so holding it while joining can't deadlock.
3. **Close the queue.** New `submit()`s fail from here on, and `notify_all`
   wakes every sleeping worker.
4. **Join.** Each worker finishes its current task, keeps popping until the
   queue is drained, sees `nullopt`, and returns.

**Shutdown while workers are executing tasks (Q8).** Nothing interrupts them.
C++ has no safe way to forcibly stop a thread. The running task completes,
then the worker drains the remaining queue. Shutdown takes as long as the
longest remaining chain of work.

**How can't a worker wait forever? (Q9)** Two guarantees from the queue:
`closed_` is set under the mutex (no lost wakeup), and `close()` uses
`notify_all` (every sleeper wakes, not just one). A worker that is busy when
`close()` happens never sleeps again, because its next `pop()` sees
`closed_ == true` in the predicate before waiting.

### Chosen semantics: drain

Tasks accepted before `close()` **always run**. Tasks submitted after
`close()` are **rejected with an exception**. The deciding check is
`if (closed_)` inside `push()`, under the same lock that `close()` takes, so
there's no window where a task is accepted and then silently dropped.

The alternative, "discard pending tasks on shutdown", is faster to stop but
would leave callers holding futures that never complete (or that report
`broken_promise` in Phase 3).

### What if `submit()` is called after shutdown? (Q14)

`queue_.push()` returns `false` and `submit()` throws `std::runtime_error`.
The caller finds out immediately, instead of getting a task that will never
run. Calling `submit()` on a *destroyed* pool is plain undefined behaviour,
like any use-after-destroy, which is why an explicit `shutdown()` exists.

### Exceptions in fire-and-forget tasks

If an exception escapes a thread's top-level function, the runtime calls
`std::terminate()`, killing the whole process. The worker wraps each task in
`try { } catch (...) { }`. With a `std::function<void()>` there is nowhere to
report the error, so it's dropped. Phase 3 fixes this by routing it into the
caller's `std::future`.

### Verification

`tests/test_thread_pool.cpp` (11 tests): worker count, default size, every task
runs, tasks run on worker threads only, a single worker survives throwing
tasks, empty task rejected, submit-after-shutdown throws, shutdown idempotent,
shutdown-from-worker rejected instead of deadlocking, destructor waits for
queued and running work.

Clean under GCC/Clang plus TSan and ASan/UBSan. Mutation checks:
- deleting `queue_.close()` from `shutdown()` makes the suite **hang**
- deleting the worker's `try/catch` makes it **terminate**
