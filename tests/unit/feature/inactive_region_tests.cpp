#include "test/test.h"
#include "test/tester.h"
#include "feature/feature.h"

namespace clice::testing {

namespace {

ZEST_SUITE(inactive_regions, Tester) {
    feature::InactiveScan scan;

    void run(llvm::StringRef source) {
        add_main("main.cpp", source);
        ASSERT(compile("-std=c++17"));
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

        ASSERT(scan.regions.size() == 2u);
        auto content = unit->main_content();
        auto begin = scan.regions[0];
        auto end = scan.regions[1];
        EXPECT(content.substr(begin, end - begin) == "int dead();\n");
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

        ASSERT(scan.regions.size() == 2u);
        auto content = unit->main_content();
        EXPECT(content.substr(scan.regions[0], scan.regions[1] - scan.regions[0]) ==
               "int dead();\n");
    }

    ZEST_CASE(IncludeGuardStaysActive) {
        run(R"cpp(
#ifndef GUARD_H
int active();
#endif
)cpp");

        EXPECT(scan.regions.size() == 0u);
    }

    ZEST_CASE(IfndefDefinedMacro) {
        run(R"cpp(
#define TAKEN 1
#ifndef TAKEN
int dead();
#endif
)cpp");

        ASSERT(scan.regions.size() == 2u);
        auto content = unit->main_content();
        EXPECT(content.substr(scan.regions[0], scan.regions[1] - scan.regions[0]) ==
               "int dead();\n");
    }

    ZEST_CASE(IfdefUndefinedMacro) {
        run(R"cpp(
#ifdef MISSING
int dead();
#endif
int b();
)cpp");

        ASSERT(scan.regions.size() == 2u);
        auto content = unit->main_content();
        EXPECT(content.substr(scan.regions[0], scan.regions[1] - scan.regions[0]) ==
               "int dead();\n");
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

        ASSERT(scan.regions.size() == 2u);
        auto content = unit->main_content();
        EXPECT(content.substr(scan.regions[0], scan.regions[1] - scan.regions[0]) ==
               "int dead();\n#ifdef INNER\nint deeper();\n#endif\nint tail();\n");
    }

};  // ZEST_SUITE(inactive_regions)

}  // namespace

}  // namespace clice::testing
