module;

#include "modules/prelude.h"

module clice;

import :compile.compilation;
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
    add_main("main.cpp", R"cpp(
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
    add_main("main.cpp", R"cpp(
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

};  // ZEST_SUITE(Location)

}  // namespace

}  // namespace clice::testing
