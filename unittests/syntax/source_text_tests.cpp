module;

#include "modules/prelude.h"

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
    SourceText text(content, dialect_of());
    auto first = text.positions();
    auto again = text.positions();
    auto position = first.position(content.find("x"), PositionEncoding::UTF16);
    ZASSERT(position);
    ZEXPECT(position->line == 1U);
    ZEXPECT(position->character == 4U);
    ZEXPECT(again.position(content.size(), PositionEncoding::UTF16)->line == 2U);
}

ZEST_CASE(CommentsAlone) {
    SourceText text(content, dialect_of());
    auto comments = text.comments();
    ZASSERT(comments.size() == 2U);
    ZEXPECT(content.substr(comments[1].range.begin, comments[1].range.length()) == "/* tail */");
    ZEXPECT(text.comments().data() == comments.data());
}

ZEST_CASE(CommentsOfScan) {
    // Once the lexical scan ran, its comments are the text's.
    SourceText text(content, dialect_of());
    const auto& scan = text.lexical();
    ZEXPECT(&text.lexical() == &scan);
    ZEXPECT(text.comments().data() == scan.comments.data());
    ZEXPECT(text.comments().size() == 2U);
}

};  // ZEST_SUITE(SourceText)

}  // namespace
}  // namespace clice::testing
