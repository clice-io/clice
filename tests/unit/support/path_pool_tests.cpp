#include "test/test.h"
#include "support/filesystem.h"
#include "support/path_pool.h"

namespace clice::testing {

namespace {

TEST_SUITE(PathPool) {

TEST_CASE(CanonicalSpelling) {
    // The rewrite itself is platform-independent and testable anywhere;
    // only its application is Windows-gated.
    auto canon = [](std::string s) {
        path::make_canonical(llvm::MutableArrayRef(s.data(), s.size()));
        return s;
    };
    EXPECT_EQ(canon(R"(D:\ws\x.h)"), "d:/ws/x.h");
    EXPECT_EQ(canon("d:/ws/x.h"), "d:/ws/x.h");
    EXPECT_EQ(canon("/usr/X.h"), "/usr/X.h");

    EXPECT_TRUE(path::needs_canonical(R"(a\b)"));
    EXPECT_TRUE(path::needs_canonical("C:/x.h"));
    EXPECT_FALSE(path::needs_canonical("c:/x.h"));
    EXPECT_FALSE(path::needs_canonical("/usr/x.h"));
}

#ifdef _WIN32
TEST_CASE(WindowsSpellingsCollapse) {
    // VS Code sends lowercase drive URIs while the CDB and clang report
    // uppercase; on Windows every spelling of one file interns to one ID
    // and resolves to the client-facing form, or every CDB lookup misses
    // and compiles fall back to guessed commands.
    PathPool pool;
    EXPECT_EQ(pool.intern("c:/a/b.h"), pool.intern(R"(C:\a\b.h)"));
    EXPECT_EQ(pool.resolve(pool.intern("C:/a/b.h")), "c:/a/b.h");
    EXPECT_EQ(pool.find(R"(c:\a\b.h)"), pool.find("C:/a/b.h"));
}
#else
TEST_CASE(PosixBytesPreserved) {
    // '\' and "C:" are ordinary filename characters on POSIX; identity is
    // the raw bytes and the Windows rewrite must not touch them.
    PathPool pool;
    EXPECT_NE(pool.intern(R"(a\b)"), pool.intern("a/b"));
    EXPECT_NE(pool.intern("C:/x.h"), pool.intern("c:/x.h"));
    EXPECT_NE(pool.intern("/c/x.h"), pool.intern("/C/x.h"));
    EXPECT_EQ(pool.resolve(pool.intern(R"(a\b)")), R"(a\b)");
}
#endif

};  // TEST_SUITE(PathPool)

}  // namespace

}  // namespace clice::testing
