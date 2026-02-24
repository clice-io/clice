#include <map>

#include "test/test.h"
#include "support/format.h"

namespace clice::testing {
namespace {

enum class ExampleEnum : unsigned char {
    Alpha = 0,
    Beta = 1,
};

struct ExampleStruct {
    int left;
    int right;
};

TEST_SUITE(FormatSupport) {

TEST_CASE(FormatLLVMStringRef) {
    llvm::StringRef value = "hello";
    EXPECT_EQ(std::format("{}", value), "hello");
}

TEST_CASE(FormatEnumAndStruct) {
    auto enum_text = std::format("{}", ExampleEnum::Alpha);
    EXPECT_FALSE(enum_text.empty());

    auto struct_text = clice::dump(ExampleStruct{1, 2});
    EXPECT_NE(struct_text.find("\"left\""), std::string::npos);
    EXPECT_NE(struct_text.find("\"right\""), std::string::npos);
}

TEST_CASE(DumpMap) {
    std::map<int, int> value = {
        {1, 2},
        {3, 4}
    };
    auto text = clice::dump(value);
    EXPECT_NE(text.find("1"), std::string::npos);
    EXPECT_NE(text.find("4"), std::string::npos);
}

};  // TEST_SUITE(FormatSupport)

}  // namespace
}  // namespace clice::testing
