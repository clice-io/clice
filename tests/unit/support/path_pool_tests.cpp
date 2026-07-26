#include "test/test.h"
#include "support/path_pool.h"

namespace clice::testing {

namespace {

TEST_SUITE(PathPool) {

TEST_CASE(SeparatorsNormalized) {
    PathPool pool;
    EXPECT_EQ(pool.intern(R"(C:\a\b.h)"), pool.intern("C:/a/b.h"));
}

TEST_CASE(DriveCaseNormalized) {
    // VS Code sends lowercase drive URIs while the CDB and clang report
    // uppercase; both spellings must intern to one ID or every CDB
    // lookup on Windows misses and compiles fall back to guessed
    // commands.
    PathPool pool;
    EXPECT_EQ(pool.intern("c:/a/b.h"), pool.intern(R"(C:\a\b.h)"));
    EXPECT_EQ(pool.resolve(pool.intern("c:/a/b.h")), "C:/a/b.h");
    EXPECT_EQ(pool.find(R"(c:\a\b.h)"), pool.find("C:/a/b.h"));
}

TEST_CASE(PosixCaseKept) {
    // Only the drive prefix is case-normalized; POSIX paths stay
    // case-sensitive.
    PathPool pool;
    EXPECT_NE(pool.intern("/c/x.h"), pool.intern("/C/x.h"));
}

};  // TEST_SUITE(PathPool)

}  // namespace

}  // namespace clice::testing
