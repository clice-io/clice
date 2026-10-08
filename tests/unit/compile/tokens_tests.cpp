module;

#include "modules/prelude.h"

module clice;

import :compile.semantics;
import :syntax.lexical_scan;
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

/// The spelled tokens the first run of expanded tokens spelled `sequence`
/// (space-separated) maps to.
std::string spelled_for(llvm::StringRef sequence) {
    auto& SM = unit->context().getSourceManager();
    llvm::SmallVector<llvm::StringRef> words;
    sequence.split(words, ' ');
    auto stream = unit->expanded_tokens();
    for(std::size_t i = 0; i + words.size() <= stream.size(); i += 1) {
        auto run = stream.slice(i, words.size());
        if(llvm::equal(run, words, [&](const clang::syntax::Token& token, llvm::StringRef word) {
               return token.text(SM) == word;
           })) {
            auto range = clang::SourceRange(run.front().location(), run.back().location());
            return text(unit->spelled_tokens(range));
        }
    }
    ZASSERT(false);
    std::unreachable();
}

std::string expansions() {
    std::string out;
    for(auto& expansion: unit->expansions_overlapping(unit->spelled_tokens())) {
        out += std::format("[{} => {}]", text(expansion.spelled), text(expansion.expanded));
    }
    return out;
}

/// Each expanded token but the eof, with the spelled token it stands for
/// (`?` for none).
std::string origins() {
    auto& SM = unit->context().getSourceManager();
    auto spelled = unit->spelled_tokens();
    std::string out;
    for(auto [index, token]: llvm::enumerate(unit->expanded_tokens())) {
        if(token.kind() == clang::tok::eof) {
            continue;
        }
        auto origin = unit->expanded_token_origin(static_cast<std::uint32_t>(index));
        out += std::format("{}{}:{}",
                           out.empty() ? "" : " ",
                           token.text(SM),
                           origin ? spelled[*origin].text(SM) : "?");
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
int x = ID(a1) + BAR;
int y = ID(ID(ID(a1) + a2));
)cpp");
    ZASSERT(compile());
    ZEXPECT(spelled_for("a1 + 1 + 2") == "ID ( a1 ) + BAR");
    ZEXPECT(spelled_for("1").empty());
    ZEXPECT(spelled_for("a1 + a2") == "ID ( a1 ) + a2");
}

ZEST_CASE(SpelledForArguments) {
    add_main("main.cpp", R"cpp(
#define ID(X) X
#define ID2(X, Y) X Y
#define FOO(X) foo(X)
#define INDIRECT FOO(y)
ID2(ID(a1), ID(a2) a3) ID2(a4, a5 a6 a7)
INDIRECT
)cpp");
    ZASSERT(compile());
    ZEXPECT(spelled_for("a1 a2").empty());
    ZEXPECT(spelled_for("a2 a3") == "ID ( a2 ) a3");
    ZEXPECT(spelled_for("a1 a2 a3") == "ID2 ( ID ( a1 ) , ID ( a2 ) a3 )");
    ZEXPECT(spelled_for("a5 a6") == "a5 a6");
    ZEXPECT(spelled_for("a1 a2 a3 a4").empty());
    ZEXPECT(spelled_for("y").empty());
}

ZEST_CASE(PragmaExpansions) {
    add_main("main.cpp", R"cpp(
#define THREADS 4
void f() {
#pragma omp parallel num_threads(THREADS)
    {}
}
)cpp");
    prepare();
    owned_args.insert(owned_args.end() - 1, "-fopenmp");
    params.arguments.clear();
    for(auto& arg: owned_args) {
        params.arguments.push_back(arg.c_str());
    }
    ZASSERT(try_compile());
    ZEXPECT(expansions() == "[THREADS => 4]");
}

ZEST_CASE(BuiltinMacroArgument) {
    add_main("main.cpp", R"cpp(
#define ATTR nodiscard
int v = __has_cpp_attribute(ATTR);
)cpp");
    ZASSERT(compile());
    auto& SM = unit->context().getSourceManager();
    auto expansions = unit->expansions_overlapping(unit->spelled_tokens());
    ZASSERT(expansions.size() == 2U);
    ZEXPECT(text(expansions[0].spelled) == "__has_cpp_attribute");
    ZASSERT(expansions[0].expanded.size() == 1U);
    auto& result = expansions[0].expanded.front();
    ZEXPECT(spelled_for(result.text(SM)) == "__has_cpp_attribute");
    ZEXPECT(text(expansions[1].spelled) == "ATTR");
    ZEXPECT(expansions[1].expanded.empty());
}

ZEST_CASE(MacroOrigins) {
    add_main("main.cpp", R"cpp(
int a;
#define TWICE(x) (x + x)
#define ID(x) x
#define ONE 1
int v = TWICE(a) + ONE;
int w = ID(ID(a));
)cpp");
    ZASSERT(compile());
    ZEXPECT(origins() ==
            "int:int a:a ;:; int:int v:v =:= (:TWICE a:TWICE +:TWICE a:a ):TWICE +:+ 1:ONE ;:; "
            "int:int w:w =:= a:a ;:;");
}

ZEST_CASE(RereadArguments) {
    add_main("main.cpp", R"cpp(
#define FIRST(a, ...) a
#define REST(a, ...) __VA_ARGS__
#define LOG(...) log(FIRST(__VA_ARGS__), __VA_ARGS__)
#define BOTH(...) log(REST(__VA_ARGS__), __VA_ARGS__)
void log(int, int, int);
int x, y;
void f() {
    LOG(x, y);
    BOTH(x, y);
}
)cpp");
    ZASSERT(compile());
    ZEXPECT(origins() ==
            "void:void log:log (:( int:int ,:, int:int ,:, int:int ):) ;:; int:int x:x ,:, y:y ;:; "
            "void:void f:f (:( ):) {:{ "
            "log:LOG (:LOG x:x ,:LOG x:LOG ,:, y:y ):LOG ;:; "
            "log:BOTH (:BOTH y:y ,:BOTH x:x ,:, y:BOTH ):BOTH ;:; }:}");
}

ZEST_CASE(HeaderOrigins) {
    add_file("header.h", "#define HM int hm;\nint h;\nHM\n");
    add_main("main.cpp", "int m;\n#include \"header.h\"\n");
    ZASSERT(compile());
    ZEXPECT(origins() == "int:int m:m ;:; int:? h:? ;:? int:? hm:? ;:?");
}

ZEST_CASE(TouchingFileEnd) {
    add_main("main.cpp", "int x");
    ZASSERT(compile());
    auto& SM = unit->context().getSourceManager();
    auto end = SM.getLocForEndOfFile(SM.getMainFileID());
    ZEXPECT(text(unit->spelled_tokens_touch(end)) == "x");
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
    ZEXPECT(spelled_where(true) == "# include \"a.h\"");
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
    ZEXPECT(llvm::none_of(unit->expanded_tokens(), [](const clang::syntax::Token& token) {
        return token.kind() == clang::tok::eod;
    }));
    unit->semantics();
}

ZEST_CASE(PrivateFragmentAnchor) {
    add_main("main.cpp", R"cpp(
export module app;
#if 0
module :private;
#endif
module :§(live)⟦private⟧;
)cpp");
    ZASSERT(compile());
    for(bool main_file_only: {true, false}) {
        auto semantics = Semantics::build(*unit, {.main_file_only = main_file_only});
        auto modules = semantics.module_declarations();
        ZASSERT(modules.size() == 2U);
        ZEXPECT(modules[1].kind == LexicalInfo::ModuleDeclaration::Kind::PrivateFragment);
        ZEXPECT(modules[1].partition_parts.front() == range("live"));
    }
}

};  // ZEST_SUITE(Tokens)

}  // namespace

}  // namespace clice::testing
