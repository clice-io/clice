#include "test/test.h"
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

    ASSERT_TRUE(result.size() > 2);
    ASSERT_EQ(result[1], "-cc1"sv);

    CompilationParams params;
    for(auto& arg: result) {
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

    ASSERT_TRUE(result.size() > 2);
    ASSERT_EQ(result[1], "-cc1"sv);

    CompilationParams params;
    for(auto& arg: result) {
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

};  // TEST_SUITE(ToolchainTests)
}  // namespace
}  // namespace clice::testing
