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

    template <fixed_string case_name,
              auto test_body,
              fixed_string path,
              std::size_t line,
              TestAttrs attrs = {}>
    inline static bool _register_test_case = [] {
        auto run_test = +[] -> TestState {
            Derived test;
            if constexpr(requires { test.setup(); }) {
                test.setup();
            }

            (test.*test_body)();

            if constexpr(requires { test.teardown(); }) {
                test.teardown();
            }

            return test.state;
        };

        test_cases().emplace_back(case_name.data(), path.data(), line, attrs, run_test);
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
        constexpr auto file_name = std::source_location::current().file_name();                    \
        constexpr auto file_len = std::string_view(file_name).size();                              \
        (void)_register_suites<>;                                                                  \
        (void)_register_test_case<#name,                                                           \
                                  &Self::test_##name,                                              \
                                  fixed_string<file_len>(file_name),                               \
                                  std::source_location::current().line() __VA_OPT__(, )            \
                                      __VA_ARGS__>;                                                \
    }                                                                                              \
    void test_##name()

#define CLICE_CHECK_IMPL(condition, return_action)                                                 \
    do {                                                                                           \
        if(condition) [[unlikely]] {                                                               \
            auto trace = cpptrace::generate_trace();                                               \
            clice::testing::print_trace(trace, std::source_location::current());                   \
            failure();                                                                             \
            return_action;                                                                         \
        }                                                                                          \
    } while(0)

#define EXPECT_TRUE(expr) CLICE_CHECK_IMPL(!(expr), (void)0)
#define EXPECT_FALSE(expr) CLICE_CHECK_IMPL((expr), (void)0)
#define EXPECT_EQ(lhs, rhs) CLICE_CHECK_IMPL((lhs) != (rhs), (void)0)
#define EXPECT_NE(lhs, rhs) CLICE_CHECK_IMPL((lhs) == (rhs), (void)0)

#define ASSERT_TRUE(expr) CLICE_CHECK_IMPL(!(expr), return)
#define ASSERT_FALSE(expr) CLICE_CHECK_IMPL((expr), return)
#define ASSERT_EQ(lhs, rhs) CLICE_CHECK_IMPL((lhs) != (rhs), return)
#define ASSERT_NE(lhs, rhs) CLICE_CHECK_IMPL((lhs) == (rhs), return)

#define CO_ASSERT_TRUE(expr) CLICE_CHECK_IMPL(!(expr), co_return)
#define CO_ASSERT_FALSE(expr) CLICE_CHECK_IMPL((expr), co_return)
#define CO_ASSERT_EQ(lhs, rhs) CLICE_CHECK_IMPL((lhs) != (rhs), co_return)
#define CO_ASSERT_NE(lhs, rhs) CLICE_CHECK_IMPL((lhs) == (rhs), co_return)

}  // namespace clice::testing
