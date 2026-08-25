#include "test/test.h"
#include "support/filesystem.h"

namespace clice::testing {

namespace {

TEST_SUITE(Filesystem) {

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

TEST_CASE(NormalizeDirectoryEquivalence) {
    // The working directory is an identity field: it keys the toolchain query
    // cache, joins the command hash and takes part in CompilationInfo dedup, so
    // every spelling of one directory must normalize to one string.
    auto norm = [](llvm::StringRef p) {
        return path::normalize_directory(p);
    };

    auto base = norm("/a/b");
    EXPECT_EQ(norm("/a/b/"), base);
    EXPECT_EQ(norm("/a/./b"), base);
    EXPECT_EQ(norm("/a/b/."), base);
    EXPECT_EQ(norm("/a/b/./"), base);
    EXPECT_EQ(norm("/a//b"), base);
    EXPECT_EQ(norm("/a/b//"), base);

    // Relative spellings normalize too; nothing here resolves against a cwd.
    EXPECT_EQ(norm("a/b/"), norm("a/./b"));
}

TEST_CASE(NormalizeDirectoryKeepsDotDot) {
    // ".." is left as written: collapsing it lexically merges distinct
    // directories whenever the skipped component is a symlink. The inequality
    // alone would also hold for a realpath-resolving implementation, so pin the
    // component itself.
    EXPECT_TRUE(llvm::StringRef(path::normalize_directory("/a/x/../b")).contains(".."));
}

TEST_CASE(NormalizeDirectoryEdges) {
    // Stripping the trailing separator must stop at the root, which would
    // otherwise normalize to the empty string and alias "no directory".
    EXPECT_EQ(path::normalize_directory("/"), "/");
    EXPECT_EQ(path::normalize_directory("//"), "/");
    EXPECT_EQ(path::normalize_directory(""), "");

    // A bare "." collapses to the empty spelling, which is the same directory
    // it denotes: empty tells the driver probe to inherit clice's cwd, and an
    // unresolved "." resolves there too. A compile database is specified to
    // carry absolute directories, so this only arises for a degenerate one.
    EXPECT_EQ(path::normalize_directory("."), "");
    EXPECT_EQ(path::normalize_directory("./"), "");
}

};  // TEST_SUITE(Filesystem)

}  // namespace

}  // namespace clice::testing
