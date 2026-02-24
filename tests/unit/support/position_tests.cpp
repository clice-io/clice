#include "test/test.h"
#include "support/position.h"

namespace clice::testing {

namespace {

TEST_SUITE(Position) {

TEST_CASE(UTF16Column) {
    llvm::StringRef content = "a\xe4\xbd\xa0" "b\n";
    PositionConverter converter(content, PositionEncoding::UTF16);

    auto position = converter.to_position(4);
    ASSERT_EQ(position.line, 0U);
    ASSERT_EQ(position.character, 2U);
}

TEST_CASE(RoundTripOffset) {
    llvm::StringRef content = "a\xe4\xbd\xa0" "b\nx\xf0\x9f\x99\x82" "y";
    constexpr std::uint32_t offsets[] = {0, 1, 4, 5, 6, 7, 11, 12};

    for(auto encoding: {PositionEncoding::UTF8, PositionEncoding::UTF16, PositionEncoding::UTF32}) {
        PositionConverter converter(content, encoding);
        for(auto offset: offsets) {
            auto position = converter.to_position(offset);
            ASSERT_EQ(converter.to_offset(position), offset);
        }
    }
}

};  // TEST_SUITE(Position)

}  // namespace

}  // namespace clice::testing
