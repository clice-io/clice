module;

#include "support/prelude.h"

module clice;

import :index.site;
import :syntax.position;
import :unittests.utils;

namespace clice::testing {
namespace {

ZEST_SUITE(LineColumn) {

ZEST_CASE(AsciiColumnsAgree) {
    // "ab\ncd\n" known by its size: byte columns are UTF-16 columns.
    std::vector<std::uint32_t> starts = {0, 3, 6};
    PositionMap map(6, starts, {});

    auto position = index::line_column(map, 4);
    ZASSERT(position);
    ZEXPECT(position->line == 1u);
    ZEXPECT(position->column == 1u);
    ZEXPECT(position->utf16_column == 1u);
}

ZEST_CASE(StoredContentCountsUtf16) {
    // The é on line 1 is two UTF-8 bytes but one UTF-16 unit.
    llvm::StringRef content = "ab\né!\n";
    auto starts = lsp::line_starts(content);
    PositionMap map(content, starts);

    auto position = index::line_column(map, 5);
    ZASSERT(position);
    ZEXPECT(position->line == 1u);
    ZEXPECT(position->column == 2u);
    ZEXPECT(position->utf16_column == 1u);
}

ZEST_CASE(CRLFLineEnds) {
    // "é\r\ncd": both offsets of the line end are at the line's end.
    llvm::StringRef content = "\xc3\xa9\r\ncd";
    auto starts = lsp::line_starts(content);
    PositionMap map(content, starts);

    for(std::uint32_t offset: {2u, 3u}) {
        auto position = index::line_column(map, offset);
        ZASSERT(position);
        ZEXPECT(position->line == 0u);
        ZEXPECT(position->column == 2u);
        ZEXPECT(position->utf16_column == 1u);
    }
}

ZEST_CASE(AstralCountsEachEncoding) {
    // The emoji is four UTF-8 bytes, two UTF-16 units and one code point.
    llvm::StringRef content = "\xf0\x9f\x98\x80x\n";
    auto starts = lsp::line_starts(content);
    PositionMap map(content, starts);

    auto position = index::line_column(map, 5);
    ZASSERT(position);
    ZEXPECT(position->position(PositionEncoding::UTF8).character == 5u);
    ZEXPECT(position->position(PositionEncoding::UTF16).character == 3u);
    ZEXPECT(position->position(PositionEncoding::UTF32).character == 2u);
}

};  // ZEST_SUITE(LineColumn)

}  // namespace
}  // namespace clice::testing
