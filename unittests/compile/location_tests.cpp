module clice;

import :compile.compilation;
import :compile.directive;
import :unittests.utils;
import :unittests.utils.tester;

namespace clice::testing {

namespace {

ZEST_SUITE(Location, Tester) {

ZEST_CASE(MacroArgumentAndBody) {
    add_focus("main.cpp", R"cpp(
#define CALL(f) f(§(one)1) + 0
int helper(int);
int x = §(call)⟦CALL(§(arg)⟦helper⟧)⟧;
)cpp");
    ZASSERT(compile());
    const auto* var = declared("x");
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
    const auto* var = declared("b");
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
    const auto* var = declared("v");
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

ZEST_CASE(WrittenSeenProduced) {
    add_focus("main.cpp", R"cpp(
#define ID(x) x
#define ONE §(body)1
#define CAT(a, b) a##b
int §(plain)plain = 0;
int nested = §(id)ID(§(one)ONE);
int §(cat)CAT(pas, ted);
)cpp");
    ZASSERT(compile());
    auto focus = unit->focus_file().file();
    auto at = [&](llvm::StringRef name) {
        return FileOffset{focus, point(name)};
    };

    auto plain = declared("plain")->getLocation();
    ZEXPECT(unit->written_at(plain) == at("plain"));
    ZEXPECT(unit->seen_at(plain) == at("plain"));
    ZEXPECT(unit->produced_at(plain) == at("plain"));

    // ONE, invoked in ID's argument, produces `1`.
    auto one = declared("nested")->getInit()->getBeginLoc();
    ZEXPECT(unit->written_at(one) == at("body"));
    ZEXPECT(unit->seen_at(one) == at("one"));
    ZEXPECT(unit->produced_at(one) == at("id"));

    auto pasted = declared("pasted")->getLocation();
    ZEXPECT(unit->written_at(pasted).file != focus);
    ZEXPECT(unit->seen_at(pasted) == at("cat"));
    ZEXPECT(unit->produced_at(pasted) == at("cat"));

    for(auto location: {plain, one, pasted}) {
        auto written = unit->written_location(location);
        ZEXPECT(written.isFileID());
        ZEXPECT(unit->seen_at(written) == unit->written_at(location));
        auto seen = unit->seen_location(location);
        ZEXPECT(seen.isFileID());
        ZEXPECT(unit->seen_at(seen) == unit->seen_at(location));
    }
}

ZEST_CASE(Concatenated) {
    add_focus("main.cpp", R"cpp(
#define CAT(a, b) a##b
#define ID(x) x
#define ZERO 0
int CAT(concat, enated) = ZERO;
int ID(argument);
)cpp");
    ZASSERT(compile());
    ZEXPECT(unit->concatenated(declared("concatenated")->getLocation()));
    ZEXPECT(!unit->concatenated(declared("concatenated")->getInit()->getBeginLoc()));
    ZEXPECT(!unit->concatenated(declared("concatenated")->getBeginLoc()));
    ZEXPECT(!unit->concatenated(declared("argument")->getLocation()));
}

ZEST_CASE(ExpansionLevels) {
    add_focus("main.cpp", R"cpp(
#define ID(x) §(param)x
#define ONE §(one)1
#define CALL(f) f(§(two)2)
int helper(int);
int a = §(id)ID(§(arg)ONE);
int b = §(call)CALL(helper);
)cpp");
    ZASSERT(compile());

    // `1` comes out of ID's argument, which ONE expanded first.
    auto one = declared("a")->getInit()->getBeginLoc();
    auto argument = unit->expansion_level(one);
    ZEXPECT(argument.argument);
    ZEXPECT(unit->written_at(argument.start).offset == point("param"));
    ZASSERT(argument.spelling.isMacroID());
    auto inner = unit->expansion_level(argument.spelling);
    ZEXPECT(!inner.argument);
    ZEXPECT(unit->written_at(inner.spelling).offset == point("one"));
    ZEXPECT(unit->seen_at(inner.start).offset == point("arg"));
    auto outer = unit->expansion_level(argument.start);
    ZEXPECT(!outer.argument);
    ZEXPECT(unit->seen_at(outer.start).offset == point("id"));

    const auto* call = llvm::dyn_cast<clang::CallExpr>(declared("b")->getInit()->IgnoreImplicit());
    ZASSERT(call);
    auto two = call->getArg(0)->getBeginLoc();
    auto body = unit->expansion_level(two);
    ZEXPECT(!body.argument);
    ZEXPECT(body.spelling.isFileID());
    ZEXPECT(unit->written_at(body.spelling).offset == point("two"));
    ZEXPECT(unit->seen_at(body.start).offset == point("call"));

    for(auto location: {one, two}) {
        auto spelling = location;
        while(spelling.isMacroID()) {
            spelling = unit->expansion_level(spelling).spelling;
        }
        ZEXPECT(unit->seen_at(spelling) == unit->written_at(location));
        auto start = location;
        while(start.isMacroID()) {
            start = unit->expansion_level(start).start;
        }
        ZEXPECT(unit->seen_at(start) == unit->produced_at(location));
    }
}

ZEST_CASE(SeenNames) {
    add_focus("main.cpp", R"cpp(
#define MAKE int made;
#define EQ ==
#define OP operator
struct S {
    bool §(call)operator(§(close))();
    bool §(eq)operator EQ(const S&);
    bool §(op)OP!=(const S&);
};
§(make)MAKE
int §(plain)plain;
)cpp");
    ZASSERT(compile());
    auto begin = [&](const SeenName& seen) {
        return unit->seen_at(seen.range.getBegin()).offset;
    };
    auto end = [&](const SeenName& seen) {
        return unit->seen_at(seen.range.getEnd()).offset;
    };

    auto plain = unit->seen_name(declared("plain")->getLocation());
    ZEXPECT(plain.written);
    ZEXPECT(begin(plain) == point("plain"));
    ZEXPECT(end(plain) == point("plain"));

    auto made = unit->seen_name(declared("made")->getLocation());
    ZEXPECT(!made.written);
    ZEXPECT(begin(made) == point("make"));
    ZEXPECT(end(made) == point("make"));

    std::vector<SeenName> operators;
    for(auto* method: declared<clang::CXXRecordDecl>("S")->methods()) {
        if(!method->isImplicit()) {
            operators.push_back(unit->seen_name(method->getNameInfo().getSourceRange()));
        }
    }
    ZASSERT(operators.size() == 3U);

    ZEXPECT(operators[0].written);
    ZEXPECT(begin(operators[0]) == point("call"));
    ZEXPECT(end(operators[0]) == point("close"));

    // EQ's body spells the name's last token.
    ZEXPECT(operators[1].written);
    ZEXPECT(begin(operators[1]) == point("eq"));
    ZEXPECT(end(operators[1]) == point("eq"));

    ZEXPECT(!operators[2].written);
    ZEXPECT(begin(operators[2]) == point("op"));
    ZEXPECT(end(operators[2]) == point("op"));
}

ZEST_CASE(OrderInUnit) {
    add_files("main.cpp", R"cpp(
#[a.h]
int header;
#[main.cpp]
int first;
#include "a.h"
#define PAIR int one; int two;
PAIR
int last;
)cpp");
    ZASSERT(compile());
    auto location = [&](llvm::StringRef name) {
        return declared(name)->getLocation();
    };

    ZEXPECT(unit->before(location("first"), location("header")));
    ZEXPECT(unit->before(location("header"), location("last")));
    ZEXPECT(!unit->before(location("header"), location("first")));

    ZEXPECT(unit->before(location("one"), location("two")));
    ZEXPECT(!unit->before(location("two"), location("one")));
    ZEXPECT(unit->before(location("header"), location("one")));
    ZEXPECT(unit->before(location("two"), location("last")));
}

ZEST_CASE(RangesOutsideFocus) {
    add_files("main.cpp", R"cpp(
#[call.h]
#define CALL(f) f(1) + 0
int helper(int);
int in_header = §(call)⟦CALL(§(arg)⟦helper⟧)⟧;
#[split.h]
int split =
#[main.cpp]
#include "call.h"
#include "split.h"
1;
)cpp");
    ZASSERT(compile());
    const auto* sum =
        llvm::dyn_cast<clang::BinaryOperator>(declared("in_header")->getInit()->IgnoreImplicit());
    ZASSERT(sum);
    const auto* call = llvm::dyn_cast<clang::CallExpr>(sum->getLHS()->IgnoreImplicit());
    ZASSERT(call);
    const auto* callee = call->getCallee()->IgnoreImplicit();
    auto header = unit->file_id(TestVFS::path("call.h"));

    // Out of the focus file clang's own mapping answers.
    auto whole = unit->written_range(sum->getSourceRange());
    ZASSERT(whole);
    ZEXPECT(whole->file == header);
    ZEXPECT(whole->range == range("call", "call.h"));
    auto argument = unit->written_range(callee->getSourceRange());
    ZASSERT(argument);
    ZEXPECT(argument->range == range("arg", "call.h"));
    ZEXPECT(!unit->written_range(call->getSourceRange()));
    auto produced = unit->produced_range(callee->getSourceRange());
    ZEXPECT(produced.file == header);
    ZEXPECT(produced.range == range("call", "call.h"));

    auto focus = unit->focus_file();
    ZEXPECT(!focus.written_range(callee->getSourceRange()));
    ZEXPECT(!focus.produced_range(callee->getSourceRange()));

    // The token range is the one of the SourceRange; a character range
    // ends where it says.
    auto tokens =
        unit->written_range(clang::CharSourceRange::getTokenRange(callee->getSourceRange()));
    ZASSERT(tokens);
    ZEXPECT(tokens->range == range("arg", "call.h"));
    auto name = declared("in_header")->getLocation();
    auto characters =
        unit->written_range(clang::CharSourceRange::getCharRange(name, name.getLocWithOffset(3)));
    ZASSERT(characters);
    ZEXPECT(characters->file == header);
    ZEXPECT(characters->range.begin == unit->seen_at(name).offset);
    ZEXPECT(characters->range.end == unit->seen_at(name).offset + 3);

    // `int split =` ends its file, `1;` follows the include.
    auto split = declared("split")->getSourceRange();
    auto split_file = unit->file_id(TestVFS::path("split.h"));
    ZEXPECT(unit->file_id(split.getBegin()) == split_file);
    ZEXPECT(unit->file_id(split.getEnd()) == focus.file());
    ZEXPECT(!unit->written_range(split));
    ZEXPECT(!unit->written_range(clang::CharSourceRange::getCharRange(split)));
    auto first = unit->produced_range(split);
    ZEXPECT(first.file == split_file);
    ZEXPECT(first.range.begin == 0U);
    ZEXPECT(first.range.end == unit->file_content(split_file).size());
}

ZEST_CASE(ProducedAcrossInvocations) {
    add_focus("main.cpp", R"cpp(
#define A 1
#define B 2
int §(plain)⟦plain⟧ = 0;
int sum = §(sum)⟦A + §(b)B⟧;
)cpp");
    ZASSERT(compile());
    auto focus = unit->focus_file();
    auto sum = declared("sum")->getInit()->getSourceRange();
    ZEXPECT(focus.produced_range(sum) == range("sum"));
    auto bounds = unit->produced_bounds(sum);
    ZEXPECT(unit->seen_at(bounds.getBegin()).offset == range("sum").begin);
    ZEXPECT(unit->seen_at(bounds.getEnd()).offset == point("b"));

    auto plain = clang::SourceRange(declared("plain")->getLocation());
    ZEXPECT(focus.produced_range(plain) == range("plain"));
    ZEXPECT(unit->produced_bounds(plain) == plain);
}

ZEST_CASE(TokenMeasures) {
    add_focus("main.cpp", R"cpp(
#define LONGNAME 1
in\
t spliced = LONGNAME;
)cpp");
    ZASSERT(compile());
    const auto* spliced = declared("spliced");

    // `in\` and `t` on the next line are one token.
    auto type = spliced->getBeginLoc();
    ZEXPECT(unit->token_length(type) == 5U);
    ZEXPECT(unit->token_spelling(type) == "int");

    auto value = spliced->getInit()->getBeginLoc();
    ZEXPECT(unit->token_length(value) == 8U);
    ZEXPECT(unit->token_spelling(value) == "1");
}

ZEST_CASE(NeighborTokens) {
    add_focus("main.cpp", R"cpp(int §(x)x /* note */ §(semi);
#define SUM 1 + 2
int y = SUM §(star)* 3;
)cpp");
    ZASSERT(compile());
    auto offset = [&](const std::optional<clang::Token>& token) {
        return unit->seen_at(token->getLocation()).offset;
    };
    const auto* x = declared("x");

    auto semi = unit->next_token(x->getLocation());
    ZASSERT(semi);
    ZEXPECT(semi->is(clang::tok::semi));
    ZEXPECT(offset(semi) == point("semi"));
    auto name = unit->previous_token(semi->getLocation());
    ZASSERT(name);
    ZEXPECT(offset(name) == point("x"));
    ZEXPECT(!unit->previous_token(x->getBeginLoc()));

    // SUM's `2` binds to `* 3`.
    const auto* sum = llvm::dyn_cast<clang::BinaryOperator>(declared("y")->getInit());
    ZASSERT(sum);
    const auto* product = llvm::dyn_cast<clang::BinaryOperator>(sum->getRHS());
    ZASSERT(product);
    ZEXPECT(!unit->next_token(sum->getLHS()->getBeginLoc()));
    auto star = unit->next_token(product->getLHS()->getBeginLoc());
    ZASSERT(star);
    ZEXPECT(star->is(clang::tok::star));
    ZEXPECT(offset(star) == point("star"));
    auto last = unit->next_token(product->getRHS()->getBeginLoc());
    ZASSERT(last);
    auto end = unit->next_token(last->getLocation());
    ZASSERT(end);
    ZEXPECT(end->is(clang::tok::eof));
}

ZEST_CASE(PresumedLocations) {
    add_files("main.cpp", R"cpp(
#[a.h]
int header;
#[main.cpp]
#include "a.h"
#define VALUE 1
#line 100 "renamed.cpp"
int value = VALUE;
)cpp");
    ZASSERT(compile());
    auto value = unit->presumed_location(declared("value")->getLocation());
    ZEXPECT(value.getLine() == 4U);
    ZEXPECT(value.getColumn() == 5U);
    ZEXPECT(llvm::StringRef(value.getFilename()).ends_with("main.cpp"));

    auto expanded = unit->presumed_location(declared("value")->getInit()->getBeginLoc());
    ZEXPECT(expanded.getLine() == 4U);
    ZEXPECT(expanded.getColumn() == 13U);

    auto header = declared("header")->getLocation();
    auto included = unit->presumed_location(header);
    ZEXPECT(included.getLine() == 1U);
    ZEXPECT(included.getIncludeLoc() == unit->include_location(unit->file_id(header)));
}

ZEST_CASE(CreateLocation) {
    add_files("main.cpp", R"cpp(
#[a.h]
int §(header)header;
#[main.cpp]
#include "a.h"
int §(x)x;
)cpp");
    ZASSERT(compile());
    auto focus = unit->focus_file();
    for(auto offset: {0U, point("x")}) {
        ZEXPECT(unit->seen_at(unit->create_location(focus.file(), offset)) ==
                FileOffset{focus.file(), offset});
        ZEXPECT(focus.offset(focus.location(offset)) == offset);
    }
    auto header = unit->file_id(TestVFS::path("a.h"));
    ZEXPECT(unit->token_spelling(unit->create_location(header, point("header", "a.h"))) ==
            "header");
}

ZEST_CASE(FocusOffsets) {
    add_files("main.cpp", R"cpp(
#[a.h]
#define HEADER_VALUE 7
int header;
#[main.cpp]
#include "a.h"
#define LOCAL §(body)5
int local = §(local)LOCAL;
int from_header = §(use)HEADER_VALUE;
)cpp");
    ZASSERT(compile());
    auto focus = unit->focus_file();

    auto local = declared("local")->getInit()->getBeginLoc();
    ZEXPECT(focus.offset(local) == point("local"));
    ZEXPECT(focus.written_offset(local) == point("body"));

    auto from_header = declared("from_header")->getInit()->getBeginLoc();
    ZEXPECT(focus.offset(from_header) == point("use"));
    ZEXPECT(!focus.written_offset(from_header));

    auto header = declared("header")->getLocation();
    ZEXPECT(!focus.offset(header));
    ZEXPECT(!focus.written_offset(header));
}

};  // ZEST_SUITE(Location)

}  // namespace

}  // namespace clice::testing
