#include "Test/Tester.h"
#include "Feature/Definition.h"

namespace clice::testing {

namespace {

suite<"Definition"> goto_definition = [] {
    std::vector<feature::CompletionItem> items;

    auto definition = [&](llvm::StringRef code) {
        CompilationParams params;
        auto annotation = AnnotatedSource::from(code);
        params.arguments = {"clang++", "-std=c++20", "main.cpp"};
        params.completion = {"main.cpp", annotation.offsets["pos"]};
        params.add_remapped_file("main.cpp", annotation.content);

        config::DefinitionOption options = {};
        items = feature::definition(params, options);
    };

    using enum feature::CompletionItemKind;

    test("Score") = [&] {
        definition(R"cpp(
int foooo(int x);
int x = fo$(pos)
)cpp");
        expect(that % items.size() == 1);
        expect(items.front().label == "foooo");
        expect(items.front().kind == Function);
    };

    test("Snippet") = [&] {
        definition(R"cpp(
int x = tru$(pos)
)cpp");
    };

    test("Overload") = [&] {
        definition(R"cpp(
int foooo(int x);
int foooo(int x, int y);
int x = fooo$(pos)
)cpp");
    };

    test("Unqualified") = [&] {
        definition(R"cpp(
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
    };

    test("Functor") = [&] {
        definition(R"cpp(
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
    };

    test("Lambda") = [&] {
        definition(R"cpp(
void bar() {
    auto foo = [](int x){ };
    fo$(pos);
}
)cpp");

        /// TODO:
        /// complete lambda as it is a function.
    };
};

}  // namespace

}  // namespace clice::testing

