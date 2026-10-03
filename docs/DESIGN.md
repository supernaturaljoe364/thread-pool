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

---

## 3. Results and exceptions through `std::future`

### The problem

A worker runs the task on *another thread*, possibly after `submit()` has
already returned. The caller needs a handle for "a value that doesn't exist
yet" that it can block on, and a way to get an *error* back across threads too.

### The three pieces

| Type | Role |
|---|---|
| `std::promise<T>` | the **write end**: `set_value(v)` / `set_exception(eptr)`, called once |
| `std::future<T>` | the **read end**: `get()` blocks until the value/exception arrives, then returns/rethrows it. One-shot, move-only |
| `std::packaged_task<R()>` | a callable **plus** a promise: calling it runs the function and feeds the result (or exception) into the promise |

Promise and future share a heap-allocated **shared state** (value slot,
exception slot, ready flag, mutex + condvar). `future.get()` is essentially
`wait for ready; return value or rethrow exception`.

### How `submit()` works (Q12)

```cpp
auto bound = [func = std::forward<F>(f), ...captured = std::forward<Args>(args)]() mutable -> R {
    return std::invoke(std::move(func), std::move(captured)...);
};
auto task = std::make_shared<std::packaged_task<R()>>(std::move(bound));
std::future<R> result = task->get_future();
enqueue([task] { (*task)(); });
return result;
```

1. **Bind** `f(args...)` into a zero-argument lambda. Arguments are
   decay-copied or moved in (`std::forward` into an init-capture). The task owns
   its inputs and can't dangle into the caller's stack frame after `submit`
   returns. To share a variable deliberately, pass `std::ref(x)`, the same
   convention as `std::thread`.
2. **`std::invoke`** makes one code path work for lambdas, function pointers,
   functors **and** member-function pointers (`submit(&Account::deposit, &acct, 50)`).
3. **Wrap in `packaged_task<R()>`** and take its future *before* enqueuing.
   After `enqueue`, a worker may run the task immediately.
4. **Type-erase to `std::function<void()>`.** The queue holds one type no
   matter what each task returns. The future carries `R`; the queue never needs
   to know it.

**Why `shared_ptr<packaged_task>`?** `packaged_task` is move-only, and
`std::function` requires its target to be *copyable*. A lambda holding a
`shared_ptr` is copyable. The cost is one extra heap allocation per task.
C++23's `std::move_only_function` removes this workaround.

**Why the `requires` clause?** Without it, `submit(f, wrong_args)` fails deep
inside `invoke_result_t` with a long template error. With it, the compiler
says *"constraints not satisfied: is_invocable_v<...> evaluated to false"* at
the call site.

**The return type** is computed with `std::invoke_result_t<decay_t<F>, decay_t<Args>...>`,
which is exactly how the stored callable is called (as rvalues), so the
declared type always matches what `invoke` produces.

### How exceptions propagate (Q13)

Inside `packaged_task::operator()` the standard library does roughly:

```cpp
try {
    promise.set_value(fn());
} catch (...) {
    promise.set_exception(std::current_exception());
}
```

`std::current_exception()` captures the in-flight exception as an
`std::exception_ptr`, a reference-counted handle to the *original exception
object*, not a sliced copy. `future.get()` calls `std::rethrow_exception`, so the
caller catches the **same dynamic type** (`TaskError` with its `code` field,
even `throw 42`) on the caller's thread.

As a result the worker never sees the exception. The `try/catch` in
`worker_loop()` is now purely defensive.

Other ways errors reach the future:
- `submit(std::function<int()>{})`: calling an empty function throws
  `std::bad_function_call`, which lands in the future.
- `pool.shutdown()` inside a task throws `std::logic_error`, which lands in the
  future.
- If a `packaged_task` were destroyed without running, its future would get
  `std::future_error(broken_promise)`. Drain-on-shutdown guarantees that never
  happens for an accepted task.

### Memory visibility

`promise.set_value()` *synchronizes-with* the `future.get()` that observes it.
Everything the task wrote before finishing *happens-before* everything the
caller does after `get()` returns. That's why a task can fill a plain
`std::vector<int>` and the caller can read it after `get()` with no atomics
(`future_get_publishes_non_atomic_writes`, verified race-free by TSan).

### Pitfalls

| Pitfall | Notes |
|---|---|
| Discarding the future | Allowed (fire-and-forget), but any exception is then silently lost. |
| Calling `get()` twice | `std::future` is one-shot; a 2nd `get()` is UB/`future_error`. Use `std::shared_future` for multiple readers. |
| **Waiting on a future inside a task** | **Deadlock risk.** If every worker blocks in `get()` waiting for tasks still in the queue, nobody is left to run them. With a 1-worker pool, `submit([&]{ return pool.submit(g).get(); })` hangs forever. Prefer returning the inner future (see `task_can_submit_followup_task`) or keep dependent work out of the pool. |
| Capturing by reference | `[&x]` in the lambda bypasses the copy-at-submit behaviour; the caller must keep `x` alive until the future is ready. |
| `std::async` vs this | `std::async`'s future **blocks in its destructor**; ours doesn't, because the pool, not the future, owns the work. |

### Verification

`tests/test_futures.cpp` (18 tests): values, strings, `void`, function pointers,
functors, member-function pointers, copy-at-submit semantics, `std::ref`,
move-only callable + argument + result, custom exception type & fields
preserved, non-`std::exception` types, worker survives alternating failures,
empty `std::function`, shutdown-from-worker via future, nested submission,
memory visibility through `get()`, ordered collection, and a future that stays
not-ready until a gate opens.

---

## 4. Test suite and correctness review

### Scenario → test map

| # | Scenario | Test(s) |
|---|---|---|
| 1 | Multiple tasks run concurrently | `test_concurrency`: `n_workers_run_n_tasks_simultaneously`, `concurrent_tasks_use_distinct_threads` |
| 2 | More tasks than workers | `test_concurrency`: `more_tasks_than_workers_all_complete`, `concurrency_never_exceeds_worker_count`, `backlog_is_visible_while_workers_are_busy` |
| 3 | Tasks returning values | `test_futures`: `returns_*`, `accepts_*`, `move_only_callable_argument_and_result`, … |
| 4 | Tasks throwing exceptions | `test_futures`: `exception_propagates_through_future`, `non_std_exception_*`, `worker_survives_exceptions_and_keeps_serving` |
| 5 | Multiple producer threads | `test_concurrency`: `multiple_producers_every_task_runs_exactly_once` |
| 6 | Workers sleep on empty queue | `test_concurrency`: `idle_workers_consume_no_cpu` |
| 7 | Workers wake for new tasks | `test_concurrency`: `sleeping_worker_wakes_for_new_task`, `all_sleeping_workers_wake_for_a_burst` |
| 8 | Graceful shutdown with queued tasks | `test_shutdown`: `shutdown_runs_every_queued_task`, `destructor_drains_queue` |
| 9 | Shutdown during execution | `test_shutdown`: `shutdown_waits_for_in_flight_tasks`, `running_task_cannot_submit_once_shutdown_began` |
| 10 | Submit after shutdown | `test_shutdown`: `submit_after_shutdown_throws_runtime_error` |
| – | Shutdown racing producers | `test_shutdown`: `every_accepted_task_runs_even_when_racing_shutdown` |
| – | Concurrent `shutdown()` | `test_shutdown`: `concurrent_shutdown_calls_all_block_until_done` |
| – | No leaked threads | `test_shutdown`: `destructor_leaves_no_worker_threads_running` (counts `/proc/self/task`) |

### Making concurrency tests deterministic

"Sleep 100 ms and hope the other thread did its thing" gives flaky tests. The
suite uses explicit events instead (`tests/test_helpers.hpp`):

- **`Gate`** (`promise<void>` + `shared_future<void>`): holds tasks at a known
  point until the test releases them. Used to keep workers busy so a backlog
  builds, or so shutdown happens while tasks are mid-flight.
- **`std::latch`**: "wait until K tasks have started". `rendezvous()` makes K
  tasks wait for each other, which can only succeed if K run at the *same
  time*. That proves concurrency instead of inferring it from timing. It has
  a timeout, so a broken pool fails the test instead of hanging it.
- **Probing `submit()`**: to know shutdown has *really begun* (queue closed),
  the test retries `submit()` until it throws. That's deterministic, unlike
  sleeping and assuming the shutdown thread got scheduled.

The remaining timing-based checks only use timeouts in the "must *not* have
happened yet" direction (e.g. `shutdown` still blocked after 100 ms while a
gate is shut). They can't produce false failures on a slow machine.
`idle_workers_consume_no_cpu` compares CPU time to wall time with a 10× margin.

---

## Race conditions (Q15)

| Potential race | Why it can't happen here |
|---|---|
| Two workers pop the same task | `front()`+`pop_front()` under the queue mutex |
| Pop from an empty queue after being woken | `wait(lock, pred)` re-checks the predicate under the lock |
| Lost wakeup (push/close between a worker's check and its sleep) | state changed under the same mutex; CV unlock+sleep is atomic |
| Task accepted after shutdown and never run | `closed_` is checked inside `push()` under the lock `close()` takes |
| Worker uses the queue after it's destroyed | `queue_` declared before `workers_`; `shutdown()` joins before members die |
| Two threads `join()` the same worker | `shutdown_mutex_` + `joinable()` check |
| Reading a task's result before it's written | `future.get()` synchronizes-with `set_value` (happens-before) |
| `pending()` / `size()` used for decisions | documented as snapshots only |

What the pool **can't** protect against: tasks that share data among
themselves without synchronization, and tasks capturing references
(`[&x]`) to objects that die before the task runs.

## Deadlocks (Q16)

| Scenario | Status |
|---|---|
| `shutdown()` / destructor called from a worker (self-join) | detected via `thread_local` → `std::logic_error` |
| Join without closing the queue | impossible by construction: `close()` precedes the joins |
| Running the task while holding the queue lock (task calls `submit`) | the lock is released before the task runs |
| `shutdown_mutex_` held while joining | workers never take it, so no cycle |
| **Task blocks on another task's future** | **Not prevented.** If all N workers wait on futures of tasks still queued, nobody runs them. Classic thread-pool starvation deadlock. Avoid blocking `get()` inside tasks. |
| Task waits on something only the submitting thread will do after `shutdown()` | user-level deadlock: shutdown waits for the task, the task waits for the caller |
| Constructor fails mid-way | queue closed before the partial set of jthreads is joined |

## Contention (Q17)

- **The single queue mutex** is the one shared hot spot. Every `submit` and
  every `pop` takes it. With tiny tasks (a few ns of work) and many
  workers/producers, threads spend more time queuing for the lock than
  working. The remedies are batching work into fewer, larger tasks, or a
  work-stealing design (a deque per worker, steal from others when empty),
  which is what TBB and Folly do.
- **Per-task allocation:** `make_shared<packaged_task>`, the future's shared
  state, and `std::function`'s heap storage all go through the allocator,
  which can contend under load.
- **Thundering herd:** `close()` uses `notify_all` on purpose, but that is
  once per pool lifetime. Ordinary pushes use `notify_one`, so one task wakes
  one worker.
- **False sharing in user code:** tasks writing to adjacent elements of a
  shared array share cache lines and slow each other down. That isn't the
  pool's doing, but it shows up in benchmarks.
