#include "test/test.h"
#include "feature/feature.h"

namespace clice::testing {

namespace {

TEST_SUITE(Formatting) {

TEST_CASE(Simple) {
    auto edits = feature::document_format("main.cpp", "int main() { return 0; }", std::nullopt);
    ASSERT_NE(edits.size(), 0U);
}

TEST_CASE(RangeFormat) {
    llvm::StringRef code = "int x=1;\nint   y =  2 ;\nint z=3;\n";
    LocalSourceRange range;
    range.begin = static_cast<std::uint32_t>(code.find("int   y"));
    range.end = static_cast<std::uint32_t>(code.find("\nint z") + 1);
    auto range_edits = feature::document_format("main.cpp", code, range);
    auto full_edits = feature::document_format("main.cpp", code, std::nullopt);
    ASSERT_NE(range_edits.size(), 0U);
    EXPECT_LE(range_edits.size(), full_edits.size());
}

TEST_CASE(Idempotent) {
    llvm::StringRef code = "int main() {\n    return 0;\n}\n";
    auto edits = feature::document_format("main.cpp", code, std::nullopt);
    EXPECT_EQ(edits.size(), 0U);
}

TEST_CASE(IncludeSort) {
    llvm::StringRef code = "#include <vector>\n#include <algorithm>\n\nint main() {}\n";
    auto edits = feature::document_format("main.cpp", code, std::nullopt);
    ASSERT_NE(edits.size(), 0U);
}

};  // TEST_SUITE(Formatting)

}  // namespace

}  // namespace clice::testing
