#include "Test/Runner.h"

#include <chrono>
#include <print>
#include <string>
#include <vector>

#include "Support/Format.h"
#include "Support/GlobPattern.h"

#include "llvm/ADT/StringMap.h"

namespace clice::testing {

Runner2& Runner2::instance() {
    static Runner2 runner;
    return runner;
}

void Runner2::add_suite(std::string_view name, std::vector<TestCase> (*cases)()) {
    suites.emplace_back(std::string(name), cases);
}

int Runner2::run_tests(llvm::StringRef filter) {
    constexpr static std::string_view GREEN = "\033[32m";
    constexpr static std::string_view YELLOW = "\033[33m";
    constexpr static std::string_view RED = "\033[31m";
    constexpr static std::string_view CLEAR = "\033[0m";

    std::uint32_t total_tests_count = 0;
    std::uint32_t total_suites_count = 0;
    std::uint32_t total_failed_tests_count = 0;
    std::uint32_t total_skipped_tests_count = 0;
    std::chrono::milliseconds total_test_duration{0};

    struct FailedTest {
        std::string name;
        std::string path;
        std::size_t line;
    };

    std::vector<FailedTest> failed_tests;

    std::optional<GlobPattern> pattern;
    if(!filter.empty()) {
        if(auto result = GlobPattern::create(filter)) {
            pattern.emplace(std::move(*result));
        }
    }

    llvm::StringMap<std::vector<TestCase>> all_suites;

    for(auto suite: suites) {
        auto cases = suite.cases();
        auto& target = all_suites[suite.name];
        for(auto& case_: cases) {
            target.emplace_back(std::move(case_));
        }
    }

    auto matches_suite_filter = [&](llvm::StringRef suite_name) -> bool {
        if(filter.empty()) {
            return true;
        }
        auto pos = filter.find_first_of('.');
        if(pos == std::string::npos) {
            return true;
        }
        return filter.substr(0, pos) == suite_name;
    };

    auto matches_test_filter = [&](llvm::StringRef suite_name, llvm::StringRef test_name) -> bool {
        std::string display_name = std::format("{}.{}", suite_name, test_name);
        if(pattern && !pattern->match(display_name)) {
            return false;
        }
        return true;
    };

    bool focus_mode = false;
    for(auto& [suite_name, test_cases]: all_suites) {
        if(!matches_suite_filter(suite_name)) {
            continue;
        }

        for(auto& test_case: test_cases) {
            if(!matches_test_filter(suite_name, test_case.name)) {
                continue;
            }

            if(test_case.attrs.focus && !test_case.attrs.skip) {
                focus_mode = true;
                break;
            }
        }

        if(focus_mode) {
            break;
        }
    }

    std::println("{}[----------] Global test environment set-up.{}", GREEN, CLEAR);
    if(focus_mode) {
        std::println("{}[  FOCUS   ] Running in focus-only mode.{}", YELLOW, CLEAR);
    }

    for(auto& [suite_name, test_cases]: all_suites) {
        if(!matches_suite_filter(suite_name)) {
            continue;
        }

        bool suite_has_tests = false;

        for(auto& test_case: test_cases) {
            std::string display_name = std::format("{}.{}", suite_name, test_case.name);
            if(!matches_test_filter(suite_name, test_case.name)) {
                continue;
            }

            suite_has_tests = true;

            if(focus_mode && !test_case.attrs.focus) {
                total_skipped_tests_count += 1;
                continue;
            }

            if(test_case.attrs.skip) {
                std::println("{}[ SKIPPED  ] {}{}", YELLOW, display_name, CLEAR);
                total_skipped_tests_count += 1;
                continue;
            }

            std::println("{}[ RUN      ] {}{}", GREEN, display_name, CLEAR);
            total_tests_count += 1;

            using namespace std::chrono;
            auto begin = system_clock::now();
            auto state = test_case.test();
            auto end = system_clock::now();

            bool curr_failed = state == TestState::Failed || state == TestState::Fatal;
            auto duration = duration_cast<milliseconds>(end - begin);
            std::println("{0}[   {1} ] {2} ({3} ms){4}",
                         curr_failed ? RED : GREEN,
                         curr_failed ? "FAILED" : "    OK",
                         display_name,
                         duration.count(),
                         CLEAR);

            total_test_duration += duration;
            if(curr_failed) {
                total_failed_tests_count += 1;
                failed_tests.push_back(FailedTest{display_name, test_case.path, test_case.line});
            }
        }

        if(suite_has_tests) {
            total_suites_count += 1;
        }
    }

    std::println("{}[----------] Global test environment tear-down. {}", GREEN, CLEAR);
    std::println("{}[==========] {} tests from {} test suites ran. ({} ms total){}",
                 GREEN,
                 total_tests_count,
                 total_suites_count,
                 total_test_duration.count(),
                 CLEAR);
    auto total_passed_tests = total_tests_count - total_failed_tests_count;
    if(total_passed_tests > 0) {
        std::println("{}[  PASSED  ] {} tests.{}", GREEN, total_passed_tests, CLEAR);
    }
    if(total_skipped_tests_count > 0) {
        std::println("{}[  SKIPPED ] {} tests.{}", YELLOW, total_skipped_tests_count, CLEAR);
    }
    if(total_failed_tests_count > 0) {
        std::println("{}[  FAILED  ] {} tests, listed below:{}",
                     RED,
                     total_failed_tests_count,
                     CLEAR);
        for(auto& failed: failed_tests) {
            std::println("{}[  FAILED  ] {}{}", RED, failed.name, CLEAR);
            std::println("             at {}:{}", failed.path, failed.line);
        }
        std::println("{}{} FAILED TEST{}{}",
                     RED,
                     total_failed_tests_count,
                     total_failed_tests_count == 1 ? "" : "S",
                     CLEAR);
    }
    return total_failed_tests_count != 0;
}

}  // namespace clice::testing
