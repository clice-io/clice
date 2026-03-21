#include <optional>

#include "test/test.h"
#include "test/tester.h"
#include "feature/feature.h"

namespace clice::testing {

namespace {

namespace protocol = eventide::ipc::protocol;

TEST_SUITE(Hover) {

Tester tester;
std::optional<protocol::Hover> result;

void run(llvm::StringRef code) {
    tester.clear();
    tester.add_main("main.cpp", code);
    ASSERT_TRUE(tester.compile());

    auto points = tester.nameless_points();
    ASSERT_EQ(points.size(), 1U);
    auto offset = points[0];
    result = feature::hover(*tester.unit, offset, {}, feature::PositionEncoding::UTF8);
}

void compile_only(llvm::StringRef code) {
    tester.clear();
    tester.add_main("main.cpp", code);
    ASSERT_TRUE(tester.compile());
}

TEST_CASE(Namespace) {
    run(R"cpp(
namespace $A {
}
)cpp");

    ASSERT_TRUE(result.has_value());
    auto* content = std::get_if<protocol::MarkupContent>(&result->contents);
    ASSERT_TRUE(content != nullptr);
    ASSERT_TRUE(content->value.find("namespace") != std::string::npos);
}

TEST_CASE(FunctionReference) {
    run(R"cpp(
int foo() { return 0; }
int x = $foo();
)cpp");

    ASSERT_TRUE(result.has_value());
    auto* content = std::get_if<protocol::MarkupContent>(&result->contents);
    ASSERT_TRUE(content != nullptr);
    ASSERT_TRUE(content->value.find("foo") != std::string::npos);
}

TEST_CASE(RecordScope) {
    compile_only(R"cpp(
typedef struct A {
    struct B {
        struct C {};
    };

    struct {
        struct D {};
    } _;
} T;

struct FORWARD_STRUCT;
struct FORWARD_CLASS;

void f() {
    struct X {};
    class Y {};

    struct {
        struct Z {};
    } _;
}

namespace n1 {
    namespace n2 {
        struct NA {
            struct NB {};
        };
    }

    namespace {
        struct NC {};
    }
}

namespace out {
    namespace in {
        struct M {
            int x;
            double y;
            char z;
            T a, b;
        };
    }
}
)cpp");
}

TEST_CASE(EnumStyle) {
    compile_only(R"cpp(
enum Free {
    A = 1,
    B = 2,
    C = 999,
};

enum class Scope: long {
    A = -8,
    B = 2,
    C = 100,
};
)cpp");
}

TEST_CASE(FunctionStyle) {
    compile_only(R"cpp(
typedef long long ll;

ll f(int x, int y, ll z = 1) { return 0; }

template<typename T, typename S>
T t(T a, T b, int c, ll d, S s) { return a; }

namespace {
    constexpr static const char* g() { return "hello"; }
}

namespace test {
    namespace {
        [[deprecated("test deprecate message")]] consteval int h() { return 1; }
    }
}

struct A {
    constexpr static A m(int left, double right) { return A(); }
};
)cpp");
}

TEST_CASE(VariableStyle) {
    compile_only(R"cpp(
void f() {
    constexpr static auto x1 = 1;
}
)cpp");
}

TEST_CASE(AutoAndDecltype) {
    compile_only(R"cpp(
$(a1)aut$(a2)o$(a3) i = -1;

$(d1)dec$(d2)ltype$(d3)(i) j = 2;

struct A { int x; };

aut$(a4)o va$(a5)r = A{};

a$(fa)uto f1() { return 1; }

de$(fn_decltype)cltype(au$(fn_decltype_auto)to) f2() {}

int f3(au$(fn_para_auto)to x) {}
)cpp");
}

TEST_CASE(Expr) {
    compile_only(R"cpp(
int xxxx = 1;
int yyyy = xx$(e1)xx;

struct A {
    int function(int param) {
        return thi$(e2)s$(e3)->$(e4)funct$(e5)ion(para$(e6)m);
    }

    int fn(int param) {
        return static$(e7)_cast<A*>(nul$(e8)lptr)->function(par$(e9)am);
    }
};
)cpp");
}

};  // TEST_SUITE(Hover)

}  // namespace

}  // namespace clice::testing
