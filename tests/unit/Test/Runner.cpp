#include <print>
#include <chrono>
#include "Test/Runner.h"

namespace clice::testing {

Runner2& Runner2::instance() {
    static Runner2 runner;
    return runner;
}

void Runner2::add_suite(std::string_view name, std::vector<TestCase> (*cases)()) {
    suites.emplace_back(std::string(name), cases);
}

int Runner2::run_tests() {
    constexpr static std::string_view GREEN = "\033[32m";
    constexpr static std::string_view YELLOW = "\033[33m";
    constexpr static std::string_view RED = "\033[31m";
    constexpr static std::string_view CLEAR = "\033[0m";

    std::uint32_t total_tests_count = 0;
    std::uint32_t total_suites_count = 0;
    std::uint32_t total_failed_tests_count = 0;
    std::chrono::milliseconds curr_test_duration;
    std::chrono::milliseconds total_test_duration;

    struct Suite {
        std::string name;
        std::vector<TestCase> cases;
    };

    std::vector<Suite> all_suites;
    for(auto suite: suites) {
        auto cases = suite.cases();
        total_suites_count += 1;
        total_tests_count += cases.size();
        all_suites.emplace_back(suite.name, std::move(cases));
    }

    std::println("{}[----------] Global test environment set-up.{}", GREEN, CLEAR);

    bool focus_only_mode = false;

    for(auto& [suite_name, test_cases]: all_suites) {
        for(auto& [test_name, test, attrs]: test_cases) {
            if(!attrs.focus) {
                focus_only_mode = true;
                continue;
            }

            std::println("{}[ RUN      ] {}.{}{}", GREEN, suite_name, test_name, CLEAR);

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

    if(!focus_only_mode) {
        for(auto& [suite_name, test_cases]: all_suites) {
            for(auto& [test_name, test, attrs]: test_cases) {
                if(attrs.skiped) {
                    std::println("{}[ SKIPPED  ] {}.{}{}", YELLOW, suite_name, test_name, CLEAR);
                    continue;
                }

                std::println("{}[ RUN      ] {}.{}{}", GREEN, suite_name, test_name, CLEAR);

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
