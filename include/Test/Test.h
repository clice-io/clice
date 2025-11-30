#pragma once

#include <print>
#include <source_location>
#include <string>
#include <vector>

#include "Platform.h"
#include "Runner.h"
#include "Support/Compare.h"
#include "Support/FileSystem.h"
#include "Support/FixedString.h"

#include "cpptrace/cpptrace.hpp"
#include "llvm/ADT/FunctionExtras.h"
#include "llvm/ADT/StringMap.h"

namespace clice::testing {

template <fixed_string TestName, typename Derived>
struct TestSuiteDef {
private:
    struct TestCaseDef {
        std::string name;
        TestState (Derived::*func)();
    };

    TestState state = TestState::Passed;

public:
    using Self = Derived;

    void failure() {
        state = TestState::Failed;
    }

    void pass() {
        state = TestState::Passed;
    }

    void skip() {
        state = TestState::Skipped;
    }

    constexpr inline static auto& test_cases() {
        static std::vector<TestCase> instance;
        return instance;
    }

    constexpr inline static auto suites() {
        return std::move(test_cases());
    }

    template <typename T = void>
    inline static bool _register_suites = [] {
        Runner2::instance().add_suite(TestName.data(), &suites);
        return true;
    }();

    template <fixed_string CaseName, auto Case, TestAttrs attrs = {}>
    inline static bool _register_test_case = [] {
        auto run_test = +[] -> TestState {
            Derived test;
            if constexpr(requires { test.setup(); }) {
                test.setup();
            }

            (test.*Case)();

            if constexpr(requires { test.teardown(); }) {
                test.teardown();
            }

            return test.state;
        };

        test_cases().emplace_back(CaseName.data(), run_test, attrs);
        return true;
    }();
};

inline void print_trace(cpptrace::stacktrace& trace, std::source_location location) {
    auto& frames = trace.frames;
    auto it = std::ranges::find_if(frames, [&](cpptrace::stacktrace_frame& frame) {
        return frame.filename != location.file_name();
    });
    frames.erase(it, frames.end());
    trace.print();
}

#define TEST_SUITE(name) struct name##TEST : TestSuiteDef<#name, name##TEST>

#define TEST_CASE(name, ...)                                                                       \
    void _register_##name() {                                                                      \
        (void)_register_suites<>;                                                                  \
        (void)_register_test_case<#name, &Self::test_##name __VA_OPT__(, ) __VA_ARGS__>;           \
    }                                                                                              \
    void test_##name()

#define EXPECT_TRUE(expr)                                                                          \
    if(expr) {                                                                                     \
        auto trace = cpptrace::generate_trace();                                                   \
        print_trace(trace, std::source_location::current());                                       \
        failure();                                                                                 \
    }

#define EXPECT_EQ(lhs, rhs)                                                                        \
    if(lhs != rhs) {                                                                               \
        auto trace = cpptrace::generate_trace();                                                   \
        print_trace(trace, std::source_location::current());                                       \
        failure();                                                                                 \
    }

#define ASSERT_TRUE(expr)                                                                          \
    if(!(expr)) {                                                                                  \
        auto trace = cpptrace::generate_trace();                                                   \
        print_trace(trace, std::source_location::current());                                       \
        return failure();                                                                          \
    }

#define ASSERT_FALSE(expr)                                                                         \
    if((expr)) {                                                                                   \
        auto trace = cpptrace::generate_trace();                                                   \
        print_trace(trace, std::source_location::current());                                       \
        return failure();                                                                          \
    }

#define ASSERT_EQ(lhs, rhs)                                                                        \
    if(lhs != rhs) {                                                                               \
        auto trace = cpptrace::generate_trace();                                                   \
        print_trace(trace, std::source_location::current());                                       \
        return failure();                                                                          \
    }

#ifndef ASSERT_NE
#define ASSERT_NE(lhs, rhs)                                                                        \
    if((lhs) == (rhs)) {                                                                           \
        auto trace = cpptrace::generate_trace();                                                   \
        clice::testing::print_trace(trace, std::source_location::current());                       \
        return failure();                                                                          \
    }
#endif

#ifndef EXPECT_NE
#define EXPECT_NE(lhs, rhs)                                                                        \
    if((lhs) == (rhs)) {                                                                           \
        auto trace = cpptrace::generate_trace();                                                   \
        clice::testing::print_trace(trace, std::source_location::current());                       \
        failure();                                                                                 \
    }
#endif

#define CO_ASSERT_TRUE(expr)                                                                       \
    if(!(expr)) {                                                                                  \
        auto trace = cpptrace::generate_trace();                                                   \
        print_trace(trace, std::source_location::current());                                       \
        failure();                                                                                 \
        co_return;                                                                                 \
    }

#define CO_ASSERT_FALSE(expr)                                                                      \
    if((expr)) {                                                                                   \
        auto trace = cpptrace::generate_trace();                                                   \
        print_trace(trace, std::source_location::current());                                       \
        failure();                                                                                 \
        co_return;                                                                                 \
    }

#define CO_ASSERT_EQ(lhs, rhs)                                                                     \
    if((lhs) != (rhs)) {                                                                           \
        auto trace = cpptrace::generate_trace();                                                   \
        print_trace(trace, std::source_location::current());                                       \
        failure();                                                                                 \
        co_return;                                                                                 \
    }

#define CO_ASSERT_NE(lhs, rhs)                                                                     \
    if((lhs) == (rhs)) {                                                                           \
        auto trace = cpptrace::generate_trace();                                                   \
        print_trace(trace, std::source_location::current());                                       \
        failure();                                                                                 \
        co_return;                                                                                 \
    }

}  // namespace clice::testing
