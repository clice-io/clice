module clice;

import :command.database;
import :command.search_config;
import :command.table;
import :unittests.utils.temp_dir;
import :unittests.utils;

namespace clice::testing {

namespace {

ZEST_SUITE(ExtractSearchConfig) {

/// Normalize a raw argv through the database and extract from the
/// structured command (the only extraction path).
SearchConfig extract(llvm::ArrayRef<const char*> args, llvm::StringRef directory) {
    FileTable file_table;
    CommandTable commands;
    CompilationDatabase db{file_table, commands};
    db.add_command(directory, "main.cpp", args);
    auto& entry = db.candidate_entries(path::join(directory, "main.cpp")).front();
    return extract_search_config(commands.get(entry.command).args, directory);
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
    auto config = extract(args, tmp.root);

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
    // In the order clang enters them, every -imacros before every
    // -include, and as written: their lookup starts from the working
    // directory, which extraction does not determine.
    TempDir tmp;
    std::vector<const char*> args = {"clang++",
                                     "-include",
                                     "b.h",
                                     "-imacros",
                                     "m.h",
                                     "-include-pch",
                                     "p.pch",
                                     "-I",
                                     tmp.c_path("inc"),
                                     "-include",
                                     "a.h",
                                     "-imacros",
                                     "n.h",
                                     "main.cpp"};
    auto config = extract(args, tmp.root);

    ZASSERT(config.forced_includes == (std::vector<std::string>{"m.h", "n.h", "b.h", "a.h"}));
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
    auto config = extract(args, tmp.root);
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
    auto config = extract(args, tmp.root);

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
    auto config = extract(args, tmp.root);

    // /shared in both Angled and System: the user's copy goes, as in clang.
    ZASSERT(config.dirs.size() == 2u);
    ZEXPECT(config.angled_start_idx == 0u);
    ZEXPECT(config.system_start_idx == 0u);
    ZEXPECT(config.dirs[0].path == spelled(tmp, "shared"));
    ZEXPECT(config.dirs[0].driver);
    ZEXPECT(config.dirs[1].path == spelled(tmp, "only_sys"));
}

ZEST_CASE(QuotedAngledBothKept) {
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
    auto config = extract(args, tmp.root);

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
    auto config = extract(args, tmp.root);

    // Before dedup: [q | dup, a2 | dup, s] angled=1, system=3
    // The Angled dup goes for the System one: [q | a2 | dup, s].
    ZASSERT(config.dirs.size() == 4u);
    ZEXPECT(config.angled_start_idx == 1u);
    ZEXPECT(config.system_start_idx == 2u);
    ZEXPECT(config.dirs[0].path == spelled(tmp, "q"));
    ZEXPECT(config.dirs[1].path == spelled(tmp, "a2"));
    ZEXPECT(config.dirs[2].path == spelled(tmp, "dup"));
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
                                     "-isystem",
                                     tmp.c_path("sys"),
                                     "main.cpp"};
    auto config = extract(args, tmp.root);

    // -iwithprefixbefore → Angled, -iwithprefix → After, behind System.
    ZASSERT(config.dirs.size() == 4u);
    ZEXPECT(config.angled_start_idx == 0u);
    ZEXPECT(config.system_start_idx == 1u);
    ZEXPECT(config.dirs[0].path == spelled(tmp, "gcc/12/include"));
    ZEXPECT(config.dirs[1].path == spelled(tmp, "sys"));
    ZEXPECT(config.dirs[2].path == spelled(tmp, "gcc/12/lib"));
    ZEXPECT(config.dirs[3].path == spelled(tmp, "gcc/13/include"));
}

ZEST_CASE(DirafterGroup) {
    TempDir tmp;
    std::vector<const char*> args = {"clang++",
                                     "-idirafter",
                                     tmp.c_path("fallback"),
                                     "-I",
                                     tmp.c_path("user"),
                                     "-isystem",
                                     tmp.c_path("sys"),
                                     "main.cpp"};
    auto config = extract(args, tmp.root);

    ZASSERT(config.dirs.size() == 3u);
    ZEXPECT(config.angled_start_idx == 0u);
    ZEXPECT(config.system_start_idx == 1u);
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
    auto config = extract(args, tmp.root);

    // After is a system group: -I's copy goes.
    ZASSERT(config.dirs.size() == 2u);
    ZEXPECT(config.angled_start_idx == 0u);
    ZEXPECT(config.system_start_idx == 0u);
    ZEXPECT(config.dirs[0].path == spelled(tmp, "shared"));
    ZEXPECT(config.dirs[1].path == spelled(tmp, "extra"));
}

ZEST_CASE(UserSystemFirst) {
    /// clang puts every -isystem ahead of the driver's -internal-isystem,
    /// wherever the command writes it.
    TempDir tmp;
    std::vector<const char*> args = {"clang++",
                                     "-internal-isystem",
                                     tmp.c_path("toolchain"),
                                     "-isystem",
                                     tmp.c_path("vendored"),
                                     "main.cpp"};
    auto config = extract(args, tmp.root);

    ZASSERT(config.dirs.size() == 2u);
    ZEXPECT(config.dirs[0].path == spelled(tmp, "vendored"));
    ZEXPECT(config.dirs[1].path == spelled(tmp, "toolchain"));
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
    auto config = extract(args, tmp.root);
    ZASSERT(config.dirs.size() == 1u);
    ZEXPECT(config.dirs[0].path == spelled(tmp, "two/inc"));

    auto isysroot = tmp.path("three");
    args = {"clang++", "-isysroot", isysroot.c_str(), second.c_str(), "-I=/inc", "main.cpp"};
    config = extract(args, tmp.root);
    ZASSERT(config.dirs.size() == 1u);
    ZEXPECT(config.dirs[0].path == spelled(tmp, "three/inc"));
}

ZEST_CASE(FrameworkDirs) {
    /// -F among the angled directories in command order, -iframework after
    /// the -isystem ones, the driver's -internal-iframework after its own.
    TempDir tmp;
    std::vector<const char*> args = {"clang++",
                                     "-internal-iframework",
                                     tmp.c_path("driver_frameworks"),
                                     "-internal-isystem",
                                     tmp.c_path("driver"),
                                     "-iframework",
                                     tmp.c_path("system_frameworks"),
                                     "-isystem",
                                     tmp.c_path("system"),
                                     "-F",
                                     tmp.c_path("frameworks"),
                                     "-I",
                                     tmp.c_path("user"),
                                     "main.cpp"};
    auto config = extract(args, tmp.root);
    ZASSERT(config.dirs.size() == 6u);
    ZEXPECT(config.system_start_idx == 2u);
    std::vector<std::pair<std::string, bool>> dirs;
    for(auto& dir: config.dirs) {
        dirs.emplace_back(dir.path, dir.framework);
    }
    std::vector<std::pair<std::string, bool>> expected = {
        {spelled(tmp, "frameworks"),        true },
        {spelled(tmp, "user"),              false},
        {spelled(tmp, "system"),            false},
        {spelled(tmp, "system_frameworks"), true },
        {spelled(tmp, "driver"),            false},
        {spelled(tmp, "driver_frameworks"), true },
    };
    ZEXPECT(dirs == expected);
    ZEXPECT(config.dirs[5].driver);
}

ZEST_CASE(FrameworkAndPlainDir) {
    /// One directory searched for frameworks and for plain headers is two
    /// entries, as in clang.
    TempDir tmp;
    std::vector<const char*> args =
        {"clang++", "-I", tmp.c_path("lib"), "-F", tmp.c_path("lib"), "main.cpp"};
    auto config = extract(args, tmp.root);
    ZASSERT(config.dirs.size() == 2u);
    ZEXPECT(!config.dirs[0].framework);
    ZEXPECT(config.dirs[1].framework);
}

ZEST_CASE(LanguageSystemDirs) {
    /// Each language searches its own of -c-isystem, -cxx-isystem,
    /// -objc-isystem and -objcxx-isystem, after -isystem and before the
    /// driver's own, whatever the command's order; Objective-C++ searches
    /// -cxx-isystem's too.
    TempDir tmp;
    auto dirs_of = [&](const char* language) {
        std::vector<const char*> args = {"clang++",
                                         "-x",
                                         language,
                                         "-internal-isystem",
                                         tmp.c_path("driver"),
                                         "-objcxx-isystem",
                                         tmp.c_path("objcxx"),
                                         "-objc-isystem",
                                         tmp.c_path("objc"),
                                         "-cxx-isystem",
                                         tmp.c_path("cxx"),
                                         "-c-isystem",
                                         tmp.c_path("c"),
                                         "-isystem",
                                         tmp.c_path("system"),
                                         "main.cpp"};
        return ranges::to<std::vector>(extract(args, tmp.root).dirs |
                                       views::transform(&SearchDir::path));
    };
    auto spelled_all = [&](std::initializer_list<llvm::StringRef> names) {
        return ranges::to<std::vector>(
            names | views::transform([&](llvm::StringRef name) { return spelled(tmp, name); }));
    };
    ZEXPECT(dirs_of("c") == spelled_all({"system", "c", "driver"}));
    ZEXPECT(dirs_of("c++") == spelled_all({"system", "cxx", "driver"}));
    ZEXPECT(dirs_of("objective-c") == spelled_all({"system", "objc", "driver"}));
    ZEXPECT(dirs_of("objective-c++") == spelled_all({"system", "cxx", "objcxx", "driver"}));
}

ZEST_CASE(WithSysroot) {
    /// -iwithsysroot puts the sysroot before an absolute path, among the
    /// -isystem directories in command order; -iframeworkwithsysroot does
    /// so for a framework directory, after the -iframework ones.
    TempDir tmp;
    auto sysroot = tmp.path("sdk");
    std::vector<const char*> args = {"clang++",
                                     "-isysroot",
                                     sysroot.c_str(),
                                     "-iframeworkwithsysroot",
                                     "/Frameworks",
                                     "-iwithsysroot",
                                     "/inc",
                                     "-iframework",
                                     tmp.c_path("frameworks"),
                                     "-isystem",
                                     tmp.c_path("system"),
                                     "main.cpp"};
    auto config = extract(args, tmp.root);
    std::vector<std::pair<std::string, bool>> dirs;
    for(auto& dir: config.dirs) {
        dirs.emplace_back(dir.path, dir.framework);
    }
    std::vector<std::pair<std::string, bool>> expected = {
        {spelled(tmp, "sdk/inc"),        false},
        {spelled(tmp, "system"),         false},
        {spelled(tmp, "frameworks"),     true },
        {spelled(tmp, "sdk/Frameworks"), true },
    };
    ZEXPECT(dirs == expected);
}

};  // ZEST_SUITE(ExtractSearchConfig)

}  // namespace

}  // namespace clice::testing
