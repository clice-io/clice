#include <print>
#include <chrono>
#include "Test/Runner.h"
#include "Support/GlobPattern.h"

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
    std::chrono::milliseconds curr_test_duration;
    std::chrono::milliseconds total_test_duration;

    std::optional<GlobPattern> pattern;
    if(!filter.empty()) {
        if(auto result = GlobPattern::create(filter)) {
            pattern.emplace(std::move(*result));
        }
    }

    struct Suite {
        std::string name;
        std::vector<TestCase> cases;
    };

    std::vector<Suite> all_suites;
    for(auto suite: suites) {
        auto cases = suite.cases();
        total_suites_count += 1;

        all_suites.emplace_back(suite.name, std::move(cases));
    }

    std::println("{}[----------] Global test environment set-up.{}", GREEN, CLEAR);

    for(auto& [suite_name, test_cases]: all_suites) {
        if(!filter.empty()) {
            auto pos = filter.find_first_of('.');
            if(pos != std::string::npos && filter.substr(0, pos) != suite_name) {
                continue;
            }
        }

        for(auto& [test_name, test, attrs]: test_cases) {
            std::string display_name = std::format("{}.{}", suite_name, test_name);
            if(pattern && !pattern->match(display_name)) {
                continue;
            }

            if(attrs.skip) {
                std::println("{}[ SKIPPED  ] {}.{}{}", YELLOW, suite_name, test_name, CLEAR);
                continue;
            }

            std::println("{}[ RUN      ] {}.{}{}", GREEN, suite_name, test_name, CLEAR);
            total_tests_count += 1;

            using namespace std::chrono;
            auto begin = system_clock::now();
            auto state = test();
            auto end = system_clock::now();

            bool curr_failed = state == TestState::Failed;
            auto duration = duration_cast<milliseconds>(end - begin);
            std::println("{0}[   {1} ] {2}.{3} ({4} ms){5}",
                         curr_failed ? RED : GREEN,
                         curr_failed ? "FAILED" : "    OK",
                         suite_name,
                         test_name,
                         duration.count(),
                         CLEAR);

            curr_test_duration += duration;
            total_test_duration += duration;
            if(curr_failed) {
                total_failed_tests_count += 1;
            }
        }
    }

    std::println("{}[----------] Global test environment tear-down. {}", GREEN, CLEAR);
    std::println("{}[==========] {} tests from {} test suites ran. ({} ms total){}",
                 GREEN,
                 total_tests_count,
                 total_suites_count,
                 total_test_duration.count(),
                 CLEAR);
    return total_failed_tests_count != 0;
}

}  // namespace clice::testing
