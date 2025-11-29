#include "Test/Test.h"
#include "Test/Tester.h"
#include "Feature/FoldingRange.h"

namespace clice::testing {
namespace {
TEST_SUITE(FoldingRange) {

Tester tester;
feature::FoldingRanges ranges;

void run(llvm::StringRef code) {
    tester.clear();
    tester.add_main("main.cpp", code);
    ASSERT_TRUE(tester.compile_with_pch());
    ranges = feature::folding_ranges(*tester.unit);
};

void expect_folding(std::uint32_t index,
                    llvm::StringRef begin,
                    llvm::StringRef end,
                    feature::FoldingRangeKind kind,
                    std::source_location loc = std::source_location::current()) {
    auto& folding = ranges[index];
    auto begin_point = tester.point(begin, "main.cpp");
    auto end_point = tester.point(end, "main.cpp");
    ASSERT_EQ(folding.range.begin, begin_point);
    ASSERT_EQ(folding.range.end, end_point);

    /// FIXME: folding.kind is not implemented
    /// expect(that % folding.kind.value() == kind.value(), loc);
};

using enum feature::FoldingRangeKind::Kind;

TEST_CASE(Namespace) {
    run(R"cpp(
namespace single_line { }

namespace with_nodes $(1){
    struct inner $(3){
        int x;
    }$(4);
}$(2)

namespace strange
                 $(5){

                 }$(6)

#define NS_BEGIN namespace ns {
#define NS_END }

$(7)NS_BEGIN
NS_END$(8)
)cpp");

    ASSERT_EQ(ranges.size(), 4U);
    expect_folding(0, "1", "2", Namespace);
    expect_folding(1, "3", "4", Namespace);
    expect_folding(2, "5", "6", Namespace);
    expect_folding(3, "7", "8", Namespace);
}

TEST_CASE(Enum) {
    run(R"cpp(
enum e1 $(1){
    A,
    B,
    C
}$(2);

enum class e2 $(3){
    A,
    B,
    C
}$(4);

enum e3 { D };

)cpp");

    ASSERT_EQ(ranges.size(), 2U);
    expect_folding(0, "1", "2", Enum);
    expect_folding(1, "3", "4", Enum);
}

TEST_CASE(Record) {
    run(R"cpp(
struct s1 $(1){
    int x;
    float y;
}$(2);

struct s2 {};

struct s3;

union u1 $(3){
    int x;
    float y;
}$(4);

struct u2 $(5){
    struct s4 $(7){

    }$(8);
}$(6);

void foo() $(9){
    struct s5 $(11){

    }$(12);
}$(10)
)cpp");

    ASSERT_EQ(ranges.size(), 6U);
    expect_folding(0, "1", "2", Struct);
    expect_folding(1, "3", "4", Union);
    expect_folding(2, "5", "6", Struct);
    expect_folding(3, "7", "8", Struct);
    expect_folding(4, "9", "10", FunctionBody);
    expect_folding(5, "11", "12", Struct);
};

TEST_CASE(Method) {
    run(R"cpp(
struct s2 $(1){
    int x;
    float y;

    s2() = default;
}$(2);

struct s3;

struct s3 $(3){
    void method() $(5){
        int x = 0;
    }$(6)

    void parameter() $(7){

    }$(8)

    void skip() {};
}$(4);
)cpp");

    ASSERT_EQ(ranges.size(), 4U);
    expect_folding(0, "1", "2", Struct);
    expect_folding(1, "3", "4", Struct);
    expect_folding(2, "5", "6", FunctionBody);
    expect_folding(3, "7", "8", FunctionBody);
};

TEST_CASE(Lambda) {
    run(R"cpp(
auto z = $(1)[
    x = 0, y = 1
]$(2) () $(3){

}$(4);

static int array[4];

auto s = $(5)[
    x=0,
    y = 1,
    z = array[
    0],
    k = -1
]$(6) () $(7){
    return;
}$(8);

auto l1 = [] () {};

auto l2 = [] () $(9){

}$(10);

auto l3 = [] () $(11){
    return 0;
}$(12);

auto l4 = [] $(13)(
    int x1,
    int x2
)$(14) {};
)cpp");

    ASSERT_EQ(ranges.size(), 7U);
    expect_folding(0, "1", "2", LambdaCapture);
    expect_folding(1, "3", "4", FunctionBody);
    expect_folding(2, "5", "6", LambdaCapture);
    expect_folding(3, "7", "8", FunctionBody);
    expect_folding(4, "9", "10", FunctionBody);
    expect_folding(5, "11", "12", FunctionBody);
    expect_folding(6, "13", "14", FunctionBody);
};

TEST_CASE(Function) {
    run(R"cpp(
void e() {};

void f $(1)(


)$(2) $(3){

}$(4)

void g $(5)(
    int x,
    int y = 2
)$(6) $(7){
    int z;
}$(8)

void h() $(9){
    int x = 0;
}$(10)

void i(  ) {   };

void j $(11)(
    int p1,
    int p2,
    ...
)$(12);

void k() $(13){

}$(14)
)cpp");

    ASSERT_EQ(ranges.size(), 7U);
    expect_folding(0, "1", "2", FunctionParams);
    expect_folding(1, "3", "4", FunctionBody);
    expect_folding(2, "5", "6", FunctionParams);
    expect_folding(3, "7", "8", FunctionBody);
    expect_folding(4, "9", "10", FunctionBody);
    expect_folding(5, "11", "12", FunctionParams);
    expect_folding(6, "13", "14", FunctionBody);
}

TEST_CASE(FunctionCall) {
    run(R"cpp(
int f(int p1, int p2, int p3, int p4, int p5, int p6) { return p1 + p2; }

int main() $(1){
    int x = f(1, 2, 3, 4, 5, 6);

    int y = f $(2)(
        1, 2, 3,
        4, 5, 6
    )$(3);

    return f $(4)(
        1, 2, 3,
        4, 5, 6
    )$(5);
}$(6)
)cpp");

    ASSERT_EQ(ranges.size(), 3U);
    expect_folding(0, "1", "6", FunctionBody);
    expect_folding(1, "2", "3", FunctionCall);
    expect_folding(2, "4", "5", FunctionCall);
}

TEST_CASE(CompoundStmt) {
    run(R"cpp(
int main() $(1){

    $(3){
        $(5){
            //
        }$(6)

        $(7){
            //
        }$(8)

        //
    }$(4)

    return 0;
}$(2)

)cpp");
}

TEST_CASE(InitializeList) {
    run(R"cpp(
struct L { int xs[4]; };

L l1 = $(1){
    1, 2, 3, 4
}$(2);

L l2 = $(3){
//
//
}$(4);

)cpp");

    ASSERT_EQ(ranges.size(), 2U);
    expect_folding(0, "1", "2", Initializer);
    expect_folding(1, "3", "4", Initializer);
}

TEST_CASE(AccessSpecifier) {
    run(R"cpp(
class c1 $(1){
public$(3):
private$(4):
protected$(5):
}$(2);

class c2 $(6){
public$(8):
    int x;

private$(9):
    float y;

protected$(10):
    double z;
}$(7);

#define PUBLIC public:
#define PRIVATE private:
#define PROTECTED protected:

class c3 $(11){
$(13)PUBLIC
    int a;
$(15)PRIVATE$(14)
    int b;
$(17)PROTECTED$(16)
    int c;
}$(12);
)cpp");

    expect_folding(0, "1", "2", Class);
    expect_folding(1, "3", "4", AccessSpecifier);
    expect_folding(2, "4", "5", AccessSpecifier);
    expect_folding(3, "5", "2", AccessSpecifier);

    expect_folding(4, "6", "7", Class);
    expect_folding(5, "8", "9", AccessSpecifier);
    expect_folding(6, "9", "10", AccessSpecifier);
    expect_folding(7, "10", "7", AccessSpecifier);

    expect_folding(8, "11", "12", Class);
    expect_folding(9, "13", "14", AccessSpecifier);
    expect_folding(10, "15", "16", AccessSpecifier);
    expect_folding(11, "17", "12", AccessSpecifier);
}

TEST_CASE(Directive) {
    run(R"cpp(
#ifdef M1

#else

    #ifdef M2


    #endif

#endif
)cpp");
}

TEST_CASE(PragmaRegion) {
    run(R"cpp(
$(1)#pragma region level1
    $(2)#pragma region level2
        $(3)#pragma region level3

        #$(4)pragma endregion level3


    #$(5)pragma endregion level2


#$(6)pragma endregion level1

#pragma endregion   // mismatch region, skipped
#pragma region  // mismatch region, skipped
)cpp");

    /// FIXME: PCH make pragma region not work
    /// expect(that % ranges.size() == 3);
    /// expect_folding(0, "1", "6", Region);
    /// expect_folding(1, "2", "5", Region);
    /// expect_folding(2, "3", "4", Region);
}

};  // TEST_SUITE(FoldingRange)
}  // namespace
}  // namespace clice::testing
