#include "Test/Test.h"
#include "Feature/Formatting.h"

namespace clice::testing {
namespace {
TEST_SUITE(Formatting) {

TEST_CASE(Simple) {
    auto edits = feature::document_format("main.cpp", "int main() { return 0; }", std::nullopt);
    ASSERT_NE(edits.size(), 0U);
}

};  // TEST_SUITE(Formatting)
}  // namespace
}  // namespace clice::testing
