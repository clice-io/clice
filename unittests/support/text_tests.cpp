module clice;

import :support.text;
import :unittests.utils;

namespace clice::testing {
namespace {

ZEST_SUITE(Text) {

ZEST_CASE(LineBeginSkipsBom) {
    llvm::StringRef text = "\xEF\xBB\xBFint a;\nint b;\n";
    ZEXPECT(line_begin(text, 5) == 3u);
    ZEXPECT(line_begin(text, 3) == 3u);
    ZEXPECT(line_begin(text, 9) == 3u);
    ZEXPECT(line_begin(text, 12) == 10u);
    ZEXPECT(begins_line(text, 3));
    // Never past the offset: one inside the mark begins its line.
    ZEXPECT(line_begin(text, 1) == 1u);
    ZEXPECT(line_begin("int a;\n", 4) == 0u);
}

ZEST_CASE(BomOnlyAtStart) {
    ZEXPECT(leading_bom("\xEF\xBB\xBFx") == utf8_bom);
    ZEXPECT(leading_bom("x\xEF\xBB\xBF").empty());
    ZEXPECT(without_bom("\xEF\xBB\xBFx") == "x");
    ZEXPECT(without_bom("\xEF\xBB") == "\xEF\xBB");
}

};  // ZEST_SUITE(Text)

}  // namespace
}  // namespace clice::testing
