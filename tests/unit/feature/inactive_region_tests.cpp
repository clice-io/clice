#include "test/test.h"
#include "test/tester.h"
#include "feature/inactive_regions.h"

namespace clice::testing {

namespace {

TEST_SUITE(inactive_regions, Tester) {

feature::InactiveScan scan;

void run(llvm::StringRef source) {
    add_main("main.cpp", source);
    ASSERT_TRUE(compile("-std=c++17"));
    scan = feature::inactive_regions(*unit);
}

TEST_CASE(FalseBranch) {
    run(R"cpp(
int a();
#if 0
int dead();
#endif
int b();
)cpp");

    ASSERT_EQ(scan.regions.size(), 2u);
    auto content = unit->interested_content();
    auto begin = scan.regions[0];
    auto end = scan.regions[1];
    EXPECT_EQ(content.substr(begin, end - begin), "int dead();\n");
}

TEST_CASE(ElseBranch) {
    run(R"cpp(
#define USE_A 1
#if USE_A
int active();
#else
int dead();
#endif
)cpp");

    ASSERT_EQ(scan.regions.size(), 2u);
    auto content = unit->interested_content();
    EXPECT_EQ(content.substr(scan.regions[0], scan.regions[1] - scan.regions[0]), "int dead();\n");
}

TEST_CASE(IncludeGuardStaysActive) {
    run(R"cpp(
#ifndef GUARD_H
int active();
#endif
)cpp");

    EXPECT_EQ(scan.regions.size(), 0u);
}

TEST_CASE(IfndefDefinedMacro) {
    run(R"cpp(
#define TAKEN 1
#ifndef TAKEN
int dead();
#endif
)cpp");

    ASSERT_EQ(scan.regions.size(), 2u);
    auto content = unit->interested_content();
    EXPECT_EQ(content.substr(scan.regions[0], scan.regions[1] - scan.regions[0]), "int dead();\n");
}

TEST_CASE(IfdefUndefinedMacro) {
    run(R"cpp(
#ifdef MISSING
int dead();
#endif
int b();
)cpp");

    ASSERT_EQ(scan.regions.size(), 2u);
    auto content = unit->interested_content();
    EXPECT_EQ(content.substr(scan.regions[0], scan.regions[1] - scan.regions[0]), "int dead();\n");
}

};  // TEST_SUITE(inactive_regions)

}  // namespace

}  // namespace clice::testing
