#include "Test/Test.h"
#include "Test/Tester.h"
#include "AST/Utility.h"
#include "Feature/DocumentSymbol.h"

namespace clice::testing {
namespace {
TEST_SUITE(DocumentSymbol) {

Tester tester;
std::vector<feature::DocumentSymbol> symbols;

void run(llvm::StringRef code) {
    tester.clear();
    tester.add_main("main.cpp", code);
    ASSERT_TRUE(tester.compile_with_pch());
    ASSERT_TRUE(tester.unit.has_value());
    symbols = feature::document_symbols(*tester.unit);
}

auto total_size_wrapper(const std::vector<feature::DocumentSymbol>& result) -> size_t {
    size_t size = 0;
    std::function<void(const std::vector<feature::DocumentSymbol>&, size_t&)> total_size =
        [&total_size](const std::vector<feature::DocumentSymbol>& result, size_t& size) {
            for(auto& item: result) {
                ++size;
                total_size(item.children, size);
            }
        };
    total_size(result, size);
    return size;
};

TEST_CASE(Namespace) {
    const char* main = R"cpp(
namespace _1 {
    namespace _2 {

    }
}

namespace _1 {
    namespace _2 {
        namespace _3 {
        }
    }
}

namespace {}

namespace _1::_2{
}
)cpp";

    run(main);
    ASSERT_EQ(total_size_wrapper(symbols), 8U);
}

TEST_CASE(Struct) {
    const char* main = R"cpp(
struct _1 {};
struct _2 {};

struct _3 {
    struct _4 {};
    struct _5 {};
};
)cpp";

    run(main);
    ASSERT_EQ(total_size_wrapper(symbols), 5U);
}

TEST_CASE(Field) {
    const char* main = R"cpp(
struct x {
    int x1;
    int x2;

    struct y {
        int y1;
        int y2;
    };

    static int x3;
};
)cpp";

    run(main);
    ASSERT_EQ(total_size_wrapper(symbols), 7U);
}

TEST_CASE(Constructor) {
    const char* main = R"cpp(
struct S {
    int x;

    S(): x(0) {}
    S(int x) : x(x) {}
    S(const S& s) : x(s.x) {}
    ~S() {}
};
)cpp";
    run(main);
    ASSERT_EQ(total_size_wrapper(symbols), 6U);
}

TEST_CASE(Method) {
    const char* main = R"cpp(

struct _0 {
    void f(int x) {}
    void f(int* x) {}
    void f1(int& x) {}
    void f2(const int& x) {}
    void f2(const _0& x) {}
    void f2(_0 x) {}
};
)cpp";

    run(main);
    ASSERT_EQ(total_size_wrapper(symbols), 7U);
}

TEST_CASE(Enum) {
    const char* main = R"cpp(

enum class A {
    _1,
    _2,
    _3,
};

enum B {
    _a,
    _b,
    _c,
};
)cpp";

    run(main);
    ASSERT_EQ(total_size_wrapper(symbols), 8U);
}

TEST_CASE(TopLevelVariable) {
    const char* main = R"cpp(
constexpr auto x = 1;
int y = 2;
)cpp";

    run(main);
    ASSERT_EQ(total_size_wrapper(symbols), 2U);
}

TEST_CASE(Macro) {
    const char* main = R"cpp(
#define CLASS(X) class X

CLASS(test) {
    int x = 1;
};

#define VAR(X) int X = 1;
VAR(test)
)cpp";

    run(main);

    /// expect(that % total_size_wrapper(symbols) == 3);
}

};  // TEST_SUITE(DocumentSymbol)
}  // namespace
}  // namespace clice::testing
