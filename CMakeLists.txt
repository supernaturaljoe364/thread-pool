cmake_minimum_required(VERSION 3.20)

project(thread_pool LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# Sanitizers are the best tool for catching concurrency bugs that tests miss.
#   cmake -B build-tsan -DTP_SANITIZER=thread
#   cmake -B build-asan -DTP_SANITIZER=address
set(TP_SANITIZER "" CACHE STRING "Sanitizer to enable: thread | address | undefined")
if(TP_SANITIZER)
  add_compile_options(-fsanitize=${TP_SANITIZER} -fno-omit-frame-pointer -g)
  add_link_options(-fsanitize=${TP_SANITIZER})
endif()

find_package(Threads REQUIRED)

# ---------------------------------------------------------------------------
# Core library
# ---------------------------------------------------------------------------
add_library(threadpool STATIC src/ThreadPool.cpp)
target_include_directories(threadpool PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(threadpool PUBLIC Threads::Threads)
target_compile_options(threadpool PRIVATE -Wall -Wextra -Wpedantic)

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
enable_testing()

function(tp_add_test name)
  add_executable(${name} tests/${name}.cpp)
  target_link_libraries(${name} PRIVATE threadpool)
  target_compile_options(${name} PRIVATE -Wall -Wextra -Wpedantic)
  add_test(NAME ${name} COMMAND ${name})
endfunction()

tp_add_test(test_blocking_queue)
tp_add_test(test_thread_pool)
tp_add_test(test_futures)
