#include "test/test.h"
#include "command/argument_parser.h"
#include "command/command.h"
#include "support/filesystem.h"

#include "llvm/Support/raw_ostream.h"

namespace clice::testing {

namespace {

using namespace std::literals;

CommandOptions quiet_options() {
    CommandOptions options;
    options.suppress_logging = true;
    return options;
}

#define EXPECT_CONTAINS(haystack, needle) EXPECT_TRUE(llvm::StringRef(haystack).contains(needle))
#define EXPECT_NOT_CONTAINS(haystack, needle)                                                      \
    EXPECT_FALSE(llvm::StringRef(haystack).contains(needle))

TEST_SUITE(Command) {

void expect_strip(llvm::StringRef argv, llvm::StringRef result) {
    CompilationDatabase database;
    llvm::StringRef file = "main.cpp";
    database.add_command("fake/", file, argv);
    ASSERT_EQ(result, print_argv(database.lookup(file, quiet_options()).front().arguments));
};

TEST_CASE(DefaultFilters) {
    /// Filter -c, -o and input file.
    expect_strip("g++ main.cpp", "g++ main.cpp");
    expect_strip("clang++ -c main.cpp", "clang++ main.cpp");
    expect_strip("clang++ -o main.o main.cpp", "clang++ main.cpp");
    expect_strip("clang++ -c -o main.o main.cpp", "clang++ main.cpp");
    expect_strip("cl.exe /c /Fomain.cpp.o main.cpp", "cl.exe main.cpp");

    /// Filter PCH related.

    /// CMake
    expect_strip("g++ -std=gnu++20 -Winvalid-pch -include cmake_pch.hxx -o main.cpp.o -c main.cpp",
                 "g++ -std=gnu++20 -Winvalid-pch -include cmake_pch.hxx main.cpp");
    expect_strip(
        "clang++ -Winvalid-pch -Xclang -include-pch -Xclang cmake_pch.hxx.pch -Xclang -include -Xclang cmake_pch.hxx -o main.cpp.o -c main.cpp",
        "clang++ -Winvalid-pch -Xclang -include -Xclang cmake_pch.hxx main.cpp");
    expect_strip("cl.exe /Yufoo.h /FIfoo.h /Fpfoo.h_v143.pch /c /Fomain.cpp.o main.cpp",
                 "cl.exe -include foo.h main.cpp");

    /// TODO: Test more commands from other build system.
};

TEST_CASE(Reuse) {
    CompilationDatabase database;
    database.add_command("fake", "test.cpp", "clang++ -std=c++23 test.cpp"sv);
    database.add_command("fake", "test2.cpp", "clang++ -std=c++23 test2.cpp"sv);

    auto options = quiet_options();
    auto command1 = database.lookup("test.cpp", options).front().arguments;
    auto command2 = database.lookup("test2.cpp", options).front().arguments;
    ASSERT_EQ(command1.size(), 3U);
    ASSERT_EQ(command2.size(), 3U);

    ASSERT_EQ(command1[0], "clang++"sv);
    ASSERT_EQ(command1[1], "-std=c++23"sv);
    ASSERT_EQ(command1[2], "test.cpp"sv);

    ASSERT_EQ(command1[0], command2[0]);
    ASSERT_EQ(command1[1], command2[1]);
    ASSERT_EQ(command2[2], "test2.cpp"sv);
};

TEST_CASE(RemoveAppend) {
    llvm::SmallVector args = {
        "clang++",
        "--output=main.o",
        "-D",
        "A",
        "-D",
        "B=0",
        "main.cpp",
    };

    CompilationDatabase database;
    database.add_command("/fake", "main.cpp", args);

    auto options = quiet_options();

    llvm::SmallVector<std::string> remove;
    llvm::SmallVector<std::string> append;

    remove = {"-DA"};
    options.remove = remove;
    auto result = database.lookup("main.cpp", options).front().arguments;
    ASSERT_EQ(print_argv(result), "clang++ -D B=0 main.cpp");

    remove = {"-D", "A"};
    options.remove = remove;
    result = database.lookup("main.cpp", options).front().arguments;
    ASSERT_EQ(print_argv(result), "clang++ -D B=0 main.cpp");

    remove = {"-DA", "-D", "B=0"};
    options.remove = remove;
    result = database.lookup("main.cpp", options).front().arguments;
    ASSERT_EQ(print_argv(result), "clang++ main.cpp");

    remove = {"-D*"};
    options.remove = remove;
    result = database.lookup("main.cpp", options).front().arguments;
    ASSERT_EQ(print_argv(result), "clang++ main.cpp");

    remove = {"-D", "*"};
    options.remove = remove;
    result = database.lookup("main.cpp", options).front().arguments;
    ASSERT_EQ(print_argv(result), "clang++ main.cpp");

    append = {"-D", "C"};
    options.append = append;
    result = database.lookup("main.cpp", options).front().arguments;
    ASSERT_EQ(print_argv(result), "clang++ -D C main.cpp");
};

TEST_CASE(DefaultFallback) {
    /// Lookup for a file not in the CDB should synthesize a default command.
    CompilationDatabase database;

    /// C++ files get "clang++ -std=c++20 <file>".
    auto cpp_results = database.lookup("unknown.cpp");
    ASSERT_EQ(cpp_results.size(), 1U);
    auto& cpp_ctx = cpp_results.front();
    ASSERT_EQ(cpp_ctx.arguments.size(), 3U);
    ASSERT_EQ(cpp_ctx.arguments[0], "clang++"sv);
    ASSERT_EQ(cpp_ctx.arguments[1], "-std=c++20"sv);
    ASSERT_EQ(cpp_ctx.arguments[2], "unknown.cpp"sv);

    /// .hpp files also get C++ default.
    auto hpp_results = database.lookup("header.hpp");
    ASSERT_EQ(hpp_results.front().arguments.size(), 3U);
    ASSERT_EQ(hpp_results.front().arguments[0], "clang++"sv);

    /// .cc files also get C++ default.
    auto cc_results = database.lookup("file.cc");
    ASSERT_EQ(cc_results.front().arguments.size(), 3U);
    ASSERT_EQ(cc_results.front().arguments[0], "clang++"sv);

    /// C files get "clang <file>".
    auto c_results = database.lookup("unknown.c");
    ASSERT_EQ(c_results.size(), 1U);
    auto& c_ctx = c_results.front();
    ASSERT_EQ(c_ctx.arguments.size(), 2U);
    ASSERT_EQ(c_ctx.arguments[0], "clang"sv);
    ASSERT_EQ(c_ctx.arguments[1], "unknown.c"sv);

    /// Other extensions also get plain clang.
    auto h_results = database.lookup("foo.h");
    ASSERT_EQ(h_results.front().arguments.size(), 2U);
    ASSERT_EQ(h_results.front().arguments[0], "clang"sv);
};

TEST_CASE(MultiCommand) {
    /// A file can have multiple compilation commands (e.g. different configs).
    CompilationDatabase database;
    database.add_command("fake", "main.cpp", "clang++ -std=c++17 main.cpp"sv);
    database.add_command("fake", "main.cpp", "clang++ -std=c++20 main.cpp"sv);
    database.add_command("fake", "other.cpp", "clang++ -std=c++23 other.cpp"sv);

    auto options = quiet_options();

    auto results = database.lookup("main.cpp", options);
    ASSERT_EQ(results.size(), 2U);

    /// Both commands are present (order depends on insert position).
    bool has_17 = false, has_20 = false;
    for(auto& ctx: results) {
        auto argv = print_argv(ctx.arguments);
        if(llvm::StringRef(argv).contains("-std=c++17"))
            has_17 = true;
        if(llvm::StringRef(argv).contains("-std=c++20"))
            has_20 = true;
    }
    EXPECT_TRUE(has_17);
    EXPECT_TRUE(has_20);

    /// other.cpp has only one.
    auto other = database.lookup("other.cpp", options);
    ASSERT_EQ(other.size(), 1U);
};

TEST_CASE(CodegenFilter) {
    /// Codegen-only options should be stripped from the canonical command.
    CompilationDatabase database;
    database.add_command(
        "fake",
        "main.cpp",
        "clang++ -std=c++20 -fPIC -fno-omit-frame-pointer -fstack-protector-strong " "-fdata-sections -ffunction-sections -flto -fcolor-diagnostics -g main.cpp"sv);

    auto result = database.lookup("main.cpp", quiet_options()).front().arguments;
    auto argv = print_argv(result);

    /// -std=c++20 must survive (semantic).
    EXPECT_CONTAINS(argv, "-std=c++20");

    /// All codegen flags must be stripped.
    EXPECT_NOT_CONTAINS(argv, "-fPIC");
    EXPECT_NOT_CONTAINS(argv, "-fno-omit-frame-pointer");
    EXPECT_NOT_CONTAINS(argv, "-fstack-protector");
    EXPECT_NOT_CONTAINS(argv, "-fdata-sections");
    EXPECT_NOT_CONTAINS(argv, "-ffunction-sections");
    EXPECT_NOT_CONTAINS(argv, "-flto");
    EXPECT_NOT_CONTAINS(argv, "-fcolor-diagnostics");
    EXPECT_NOT_CONTAINS(argv, "-g");
};

TEST_CASE(DependencyScanFilter) {
    /// Dependency scan options should be stripped.
    CompilationDatabase database;
    database.add_command("fake",
                         "main.cpp",
                         "clang++ -std=c++20 -MD -MF main.d -MT main.o main.cpp"sv);

    auto result = database.lookup("main.cpp", quiet_options()).front().arguments;
    auto argv = print_argv(result);

    EXPECT_CONTAINS(argv, "-std=c++20");
    EXPECT_NOT_CONTAINS(argv, "-MD");
    EXPECT_NOT_CONTAINS(argv, "-MF");
    EXPECT_NOT_CONTAINS(argv, "-MT");
    EXPECT_NOT_CONTAINS(argv, "main.d");
};

TEST_CASE(ModuleFilter) {
    /// Module-related options should be stripped.
    expect_strip("clang++ -std=c++20 -fmodule-file=mod.pcm main.cpp",
                 "clang++ -std=c++20 main.cpp");
    expect_strip("clang++ -std=c++20 -fprebuilt-module-path=/tmp main.cpp",
                 "clang++ -std=c++20 main.cpp");
};

TEST_CASE(UserContentClassification) {
    /// -D, -U, -include go to per-file patch; -std=, -W go to canonical.
    /// Files with different -D but same -std/-W share canonical.
    CompilationDatabase database;
    database.add_command("fake", "a.cpp", "clang++ -std=c++20 -Wall -DA=1 -DFOO a.cpp"sv);
    database.add_command("fake", "b.cpp", "clang++ -std=c++20 -Wall -DB=2 b.cpp"sv);

    auto options = quiet_options();

    auto a_argv = print_argv(database.lookup("a.cpp", options).front().arguments);
    auto b_argv = print_argv(database.lookup("b.cpp", options).front().arguments);

    /// Both must contain canonical flags.
    EXPECT_CONTAINS(a_argv, "-std=c++20");
    EXPECT_CONTAINS(a_argv, "-Wall");
    EXPECT_CONTAINS(b_argv, "-std=c++20");
    EXPECT_CONTAINS(b_argv, "-Wall");

    /// a.cpp has its own defines.
    EXPECT_CONTAINS(a_argv, "-D");
    EXPECT_CONTAINS(a_argv, "A=1");
    EXPECT_CONTAINS(a_argv, "FOO");

    /// b.cpp has its own defines.
    EXPECT_CONTAINS(b_argv, "-D");
    EXPECT_CONTAINS(b_argv, "B=2");

    /// Cross check: a.cpp should not have B=2, b.cpp should not have A=1.
    EXPECT_NOT_CONTAINS(a_argv, "B=2");
    EXPECT_NOT_CONTAINS(b_argv, "A=1");
};

TEST_CASE(IncludePathAbsolutize) {
    /// Relative include paths should be absolutized against the directory.
    CompilationDatabase database;
    database.add_command("/project/build",
                         "main.cpp",
                         "clang++ -Iinclude -isystem sys/inc -iquote ../src main.cpp"sv);

    auto result = database.lookup("main.cpp", quiet_options()).front().arguments;

    /// Check each argument individually with separator normalization
    /// (print_argv escapes backslashes, breaking convert_to_slash on Windows).
    auto has_path = [](llvm::ArrayRef<const char*> args, llvm::StringRef needle) {
        for(auto* arg: args) {
            if(path::convert_to_slash(arg).find(needle.str()) != std::string::npos)
                return true;
        }
        return false;
    };

    /// Relative paths must be resolved against /project/build.
    EXPECT_TRUE(has_path(result, "/project/build/include"));
    EXPECT_TRUE(has_path(result, "/project/build/sys/inc"));
    /// ../src relative to /project/build → /project/src (or /project/build/../src)
    EXPECT_TRUE(has_path(result, "/project/"));

    /// Absolute paths should be kept as-is.
    CompilationDatabase database2;
    database2.add_command("/project/build", "main.cpp", "clang++ -I/usr/include main.cpp"sv);

    auto result2 = database2.lookup("main.cpp", quiet_options()).front().arguments;
    EXPECT_TRUE(has_path(result2, "/usr/include"));
};

TEST_CASE(SemanticOptionsPreserved) {
    /// Flags that affect semantics must survive.
    expect_strip("clang++ -std=c++20 -fno-exceptions -fno-rtti -pedantic main.cpp",
                 "clang++ -std=c++20 -fno-exceptions -fno-rtti -pedantic main.cpp");
    expect_strip("clang++ -std=c++20 -Wall -Werror main.cpp",
                 "clang++ -std=c++20 -Wall -Werror main.cpp");
};

TEST_CASE(LookupSearchConfig) {
    CompilationDatabase database;
    database.add_command(
        "/project",
        "main.cpp",
        "clang++ -std=c++20 -I/usr/include -isystem /usr/local/include main.cpp"sv);

    ASSERT_FALSE(database.has_cached_configs());

    auto options = quiet_options();
    auto config = database.lookup_search_config("main.cpp", options);

    /// Should have search dirs from the command.
    EXPECT_FALSE(config.dirs.empty());

    /// Second call should hit cache.
    EXPECT_TRUE(database.has_cached_configs());
    auto config2 = database.lookup_search_config("main.cpp", options);
    ASSERT_EQ(config.dirs.size(), config2.dirs.size());
};

TEST_CASE(ResolvePath) {
    CompilationDatabase database;
    database.add_command("fake", "test/main.cpp", "clang++ test/main.cpp"sv);

    /// After add_command, lookup should work and resolve_path via the file in arguments.
    auto result = database.lookup("test/main.cpp", quiet_options()).front().arguments;
    /// The last argument is the file, resolved from PathPool.
    ASSERT_EQ(result.back(), "test/main.cpp"sv);
};

TEST_CASE(MoveSemantics) {
    CompilationDatabase db1;
    db1.add_command("fake", "main.cpp", "clang++ -std=c++23 main.cpp"sv);

    /// Move construct.
    CompilationDatabase db2 = std::move(db1);

    auto options = quiet_options();
    auto result = db2.lookup("main.cpp", options).front().arguments;
    ASSERT_EQ(result.size(), 3U);
    ASSERT_EQ(result[1], "-std=c++23"sv);

    /// Move assign.
    CompilationDatabase db3;
    db3 = std::move(db2);
    result = db3.lookup("main.cpp", options).front().arguments;
    ASSERT_EQ(result.size(), 3U);
    ASSERT_EQ(result[1], "-std=c++23"sv);
};

/// Write JSON to a temp file, load into a CDB, remove the file.
/// Returns the number of entries loaded.
std::size_t load_json(CompilationDatabase& database, llvm::StringRef json) {
    auto path = fs::createTemporaryFile("cdb", "json");
    if(!path)
        return 0;
    {
        std::error_code ec;
        llvm::raw_fd_ostream out(*path, ec);
        if(ec)
            return 0;
        out << json;
    }
    auto count = database.load(*path);
    llvm::sys::fs::remove(*path);
    return count;
}

TEST_CASE(LoadMixedFormats) {
    /// "arguments" array and "command" string can coexist in the same CDB.
    /// Use relative file paths so that the test works on both Linux and Windows
    /// (paths like "/src/a.cpp" are not absolute on Windows — no drive letter).
    CompilationDatabase database;
    auto count = load_json(database, R"([
        {"directory": "/build", "file": "a.cpp",
         "arguments": ["clang++", "-std=c++20", "a.cpp"]},
        {"directory": "/build", "file": "b.cpp",
         "command": "clang++ -std=c++23 b.cpp"}
    ])");

    ASSERT_EQ(count, 2U);

    auto options = quiet_options();

    auto a = database.lookup(path::join("/build", "a.cpp"), options);
    ASSERT_EQ(a.size(), 1U);
    EXPECT_CONTAINS(print_argv(a.front().arguments), "-std=c++20");

    auto b = database.lookup(path::join("/build", "b.cpp"), options);
    ASSERT_EQ(b.size(), 1U);
    EXPECT_CONTAINS(print_argv(b.front().arguments), "-std=c++23");
};

TEST_CASE(LoadErrorRecovery) {
    /// Bad entries should be skipped; good entries still load.
    CompilationDatabase database;
    auto count = load_json(database, R"([
        {"file": "no_dir.cpp",
         "arguments": ["clang++", "no_dir.cpp"]},
        {"directory": "/build",
         "arguments": ["clang++", "no_file.cpp"]},
        {"directory": "/build", "file": "no_args.cpp"},
        {"directory": "/build", "file": "good.cpp",
         "arguments": ["clang++", "-std=c++20", "good.cpp"]},
        42,
        {"directory": "/build", "file": "also_good.cpp",
         "command": "clang++ -Wall also_good.cpp"}
    ])");

    /// Only the two valid entries should survive.
    ASSERT_EQ(count, 2U);

    auto options = quiet_options();

    auto good = database.lookup(path::join("/build", "good.cpp"), options);
    ASSERT_EQ(good.size(), 1U);
    EXPECT_CONTAINS(print_argv(good.front().arguments), "-std=c++20");

    auto also = database.lookup(path::join("/build", "also_good.cpp"), options);
    ASSERT_EQ(also.size(), 1U);
    EXPECT_CONTAINS(print_argv(also.front().arguments), "-Wall");
};

TEST_CASE(LoadEmptyCommand) {
    /// Whitespace-only or empty "command" should not crash.
    CompilationDatabase database;
    auto count = load_json(database, R"([
        {"directory": "/build", "file": "empty.cpp", "command": ""},
        {"directory": "/build", "file": "spaces.cpp", "command": "   "},
        {"directory": "/build", "file": "ok.cpp",
         "command": "clang++ -std=c++20 ok.cpp"}
    ])");

    /// Only the valid entry survives.
    ASSERT_EQ(count, 1U);

    auto ok = database.lookup(path::join("/build", "ok.cpp"), quiet_options());
    ASSERT_EQ(ok.size(), 1U);
    EXPECT_CONTAINS(print_argv(ok.front().arguments), "-std=c++20");
};

TEST_CASE(LoadReload) {
    /// Second load() replaces all entries from the first.
    CompilationDatabase database;

    auto file_a = path::join("/build", "a.cpp");
    auto file_b = path::join("/build", "b.cpp");

    load_json(database, R"([
        {"directory": "/build", "file": "a.cpp",
         "arguments": ["clang++", "-std=c++17", "a.cpp"]}
    ])");

    auto options = quiet_options();

    auto a = database.lookup(file_a, options);
    ASSERT_EQ(a.size(), 1U);
    EXPECT_CONTAINS(print_argv(a.front().arguments), "-std=c++17");

    /// Reload with different content.
    auto count = load_json(database, R"([
        {"directory": "/build", "file": "b.cpp",
         "arguments": ["clang++", "-std=c++23", "b.cpp"]}
    ])");

    ASSERT_EQ(count, 1U);

    /// Old entry gone (falls back to default).
    auto a2 = database.lookup(file_a, options);
    ASSERT_EQ(a2.size(), 1U);
    EXPECT_NOT_CONTAINS(print_argv(a2.front().arguments), "-std=c++17");

    /// New entry present.
    auto b = database.lookup(file_b, options);
    ASSERT_EQ(b.size(), 1U);
    EXPECT_CONTAINS(print_argv(b.front().arguments), "-std=c++23");
};

TEST_CASE(LoadCommandQuoting) {
    /// "command" string with spaces in paths and quoted defines.
    CompilationDatabase database;
    auto count = load_json(database, R"([
        {"directory": "/build", "file": "main.cpp",
         "command": "clang++ -std=c++20 \"-DMSG=hello world\" -I\"/path with spaces\" main.cpp"}
    ])");

    ASSERT_EQ(count, 1U);

    auto result = database.lookup(path::join("/build", "main.cpp"), quiet_options());
    ASSERT_EQ(result.size(), 1U);
    auto argv = print_argv(result.front().arguments);

    /// The define and include path should be present after shell tokenization.
    EXPECT_CONTAINS(argv, "hello world");
    EXPECT_CONTAINS(argv, "/path with spaces");
};

TEST_CASE(LoadRelativePath) {
    /// load() should resolve relative file paths against directory.
    CompilationDatabase database;
    auto count = load_json(database, R"([
        {"directory": "/project/build", "file": "src/main.cpp",
         "arguments": ["clang++", "-std=c++20", "src/main.cpp"]},
        {"directory": "/other/build", "file": "src/main.cpp",
         "arguments": ["clang++", "-std=c++17", "src/main.cpp"]}
    ])");

    ASSERT_EQ(count, 2U);

    auto options = quiet_options();

    /// Lookup by the resolved absolute path (use path::join for correct separator).
    auto results = database.lookup(path::join("/project/build", "src/main.cpp"), options);
    ASSERT_EQ(results.size(), 1U);
    EXPECT_CONTAINS(print_argv(results.front().arguments), "-std=c++20");

    auto results2 = database.lookup(path::join("/other/build", "src/main.cpp"), options);
    ASSERT_EQ(results2.size(), 1U);
    EXPECT_CONTAINS(print_argv(results2.front().arguments), "-std=c++17");

    /// Relative path lookup should not match (different path_id).
    auto results3 = database.lookup("src/main.cpp", options);
    ASSERT_EQ(results3.size(), 1U);
    /// Falls back to default command since no match.
    EXPECT_CONTAINS(print_argv(results3.front().arguments), "clang");
};

TEST_CASE(Module) {
    // TODO: revisit module command handling.
}

TEST_CASE(ResourceDir) {
    // When query_toolchain is enabled, resource dir is injected automatically.
    CompilationDatabase database;
    database.add_command("/fake", "main.cpp", "clang++ -std=c++23 test.cpp"sv);

    // Without query_toolchain, no resource dir injection.
    auto args_no_tc = database.lookup("main.cpp").front().arguments;
    ASSERT_EQ(args_no_tc.size(), 3U);
    ASSERT_EQ(args_no_tc[0], "clang++"sv);
    ASSERT_EQ(args_no_tc[1], "-std=c++23"sv);
    ASSERT_EQ(args_no_tc[2], "main.cpp"sv);

    // With query_toolchain, resource dir is present in the result.
    auto args_tc = database.lookup("main.cpp", {.query_toolchain = true}).front().arguments;
    bool has_resource_dir = false;
    for(size_t i = 0; i + 1 < args_tc.size(); ++i) {
        if(args_tc[i] == "-resource-dir"sv) {
            EXPECT_EQ(llvm::StringRef(args_tc[i + 1]), resource_dir());
            has_resource_dir = true;
            break;
        }
    }
    if(resource_dir().empty()) {
        EXPECT_FALSE(has_resource_dir);
    } else {
        EXPECT_TRUE(has_resource_dir);
    }
};

};  // TEST_SUITE(Command)

}  // namespace

}  // namespace clice::testing
