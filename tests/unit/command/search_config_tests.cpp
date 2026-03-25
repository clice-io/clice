#include "test/temp_dir.h"
#include "test/test.h"
#include "command/search_config.h"

namespace clice::testing {

namespace {

TEST_SUITE(ExtractSearchConfig) {

TEST_CASE(ReordersDirectoryGroups) {
    // TempDir gives cross-platform absolute paths (drive letter on Windows).
    TempDir tmp;
    std::vector<const char*> args = {"clang++",
                                     "-internal-isystem",
                                     tmp.c_path("stdlib"),
                                     "-internal-isystem",
                                     tmp.c_path("clang"),
                                     "-internal-externc-isystem",
                                     tmp.c_path("sysroot"),
                                     "-I",
                                     tmp.c_path("user"),
                                     "-iquote",
                                     tmp.c_path("quoted"),
                                     "main.cpp"};
    auto config = extract_search_config(args, tmp.root.str());

    // Expected order: [quoted | user | stdlib, clang, sysroot]
    ASSERT_EQ(config.dirs.size(), 5u);
    EXPECT_EQ(config.angled_start_idx, 1u);
    EXPECT_EQ(config.system_start_idx, 2u);

    EXPECT_EQ(config.dirs[0].path, tmp.path("quoted"));
    EXPECT_EQ(config.dirs[1].path, tmp.path("user"));
    EXPECT_EQ(config.dirs[2].path, tmp.path("stdlib"));
    EXPECT_EQ(config.dirs[3].path, tmp.path("clang"));
    EXPECT_EQ(config.dirs[4].path, tmp.path("sysroot"));
}

TEST_CASE(PreservesWithinGroupOrder) {
    TempDir tmp;
    std::vector<const char*> args = {"clang++",
                                     "-I",
                                     tmp.c_path("b"),
                                     "-I",
                                     tmp.c_path("a"),
                                     "-isystem",
                                     tmp.c_path("s2"),
                                     "-isystem",
                                     tmp.c_path("s1"),
                                     "main.cpp"};
    auto config = extract_search_config(args, tmp.root.str());

    ASSERT_EQ(config.dirs.size(), 4u);
    EXPECT_EQ(config.angled_start_idx, 0u);
    EXPECT_EQ(config.system_start_idx, 2u);
    EXPECT_EQ(config.dirs[0].path, tmp.path("b"));
    EXPECT_EQ(config.dirs[1].path, tmp.path("a"));
    EXPECT_EQ(config.dirs[2].path, tmp.path("s2"));
    EXPECT_EQ(config.dirs[3].path, tmp.path("s1"));
}

TEST_CASE(DeduplicatesAngledSystem) {
    TempDir tmp;
    std::vector<const char*> args = {"clang++",
                                     "-I",
                                     tmp.c_path("shared"),
                                     "-internal-isystem",
                                     tmp.c_path("shared"),
                                     "-internal-isystem",
                                     tmp.c_path("only_sys"),
                                     "main.cpp"};
    auto config = extract_search_config(args, tmp.root.str());

    // /shared in both Angled and System → keep Angled copy.
    ASSERT_EQ(config.dirs.size(), 2u);
    EXPECT_EQ(config.angled_start_idx, 0u);
    EXPECT_EQ(config.system_start_idx, 1u);
    EXPECT_EQ(config.dirs[0].path, tmp.path("shared"));
    EXPECT_EQ(config.dirs[1].path, tmp.path("only_sys"));
}

TEST_CASE(QuotedAngledSamePathKeptInBoth) {
    // clang's RemoveDuplicates starts from NumQuoted, so a path in both
    // Quoted (-iquote) and Angled (-I) must be kept in both segments.
    // This matters for #include <...> lookup and #include_next correctness.
    TempDir tmp;
    std::vector<const char*> args = {"clang++",
                                     "-iquote",
                                     tmp.c_path("shared"),
                                     "-I",
                                     tmp.c_path("shared"),
                                     "-I",
                                     tmp.c_path("other"),
                                     "main.cpp"};
    auto config = extract_search_config(args, tmp.root.str());

    // "shared" must appear in both Quoted and Angled segments.
    ASSERT_EQ(config.dirs.size(), 3u);
    EXPECT_EQ(config.angled_start_idx, 1u);
    EXPECT_EQ(config.dirs[0].path, tmp.path("shared"));  // Quoted
    EXPECT_EQ(config.dirs[1].path, tmp.path("shared"));  // Angled (not deduped)
    EXPECT_EQ(config.dirs[2].path, tmp.path("other"));
}

TEST_CASE(DeduplicateAdjustsIndices) {
    TempDir tmp;
    std::vector<const char*> args = {"clang++",
                                     "-iquote",
                                     tmp.c_path("q"),
                                     "-I",
                                     tmp.c_path("dup"),
                                     "-I",
                                     tmp.c_path("a2"),
                                     "-isystem",
                                     tmp.c_path("dup"),
                                     "-isystem",
                                     tmp.c_path("s"),
                                     "main.cpp"};
    auto config = extract_search_config(args, tmp.root.str());

    // Before dedup: [q | dup, a2 | dup, s] angled=1, system=3
    // dup in system removed. system_start_idx stays 3.
    ASSERT_EQ(config.dirs.size(), 4u);
    EXPECT_EQ(config.angled_start_idx, 1u);
    EXPECT_EQ(config.system_start_idx, 3u);
    EXPECT_EQ(config.dirs[0].path, tmp.path("q"));
    EXPECT_EQ(config.dirs[1].path, tmp.path("dup"));
    EXPECT_EQ(config.dirs[2].path, tmp.path("a2"));
    EXPECT_EQ(config.dirs[3].path, tmp.path("s"));
}

TEST_CASE(PrefixIncludeOptions) {
    TempDir tmp;
    // -iprefix sets a prefix; -iwithprefixbefore/iwithprefix append to it.
    // The trailing separator in the prefix path ensures correct concatenation.
    auto prefix12 = tmp.path("gcc/12/");
    auto prefix13 = tmp.path("gcc/13/");
    std::vector<const char*> args = {"clang++",
                                     "-iprefix",
                                     prefix12.c_str(),
                                     "-iwithprefixbefore",
                                     "include",
                                     "-iwithprefix",
                                     "lib",
                                     "-iprefix",
                                     prefix13.c_str(),
                                     "-iwithprefix",
                                     "include",
                                     "main.cpp"};
    auto config = extract_search_config(args, tmp.root.str());

    // -iwithprefixbefore → Angled, -iwithprefix → After
    ASSERT_EQ(config.dirs.size(), 3u);
    EXPECT_EQ(config.angled_start_idx, 0u);
    EXPECT_EQ(config.system_start_idx, 1u);
    EXPECT_EQ(config.after_start_idx, 1u);
    EXPECT_EQ(config.dirs[0].path, tmp.path("gcc/12/include"));
    EXPECT_EQ(config.dirs[1].path, tmp.path("gcc/12/lib"));
    EXPECT_EQ(config.dirs[2].path, tmp.path("gcc/13/include"));
}

TEST_CASE(DirafterGroup) {
    TempDir tmp;
    std::vector<const char*> args = {"clang++",
                                     "-I",
                                     tmp.c_path("user"),
                                     "-isystem",
                                     tmp.c_path("sys"),
                                     "-idirafter",
                                     tmp.c_path("fallback"),
                                     "main.cpp"};
    auto config = extract_search_config(args, tmp.root.str());

    ASSERT_EQ(config.dirs.size(), 3u);
    EXPECT_EQ(config.angled_start_idx, 0u);
    EXPECT_EQ(config.system_start_idx, 1u);
    EXPECT_EQ(config.after_start_idx, 2u);
    EXPECT_EQ(config.dirs[0].path, tmp.path("user"));
    EXPECT_EQ(config.dirs[1].path, tmp.path("sys"));
    EXPECT_EQ(config.dirs[2].path, tmp.path("fallback"));
}

TEST_CASE(DirafterDeduplication) {
    TempDir tmp;
    std::vector<const char*> args = {"clang++",
                                     "-I",
                                     tmp.c_path("shared"),
                                     "-idirafter",
                                     tmp.c_path("shared"),
                                     "-idirafter",
                                     tmp.c_path("extra"),
                                     "main.cpp"};
    auto config = extract_search_config(args, tmp.root.str());

    ASSERT_EQ(config.dirs.size(), 2u);
    EXPECT_EQ(config.angled_start_idx, 0u);
    EXPECT_EQ(config.after_start_idx, 1u);
    EXPECT_EQ(config.dirs[0].path, tmp.path("shared"));
    EXPECT_EQ(config.dirs[1].path, tmp.path("extra"));
}

};  // TEST_SUITE(ExtractSearchConfig)

}  // namespace

}  // namespace clice::testing
