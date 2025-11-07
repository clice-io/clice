#include "Test/Test.h"
#include "Support/Logging.h"
#include "Support/GlobPattern.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Signals.h"

using namespace clice;
using namespace clice::testing;

constexpr static std::string_view GREEN = "\033[32m";
constexpr static std::string_view YELLOW = "\033[33m";
constexpr static std::string_view RED = "\033[31m";
constexpr static std::string_view CLEAR = "\033[0m";

namespace {

namespace cl = llvm::cl;

cl::OptionCategory unittest_category("clice Unittest Options");

cl::opt<std::string> test_dir{
    "test-dir",
    cl::desc("Specify the test source directory path"),
    cl::value_desc("path"),
    cl::Required,
    cl::cat(unittest_category),
};

cl::opt<std::string> test_filter{
    "test-filter",
    cl::desc("A glob pattern to run subset of tests"),
    cl::cat(unittest_category),
};

cl::opt<bool> enable_example{
    "enable-example",
    cl::init(false),
    cl::cat(unittest_category),
};

std::optional<GlobPattern> pattern;

}  // namespace

namespace clice::testing {

Runner& Runner::instance() {
    static Runner runner;
    return runner;
}

void Runner::add_suite(std::string_view name, Suite suite) {
    suites[name].emplace_back(suite);
}

void Runner::on_test(std::string_view name, Test test, bool skipped) {
    std::string full_name = std::format("{}.{}", curr_suite_name, name);

    /// If this test if filter, directly return.
    if(pattern && !pattern->match(full_name)) {
        return;
    }

    /// If there is any test in the suite, we print the suite start info.
    if(all_skipped) {
        std::println("{}[----------] tests from {}{}", GREEN, curr_suite_name, CLEAR);
        all_skipped = false;
    }

    if(skipped) {
        /// If this test is marked as skipped, only print skip information.
        std::println("{}[ SKIPPED  ] {}{}", YELLOW, full_name, CLEAR);
        return;
    }

    /// Reset whether this test is failed or fatal.
    curr_failed = false;
    curr_fatal = false;

    using namespace std::chrono;

    std::println("{}[ RUN      ] {}.{}{}", GREEN, curr_suite_name, name, CLEAR);
    auto begin = system_clock::now();

    test();

    auto duration = duration_cast<milliseconds>(system_clock::now() - begin);
    std::println("{}[   {} ] {} ({} ms){}",
                 curr_failed ? RED : GREEN,
                 curr_failed ? "FAILED" : "    OK",
                 full_name,
                 duration.count(),
                 CLEAR);

    /// Update test information.
    curr_tests_count += 1;
    total_tests_count += 1;

    curr_test_duration += duration;
    total_test_duration += duration;

    if(curr_failed) {
        curr_failed_tests_count += 1;
        total_failed_tests_count += 1;
    }
}

void Runner::fail(const may_failure& failure) {
    if(failure.failed) {
        curr_failed = true;
        std::println("{}Failure at {}:{}:{}! [{}]{}",
                     RED,
                     failure.location.file_name(),
                     failure.location.line(),
                     failure.location.column(),
                     failure.expression,
                     CLEAR);
        std::println("{}", failure.message);
    }

    if(failure.fatal) {
        curr_fatal = true;
        std::println("{}--> Test stopped due to fatal error.{}", RED, CLEAR);
        std::exit(1);
    }
}

int Runner::run_tests() {
    /// Register all tests.
    std::println("{}[----------] Global test environment set-up.{}", GREEN, CLEAR);

    for(auto& [suite_name, suite]: suites) {
        if(!enable_example && suite_name == "TEST.Example") {
            continue;
        }

        if(!test_filter.empty()) {
            auto pos = test_filter.find_first_of('.');
            if(pos != std::string::npos && test_filter.substr(0, pos) != suite_name) {
                continue;
            }
        }

        curr_fatal = false;
        all_skipped = true;
        curr_suite_name = suite_name;
        curr_tests_count = 0;
        curr_failed_tests_count = 0;
        curr_test_duration = std::chrono::milliseconds();

        for(auto& callback: suite) {
            callback();
        }

        /// If there is any test in the suite, we print the suite info.
        if(!all_skipped) {
            total_suites_count += 1;
            std::println("{}[----------] {} tests from {} ({} ms total)\n{}",
                         GREEN,
                         curr_tests_count,
                         suite_name,
                         total_test_duration.count(),
                         CLEAR);
        }
    }

    std::println("{}[----------] Global test environment tear-down{}", GREEN, CLEAR);
    std::println("{}[==========] {} tests from {} test suites ran. ({} ms total){}",
                 GREEN,
                 total_tests_count,
                 total_suites_count,
                 total_test_duration.count(),
                 CLEAR);

    return total_failed_tests_count != 0;
}

}  // namespace clice::testing

int main(int argc, const char* argv[]) {
    llvm::sys::PrintStackTraceOnErrorSignal(argv[0]);
    llvm::cl::HideUnrelatedOptions(unittest_category);
    llvm::cl::ParseCommandLineOptions(argc, argv, "clice test\n");

    logging::stderr_logger("clice", logging::options);

    if(!test_filter.empty()) {
        if(auto result = GlobPattern::create(test_filter)) {
            pattern.emplace(std::move(*result));
        }
    }

    if(auto result = fs::init_resource_dir(argv[0]); !result) {
        std::println("Failed to get resource directory, because {}", result.error());
        return 1;
    }

    using namespace clice::testing;
    return Runner::instance().run_tests();
}
