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

ZEST_SUITE(FormatSupport) {

ZEST_CASE(FormatLLVMStringRef) {
    llvm::StringRef value = "hello";
    ZEXPECT(std::format("{}", value) == "hello");
    ZEXPECT(std::format("{}", llvm::StringLiteral("hello")) == "hello");
}

ZEST_CASE(FormatEnumAndStruct) {
    auto enum_text = std::format("{}", ExampleEnum::Alpha);
    ZEXPECT(!enum_text.empty());

    auto struct_text = clice::dump(ExampleStruct{1, 2});
    ZEXPECT(struct_text.find("\"left\"") != std::string::npos);
    ZEXPECT(struct_text.find("\"right\"") != std::string::npos);
}

ZEST_CASE(DumpMap) {
    std::map<int, int> value = {
        {1, 2},
        {3, 4}
    };
    auto text = clice::dump(value);
    ZEXPECT(text.find("1") != std::string::npos);
    ZEXPECT(text.find("4") != std::string::npos);
}

};  // ZEST_SUITE(FormatSupport)

}  // namespace
}  // namespace clice::testing
