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
    // commands. Lowercase wins: clients key documents by vscode-uri's
    // lowercase-drive form, so resolved strings must emit as-is.
    PathPool pool;
    EXPECT_EQ(pool.intern("c:/a/b.h"), pool.intern(R"(C:\a\b.h)"));
    EXPECT_EQ(pool.resolve(pool.intern("C:/a/b.h")), "c:/a/b.h");
    EXPECT_EQ(pool.find(R"(c:\a\b.h)"), pool.find("C:/a/b.h"));
}

TEST_CASE(CanonicalizeHelper) {
    EXPECT_EQ(canonicalize_path(R"(D:\ws\x.h)"), "d:/ws/x.h");
    EXPECT_EQ(canonicalize_path("d:/ws/x.h"), "d:/ws/x.h");
    EXPECT_EQ(canonicalize_path("/usr/X.h"), "/usr/X.h");
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
