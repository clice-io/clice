module;

#include "support/prelude.h"

module clice;

import :syntax.lexer;
import :syntax.position;
import :syntax.source_text;
import :unittests.utils;

namespace clice::testing {
namespace {

ZEST_SUITE(SourceText) {

llvm::StringRef content = "// lead\nint x; /* tail */\n";

ZEST_CASE(PositionsKept) {
    SourceText text(content, dialect_of(), keywords(dialect_of()));
    auto first = text.positions();
    auto again = text.positions();
    auto position = first.position(content.find("x"), PositionEncoding::UTF16);
    ZASSERT(position);
    ZEXPECT(position->line == 1U);
    ZEXPECT(position->character == 4U);
    ZEXPECT(again.position(content.size(), PositionEncoding::UTF16)->line == 2U);
}

ZEST_CASE(CommentsAlone) {
    SourceText text(content, dialect_of(), keywords(dialect_of()));
    auto comments = text.comments();
    ZASSERT(comments.size() == 2U);
    ZEXPECT(content.substr(comments[1].range.begin, comments[1].range.length()) == "/* tail */");
    ZEXPECT(text.comments().data() == comments.data());
}

ZEST_CASE(CommentsOfScan) {
    // Once the lexical scan ran, its comments are the text's.
    SourceText text(content, dialect_of(), keywords(dialect_of()));
    const auto& scan = text.lexical();
    ZEXPECT(&text.lexical() == &scan);
    ZEXPECT(text.comments().data() == scan.comments.data());
    ZEXPECT(text.comments().size() == 2U);
}

ZEST_CASE(KeywordKinds) {
    // A keyword is told by its spelling, line splices folded away; an
    // alternative operator name is the punctuator it spells.
    llvm::StringRef code = R"(in\
t x and y;)";
    SourceText text(code, dialect_of(), keywords(dialect_of()));
    const auto& tokens = text.lexical().tokens;
    ZASSERT(tokens.size() == 5U);
    ZEXPECT(text.kind(tokens[0]) == clang::tok::kw_int);
    ZEXPECT(text.kind(tokens[1]) == clang::tok::identifier);
    ZEXPECT(text.kind(tokens[2]) == clang::tok::ampamp);
    ZEXPECT(text.kind(tokens[4]) == clang::tok::semi);
}

ZEST_CASE(OwnedReadLater) {
    // An owned text answers positions before it is read in a dialect.
    SourceText text(std::string("int x;\nint y;\n"));
    ZEXPECT(text.positions().position(7, PositionEncoding::UTF16)->line == 1U);
    text.set_dialect(dialect_of());
    ZEXPECT(text.lexical().tokens.size() == 6U);
}

ZEST_CASE(DialectChangeRescans) {
    // `R"(` opens a raw string in C++, not in C11; `class` is a keyword in
    // C++ only.
    SourceText text(std::string(R"code(class R"(a)";)code"));
    text.set_dialect(dialect_of());
    ZASSERT(text.lexical().tokens.size() == 3U);
    ZEXPECT(text.kind(text.lexical().tokens[0]) == clang::tok::kw_class);

    text.set_dialect(dialect_of("main.c", true, "c11"));
    const auto& tokens = text.lexical().tokens;
    ZASSERT(tokens.size() == 4U);
    ZEXPECT(text.kind(tokens[0]) == clang::tok::identifier);
}

};  // ZEST_SUITE(SourceText)

}  // namespace
}  // namespace clice::testing
