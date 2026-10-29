module clice;

import :support.format;
import :unittests.utils;

namespace clice::testing {
namespace {

enum class ExampleEnum : unsigned char {
    Alpha = 0,
    Beta = 1,
};

ZEST_SUITE(FormatSupport) {

ZEST_CASE(FormatLLVMStringRef) {
    llvm::StringRef value = "hello";
    ZEXPECT(std::format("{}", value) == "hello");
    ZEXPECT(std::format("{}", llvm::StringLiteral("hello")) == "hello");
}

ZEST_CASE(FormatEnum) {
    ZEXPECT(std::format("{}", ExampleEnum::Alpha) == "Alpha");
}

};  // ZEST_SUITE(FormatSupport)

}  // namespace
}  // namespace clice::testing
