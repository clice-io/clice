module clice;

import :feature.hover;
import :unittests.utils;
import :unittests.utils.tester;

namespace clice::testing {

namespace {

ZEST_SUITE(hover, Tester) {

/// No case holds a byte outside UTF-8: its sources and the protocol are UTF-8.
ZEST_CASE(expansion_cut_outside_utf8) {
    // A byte outside UTF-8 counts as a character: a line of them is cut
    // past 4096.
    add_focus("main.cpp",
              "#define TEXT R\"(" + std::string(5000, '\xA0') + ")\"\nconst char* text = §TEXT;\n");
    ZASSERT(compile());
    auto info = feature::hover_info(*unit, nameless_points()[0]);
    ZASSERT(info);
    ZEXPECT(info->expansion.size() > 4000);
}

};  // ZEST_SUITE(hover)

}  // namespace

}  // namespace clice::testing
