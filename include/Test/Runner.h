#pragma once

#include <string>
#include <vector>

#include "llvm/ADT/FunctionExtras.h"
#include "llvm/ADT/StringRef.h"

namespace clice::testing {

enum class TestState {
    Passed,
    Skipped,
    Failed,
    Fatal,
};

struct TestAttrs {
    bool skip = false;
    bool focus = false;
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

    int run_tests(llvm::StringRef filter);

private:
    std::vector<TestSuite> suites;
};

}  // namespace clice::testing
