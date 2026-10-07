module;

#include "modules/prelude.h"

module clice;

import :command.command;
import :command.search_config;
import :tests.unit.test.temp_dir;
import :tests.unit.test.test;

namespace clice::testing {

namespace {

ZEST_SUITE(ExtractSearchConfig) {

/// Normalize a raw argv through the database and extract from the
/// structured command (the only extraction path).
SearchConfig extract(llvm::ArrayRef<const char*> args, llvm::StringRef directory) {
    FileTable file_table;
    CompilationDatabase db{file_table};
    db.add_command(directory, "main.cpp", args);
    auto& entry = db.candidate_entries(path::join(directory, "main.cpp")).front();
    return extract_search_config(db.config(entry.config).args, directory);
}

/// A search directory as extraction spells it: absolute, canonically.
std::string spelled(const TempDir& tmp, llvm::StringRef relative) {
    return Spelling::absolute(tmp.path(relative)).str();
}

ZEST_CASE(ReordersDirectoryGroups) {
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
    auto config = extract(args, tmp.root.str());

    // Expected order: [quoted | user | stdlib, clang, sysroot]
    ZASSERT(config.dirs.size() == 5u);
    ZEXPECT(config.angled_start_idx == 1u);
    ZEXPECT(config.system_start_idx == 2u);

    ZEXPECT(config.dirs[0].path == spelled(tmp, "quoted"));
    ZEXPECT(config.dirs[1].path == spelled(tmp, "user"));
    ZEXPECT(config.dirs[2].path == spelled(tmp, "stdlib"));
    ZEXPECT(config.dirs[3].path == spelled(tmp, "clang"));
    ZEXPECT(config.dirs[4].path == spelled(tmp, "sysroot"));
}

ZEST_CASE(KeepsForcedIncludes) {
    // In command order and as written: their lookup starts from the
    // working directory, which extraction does not settle.
    TempDir tmp;
    std::vector<const char*> args = {"clang++",
                                     "-include",
                                     "b.h",
                                     "-include-pch",
                                     "p.pch",
                                     "-I",
                                     tmp.c_path("inc"),
                                     "-include",
                                     "a.h",
                                     "main.cpp"};
    auto config = extract(args, tmp.root.str());

    ZASSERT(config.forced_includes == (std::vector<std::string>{"b.h", "a.h"}));
}

ZEST_CASE(MarksDriverDirs) {
    // Only the driver's own flags add a toolchain directory; a user's
    // -isystem is the user's.
    TempDir tmp;
    std::vector<const char*> args = {"clang++",
                                     "-isystem",
                                     tmp.c_path("vendored"),
                                     "-internal-isystem",
                                     tmp.c_path("stdlib"),
                                     "-internal-externc-isystem",
                                     tmp.c_path("sysroot"),
                                     "main.cpp"};
    auto config = extract(args, tmp.root.str());
    ZASSERT(config.dirs.size() == 3u);
    ZEXPECT(!config.dirs[0].driver);
    ZEXPECT(config.dirs[1].driver);
    ZEXPECT(config.dirs[2].driver);
}

ZEST_CASE(PreservesWithinGroupOrder) {
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
    auto config = extract(args, tmp.root.str());

    ZASSERT(config.dirs.size() == 4u);
    ZEXPECT(config.angled_start_idx == 0u);
    ZEXPECT(config.system_start_idx == 2u);
    ZEXPECT(config.dirs[0].path == spelled(tmp, "b"));
    ZEXPECT(config.dirs[1].path == spelled(tmp, "a"));
    ZEXPECT(config.dirs[2].path == spelled(tmp, "s2"));
    ZEXPECT(config.dirs[3].path == spelled(tmp, "s1"));
}

ZEST_CASE(DeduplicatesAngledSystem) {
    TempDir tmp;
    std::vector<const char*> args = {"clang++",
                                     "-I",
                                     tmp.c_path("shared"),
                                     "-internal-isystem",
                                     tmp.c_path("shared"),
                                     "-internal-isystem",
                                     tmp.c_path("only_sys"),
                                     "main.cpp"};
    auto config = extract(args, tmp.root.str());

    // /shared in both Angled and System → keep Angled copy.
    ZASSERT(config.dirs.size() == 2u);
    ZEXPECT(config.angled_start_idx == 0u);
    ZEXPECT(config.system_start_idx == 1u);
    ZEXPECT(config.dirs[0].path == spelled(tmp, "shared"));
    ZEXPECT(config.dirs[1].path == spelled(tmp, "only_sys"));
}

ZEST_CASE(QuotedAngledSamePathKeptInBoth) {
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
    auto config = extract(args, tmp.root.str());

    // "shared" must appear in both Quoted and Angled segments.
    ZASSERT(config.dirs.size() == 3u);
    ZEXPECT(config.angled_start_idx == 1u);
    ZEXPECT(config.dirs[0].path == spelled(tmp, "shared"));  // Quoted
    ZEXPECT(config.dirs[1].path == spelled(tmp, "shared"));  // Angled (not deduped)
    ZEXPECT(config.dirs[2].path == spelled(tmp, "other"));
}

ZEST_CASE(DeduplicateAdjustsIndices) {
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
    auto config = extract(args, tmp.root.str());

    // Before dedup: [q | dup, a2 | dup, s] angled=1, system=3
    // dup in system removed. system_start_idx stays 3.
    ZASSERT(config.dirs.size() == 4u);
    ZEXPECT(config.angled_start_idx == 1u);
    ZEXPECT(config.system_start_idx == 3u);
    ZEXPECT(config.dirs[0].path == spelled(tmp, "q"));
    ZEXPECT(config.dirs[1].path == spelled(tmp, "dup"));
    ZEXPECT(config.dirs[2].path == spelled(tmp, "a2"));
    ZEXPECT(config.dirs[3].path == spelled(tmp, "s"));
}

ZEST_CASE(PrefixIncludeOptions) {
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
    auto config = extract(args, tmp.root.str());

    // -iwithprefixbefore → Angled, -iwithprefix → After
    ZASSERT(config.dirs.size() == 3u);
    ZEXPECT(config.angled_start_idx == 0u);
    ZEXPECT(config.system_start_idx == 1u);
    ZEXPECT(config.after_start_idx == 1u);
    ZEXPECT(config.dirs[0].path == spelled(tmp, "gcc/12/include"));
    ZEXPECT(config.dirs[1].path == spelled(tmp, "gcc/12/lib"));
    ZEXPECT(config.dirs[2].path == spelled(tmp, "gcc/13/include"));
}

ZEST_CASE(DirafterGroup) {
    TempDir tmp;
    std::vector<const char*> args = {"clang++",
                                     "-I",
                                     tmp.c_path("user"),
                                     "-isystem",
                                     tmp.c_path("sys"),
                                     "-idirafter",
                                     tmp.c_path("fallback"),
                                     "main.cpp"};
    auto config = extract(args, tmp.root.str());

    ZASSERT(config.dirs.size() == 3u);
    ZEXPECT(config.angled_start_idx == 0u);
    ZEXPECT(config.system_start_idx == 1u);
    ZEXPECT(config.after_start_idx == 2u);
    ZEXPECT(config.dirs[0].path == spelled(tmp, "user"));
    ZEXPECT(config.dirs[1].path == spelled(tmp, "sys"));
    ZEXPECT(config.dirs[2].path == spelled(tmp, "fallback"));
}

ZEST_CASE(DirafterDeduplication) {
    TempDir tmp;
    std::vector<const char*> args = {"clang++",
                                     "-I",
                                     tmp.c_path("shared"),
                                     "-idirafter",
                                     tmp.c_path("shared"),
                                     "-idirafter",
                                     tmp.c_path("extra"),
                                     "main.cpp"};
    auto config = extract(args, tmp.root.str());

    ZASSERT(config.dirs.size() == 2u);
    ZEXPECT(config.angled_start_idx == 0u);
    ZEXPECT(config.after_start_idx == 1u);
    ZEXPECT(config.dirs[0].path == spelled(tmp, "shared"));
    ZEXPECT(config.dirs[1].path == spelled(tmp, "extra"));
}

ZEST_CASE(LastSysrootWins) {
    /// Like clang: the last --sysroot, unless an -isysroot names another.
    TempDir tmp;
    auto first = "--sysroot=" + tmp.path("one");
    auto second = "--sysroot=" + tmp.path("two");
    std::vector<const char*> args = {"clang++",
                                     first.c_str(),
                                     second.c_str(),
                                     "-I=/inc",
                                     "main.cpp"};
    auto config = extract(args, tmp.root.str());
    ZASSERT(config.dirs.size() == 1u);
    ZEXPECT(config.dirs[0].path == spelled(tmp, "two/inc"));

    auto isysroot = tmp.path("three");
    args = {"clang++", "-isysroot", isysroot.c_str(), second.c_str(), "-I=/inc", "main.cpp"};
    config = extract(args, tmp.root.str());
    ZASSERT(config.dirs.size() == 1u);
    ZEXPECT(config.dirs[0].path == spelled(tmp, "three/inc"));
}

};  // ZEST_SUITE(ExtractSearchConfig)

}  // namespace

}  // namespace clice::testing
