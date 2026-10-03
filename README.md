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
