module;

#include "modules/prelude.h"

module clice;

import :compile.compilation;
import :compile.directive;
import :unittests.utils;
import :unittests.utils.tester;

namespace clice::testing {

namespace {

ZEST_SUITE(Location, Tester) {

const clang::VarDecl* variable(llvm::StringRef name) {
    for(auto* decl: unit->top_level_decls()) {
        if(auto* var = llvm::dyn_cast<clang::VarDecl>(decl); var && var->getName() == name) {
            return var;
        }
    }
    return nullptr;
}

ZEST_CASE(MacroArgumentAndBody) {
    add_focus("main.cpp", R"cpp(
#define CALL(f) f(§(one)1) + 0
int helper(int);
int x = §(call)⟦CALL(§(arg)⟦helper⟧)⟧;
)cpp");
    ZASSERT(compile());
    const auto* var = variable("x");
    ZASSERT(var);
    const auto* sum = llvm::dyn_cast<clang::BinaryOperator>(var->getInit()->IgnoreImplicit());
    ZASSERT(sum);
    const auto* call = llvm::dyn_cast<clang::CallExpr>(sum->getLHS()->IgnoreImplicit());
    ZASSERT(call);
    const auto* callee = call->getCallee()->IgnoreImplicit();
    const auto* one = call->getArg(0);
    auto focus = unit->focus_file();

    // `helper(1)` takes its name from the argument and the rest from part
    // of the macro's own body: no text was written as it. The whole
    // expansion was written as the invocation.
    ZEXPECT(!focus.written_range(call->getSourceRange()));
    ZEXPECT(focus.produced_range(call->getSourceRange()) == range("call"));
    ZEXPECT(focus.written_range(sum->getSourceRange()) == range("call"));

    ZEXPECT(focus.written_range(callee->getSourceRange()) == range("arg"));
    ZEXPECT(focus.produced_range(callee->getSourceRange()) == range("call"));
    ZEXPECT(focus.offset(callee->getBeginLoc()) == range("arg").begin);
    ZEXPECT(unit->produced_at(callee->getBeginLoc()).offset == range("call").begin);

    ZEXPECT(unit->written_at(one->getBeginLoc()).offset == point("one"));
    ZEXPECT(focus.offset(one->getBeginLoc()) == range("call").begin);
}

ZEST_CASE(SplitAngleBrackets) {
    add_focus("main.cpp", R"cpp(
template <typename T>
struct Box {};

§(outer)⟦Box<§(inner)⟦Box<int>⟧>⟧ b;
)cpp");
    ZASSERT(compile());
    const auto* var = variable("b");
    ZASSERT(var);
    auto outer =
        var->getTypeSourceInfo()->getTypeLoc().getAs<clang::TemplateSpecializationTypeLoc>();
    ZASSERT(!outer.isNull());
    auto inner = outer.getArgLoc(0).getTypeSourceInfo()->getTypeLoc();
    auto focus = unit->focus_file();

    // The parser splits `>>` and the first `>` becomes a location of its
    // own over half the token.
    ZEXPECT(focus.written_range(inner.getSourceRange()) == range("inner"));
    ZEXPECT(focus.written_range(outer.getSourceRange()) == range("outer"));
}

ZEST_CASE(SplitAfterMacro) {
    add_focus("main.cpp", R"cpp(
namespace n {
template <typename T>
struct Box {};
}
#define Vec n::Box

Vec<§(inner)⟦Vec<int>⟧> v;
)cpp");
    ZASSERT(compile());
    const auto* var = variable("v");
    ZASSERT(var);
    auto outer =
        var->getTypeSourceInfo()->getTypeLoc().getAs<clang::TemplateSpecializationTypeLoc>();
    ZASSERT(!outer.isNull());
    auto inner = outer.getArgLoc(0).getTypeSourceInfo()->getTypeLoc();
    auto focus = unit->focus_file();

    // The inner type starts with the whole of what the inner `Vec` expands
    // to and ends at the first `>` of `>>`.
    ZEXPECT(focus.written_range(inner.getSourceRange()) == range("inner"));
    ZEXPECT(focus.produced_range(inner.getSourceRange()) == range("inner"));
}

ZEST_CASE(UnclosedBlock) {
    add_focus("main.cpp", R"cpp(
void f() §(body)⟦{
    int x = 1;⟧
// trailing
)cpp");
    ZASSERT(compile());
    const clang::FunctionDecl* function = nullptr;
    for(auto* decl: unit->top_level_decls()) {
        if(auto* candidate = llvm::dyn_cast<clang::FunctionDecl>(decl)) {
            function = candidate;
        }
    }
    ZASSERT(function);
    ZASSERT(function->getBody());

    // The block ends at the eof, which the written text leaves out.
    ZEXPECT(unit->focus_file().written_range(function->getBody()->getSourceRange()) ==
            range("body"));
}

ZEST_CASE(PreambleIsFocus) {
    add_file("a.h", "int a();\n");
    add_focus("main.cpp", R"cpp(#include "a.h"
#define §(name)⟦LOCAL⟧ 1
int x = LOCAL;
)cpp");
    ZASSERT(compile_with_pch());
    auto focus = unit->focus_file();
    const clang::MacroInfo* local = nullptr;
    for(auto& macro: unit->directives()[focus.file()].macros) {
        if(macro.kind == MacroRef::Ref) {
            local = macro.macro;
        }
    }
    ZASSERT(local);

    // The `#define` lies in the preamble the PCH recorded, a file of its
    // own at the same offsets.
    auto definition = local->getDefinitionLoc();
    ZEXPECT(unit->file_id(definition) != focus.file());
    ZEXPECT(focus.contains(unit->file_id(definition)));
    ZEXPECT(focus.offset(definition) == range("name").begin);
    ZEXPECT(focus.written_range(definition) == range("name"));
}

};  // ZEST_SUITE(Location)

}  // namespace

}  // namespace clice::testing
