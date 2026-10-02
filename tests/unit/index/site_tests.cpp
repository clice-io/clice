#include <vector>

#include "test/test.h"
#include "index/site.h"

namespace clice::testing {
namespace {

using index::Coordinates;

ZEST_SUITE(Coordinates) {

// Line starts of "ab\ncd\n" — two 2-byte lines plus the empty last line.
std::vector<std::uint32_t> starts = {0, 3, 6};

ZEST_CASE(AsciiArithmetic) {
    // No stored content: byte columns are UTF-16 columns, mapping is pure
    // line-table arithmetic bounded by the content size.
    Coordinates map("", 6, starts);

    auto pos = map.position(4);
    ASSERT(pos);
    EXPECT(pos->line == 1u);
    EXPECT(pos->column == 1u);
    EXPECT(pos->utf16_column == 1u);
    EXPECT(map.offset(1, 1) == std::optional<std::uint32_t>(4));

    // The newline offset is its line's end position, not the next line.
    auto line_end = map.position(2);
    ASSERT(line_end);
    EXPECT(line_end->line == 0u);
    EXPECT(line_end->column == 2u);

    // Past the content, past the line: refused.
    EXPECT(!map.position(7).has_value());
    EXPECT(!map.offset(3, 0).has_value());
    EXPECT(!map.offset(0, 3).has_value());
    // A huge column must not wrap the offset back into bounds.
    EXPECT(!map.offset(1, 0xfffffffd).has_value());

    auto bounds = map.line_bounds(1);
    ASSERT(bounds);
    EXPECT(bounds->begin == 3u);
    EXPECT(bounds->end == 5u);
    EXPECT(!map.line_bounds(3).has_value());
}

ZEST_CASE(EmptyLineTable) {
    Coordinates map("", 6, {});
    EXPECT(!map.position(0).has_value());
    EXPECT(!map.offset(0, 0).has_value());
}

ZEST_CASE(StoredContentCountsUtf16) {
    // The é on line 1 is two UTF-8 bytes but one UTF-16 unit, so the
    // offset past it has a smaller UTF-16 column than its byte column.
    llvm::StringRef content = "ab\né!\n";
    std::vector<std::uint32_t> line_starts = {0, 3, 7};
    Coordinates map(content, static_cast<std::uint32_t>(content.size()), line_starts);

    auto pos = map.position(5);
    ASSERT(pos);
    EXPECT(pos->line == 1u);
    EXPECT(pos->column == 2u);
    EXPECT(pos->utf16_column == 1u);
    EXPECT(map.offset(1, 1) == std::optional<std::uint32_t>(5));
}

};  // ZEST_SUITE(Coordinates)

}  // namespace
}  // namespace clice::testing
