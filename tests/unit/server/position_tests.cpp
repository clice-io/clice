#include <vector>

#include "test/test.h"
#include "server/protocol/position.h"

namespace clice::testing {
namespace {

TEST_SUITE(IndexedLineMap) {

// Line starts of "ab\ncd\n" — two 2-byte lines plus the empty last line.
std::vector<std::uint32_t> starts = {0, 3, 6};

TEST_CASE(AsciiArithmetic) {
    // No stored content: byte offsets are UTF-16 offsets, mapping is pure
    // line-table arithmetic bounded by the content size.
    IndexedLineMap map("", 6, starts);

    auto pos = map.to_position(4);
    ASSERT_TRUE(pos.has_value());
    EXPECT_EQ(pos->line, 1u);
    EXPECT_EQ(pos->character, 1u);

    auto offset = map.to_offset(*pos);
    ASSERT_TRUE(offset.has_value());
    EXPECT_EQ(*offset, 4u);

    // The newline offset is its line's end position, not the next line.
    auto line_end = map.to_position(2);
    ASSERT_TRUE(line_end.has_value());
    EXPECT_EQ(line_end->line, 0u);
    EXPECT_EQ(line_end->character, 2u);

    // Past the content, past the line, inverted range: all refused.
    EXPECT_FALSE(map.to_position(7).has_value());
    EXPECT_FALSE(map.to_offset({.line = 3, .character = 0}).has_value());
    EXPECT_FALSE(map.to_offset({.line = 0, .character = 3}).has_value());
    // A huge character must not wrap the offset back into bounds.
    EXPECT_FALSE(map.to_offset({.line = 1, .character = 0xfffffffd}).has_value());
    EXPECT_FALSE(map.to_range(4, 2).has_value());

    auto range = map.to_range(3, 5);
    ASSERT_TRUE(range.has_value());
    EXPECT_EQ(range->start.line, 1u);
    EXPECT_EQ(range->start.character, 0u);
    EXPECT_EQ(range->end.character, 2u);
}

TEST_CASE(EmptyLineTable) {
    IndexedLineMap map("", 6, {});
    EXPECT_FALSE(map.to_position(0).has_value());
    EXPECT_FALSE(map.to_offset({.line = 0, .character = 0}).has_value());
}

TEST_CASE(StoredContentDelegates) {
    // Stored (non-ASCII) content delegates to LineMap: the é on line 1 is
    // two UTF-8 bytes but one UTF-16 unit, so the offset past it maps to
    // a smaller character than its byte column.
    llvm::StringRef content = "ab\né!\n";
    std::vector<std::uint32_t> line_starts = {0, 3, 7};
    IndexedLineMap map(content, static_cast<std::uint32_t>(content.size()), line_starts);

    auto pos = map.to_position(5);
    ASSERT_TRUE(pos.has_value());
    EXPECT_EQ(pos->line, 1u);
    EXPECT_EQ(pos->character, 1u);

    auto offset = map.to_offset(*pos);
    ASSERT_TRUE(offset.has_value());
    EXPECT_EQ(*offset, 5u);
}

};  // TEST_SUITE(IndexedLineMap)

}  // namespace
}  // namespace clice::testing
