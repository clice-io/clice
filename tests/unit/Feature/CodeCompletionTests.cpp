#include "Test/Test.h"
#include "Test/Tester.h"
#include "Feature/CodeCompletion.h"

namespace clice::testing {
namespace {
TEST_SUITE(CodeCompletion) {

std::vector<feature::CompletionItem> items;

void code_complete(llvm::StringRef code) {
    CompilationParams params;
    auto annotation = AnnotatedSource::from(code);
    params.arguments = {"clang++", "-std=c++20", "main.cpp"};
    params.completion = {"main.cpp", annotation.offsets["pos"]};
    params.add_remapped_file("main.cpp", annotation.content);

    config::CodeCompletionOption options = {};
    items = feature::code_complete(params, options);
};

using enum feature::CompletionItemKind;

TEST_CASE(Score) {
    code_complete(R"cpp(
int foooo(int x);
int x = fo$(pos)
)cpp");
    ASSERT_EQ(items.size(), 1U);
    ASSERT_EQ(items.front().label, "foooo");
    ASSERT_EQ(items.front().kind, Function);
}

TEST_CASE(Snippet) {
    code_complete(R"cpp(
int x = tru$(pos)
)cpp");
}

TEST_CASE(Overload) {
    code_complete(R"cpp(
int foooo(int x);
int foooo(int x, int y);
int x = fooo$(pos)
)cpp");
}

TEST_CASE(Unqualified) {
    code_complete(R"cpp(
namespace A {
    void fooooo();
}

void bar() {
    fo$(pos)
}
)cpp");

    /// EXPECT: "A::fooooo"
    /// To implement this we need to search code completion result from index
    /// or traverse AST to collect interesting names.
}

TEST_CASE(Functor) {
    code_complete(R"cpp(
    struct X {
        void operator() () {}
    };

void bar() {
    X foo;
    fo$(pos);
}
)cpp");

    /// TODO:
    /// complete lambda as it is a variable.
}

TEST_CASE(Lambda) {
    code_complete(R"cpp(
void bar() {
    auto foo = [](int x){ };
    fo$(pos);
}
)cpp");

    /// TODO:
    /// complete lambda as it is a function.
}

};  // TEST_SUITE(CodeCompletion)
}  // namespace
}  // namespace clice::testing
