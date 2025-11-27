#include <string>
#include <print>
#include <vector>
#include "Support/FixedString.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/FunctionExtras.h"
#include "Test.h"
#include "Runner.h"

namespace clice::testing {

template <fixed_string TestName, typename Derived>
struct TestSuiteDef {
private:
    struct TestCaseDef {
        std::string name;
        TestState (Derived::*func)();
    };

public:
    using Self = Derived;

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

    template <fixed_string CaseName, auto Case>
    inline static bool _register_test_case = [] {
        auto run_test = +[] -> TestState {
            Derived test;
            if constexpr(requires { test.setup(); }) {
                test.setup();
            }

            auto state = (test.*Case)({});

            if constexpr(requires { test.teardown(); }) {
                test.teardown();
            }

            return state;
        };

        test_cases().emplace_back(CaseName.data(), run_test, TestAttrs::parse(Case));
        return true;
    }();
};

#define TEST_SUITE(name) struct name : TestSuiteDef<#name, name>

#define TEST_CASE(name, ...)                                                                       \
    void _register_##name() {                                                                      \
        (void)_register_suites<>;                                                                  \
        (void)_register_test_case<#name, &Self::test_##name>;                                      \
    }                                                                                              \
    TestState test_##name(test_tag<__VA_ARGS__>)

}  // namespace clice::testing
