module;

#include "modules/prelude.h"

module clice;

import :index.site;
import :tests.unit.test.test;

namespace clice::testing {
namespace {

using index::Coordinates;

ZEST_SUITE(Coordinates) {

// Line starts of "ab\ncd\n" — two 2-byte lines plus the empty last line.
std::vector<std::uint32_t> starts = {0, 3, 6};

ZEST_CASE(AsciiArithmetic) {
    // No stored content: byte columns are UTF-16 columns, mapping is pure
    // line-table arithmetic bounded by the content size.
    Coordinates map(6, starts, {});

    auto pos = map.position(4);
    ZASSERT(pos);
    ZEXPECT(pos->line == 1u);
    ZEXPECT(pos->column == 1u);
    ZEXPECT(pos->utf16_column == 1u);
    ZEXPECT(map.offset(1, 1) == std::optional<std::uint32_t>(4));

    // The newline offset is its line's end position, not the next line.
    auto line_end = map.position(2);
    ZASSERT(line_end);
    ZEXPECT(line_end->line == 0u);
    ZEXPECT(line_end->column == 2u);

    // Past the content, past the last line: refused. Past a line's end:
    // its end, a huge column included.
    ZEXPECT(!map.position(7).has_value());
    ZEXPECT(!map.offset(3, 0).has_value());
    ZEXPECT(map.offset(0, 3) == std::optional<std::uint32_t>(2));
    ZEXPECT(map.offset(1, 0xfffffffd) == std::optional<std::uint32_t>(5));

    auto bounds = map.line_bounds(1);
    ZASSERT(bounds);
    ZEXPECT(bounds->begin == 3u);
    ZEXPECT(bounds->end == 5u);
    ZEXPECT(!map.line_bounds(3).has_value());
}

ZEST_CASE(EmptyLineTable) {
    Coordinates map(6, {}, {});
    ZEXPECT(!map.position(0).has_value());
    ZEXPECT(!map.offset(0, 0).has_value());
}

ZEST_CASE(StoredContentCountsUtf16) {
    // The é on line 1 is two UTF-8 bytes but one UTF-16 unit, so the
    // offset past it has a smaller UTF-16 column than its byte column.
    llvm::StringRef content = "ab\né!\n";
    std::vector<std::uint32_t> line_starts = {0, 3, 7};
    Coordinates map(content, line_starts);

    auto pos = map.position(5);
    ZASSERT(pos);
    ZEXPECT(pos->line == 1u);
    ZEXPECT(pos->column == 2u);
    ZEXPECT(pos->utf16_column == 1u);
    ZEXPECT(map.offset(1, 1) == std::optional<std::uint32_t>(5));
}

ZEST_CASE(CRLFLineEnds) {
    // "ab\r\ncd": the '\r' belongs to the line's end, not its text.
    std::vector<std::uint32_t> crlf_starts = {0, 4};
    std::vector<std::uint64_t> crlf_lines = {1};
    Coordinates ascii(6, crlf_starts, crlf_lines);
    llvm::StringRef content = "\xc3\xa9\r\ncd";
    Coordinates stored(content, crlf_starts);

    for(auto* map: {&ascii, &stored}) {
        for(std::uint32_t offset: {2u, 3u}) {
            auto pos = map->position(offset);
            ZASSERT(pos);
            ZEXPECT(pos->line == 0u);
            ZEXPECT(pos->column == 2u);
        }
        ZEXPECT(map->offset(0, 5) == std::optional<std::uint32_t>(2));
        ZEXPECT(map->line_bounds(0) == std::optional(LocalSourceRange{0, 2}));
    }
    ZEXPECT(stored.position(3)->utf16_column == 1u);
}

};  // ZEST_SUITE(Coordinates)

}  // namespace
}  // namespace clice::testing
