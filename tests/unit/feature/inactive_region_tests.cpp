module;

#include "modules/prelude.h"

module clice;

import :feature.feature;
import :tests.unit.test.test;
import :tests.unit.test.tester;

namespace clice::testing {

namespace {

ZEST_SUITE(inactive_regions, Tester) {

feature::InactiveScan scan;

void run(llvm::StringRef source) {
    add_main("main.cpp", source);
    ZASSERT(compile("-std=c++17"));
    scan = feature::inactive_regions(*unit);
}

ZEST_CASE(FalseBranch) {
    run(R"cpp(
int a();
#if 0
int dead();
#endif
int b();
)cpp");

    ZASSERT(scan.regions.size() == 2u);
    auto content = unit->main_content();
    auto begin = scan.regions[0];
    auto end = scan.regions[1];
    ZEXPECT(content.substr(begin, end - begin) == "int dead();\n");
}

ZEST_CASE(ElseBranch) {
    run(R"cpp(
#define USE_A 1
#if USE_A
int active();
#else
int dead();
#endif
)cpp");

    ZASSERT(scan.regions.size() == 2u);
    auto content = unit->main_content();
    ZEXPECT(content.substr(scan.regions[0], scan.regions[1] - scan.regions[0]) == "int dead();\n");
}

ZEST_CASE(IncludeGuardStaysActive) {
    run(R"cpp(
#ifndef GUARD_H
int active();
#endif
)cpp");

    ZEXPECT(scan.regions.size() == 0u);
}

ZEST_CASE(IfndefDefinedMacro) {
    run(R"cpp(
#define TAKEN 1
#ifndef TAKEN
int dead();
#endif
)cpp");

    ZASSERT(scan.regions.size() == 2u);
    auto content = unit->main_content();
    ZEXPECT(content.substr(scan.regions[0], scan.regions[1] - scan.regions[0]) == "int dead();\n");
}

ZEST_CASE(IfdefUndefinedMacro) {
    run(R"cpp(
#ifdef MISSING
int dead();
#endif
int b();
)cpp");

    ZASSERT(scan.regions.size() == 2u);
    auto content = unit->main_content();
    ZEXPECT(content.substr(scan.regions[0], scan.regions[1] - scan.regions[0]) == "int dead();\n");
}

ZEST_CASE(NestedRegionsMerge) {
    run(R"cpp(
#if 0
int dead();
#ifdef INNER
int deeper();
#endif
int tail();
#endif
int b();
)cpp");

    ZASSERT(scan.regions.size() == 2u);
    auto content = unit->main_content();
    ZEXPECT(content.substr(scan.regions[0], scan.regions[1] - scan.regions[0]) ==
            "int dead();\n#ifdef INNER\nint deeper();\n#endif\nint tail();\n");
}

};  // ZEST_SUITE(inactive_regions)

}  // namespace

}  // namespace clice::testing
