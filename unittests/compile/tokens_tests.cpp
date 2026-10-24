module;

#include "support/prelude.h"

module clice;

import :semantic.semantics;
import :syntax.lexical_scan;
import :unittests.utils.tester;
import :unittests.utils;

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

/// Spelled tokens as text, the directive ends left out.
std::string text(llvm::ArrayRef<Token> tokens) {
    auto content = unit->focus_file().content();
    std::string out;
    for(auto& token: tokens) {
        if(token.is_eod()) {
            continue;
        }
        if(!out.empty()) {
            out += ' ';
        }
        out += token.text(content);
    }
    return out;
}

/// The spelled tokens that survived preprocessing, or not.
std::string spelled_where(bool away) {
    auto spelled = unit->focus_file().tokens().spelled();
    auto flags = unit->focus_file().tokens().preprocessed_away();
    std::vector<Token> picked;
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
    for(auto& token: unit->focus_file().tokens().expanded()) {
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
    auto stream = unit->focus_file().tokens().expanded();
    for(std::size_t i = 0; i + words.size() <= stream.size(); i += 1) {
        auto run = stream.slice(i, words.size());
        if(ranges::equal(run, words, [&](const clang::syntax::Token& token, llvm::StringRef word) {
               return token.text(SM) == word;
           })) {
            auto range = clang::SourceRange(run.front().location(), run.back().location());
            return text(unit->focus_file().tokens().spelled_for(range));
        }
    }
    ZASSERT(false);
    std::unreachable();
}

std::string expansions() {
    std::string out;
    for(auto& expansion:
        unit->focus_file().tokens().expansions_overlapping(unit->focus_file().tokens().spelled())) {
        out += std::format("[{} => {}]", text(expansion.spelled), text(expansion.expanded));
    }
    return out;
}

/// Each expanded token but the eof, with the spelled token it stands for
/// (`?` for none).
std::string origins() {
    auto& SM = unit->context().getSourceManager();
    auto content = unit->focus_file().content();
    auto spelled = unit->focus_file().tokens().spelled();
    std::string out;
    for(auto [index, token]: views::enumerate(unit->focus_file().tokens().expanded())) {
        if(token.kind() == clang::tok::eof) {
            continue;
        }
        auto origin = unit->focus_file().tokens().origin(static_cast<std::uint32_t>(index));
        out += std::format("{}{}:{}",
                           out.empty() ? "" : " ",
                           token.text(SM),
                           origin ? spelled[*origin].text(content) : "?");
    }
    return out;
}

/// The focus-file offset of the spelled token the `nth` expanded token
/// spelled `spelling` stands for.
std::optional<std::uint32_t> origin_offset(llvm::StringRef spelling, std::size_t nth) {
    auto& token = expanded(spelling, nth);
    auto index = static_cast<std::uint32_t>(&token - unit->focus_file().tokens().expanded().data());
    auto origin = unit->focus_file().tokens().origin(index);
    if(!origin) {
        return std::nullopt;
    }
    return unit->focus_file().tokens().spelled()[*origin].range.begin;
}

ZEST_CASE(PreprocessedAway) {
    add_focus("main.cpp", R"cpp(
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
    add_focus("main.cpp", R"cpp(
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
    add_focus("main.cpp", R"cpp(
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
    add_focus("main.cpp", R"cpp(
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
    add_focus("main.cpp", R"cpp(
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
    add_focus("main.cpp", R"cpp(
#define THREADS 4
void f() {
#pragma omp parallel num_threads(THREADS)
    {}
}
)cpp");
    prepare();
    params.input.arguments.insert(params.input.arguments.end() - 1, "-fopenmp");
    ZASSERT(try_compile());
    ZEXPECT(expansions() == "[THREADS => 4]");
}

ZEST_CASE(BuiltinMacroArgument) {
    add_focus("main.cpp", R"cpp(
#define ATTR nodiscard
int v = __has_cpp_attribute(ATTR);
)cpp");
    ZASSERT(compile());
    auto& SM = unit->context().getSourceManager();
    auto expansions =
        unit->focus_file().tokens().expansions_overlapping(unit->focus_file().tokens().spelled());
    ZASSERT(expansions.size() == 2U);
    ZEXPECT(text(expansions[0].spelled) == "__has_cpp_attribute");
    ZASSERT(expansions[0].expanded.size() == 1U);
    auto& result = expansions[0].expanded.front();
    ZEXPECT(spelled_for(result.text(SM)) == "__has_cpp_attribute");
    ZEXPECT(text(expansions[1].spelled) == "ATTR");
    ZEXPECT(expansions[1].expanded.empty());
}

ZEST_CASE(MacroOrigins) {
    add_focus("main.cpp", R"cpp(
int a;
#define TWICE(x) (x + x)
#define ID(x) x
#define ONE 1
int v = TWICE(§(twice)a) + ONE;
int w = ID(ID(§(nested)a));
)cpp");
    ZASSERT(compile());
    ZEXPECT(origins() ==
            "int:int a:a ;:; int:int v:v =:= (:TWICE a:TWICE +:TWICE a:a ):TWICE +:+ 1:ONE ;:; "
            "int:int w:w =:= a:a ;:;");
    ZEXPECT(origin_offset("a", 2) == std::optional(point("twice")));
    ZEXPECT(origin_offset("a", 3) == std::optional(point("nested")));
}

ZEST_CASE(RereadArguments) {
    add_focus("main.cpp", R"cpp(
#define FIRST(a, ...) a
#define REST(a, ...) __VA_ARGS__
#define LOG(...) log(FIRST(__VA_ARGS__), __VA_ARGS__)
#define BOTH(...) log(REST(__VA_ARGS__), __VA_ARGS__)
void log(int, int, int);
int x, y;
void f() {
    LOG(§(log_x)x, §(log_y)y);
    BOTH(§(both_x)x, §(both_y)y);
}
)cpp");
    ZASSERT(compile());
    ZEXPECT(origins() ==
            "void:void log:log (:( int:int ,:, int:int ,:, int:int ):) ;:; int:int x:x ,:, y:y ;:; "
            "void:void f:f (:( ):) {:{ "
            "log:LOG (:LOG x:x ,:LOG x:LOG ,:, y:y ):LOG ;:; "
            "log:BOTH (:BOTH y:y ,:BOTH x:x ,:, y:BOTH ):BOTH ;:; }:}");
    ZEXPECT(origin_offset("x", 1) == std::optional(point("log_x")));
    ZEXPECT(origin_offset("y", 1) == std::optional(point("log_y")));
    ZEXPECT(origin_offset("y", 2) == std::optional(point("both_y")));
    ZEXPECT(origin_offset("x", 3) == std::optional(point("both_x")));
}

ZEST_CASE(StringizedArguments) {
    add_focus("main.cpp", R"cpp(
#define STR(x) #x
#define CHECK(e) ((e) ? 0 : fail(STR(e)))
#define EQUAL(e) ((e) == (e) ? 0 : fail(STR(e)))
int fail(const char*);
int n, m;
int v = CHECK(§(n)n);
int w = §(equal)EQUAL(§(m)m);
)cpp");
    ZASSERT(compile());
    ZEXPECT(origin_offset("n", 1) == std::optional(point("n")));
    ZEXPECT(origin_offset("m", 1) == std::optional(point("equal")));
    ZEXPECT(origin_offset("m", 2) == std::optional(point("m")));
}

ZEST_CASE(RescannedArguments) {
    add_focus("main.cpp", R"cpp(
#define EVAL0(...) __VA_ARGS__
#define EVAL1(...) EVAL0(EVAL0(__VA_ARGS__))
#define EVAL2(...) EVAL1(EVAL1(__VA_ARGS__))
#define EVAL3(...) EVAL2(EVAL2(__VA_ARGS__))
#define EVAL4(...) EVAL3(EVAL3(__VA_ARGS__))
#define EVAL5(...) EVAL4(EVAL4(__VA_ARGS__))
#define EVAL6(...) EVAL5(EVAL5(__VA_ARGS__))
#define EVAL7(...) EVAL6(EVAL6(__VA_ARGS__))
#define EVAL8(...) EVAL7(EVAL7(__VA_ARGS__))
#define EVAL9(...) EVAL8(EVAL8(__VA_ARGS__))
#define EVAL10(...) EVAL9(EVAL9(__VA_ARGS__))
#define EVAL11(...) EVAL10(EVAL10(__VA_ARGS__))
#define EVAL12(...) EVAL11(EVAL11(__VA_ARGS__))
#define EVAL13(...) EVAL12(EVAL12(__VA_ARGS__))
#define EVAL14(...) EVAL13(EVAL13(__VA_ARGS__))
#define EVAL15(...) EVAL14(EVAL14(__VA_ARGS__))
#define TWICE(x) x + x
int a, b;
int v = EVAL15(§(a)a + TWICE(§(b)b));
)cpp");
    ZASSERT(compile());
    ZEXPECT(origins() ==
            "int:int a:a ,:, b:b ;:; int:int v:v =:= a:a +:+ b:EVAL15 +:EVAL15 b:b ;:;");
    ZEXPECT(origin_offset("a", 1) == std::optional(point("a")));
    ZEXPECT(origin_offset("b", 2) == std::optional(point("b")));
}

ZEST_CASE(HeaderOrigins) {
    add_file("header.h", R"cpp(
#define HM int hm;
int h;
HM
)cpp");
    add_focus("main.cpp", R"cpp(
int m;
#include "header.h"
)cpp");
    ZASSERT(compile());
    ZEXPECT(origins() == "int:int m:m ;:; int:? h:? ;:? int:? hm:? ;:?");
    auto eof = static_cast<std::uint32_t>(unit->focus_file().tokens().expanded().size() - 1);
    ZEXPECT(unit->focus_file().tokens().expanded()[eof].kind() == clang::tok::eof);
    ZEXPECT(!unit->focus_file().tokens().origin(eof));
}

ZEST_CASE(CommentsShareFlags) {
    // A comment goes with the token before it.
    add_focus("main.cpp", R"cpp(
#if 0
int a; // gone
#endif
int b; // kept
)cpp");
    ZASSERT(compile());
    ZEXPECT(spelled_where(false) == "int b ; // kept");
    ZEXPECT(spelled_where(true) == "# if 0 int a ; // gone # endif");
}

ZEST_CASE(TouchingSkipsComments) {
    add_focus("main.cpp", "int x/* c */;");
    ZASSERT(compile());
    auto focus = unit->focus_file();
    auto x = focus.location(static_cast<std::uint32_t>(std::strlen("int x")));
    ZEXPECT(text(focus.tokens().spelled_touching(x)) == "x");
    auto semi = focus.location(static_cast<std::uint32_t>(std::strlen("int x/* c */")));
    ZEXPECT(text(focus.tokens().spelled_touching(semi)) == ";");
}

ZEST_CASE(TouchingFileEnd) {
    add_focus("main.cpp", "int x");
    ZASSERT(compile());
    auto& SM = unit->context().getSourceManager();
    auto end = SM.getLocForEndOfFile(SM.getMainFileID());
    ZEXPECT(text(unit->focus_file().tokens().spelled_touching(end)) == "x");
}

ZEST_CASE(SplitShiftRange) {
    add_focus("main.cpp", "template <class T> struct A {};\nA<A<int>> x;\n");
    ZASSERT(compile());
    auto& shift = expanded(">>");
    auto range =
        clang::SourceRange(expanded("A", 2).location(), shift.location().getLocWithOffset(1));
    ZEXPECT(text(unit->focus_file().tokens().expanded(range)) == "A < int >>");
}

ZEST_CASE(PreambleAway) {
    add_file("a.h", "#define A 1\n");
    add_focus("main.cpp", "#include \"a.h\"\nint x = A;\n");
    ZASSERT(compile_with_pch());
    ZEXPECT(spelled_where(false) == "int x = A ;");
    ZEXPECT(spelled_where(true) == "# include \"a.h\"");
    ZEXPECT(expansions() == "[A => 1]");
}

ZEST_CASE(AssemblerComment) {
    add_focus("main.S", "# comment\n.text\n");
    prepare("-std=c11");
    auto language = ranges::find(params.input.arguments, "c");
    ZASSERT(language != params.input.arguments.end());
    *language = "assembler-with-cpp";
    ZASSERT(try_compile());
    ZEXPECT(spelled_where(false) == "# comment . text");
    ZEXPECT(ranges::none_of(
        unit->focus_file().tokens().expanded(),
        [](const clang::syntax::Token& token) { return token.kind() == clang::tok::eod; }));
    unit->get<Semantics>();
}

ZEST_CASE(PrivateFragmentAnchor) {
    add_focus("main.cpp", R"cpp(
export module app;
#if 0
module :private;
#endif
module :§(live)⟦private⟧;
)cpp");
    ZASSERT(compile());
    for(bool all_files: {false, true}) {
        auto semantics = Semantics::build(*unit, {.all_files = all_files});
        auto modules = semantics.module_declarations();
        ZASSERT(modules.size() == 2U);
        ZEXPECT(modules[1].kind == LexicalInfo::ModuleDeclaration::Kind::PrivateFragment);
        ZEXPECT(modules[1].partition_parts.front() == range("live"));
    }
}

};  // ZEST_SUITE(Tokens)

}  // namespace

}  // namespace clice::testing
