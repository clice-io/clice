#include "test/test.h"
#include "syntax/include_resolver.h"
#include "syntax/scan.h"

#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"

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

/// RAII helper for a temporary directory tree.
struct TempDir {
    llvm::SmallString<128> root;

    TempDir() {
        llvm::sys::fs::createUniqueDirectory("clice-test", root);
    }

    ~TempDir() {
        llvm::sys::fs::remove_directories(root);
    }

    std::string path(llvm::StringRef relative) {
        llvm::SmallString<256> result(root);
        llvm::sys::path::append(result, relative);
        return std::string(result);
    }

    void mkdir(llvm::StringRef relative) {
        auto p = path(relative);
        llvm::sys::fs::create_directories(p);
    }

    void touch(llvm::StringRef relative, llvm::StringRef content = "") {
        auto p = path(relative);
        auto dir = llvm::sys::path::parent_path(p);
        llvm::sys::fs::create_directories(dir);
        std::error_code ec;
        llvm::raw_fd_ostream out(p, ec);
        if(!ec) {
            out << content;
        }
    }
};

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

};  // TEST_SUITE(IncludeResolver)

}  // namespace
}  // namespace clice::testing
