#pragma once

#include "TExpr.h"
#include "Platform.h"
#include "LocationChain.h"
#include "Support/JSON.h"
#include "Support/Format.h"
#include "Support/Compare.h"
#include "Support/FileSystem.h"
#include "Support/FixedString.h"
#include "llvm/ADT/FunctionExtras.h"

namespace clice::testing {

struct may_failure;

class Runner {
public:
    static Runner& instance();

    using Suite = void (*)();
    using Test = llvm::unique_function<void()>;

    void add_suite(std::string_view name, Suite suite);

    void on_test(std::string_view name, Test test, bool skipped);

    /// Current test is failed, continue to execute the next test in the suite.
    void fail(const may_failure& failure);

    bool fatal_error_occured() {
        return curr_fatal;
    }

    /// Run all test suites.
    int run_tests();

private:
    Runner() = default;
    Runner(const Runner&) = delete;
    Runner(Runner&&) = delete;

private:
    bool curr_failed = false;
    bool skipped = false;
    bool curr_fatal = false;

    /// Whether all tests in this test suite are skipped.
    bool all_skipped = true;

    std::string curr_suite_name;
    std::uint32_t curr_tests_count = 0;
    std::uint32_t curr_failed_tests_count = 0;
    std::uint32_t total_tests_count = 0;
    std::uint32_t total_suites_count = 0;
    std::uint32_t total_failed_tests_count = 0;
    std::chrono::milliseconds curr_test_duration;
    std::chrono::milliseconds total_test_duration;
    std::unordered_map<std::string_view, std::vector<Suite>> suites;
};

template <fixed_string suite_name>
struct suite {
    template <typename Suite>
    suite(Suite suite) {
        static_assert(std::convertible_to<Suite, Runner::Suite>, "Suite must be stateless!");
        Runner::instance().add_suite(suite_name, suite);
    }
};

struct test {
    test(std::string_view name) : name(name) {}

    template <typename Test>
    void operator= (Test&& test) {
        Runner::instance().on_test(name, std::forward<Test>(test), skipped);
    }

    bool skipped = false;
    std::string name;
};

struct may_failure {
    bool failed = false;
    bool fatal = false;
    std::string expression;
    std::source_location location;
    std::string message;

    may_failure& operator<< (std::string message) {
        this->message += std::move(message);
        return *this;
    }

    ~may_failure() {
        Runner::instance().fail(*this);
    }
};

constexpr inline struct {
    template <typename TExpr>
    may_failure operator() (const TExpr& expr,
                            std::source_location location = std::source_location::current()) const {
        bool failed = false;
        std::string expression = "false";

        if constexpr(is_expr_v<TExpr>) {
            auto result = expr();
            if(!static_cast<bool>(result)) {
                failed = true;

                /// TODO: use pretty print, if the expression is too long.
                expression = std::format("{}", expr);
            }
        } else {
            if(!static_cast<bool>(expr)) {
                failed = true;
            }
        }

        return may_failure{failed, false, expression, location};
    }
} expect;

constexpr inline struct {
    test&& operator/ (test&& test) const {
        test.skipped = true;
        return std::move(test);
    }
} skip;

struct skip_if {
    bool condition;

    test&& operator/ (test&& test) const {
        test.skipped = condition;
        return std::move(test);
    }
};

struct skip_unless {
    bool condition;

    test&& operator/ (test&& test) const {
        test.skipped = !condition;
        return std::move(test);
    }
};

constexpr inline struct {
    may_failure&& operator/ (may_failure&& failure) const {
        if(failure.failed) {
            failure.fatal = true;
        }
        return std::move(failure);
    }
} fatal;

struct that_t {
    template <typename TExpr>
    constexpr decltype(auto) operator% (const TExpr& expr) const {
        return expr;
    }
};

inline that_t that;

}  // namespace clice::testing
