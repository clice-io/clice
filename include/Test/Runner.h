#pragma once

#include <vector>
#include <string>
#include "llvm/ADT/FunctionExtras.h"

namespace clice::testing {

enum class TestState {
    Passed,
    Skipped,
    Failed,
    Fatal,
};

struct skip_test {};

struct focus {};

template <typename... Ts>
struct test_tag {};

struct TestAttrs {
    bool skiped = false;
    bool focus = false;

    template <typename C, typename... Args>
    static TestAttrs parse(TestState (C::*)(test_tag<Args...>)) {
        TestAttrs attrs;
        if constexpr((std::is_same_v<struct skip, Args> || ...)) {
            attrs.skiped = true;
        }

        if constexpr((std::is_same_v<struct focus, Args> || ...)) {
            attrs.focus = true;
        }
        return attrs;
    }
};

struct TestCase {
    std::string name;
    llvm::unique_function<TestState()> test;
    TestAttrs attrs;
};

struct TestSuite {
    std::string name;
    std::vector<TestCase> (*cases)();
};

class Runner2 {
public:
    static Runner2& instance();

    void add_suite(std::string_view suite, std::vector<TestCase> (*cases)());

    int run_tests();

private:
    std::vector<TestSuite> suites;
};

}  // namespace clice::testing
