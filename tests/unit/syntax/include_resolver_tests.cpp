#include "test/temp_dir.h"
#include "test/test.h"
#include "syntax/include_resolver.h"
#include "syntax/scan.h"

namespace clice::testing {
namespace {

// ============================================================================
// scan() — is_angled and is_include_next fields
// ============================================================================

TEST_SUITE(IncludeResolver) {

TEST_CASE(ScanAngledVsQuoted) {
    auto result = scan(R"(
#include <vector>
#include "local.h"
)");

    ASSERT_EQ(result.includes.size(), 2u);
    EXPECT_EQ(result.includes[0].path, "vector");
    EXPECT_TRUE(result.includes[0].is_angled);
    EXPECT_FALSE(result.includes[0].is_include_next);

    EXPECT_EQ(result.includes[1].path, "local.h");
    EXPECT_FALSE(result.includes[1].is_angled);
    EXPECT_FALSE(result.includes[1].is_include_next);
}

TEST_CASE(ScanIncludeNext) {
    auto result = scan(R"(
#include_next <stdlib.h>
)");

    ASSERT_EQ(result.includes.size(), 1u);
    EXPECT_EQ(result.includes[0].path, "stdlib.h");
    EXPECT_TRUE(result.includes[0].is_angled);
    EXPECT_TRUE(result.includes[0].is_include_next);
}

TEST_CASE(ScanMixedDirectives) {
    auto result = scan(R"(
#include <system.h>
#include "quoted.h"
#ifdef FOO
#include <conditional_angled.h>
#include "conditional_quoted.h"
#endif
#include_next "next_quoted.h"
)");

    ASSERT_EQ(result.includes.size(), 5u);

    EXPECT_TRUE(result.includes[0].is_angled);
    EXPECT_FALSE(result.includes[0].conditional);

    EXPECT_FALSE(result.includes[1].is_angled);
    EXPECT_FALSE(result.includes[1].conditional);

    EXPECT_TRUE(result.includes[2].is_angled);
    EXPECT_TRUE(result.includes[2].conditional);

    EXPECT_FALSE(result.includes[3].is_angled);
    EXPECT_TRUE(result.includes[3].conditional);

    EXPECT_FALSE(result.includes[4].is_angled);
    EXPECT_TRUE(result.includes[4].is_include_next);
}

// ============================================================================
// resolve_include() — tests with real filesystem
// ============================================================================

TEST_CASE(ResolveAbsolutePath) {
    TempDir tmp;
    tmp.touch("header.h");

    auto abs_path = tmp.path("header.h");
    SearchConfig config;
    DirListingCache dir_cache;

    auto result = resolve_include(abs_path, false, "", false, 0, config, dir_cache);

    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(llvm::sys::fs::equivalent(result->path, abs_path));
}

TEST_CASE(ResolveQuotedIncludeFromIncluderDir) {
    TempDir tmp;
    tmp.touch("src/main.cpp");
    tmp.touch("src/local.h");

    SearchConfig config;
    config.dirs.push_back({tmp.path("include")});
    config.angled_start_idx = 0;

    DirListingCache dir_cache;

    auto result = resolve_include("local.h", false, tmp.path("src"), false, 0, config, dir_cache);

    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(llvm::sys::fs::equivalent(result->path, tmp.path("src/local.h")));
}

TEST_CASE(ResolveAngledIncludeFromSearchDirs) {
    TempDir tmp;
    tmp.touch("include/sys/types.h");

    SearchConfig config;
    config.dirs.push_back({tmp.path("include")});
    config.angled_start_idx = 0;

    DirListingCache dir_cache;

    auto result = resolve_include("sys/types.h", true, "", false, 0, config, dir_cache);

    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(llvm::sys::fs::equivalent(result->path, tmp.path("include/sys/types.h")));
}

TEST_CASE(ResolveAngledSkipsQuotedDirs) {
    TempDir tmp;
    tmp.touch("quoted/header.h", "// quoted");
    tmp.touch("angled/header.h", "// angled");

    SearchConfig config;
    config.dirs.push_back({tmp.path("quoted")});  // index 0 — quoted only
    config.dirs.push_back({tmp.path("angled")});  // index 1 — angled starts
    config.angled_start_idx = 1;

    DirListingCache dir_cache;

    auto result = resolve_include("header.h", true, "", false, 0, config, dir_cache);

    ASSERT_TRUE(result.has_value());
    // Angled include should skip quoted dir and find in angled dir.
    EXPECT_TRUE(llvm::sys::fs::equivalent(result->path, tmp.path("angled/header.h")));
    EXPECT_EQ(result->found_dir_idx, 1u);
}

TEST_CASE(ResolveIncludeNext) {
    TempDir tmp;
    tmp.touch("dir1/stdlib.h", "// first");
    tmp.touch("dir2/stdlib.h", "// second");

    SearchConfig config;
    config.dirs.push_back({tmp.path("dir1")});  // index 0
    config.dirs.push_back({tmp.path("dir2")});  // index 1
    config.angled_start_idx = 0;

    DirListingCache dir_cache;

    // Simulate #include_next from a file found at dir index 0.
    auto result = resolve_include("stdlib.h", true, "", true, 0, config, dir_cache);

    ASSERT_TRUE(result.has_value());
    // Should skip dir1 (found_dir_idx=0) and find in dir2.
    EXPECT_TRUE(llvm::sys::fs::equivalent(result->path, tmp.path("dir2/stdlib.h")));
    EXPECT_EQ(result->found_dir_idx, 1u);
}

TEST_CASE(ResolveNotFound) {
    TempDir tmp;

    SearchConfig config;
    config.dirs.push_back({tmp.path("include")});
    config.angled_start_idx = 0;

    DirListingCache dir_cache;

    auto result =
        resolve_include("nonexistent.h", false, tmp.path("src"), false, 0, config, dir_cache);

    EXPECT_FALSE(result.has_value());
}

TEST_CASE(ResolveStatCacheHits) {
    TempDir tmp;
    tmp.touch("include/cached.h");

    SearchConfig config;
    config.dirs.push_back({tmp.path("include")});
    config.angled_start_idx = 0;

    DirListingCache dir_cache;

    // First resolution — populates cache.
    auto result1 = resolve_include("cached.h", true, "", false, 0, config, dir_cache);

    ASSERT_TRUE(result1.has_value());

    // Second resolution — should use cache (no filesystem I/O needed).
    auto result2 = resolve_include("cached.h", true, "", false, 0, config, dir_cache);

    ASSERT_TRUE(result2.has_value());
    EXPECT_EQ(result1->path, result2->path);
}

TEST_CASE(ResolveQuotedFallsBackToSearchDirs) {
    TempDir tmp;
    // Header not in includer dir, but in search dir.
    tmp.touch("include/fallback.h");

    SearchConfig config;
    config.dirs.push_back({tmp.path("include")});
    config.angled_start_idx = 0;

    DirListingCache dir_cache;

    auto result =
        resolve_include("fallback.h", false, tmp.path("src"), false, 0, config, dir_cache);

    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(llvm::sys::fs::equivalent(result->path, tmp.path("include/fallback.h")));
}

// ============================================================================
// Three-tier search directory tests
// ============================================================================

TEST_CASE(AngledSkipsQuotedDirs) {
    TempDir tmp;
    tmp.touch("iquote/header.h", "// iquote");
    tmp.touch("idir/header.h", "// I dir");
    tmp.touch("sys/header.h", "// system");

    // Layout: [iquote | idir | sys]
    SearchConfig config;
    config.dirs.push_back({tmp.path("iquote")});  // 0: Quoted
    config.dirs.push_back({tmp.path("idir")});    // 1: Angled
    config.dirs.push_back({tmp.path("sys")});     // 2: System
    config.angled_start_idx = 1;
    config.system_start_idx = 2;

    DirListingCache dir_cache;

    // <header.h> should skip iquote, find in idir (Angled before System).
    auto result = resolve_include("header.h", true, "", false, 0, config, dir_cache);
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(llvm::sys::fs::equivalent(result->path, tmp.path("idir/header.h")));
    EXPECT_EQ(result->found_dir_idx, 1u);
}

TEST_CASE(AngledMissesQuotedOnly) {
    TempDir tmp;
    tmp.touch("iquote/only_here.h");

    // Layout: [iquote | (no angled) | (no system)]
    SearchConfig config;
    config.dirs.push_back({tmp.path("iquote")});
    config.angled_start_idx = 1;
    config.system_start_idx = 1;

    DirListingCache dir_cache;

    // <only_here.h> should NOT find it — only in quoted dir.
    auto result = resolve_include("only_here.h", true, "", false, 0, config, dir_cache);
    EXPECT_FALSE(result.has_value());
}

TEST_CASE(QuotedSearchesAllDirs) {
    TempDir tmp;
    tmp.touch("sys/deep.h", "// system");

    // Layout: [iquote | idir | sys]
    SearchConfig config;
    config.dirs.push_back({tmp.path("iquote")});
    config.dirs.push_back({tmp.path("idir")});
    config.dirs.push_back({tmp.path("sys")});
    config.angled_start_idx = 1;
    config.system_start_idx = 2;

    DirListingCache dir_cache;

    // "deep.h" is only in system dir, but quoted search goes through all.
    auto result = resolve_include("deep.h", false, "", false, 0, config, dir_cache);
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(llvm::sys::fs::equivalent(result->path, tmp.path("sys/deep.h")));
}

TEST_CASE(AngledBeforeSystem) {
    TempDir tmp;
    tmp.touch("idir/priority.h", "// angled");
    tmp.touch("sys/priority.h", "// system");

    SearchConfig config;
    config.dirs.push_back({tmp.path("idir")});  // 0: Angled
    config.dirs.push_back({tmp.path("sys")});   // 1: System
    config.angled_start_idx = 0;
    config.system_start_idx = 1;

    DirListingCache dir_cache;

    // <priority.h> should find in Angled (index 0) before System (index 1).
    auto result = resolve_include("priority.h", true, "", false, 0, config, dir_cache);
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(llvm::sys::fs::equivalent(result->path, tmp.path("idir/priority.h")));
    EXPECT_EQ(result->found_dir_idx, 0u);
}

TEST_CASE(AfterSearchedLast) {
    TempDir tmp;
    tmp.touch("after/fallback.h", "// after");

    // Layout: [| /angled | /sys | /after]
    SearchConfig config;
    config.dirs.push_back({tmp.path("angled")});
    config.dirs.push_back({tmp.path("sys")});
    config.dirs.push_back({tmp.path("after")});
    config.angled_start_idx = 0;
    config.system_start_idx = 1;
    config.after_start_idx = 2;

    DirListingCache dir_cache;

    // <fallback.h> not in angled or sys, found in after.
    auto result = resolve_include("fallback.h", true, "", false, 0, config, dir_cache);
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(llvm::sys::fs::equivalent(result->path, tmp.path("after/fallback.h")));
    EXPECT_EQ(result->found_dir_idx, 2u);
}

TEST_CASE(IncludeNextPropagatesIdx) {
    TempDir tmp;
    tmp.touch("dir0/limits.h", "// local");
    tmp.touch("dir1/limits.h", "// system1");
    tmp.touch("dir2/limits.h", "// system2");

    SearchConfig config;
    config.dirs.push_back({tmp.path("dir0")});
    config.dirs.push_back({tmp.path("dir1")});
    config.dirs.push_back({tmp.path("dir2")});
    config.angled_start_idx = 0;
    config.system_start_idx = 1;

    DirListingCache dir_cache;

    // File found at dir1 (index 1) does #include_next <limits.h>
    auto result = resolve_include("limits.h", true, "", true, 1, config, dir_cache);
    ASSERT_TRUE(result.has_value());
    // Should skip dirs 0-1, find in dir2.
    EXPECT_TRUE(llvm::sys::fs::equivalent(result->path, tmp.path("dir2/limits.h")));
    EXPECT_EQ(result->found_dir_idx, 2u);
}

// TODO: add tests for:
// - #include_next crossing segment boundaries (angled→system)
// - #include_next at last search dir (should return nullopt)
// - Relative paths with .. components ("../sibling/header.h")
// - ResolvedSearchConfig overload (the production hot path)

};  // TEST_SUITE(IncludeResolver)

}  // namespace
}  // namespace clice::testing
