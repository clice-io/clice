#include "Test/Test.h"
#include "Test/Tester.h"
#include "Feature/SemanticToken.h"

namespace clice::testing {

namespace {

using enum SymbolKind::Kind;
using enum SymbolModifiers::Kind;

TEST_SUITE(SemanticToken) {

Tester tester;
feature::SemanticTokens tokens;

void run(llvm::StringRef code) {
    tester.clear();
    tester.add_main("main.cpp", code);
    ASSERT_TRUE(tester.compile_with_pch());
    tokens = feature::semantic_tokens(*tester.unit);
};

void expect_token(llvm::StringRef pos,
                  SymbolKind kind,
                  SymbolModifiers modifiers = SymbolModifiers(),
                  std::source_location loc = std::source_location::current()) {
    auto range = tester.range(pos);

    auto found = false;

    for(auto& token: tokens) {
        if(token.range == range) {
            ASSERT_EQ(token.kind, kind);
            ASSERT_EQ(token.modifiers, modifiers);
            found = true;
            break;
        }
    }

    ASSERT_TRUE(found);
};

TEST_CASE(Include) {
    run(R"cpp(
 @0[#include] @1[<stddef.h>]
 @2[#include] @3["stddef.h"]
 @4[#] @5[include] @6["stddef.h"]
 )cpp");

    expect_token("0", Directive);
    expect_token("1", Header);
    expect_token("2", Directive);
    expect_token("3", Header);
    expect_token("4", Directive);
    expect_token("5", Directive);
    expect_token("6", Header);
}

TEST_CASE(Comment) {
    run(R"cpp(
@line[/// line comment]
int x = 1;
)cpp");

    expect_token("line", Comment);
}

TEST_CASE(Keyword) {
    run(R"cpp(
@int[int] main() {
    @return[return] 0;
}
)cpp");

    expect_token("int", Keyword);
    expect_token("return", Keyword);
}

TEST_CASE(Macro) {
    run(R"cpp(
@directive[#define] @macro[FOO]
)cpp");

    expect_token("directive", Directive);
    expect_token("macro", Macro);
}

TEST_CASE(FinalAndOverride) {
    run(R"cpp(
struct A @final[final] {};

struct B {
    virtual void foo();
};

struct C : B {
    void foo() @override[override];
};

struct D : C {
    void foo() @final2[final];
};
)cpp");

    expect_token("final", Keyword);
    expect_token("override", Keyword);
    expect_token("final2", Keyword);
}

TEST_CASE(VarDecl) {
    run(R"cpp(
extern int @x1[x];

int @x2[x] = 1;

template <typename T, typename U>
extern int @y1[y];

template <typename T, typename U>
int @y2[y] = 2;

template<typename T>
extern int @y3[y]<T, int>;

template<typename T>
int @y4[y]<T, int> = 4;

template<>
int @y5[y]<int, int> = 5;

int main() {
    @x3[x] = 6;
}
)cpp");

    expect_token("x1", Variable, Declaration);
    expect_token("x2", Variable, Definition);
    expect_token("y1", Variable, SymbolModifiers(Declaration, Templated));
    expect_token("y2", Variable, SymbolModifiers(Definition, Templated));
    expect_token("y3", Variable, SymbolModifiers(Declaration, Templated));
    expect_token("y4", Variable, SymbolModifiers(Definition, Templated));
    expect_token("y5", Variable, Definition);
    expect_token("x3", Variable, SymbolModifiers());
}

TEST_CASE(FunctionDecl) {
    run(R"cpp(
extern int @foo1[foo]();

int @foo2[foo]() {
    return 0;
}

template <typename T>
extern int @bar1[bar]();

template <typename T>
int @bar2[bar]() {
    return 1;
}
)cpp");

    expect_token("foo1", Function, Declaration);
    expect_token("foo2", Function, Definition);
    expect_token("bar1", Function, SymbolModifiers(Declaration, Templated));
    expect_token("bar2", Function, SymbolModifiers(Definition, Templated));
}

TEST_CASE(RecordDecl) {
    run(R"cpp(
class @A1[A];

class @A2[A] {};

struct @B1[B];

struct @B2[B] {};

union @C1[C];

union @C2[C] {};
)cpp");

    expect_token("A1", Class, Declaration);
    expect_token("A2", Class, Definition);
    expect_token("B1", Struct, Declaration);
    expect_token("B2", Struct, Definition);
    expect_token("C1", Union, Declaration);
    expect_token("C2", Union, Definition);
}

};  // TEST_SUITE(SemanticToken)

}  // namespace

}  // namespace clice::testing
