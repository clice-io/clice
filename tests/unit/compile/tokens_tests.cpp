module;

#include "modules/prelude.h"

module clice;

import :compile.semantics;
import :compile.tokens;
import :tests.unit.test.test;
import :tests.unit.test.tester;

namespace clice::testing {

namespace {

ZEST_SUITE(Tokens, Tester) {

std::string text(llvm::ArrayRef<clang::syntax::Token> tokens) {
    auto& SM = unit->context().getSourceManager();
    std::string out;
    for(auto& token: tokens) {
        if(!out.empty()) {
            out += ' ';
        }
        out += token.text(SM);
    }
    return out;
}

/// The spelled tokens that survived preprocessing, or not.
std::string spelled_where(bool away) {
    auto spelled = unit->spelled_tokens();
    auto& flags = unit->preprocessed_away();
    std::vector<clang::syntax::Token> picked;
    for(std::size_t i = 0; i < spelled.size(); i += 1) {
        if(flags[i] == away) {
            picked.push_back(spelled[i]);
        }
    }
    return text(picked);
}

/// The `nth` expanded token spelled `spelling`.
const clang::syntax::Token& expanded(llvm::StringRef spelling, std::size_t nth = 0) {
    auto& SM = unit->context().getSourceManager();
    for(auto& token: unit->expanded_tokens()) {
        if(token.text(SM) == spelling) {
            if(nth == 0) {
                return token;
            }
            nth -= 1;
        }
    }
    ZASSERT(false);
    std::unreachable();
}

std::string spelled_for(const clang::syntax::Token& first, const clang::syntax::Token& last) {
    return text(unit->spelled_tokens(clang::SourceRange(first.location(), last.location())));
}

std::string expansions() {
    std::string out;
    for(auto& expansion: unit->expansions_overlapping(unit->spelled_tokens())) {
        out += std::format("[{} => {}]", text(expansion.spelled), text(expansion.expanded));
    }
    return out;
}

ZEST_CASE(PreprocessedAway) {
    add_main("main.cpp", R"cpp(
#define EMPTY
#define ID(x) x
int a EMPTY;
#if 0
int b;
#endif
int c = ID(1);
)cpp");
    ZASSERT(compile());
    ZEXPECT(spelled_where(false) == "int a ; int c = ID ( 1 ) ;");
    ZEXPECT(spelled_where(true) ==
            "# define EMPTY # define ID ( x ) x EMPTY # if 0 int b ; # endif");
}

ZEST_CASE(TopLevelExpansions) {
    add_main("main.cpp", R"cpp(
#define F(x) x + 1
#define E
#define ON 1
int v = F(2) E;
#if ON
int w;
#endif
)cpp");
    ZASSERT(compile());
    ZEXPECT(expansions() == "[F ( 2 ) => 2 + 1][E => ][ON => ]");
}

ZEST_CASE(NestedInvocationsMerge) {
    add_main("main.cpp", R"cpp(
#define ID(x) x
#define B(x) x
#define A 1 + B
int v = ID(ID(1));
int w = A(2);
)cpp");
    ZASSERT(compile());
    ZEXPECT(expansions() == "[ID ( ID ( 1 ) ) => 1][A ( 2 ) => 1 + 2]");
}

ZEST_CASE(SpelledForRanges) {
    add_main("main.cpp", R"cpp(
#define ID(x) x
#define BAR 1 + 2
int a1, a2;
int x = ID(1) + BAR;
int y = ID(ID(ID(a1) + a2));
)cpp");
    ZASSERT(compile());
    ZEXPECT(spelled_for(expanded("1"), expanded("1")) == "1");
    ZEXPECT(spelled_for(expanded("1"), expanded("2")) == "ID ( 1 ) + BAR");
    ZEXPECT(spelled_for(expanded("1", 1), expanded("1", 1)).empty());
    ZEXPECT(spelled_for(expanded("a1", 1), expanded("a1", 1)) == "a1");
    ZEXPECT(spelled_for(expanded("a2", 1), expanded("a2", 1)) == "a2");
    ZEXPECT(spelled_for(expanded("a1", 1), expanded("a2", 1)) == "ID ( a1 ) + a2");
}

ZEST_CASE(SplitShiftRange) {
    add_main("main.cpp", "template <class T> struct A {};\nA<A<int>> x;\n");
    ZASSERT(compile());
    auto& shift = expanded(">>");
    auto range =
        clang::SourceRange(expanded("A", 2).location(), shift.location().getLocWithOffset(1));
    ZEXPECT(text(unit->expanded_tokens(range)) == "A < int >>");
}

ZEST_CASE(PreambleAway) {
    add_file("a.h", "#define A 1\n");
    add_main("main.cpp", "#include \"a.h\"\nint x = A;\n");
    ZASSERT(compile_with_pch());
    ZEXPECT(spelled_where(false) == "int x = A ;");
    ZEXPECT(expansions() == "[A => 1]");
}

ZEST_CASE(AssemblerComment) {
    add_main("main.S", "# comment\n.text\n");
    prepare("-std=c11");
    auto language = std::ranges::find(owned_args, "c");
    ZASSERT(language != owned_args.end());
    *language = "assembler-with-cpp";
    params.arguments.clear();
    for(auto& arg: owned_args) {
        params.arguments.push_back(arg.c_str());
    }
    ZASSERT(try_compile());
    ZEXPECT(spelled_where(false) == "# comment . text");
    unit->semantics();
}

ZEST_CASE(PrivateFragmentAnchor) {
    add_main("main.cpp", R"cpp(
export module app;
#if 0
module :private;
#endif
)cpp");
    ZASSERT(compile());
    auto modules = unit->semantics().module_declarations();
    ZASSERT(modules.size() == 1U);
    ZEXPECT(modules[0].kind == LexicalInfo::ModuleDeclaration::Kind::Declaration);
    auto whole = Semantics::build(*unit, {.main_file_only = false});
    ZEXPECT(whole.module_declarations().size() == 1U);
}

};  // ZEST_SUITE(Tokens)

}  // namespace

}  // namespace clice::testing
