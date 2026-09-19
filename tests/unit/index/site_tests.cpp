#include <vector>

#include "test/test.h"
#include "index/site.h"

namespace clice::testing {
namespace {

using index::Coordinates;

TEST_SUITE(Coordinates) {

// Line starts of "ab\ncd\n" — two 2-byte lines plus the empty last line.
std::vector<std::uint32_t> starts = {0, 3, 6};

TEST_CASE(AsciiArithmetic) {
    // No stored content: byte columns are UTF-16 columns, mapping is pure
    // line-table arithmetic bounded by the content size.
    Coordinates map("", 6, starts);

    auto pos = map.position(4);
    ASSERT_TRUE(pos.has_value());
    EXPECT_EQ(pos->line, 1u);
    EXPECT_EQ(pos->column, 1u);
    EXPECT_EQ(pos->utf16_column, 1u);
    EXPECT_EQ(map.offset(1, 1), std::optional<std::uint32_t>(4));

    // The newline offset is its line's end position, not the next line.
    auto line_end = map.position(2);
    ASSERT_TRUE(line_end.has_value());
    EXPECT_EQ(line_end->line, 0u);
    EXPECT_EQ(line_end->column, 2u);

    // Past the content, past the line: refused.
    EXPECT_FALSE(map.position(7).has_value());
    EXPECT_FALSE(map.offset(3, 0).has_value());
    EXPECT_FALSE(map.offset(0, 3).has_value());
    // A huge column must not wrap the offset back into bounds.
    EXPECT_FALSE(map.offset(1, 0xfffffffd).has_value());

    auto bounds = map.line_bounds(1);
    ASSERT_TRUE(bounds.has_value());
    EXPECT_EQ(bounds->begin, 3u);
    EXPECT_EQ(bounds->end, 5u);
    EXPECT_FALSE(map.line_bounds(3).has_value());
}

TEST_CASE(EmptyLineTable) {
    Coordinates map("", 6, {});
    EXPECT_FALSE(map.position(0).has_value());
    EXPECT_FALSE(map.offset(0, 0).has_value());
}

TEST_CASE(StoredContentCountsUtf16) {
    // The é on line 1 is two UTF-8 bytes but one UTF-16 unit, so the
    // offset past it has a smaller UTF-16 column than its byte column.
    llvm::StringRef content = "ab\né!\n";
    std::vector<std::uint32_t> line_starts = {0, 3, 7};
    Coordinates map(content, static_cast<std::uint32_t>(content.size()), line_starts);

    auto pos = map.position(5);
    ASSERT_TRUE(pos.has_value());
    EXPECT_EQ(pos->line, 1u);
    EXPECT_EQ(pos->column, 2u);
    EXPECT_EQ(pos->utf16_column, 1u);
    EXPECT_EQ(map.offset(1, 1), std::optional<std::uint32_t>(5));
}

};  // TEST_SUITE(Coordinates)

}  // namespace
}  // namespace clice::testing
