#include "test/temp_dir.h"
#include "test/test.h"
#include "syntax/include_resolver.h"
#include "syntax/scan.h"

namespace clice::testing {
namespace {

// ============================================================================
// scan() — is_angled and is_include_next fields
// ============================================================================

ZEST_SUITE(IncludeResolver){

    ZEST_CASE(ScanAngledVsQuoted){auto result = scan_quick(R"(
#include <vector>
#include "local.h"
)");

ASSERT(result.includes.size() == 2u);
EXPECT(result.includes[0].path == "vector");
EXPECT(result.includes[0].is_angled);
EXPECT(!result.includes[0].is_include_next);

EXPECT(result.includes[1].path == "local.h");
EXPECT(!result.includes[1].is_angled);
EXPECT(!result.includes[1].is_include_next);

}  // namespace

ZEST_CASE(ScanIncludeNext) {
    auto result = scan_quick(R"(
#include_next <stdlib.h>
)");

    ASSERT(result.includes.size() == 1u);
    EXPECT(result.includes[0].path == "stdlib.h");
    EXPECT(result.includes[0].is_angled);
    EXPECT(result.includes[0].is_include_next);
}

ZEST_CASE(ScanMixedDirectives) {
    auto result = scan_quick(R"(
#include <system.h>
#include "quoted.h"
#ifdef FOO
#include <conditional_angled.h>
#include "conditional_quoted.h"
#endif
#include_next "next_quoted.h"
)");

    ASSERT(result.includes.size() == 5u);

    EXPECT(result.includes[0].is_angled);
    EXPECT(!result.includes[0].conditional);

    EXPECT(!result.includes[1].is_angled);
    EXPECT(!result.includes[1].conditional);

    EXPECT(result.includes[2].is_angled);
    EXPECT(result.includes[2].conditional);

    EXPECT(!result.includes[3].is_angled);
    EXPECT(result.includes[3].conditional);

    EXPECT(!result.includes[4].is_angled);
    EXPECT(result.includes[4].is_include_next);
}

// ============================================================================
// resolve_include() — tests with real filesystem
// ============================================================================

ZEST_CASE(ResolveAbsolutePath) {
    TempDir tmp;
    tmp.touch("header.h");

    auto abs_path = tmp.path("header.h");
    SearchConfig config;
    vfs::DirCache cache;
    vfs::Scope scope(cache);

    auto result = resolve_include(abs_path, false, "", false, 0, config, scope);

    ASSERT(result);
    EXPECT(llvm::sys::fs::equivalent(result->path, abs_path));
}

ZEST_CASE(ResolveQuotedIncludeFromIncluderDir) {
    TempDir tmp;
    tmp.touch("src/main.cpp");
    tmp.touch("src/local.h");

    SearchConfig config;
    config.dirs.push_back({tmp.path("include")});
    config.angled_start_idx = 0;

    vfs::DirCache cache;
    vfs::Scope scope(cache);

    auto result = resolve_include("local.h", false, tmp.path("src"), false, 0, config, scope);

    ASSERT(result);
    EXPECT(llvm::sys::fs::equivalent(result->path, tmp.path("src/local.h")));
}

ZEST_CASE(ResolveAngledIncludeFromSearchDirs) {
    TempDir tmp;
    tmp.touch("include/sys/types.h");

    SearchConfig config;
    config.dirs.push_back({tmp.path("include")});
    config.angled_start_idx = 0;

    vfs::DirCache cache;
    vfs::Scope scope(cache);

    auto result = resolve_include("sys/types.h", true, "", false, 0, config, scope);

    ASSERT(result);
    EXPECT(llvm::sys::fs::equivalent(result->path, tmp.path("include/sys/types.h")));
}

ZEST_CASE(ResolveAngledSkipsQuotedDirs) {
    TempDir tmp;
    tmp.touch("quoted/header.h", "// quoted");
    tmp.touch("angled/header.h", "// angled");

    SearchConfig config;
    config.dirs.push_back({tmp.path("quoted")});  // index 0 — quoted only
    config.dirs.push_back({tmp.path("angled")});  // index 1 — angled starts
    config.angled_start_idx = 1;

    vfs::DirCache cache;
    vfs::Scope scope(cache);

    auto result = resolve_include("header.h", true, "", false, 0, config, scope);

    ASSERT(result);
    // Angled include should skip quoted dir and find in angled dir.
    EXPECT(llvm::sys::fs::equivalent(result->path, tmp.path("angled/header.h")));
    EXPECT(result->found_dir_idx == 1u);
}

ZEST_CASE(ResolveIncludeNext) {
    TempDir tmp;
    tmp.touch("dir1/stdlib.h", "// first");
    tmp.touch("dir2/stdlib.h", "// second");

    SearchConfig config;
    config.dirs.push_back({tmp.path("dir1")});  // index 0
    config.dirs.push_back({tmp.path("dir2")});  // index 1
    config.angled_start_idx = 0;

    vfs::DirCache cache;
    vfs::Scope scope(cache);

    // Simulate #include_next from a file found at dir index 0.
    auto result = resolve_include("stdlib.h", true, "", true, 0, config, scope);

    ASSERT(result);
    // Should skip dir1 (found_dir_idx=0) and find in dir2.
    EXPECT(llvm::sys::fs::equivalent(result->path, tmp.path("dir2/stdlib.h")));
    EXPECT(result->found_dir_idx == 1u);
}

ZEST_CASE(ResolveNotFound) {
    TempDir tmp;

    SearchConfig config;
    config.dirs.push_back({tmp.path("include")});
    config.angled_start_idx = 0;

    vfs::DirCache cache;
    vfs::Scope scope(cache);

    auto result = resolve_include("nonexistent.h", false, tmp.path("src"), false, 0, config, scope);

    EXPECT(!result.has_value());
}

ZEST_CASE(ResolveStatCacheHits) {
    TempDir tmp;
    tmp.touch("include/cached.h");

    SearchConfig config;
    config.dirs.push_back({tmp.path("include")});
    config.angled_start_idx = 0;

    vfs::DirCache cache;
    vfs::Scope scope(cache);

    // First resolution — populates cache.
    auto result1 = resolve_include("cached.h", true, "", false, 0, config, scope);

    ASSERT(result1);

    // Second resolution — should use cache (no filesystem I/O needed).
    auto result2 = resolve_include("cached.h", true, "", false, 0, config, scope);

    ASSERT(result2);
    EXPECT(result1->path == result2->path);
}

ZEST_CASE(ResolveQuotedFallsBackToSearchDirs) {
    TempDir tmp;
    // Header not in includer dir, but in search dir.
    tmp.touch("include/fallback.h");

    SearchConfig config;
    config.dirs.push_back({tmp.path("include")});
    config.angled_start_idx = 0;

    vfs::DirCache cache;
    vfs::Scope scope(cache);

    auto result = resolve_include("fallback.h", false, tmp.path("src"), false, 0, config, scope);

    ASSERT(result);
    EXPECT(llvm::sys::fs::equivalent(result->path, tmp.path("include/fallback.h")));
}

// ============================================================================
// Three-tier search directory tests
// ============================================================================

ZEST_CASE(AngledSkipsQuotedDirs) {
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

    vfs::DirCache cache;
    vfs::Scope scope(cache);

    // <header.h> should skip iquote, find in idir (Angled before System).
    auto result = resolve_include("header.h", true, "", false, 0, config, scope);
    ASSERT(result);
    EXPECT(llvm::sys::fs::equivalent(result->path, tmp.path("idir/header.h")));
    EXPECT(result->found_dir_idx == 1u);
}

ZEST_CASE(AngledMissesQuotedOnly) {
    TempDir tmp;
    tmp.touch("iquote/only_here.h");

    // Layout: [iquote | (no angled) | (no system)]
    SearchConfig config;
    config.dirs.push_back({tmp.path("iquote")});
    config.angled_start_idx = 1;
    config.system_start_idx = 1;

    vfs::DirCache cache;
    vfs::Scope scope(cache);

    // <only_here.h> should NOT find it — only in quoted dir.
    auto result = resolve_include("only_here.h", true, "", false, 0, config, scope);
    EXPECT(!result.has_value());
}

ZEST_CASE(QuotedSearchesAllDirs) {
    TempDir tmp;
    tmp.touch("sys/deep.h", "// system");

    // Layout: [iquote | idir | sys]
    SearchConfig config;
    config.dirs.push_back({tmp.path("iquote")});
    config.dirs.push_back({tmp.path("idir")});
    config.dirs.push_back({tmp.path("sys")});
    config.angled_start_idx = 1;
    config.system_start_idx = 2;

    vfs::DirCache cache;
    vfs::Scope scope(cache);

    // "deep.h" is only in system dir, but quoted search goes through all.
    auto result = resolve_include("deep.h", false, "", false, 0, config, scope);
    ASSERT(result);
    EXPECT(llvm::sys::fs::equivalent(result->path, tmp.path("sys/deep.h")));
}

ZEST_CASE(AngledBeforeSystem) {
    TempDir tmp;
    tmp.touch("idir/priority.h", "// angled");
    tmp.touch("sys/priority.h", "// system");

    SearchConfig config;
    config.dirs.push_back({tmp.path("idir")});  // 0: Angled
    config.dirs.push_back({tmp.path("sys")});   // 1: System
    config.angled_start_idx = 0;
    config.system_start_idx = 1;

    vfs::DirCache cache;
    vfs::Scope scope(cache);

    // <priority.h> should find in Angled (index 0) before System (index 1).
    auto result = resolve_include("priority.h", true, "", false, 0, config, scope);
    ASSERT(result);
    EXPECT(llvm::sys::fs::equivalent(result->path, tmp.path("idir/priority.h")));
    EXPECT(result->found_dir_idx == 0u);
}

ZEST_CASE(AfterSearchedLast) {
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

    vfs::DirCache cache;
    vfs::Scope scope(cache);

    // <fallback.h> not in angled or sys, found in after.
    auto result = resolve_include("fallback.h", true, "", false, 0, config, scope);
    ASSERT(result);
    EXPECT(llvm::sys::fs::equivalent(result->path, tmp.path("after/fallback.h")));
    EXPECT(result->found_dir_idx == 2u);
}

ZEST_CASE(IncludeNextPropagatesIdx) {
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

    vfs::DirCache cache;
    vfs::Scope scope(cache);

    // File found at dir1 (index 1) does #include_next <limits.h>
    auto result = resolve_include("limits.h", true, "", true, 1, config, scope);
    ASSERT(result);
    // Should skip dirs 0-1, find in dir2.
    EXPECT(llvm::sys::fs::equivalent(result->path, tmp.path("dir2/limits.h")));
    EXPECT(result->found_dir_idx == 2u);
}

ZEST_CASE(IncludeNextOutsideDirs) {
    TempDir tmp;
    tmp.touch("inc0/target.h", "// first");
    tmp.touch("inc1/target.h", "// second");
    tmp.touch("src/a.h", "#include_next \"target.h\"");

    SearchConfig config;
    config.dirs.push_back({tmp.path("inc0")});
    config.dirs.push_back({tmp.path("inc1")});
    config.angled_start_idx = 0;

    vfs::DirCache cache;
    vfs::Scope scope(cache);

    // src/a.h was found next to its includer, through no search dir: clang
    // looks its #include_next up like a plain include, from the start.
    auto includer =
        resolve_include("a.h", false, tmp.path("src"), false, std::nullopt, config, scope);
    ASSERT(includer);
    EXPECT(includer->found_dir_idx == std::nullopt);

    auto result = resolve_include("target.h",
                                  false,
                                  tmp.path("src"),
                                  true,
                                  includer->found_dir_idx,
                                  config,
                                  scope);
    ASSERT(result);
    EXPECT(llvm::sys::fs::equivalent(result->path, tmp.path("inc0/target.h")));
    EXPECT(result->found_dir_idx == 0u);
}

ZEST_CASE(CaseMatchesVolume) {
    // A header included in another case than its name on disk: found
    // exactly where the volume opens it — Windows and macOS by default —
    // and nowhere else.
    TempDir tmp;
    tmp.touch("a/MyHeader.h");
    tmp.touch("b/Sub/x.h");
    bool insensitive = llvm::sys::fs::exists(tmp.path("a/myheader.h"));

    SearchConfig config;
    config.dirs.push_back({tmp.path("a")});
    config.dirs.push_back({tmp.path("b")});

    vfs::DirCache cache;
    vfs::Scope scope(cache);
    auto header = resolve_include("myheader.h", true, "", false, 0, config, scope);
    auto nested = resolve_include("sub/x.h", true, "", false, 0, config, scope);
    ASSERT(header.has_value() == insensitive);
    ASSERT(nested.has_value() == insensitive);
    if(insensitive) {
        EXPECT(llvm::sys::fs::equivalent(header->path, tmp.path("a/MyHeader.h")));
        EXPECT(nested->found_dir_idx == 1u);
    }
}

ZEST_CASE(NormalizationMatchesVolume) {
    // The name on disk decomposed (NFD), the include composed (NFC): APFS
    // opens one by the other, other volumes do not.
    TempDir tmp;
    tmp.touch("inc/e\xCC\x81.h");
    bool insensitive = llvm::sys::fs::exists(tmp.path("inc/\xC3\xA9.h"));

    SearchConfig config;
    config.dirs.push_back({tmp.path("inc")});

    vfs::DirCache cache;
    vfs::Scope scope(cache);
    auto result = resolve_include("\xC3\xA9.h", true, "", false, 0, config, scope);
    ASSERT(result.has_value() == insensitive);
}

// TODO: add tests for:
// - #include_next crossing segment boundaries (angled→system)
// - #include_next at last search dir (should return nullopt)
// - Relative paths with .. components ("../sibling/header.h")
// - ResolvedSearchConfig overload (the production hot path)

};  // namespace clice::testing

}  // namespace
}  // namespace clice::testing
