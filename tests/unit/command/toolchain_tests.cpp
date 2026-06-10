#include "test/test.h"
#include "command/argument_parser.h"
#include "command/command.h"
#include "command/toolchain.h"
#include "compile/compilation.h"
#include "support/logging.h"

namespace clice::testing {
namespace {

using namespace std::string_view_literals;

TEST_SUITE(ToolchainTests) {

void EXPECT_FAMILY(llvm::StringRef name, CompilerFamily family) {
    ASSERT_EQ(Toolchain::driver_family(name), family);
};

TEST_CASE(Family) {
    using enum CompilerFamily;

    EXPECT_FAMILY("gcc", GCC);
    EXPECT_FAMILY("g++", GCC);
    EXPECT_FAMILY("x86_64-linux-gnu-g++-14", GCC);
    EXPECT_FAMILY("arm-none-eabi-gcc", GCC);

    EXPECT_FAMILY("clang", Clang);
    EXPECT_FAMILY("clang++", Clang);
    EXPECT_FAMILY("clang.exe", Clang);
    EXPECT_FAMILY("clang++.exe", Clang);
    EXPECT_FAMILY("clang-20", Clang);
    EXPECT_FAMILY("clang-20.exe", Clang);
    EXPECT_FAMILY("clang-cl", ClangCL);
    EXPECT_FAMILY("clang-cl-20", ClangCL);
    EXPECT_FAMILY("clang-cl-20.exe", ClangCL);

    EXPECT_FAMILY("cl.exe", MSVC);

    EXPECT_FAMILY("zig", Zig);
    EXPECT_FAMILY("zig.exe", Zig);
};

TEST_CASE(GCC, skip = !(CIEnvironment && (Windows || Linux))) {
    auto file = fs::createTemporaryFile("clice", "cpp");
    if(!file) {
        LOG_ERROR_RET(void(), "{}", file.error());
    }

    auto result = Toolchain::query(
        {"g++", "-std=c++23", "-resource-dir", resource_dir().data(), "-xc++", file->c_str()});
    ASSERT_TRUE(result.has_value());

    ASSERT_TRUE(result->size() > 2);
    ASSERT_EQ((*result)[1], "-cc1"sv);

    CompilationParams params;
    for(auto& arg: *result) {
        params.arguments.push_back(arg.c_str());
    }
    params.add_remapped_file(file->c_str(), R"(
            #include <print>
            int main() {
                std::println("Hello world!");
                return 0;
            }
        )");

    auto unit = compile(params);
    ASSERT_TRUE(unit.completed());
    ASSERT_TRUE(unit.diagnostics().empty());
};

TEST_CASE(MSVC, skip = !CIEnvironment) {
    // TODO: add MSVC toolchain test when CI provides toolchain.
}

TEST_CASE(Clang, skip = !CIEnvironment) {
    auto file = fs::createTemporaryFile("clice", "cpp");
    if(!file) {
        LOG_ERROR_RET(void(), "{}", file.error());
    }

    auto result = Toolchain::query(
        {"clang++", "-std=c++23", "-resource-dir", resource_dir().data(), "-xc++", file->c_str()});
    ASSERT_TRUE(result.has_value());

    ASSERT_TRUE(result->size() > 2);
    ASSERT_EQ((*result)[1], "-cc1"sv);

    CompilationParams params;
    for(auto& arg: *result) {
        params.arguments.push_back(arg.c_str());
    }
    params.add_remapped_file(file->c_str(), R"(
            #include <print>
            int main() {
                std::println("Hello world!");
                return 0;
            }
        )");

    auto unit = compile(params);
    ASSERT_TRUE(unit.completed());
    ASSERT_TRUE(unit.diagnostics().empty());
};

TEST_CASE(Zig, skip = !CIEnvironment) {
    // TODO: add Zig toolchain test when available in CI.
}

TEST_CASE(InitiallyEmpty) {
    Toolchain tc;
    EXPECT_FALSE(tc.has_cache());
}

TEST_CASE(KeyIgnoresUserContent) {
    Toolchain tc;
    std::vector<const char*> base = {"clang++", "-std=c++23"};
    std::vector<const char*> with_user = {"clang++",
                                          "-std=c++23",
                                          "-I/usr/include",
                                          "-DFOO=1",
                                          "-include",
                                          "foo.h",
                                          "-isystem",
                                          "/opt/include"};
    EXPECT_EQ(tc.cache_key("/tmp/a.cpp", base), tc.cache_key("/tmp/a.cpp", with_user));
}

TEST_CASE(KeyTracksSemantics) {
    Toolchain tc;
    std::vector<const char*> base = {"clang++", "-std=c++23"};
    auto key = tc.cache_key("/tmp/a.cpp", base);

    std::vector<const char*> driver = {"g++", "-std=c++23"};
    EXPECT_NE(key, tc.cache_key("/tmp/a.cpp", driver));

    std::vector<const char*> target = {"clang++", "-std=c++23", "--target=aarch64-linux-gnu"};
    EXPECT_NE(key, tc.cache_key("/tmp/a.cpp", target));

    std::vector<const char*> lang = {"clang++", "-std=c++23", "-x", "c"};
    EXPECT_NE(key, tc.cache_key("/tmp/a.cpp", lang));

    EXPECT_NE(key, tc.cache_key("/tmp/a.c", base));

    // Any non-user-content flag affects the key, not just toolchain options.
    std::vector<const char*> semantic = {"clang++", "-std=c++23", "-fno-exceptions"};
    EXPECT_NE(key, tc.cache_key("/tmp/a.cpp", semantic));
}

TEST_CASE(ResolveEmptyFlags) {
    Toolchain tc;
    CompileCommand cmd;
    cmd.source_file = "main.cpp";
    EXPECT_FALSE(tc.resolve(cmd).has_value());
    EXPECT_FALSE(tc.has_cache());
}

TEST_CASE(QueryEmptyArgs) {
    EXPECT_FALSE(Toolchain::query({}).has_value());
}

TEST_CASE(QueryMissingDriver) {
    EXPECT_FALSE(Toolchain::query({"clice-nonexistent-driver"}).has_value());
}

TEST_CASE(WarmSkipsEmptyFlags) {
    Toolchain tc;
    CompileCommand cmd;
    cmd.source_file = "main.cpp";
    llvm::SmallVector<CompileCommand> cmds = {cmd};
    tc.warm(cmds);
    EXPECT_FALSE(tc.has_cache());
    EXPECT_EQ(tc.cache_size(), std::size_t(0));
}

TEST_CASE(ResolveKeepsSemanticFlags, skip = !CIEnvironment) {
    auto file = fs::createTemporaryFile("clice", "cpp");
    if(!file) {
        LOG_ERROR_RET(void(), "{}", file.error());
    }

    CompileCommand cmd;
    cmd.resolved.flags = {"clang++", "-std=c++23", "-fms-extensions", "-Wno-everything"};
    cmd.source_file = file->c_str();

    Toolchain tc;
    ASSERT_TRUE(tc.resolve(cmd).has_value());
    EXPECT_TRUE(cmd.resolved.is_cc1);

    // Semantic flags must survive resolution to cc1 (they were dropped when
    // the query only forwarded toolchain options).
    bool has_ms_extensions = false;
    bool has_wno_everything = false;
    for(auto* arg: cmd.to_argv()) {
        if(arg == "-fms-extensions"sv)
            has_ms_extensions = true;
        if(arg == "-Wno-everything"sv)
            has_wno_everything = true;
    }
    EXPECT_TRUE(has_ms_extensions);
    EXPECT_TRUE(has_wno_everything);
}

TEST_CASE(Resolve, skip = !CIEnvironment) {
    auto file = fs::createTemporaryFile("clice", "cpp");
    if(!file) {
        LOG_ERROR_RET(void(), "{}", file.error());
    }

    CompileCommand cmd;
    std::vector<const char*> flags = {"clang++", "-std=c++23", "-I/usr/include", "-DFOO=1"};
    cmd.resolved.flags = std::move(flags);
    cmd.source_file = file->c_str();

    Toolchain tc;
    auto ok = tc.resolve(cmd);
    ASSERT_TRUE(ok.has_value());
    EXPECT_TRUE(tc.has_cache());
    EXPECT_TRUE(cmd.resolved.is_cc1);

    auto argv = cmd.to_argv();
    bool has_cc1 = false;
    bool has_include = false;
    bool has_define = false;
    bool has_main_file = false;
    for(std::size_t i = 0; i < argv.size(); ++i) {
        if(argv[i] == "-cc1"sv)
            has_cc1 = true;
        if(argv[i] == "-I"sv && i + 1 < argv.size() && argv[i + 1] == "/usr/include"sv)
            has_include = true;
        if(argv[i] == "-D"sv && i + 1 < argv.size() && argv[i + 1] == "FOO=1"sv)
            has_define = true;
        if(argv[i] == "-main-file-name"sv)
            has_main_file = true;
    }
    EXPECT_TRUE(has_cc1);
    EXPECT_TRUE(has_include);
    EXPECT_TRUE(has_define);
    EXPECT_TRUE(has_main_file);
}

TEST_CASE(Warm, skip = !CIEnvironment) {
    auto file1 = fs::createTemporaryFile("clice", "cpp");
    auto file2 = fs::createTemporaryFile("clice", "cpp");
    if(!file1 || !file2) {
        LOG_ERROR_RET(void(), "failed to create temp files");
    }

    CompileCommand cmd1;
    cmd1.resolved.flags = {"clang++", "-std=c++23"};
    cmd1.source_file = file1->c_str();

    CompileCommand cmd2;
    cmd2.resolved.flags = {"clang++", "-std=c++23"};
    cmd2.source_file = file2->c_str();

    CompileCommand cmd3;
    cmd3.resolved.flags = {"clang++", "-std=c++17"};
    cmd3.source_file = file1->c_str();

    Toolchain tc;
    llvm::SmallVector<CompileCommand> cmds = {cmd1, cmd2, cmd3};
    tc.warm(cmds);
    EXPECT_TRUE(tc.has_cache());

    // After warm, resolve should hit cache (no subprocess).
    auto ok = tc.resolve(cmd1);
    ASSERT_TRUE(ok.has_value());
    EXPECT_TRUE(cmd1.resolved.is_cc1);
}

};  // TEST_SUITE(ToolchainTests)
}  // namespace
}  // namespace clice::testing
