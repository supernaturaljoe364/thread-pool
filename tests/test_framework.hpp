#pragma once

// Minimal self-registering test harness so the project has no external
// dependencies. Each test binary includes this header, defines tests with
// TEST(name), and calls RUN_ALL_TESTS() from main().

#include <cstdlib>
#include <exception>
#include <functional>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace testing {

struct TestCase {
    std::string name;
    std::function<void()> body;
};

inline std::vector<TestCase>& registry() {
    static std::vector<TestCase> tests;
    return tests;
}

struct Registrar {
    Registrar(std::string name, std::function<void()> body) {
        registry().push_back({std::move(name), std::move(body)});
    }
};

struct AssertionFailure : std::exception {
    std::string message;
    explicit AssertionFailure(std::string msg) : message(std::move(msg)) {}
    const char* what() const noexcept override { return message.c_str(); }
};

inline int run_all() {
    int failed = 0;
    for (const auto& test : registry()) {
        try {
            test.body();
            std::cout << "[ PASS ] " << test.name << '\n';
        } catch (const AssertionFailure& e) {
            ++failed;
            std::cout << "[ FAIL ] " << test.name << "\n         " << e.what() << '\n';
        } catch (const std::exception& e) {
            ++failed;
            std::cout << "[ FAIL ] " << test.name
                      << "\n         unexpected exception: " << e.what() << '\n';
        }
    }
    std::cout << registry().size() - failed << '/' << registry().size() << " tests passed\n";
    return failed == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}

}  // namespace testing

#define TEST(name)                                                       \
    static void name();                                                  \
    static ::testing::Registrar name##_registrar(#name, name);           \
    static void name()

#define EXPECT_TRUE(cond)                                                \
    do {                                                                 \
        if (!(cond)) {                                                   \
            std::ostringstream os_;                                      \
            os_ << __FILE__ << ':' << __LINE__ << ": expected " #cond;   \
            throw ::testing::AssertionFailure(os_.str());                \
        }                                                                \
    } while (0)

#define EXPECT_EQ(a, b)                                                  \
    do {                                                                 \
        const auto a_ = (a);                                             \
        const auto b_ = (b);                                             \
        if (!(a_ == b_)) {                                               \
            std::ostringstream os_;                                      \
            os_ << __FILE__ << ':' << __LINE__ << ": expected " #a       \
                << " == " #b << " (" << a_ << " vs " << b_ << ')';       \
            throw ::testing::AssertionFailure(os_.str());                \
        }                                                                \
    } while (0)

#define RUN_ALL_TESTS() ::testing::run_all()
