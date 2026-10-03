# thread-pool

A fixed-size thread pool in C++20, built on `std::mutex`, `std::condition_variable`
and `std::packaged_task`. No external dependencies.

- N worker threads pull tasks from a shared, thread-safe queue
- `submit()` returns a `std::future` for the task's result
- exceptions thrown inside a task are rethrown by `future.get()` on the caller's thread
- `submit_batch()` queues many tasks with one lock and one wakeup
- graceful shutdown: queued tasks finish, new ones are rejected, workers are joined
- tested under ThreadSanitizer and AddressSanitizer

## Usage

```cpp
#include "ThreadPool.hpp"
#include <iostream>
#include <stdexcept>

int main() {
    ThreadPool pool(4);  // defaults to std::thread::hardware_concurrency()

    // Any no-argument callable. Pass inputs by capturing them.
    int x = 21;
    std::future<int> answer = pool.submit([x] { return x * 2; });
    std::cout << answer.get() << '\n';  // 42

    // Exceptions travel through the future.
    auto failing = pool.submit([] { throw std::runtime_error("oops"); });
    try {
        failing.get();
    } catch (const std::exception& e) {
        std::cout << "task failed: " << e.what() << '\n';
    }

    // Many small tasks: f(0) .. f(999) queued in one go.
    auto squares = pool.submit_batch(1000, [](std::size_t i) { return i * i; });
    std::size_t sum = 0;
    for (auto& f : squares) sum += f.get();
    std::cout << sum << '\n';

    pool.shutdown();  // optional, the destructor does the same
}
```

`submit()` throws `std::runtime_error` once the pool has been shut down.

## Build

Needs CMake 3.20+ and GCC 11+ or Clang 14+.

```bash
cmake -B build                  # Release by default
cmake --build build -j
ctest --test-dir build          # 61 tests across 5 test binaries
```

Examples and benchmarks:

```bash
./build/basic_demo              # 20 tasks on 4 workers, shows which worker ran what
./build/speedup_demo            # prime counting: sequential vs pool
./build/bench_workers           # speedup as the worker count grows
./build/bench_granularity       # task size vs overhead, submit vs submit_batch
```

### Sanitizers

```bash
cmake -B build-tsan -DTP_SANITIZER=thread -DCMAKE_BUILD_TYPE=Debug
cmake --build build-tsan -j
ctest --test-dir build-tsan
```

`-DTP_SANITIZER=address,undefined` builds with ASan + UBSan instead.

On WSL2, TSan binaries can abort with `unexpected memory mapping` because of the
kernel's ASLR settings. Running them with ASLR disabled fixes it:
`setarch $(uname -m) -R ./build-tsan/test_shutdown`.

## Layout

```
include/
  BlockingQueue.hpp    thread-safe MPMC queue (mutex + condition_variable)
  ThreadPool.hpp       pool interface + submit/submit_batch templates
src/
  ThreadPool.cpp       workers, shutdown, enqueue
tests/                 unit + concurrency tests (small built-in test harness)
examples/              demo programs
benchmarks/            worker-count and task-size benchmarks
docs/DESIGN.md         design notes: synchronisation, shutdown, races, deadlocks
```

## Design

```
 submit() ──► [ BlockingQueue: deque + mutex + condvar ] ──► worker 0..N-1
   │                                                            │
   └── std::future ◄──── packaged_task stores result/exception ─┘
```

**Worker loop.** Each worker runs `while (auto task = queue.pop()) task();`.
`pop()` sleeps on the condition variable while the queue is empty and returns
`nullopt` only once the queue is closed *and* drained, so that's when the worker
exits. Tasks run outside the queue lock.

**Results.** `submit(f)` wraps `f` in a `std::packaged_task<R()>`, keeps its
future, and puts a type-erased `std::function<void()>` on the queue. Running the
packaged task stores either the return value or the thrown exception in the
future's shared state, so a throwing task never takes a worker down.

**Shutdown.** `shutdown()` (also called by the destructor) closes the queue,
which rejects new submits and `notify_all`s the sleeping workers. Then it joins
every worker. Work that was already queued still runs. Running tasks are never
interrupted. `submit()` after shutdown throws.

A few details that matter for correctness:

- the `closed` flag is a plain `bool` written under the queue mutex, not an
  atomic: setting it without the lock could slip between a worker's predicate
  check and its sleep, and that worker would never wake up (lost wakeup)
- the queue is declared before the workers, so it's destroyed after them
- calling `shutdown()` from inside a task would make a worker join itself; it's
  detected and throws `std::logic_error` instead of deadlocking

`docs/DESIGN.md` has the longer write-up: why a mutex and not atomics, what
`wait()` does, race conditions, deadlock cases and contention.

## Benchmarks

AMD Ryzen 7 7435HS, WSL2 (8 hardware threads visible, 4 cores x 2 SMT), GCC 13, `-O3`.
Best of 3 runs.

**Worker count** (`bench_workers`): counting primes below 6.4M in 64 chunks.

| workers | time (ms) | speedup | efficiency |
|--------:|----------:|--------:|-----------:|
| seq     | 761.6     | 1.00x   | -          |
| 1       | 765.0     | 1.00x   | 100%       |
| 2       | 403.3     | 1.89x   | 94%        |
| 4       | 218.7     | 3.48x   | 87%        |
| 8       | 129.2     | 5.89x   | 74%        |
| 16      | 124.8     | 6.10x   | 38%        |

Close to linear up to the 4 physical cores. 8 workers adds ~70% more, not 100%,
because SMT siblings share a core's execution units. 16 workers is no faster than
8. More threads than hardware threads just take turns.

**Task size** (`bench_granularity`): the same ~4M units of work split into tasks
of different sizes, 8 workers. One unit ≈ 1 ns.

| units/task | tasks  | seq (ms) | submit (ms) | batch (ms) | submit | batch |
|-----------:|-------:|---------:|------------:|-----------:|-------:|------:|
| 16         | 262144 | 3.7      | 6115.7      | 157.0      | 0.00x  | 0.02x |
| 256        | 16384  | 5.8      | 376.6       | 8.6        | 0.02x  | 0.67x |
| 1024       | 4096   | 5.9      | 95.2        | 1.8        | 0.06x  | 3.21x |
| 4096       | 1024   | 5.9      | 1.9         | 1.2        | 3.13x  | 4.74x |
| 16384      | 256    | 5.9      | 5.8         | 1.1        | 1.02x  | 5.58x |
| 65536      | 64     | 5.9      | 1.0         | 1.0        | 6.01x  | 5.80x |

With tiny tasks the workers drain the queue faster than it fills, so they go
back to sleep, and every `submit()` has to wake one. On WSL2 that wakeup (a
futex syscall that goes through the hypervisor) costs ~20 µs, much more than the
task itself. `submit_batch()` takes the lock and wakes the workers once for the
whole batch, which brings per-task overhead down to under 1 µs. Around 4k–16k
units the plain `submit` numbers are noisy run to run: it depends on whether
the producer stays ahead (workers never sleep) or not (a wakeup per task).

Takeaway: give each task at least a few microseconds of work, or batch.

## Limitations

- **One shared queue.** Every submit and every pop takes the same mutex, which
  becomes the bottleneck with many tiny tasks or many workers. Work-stealing
  pools (a deque per worker) avoid this.
- **No priorities, cancellation or timeouts.** Once queued, a task will run.
- **Blocking on a future inside a task can deadlock.** If every worker waits on
  a task that's still in the queue, nobody is left to run it.
- **Two allocations per task** (`shared_ptr<packaged_task>` and the
  `std::function`). C++23's `std::move_only_function` would remove one.
