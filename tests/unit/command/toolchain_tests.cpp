#include "test/test.h"
#include "command/argument_parser.h"
#include "command/command.h"
#include "command/toolchain.h"
#include "compile/compilation.h"
#include "support/logging.h"

#include "llvm/Support/Allocator.h"
#include "llvm/Support/StringSaver.h"

namespace clice::testing {
namespace {

using namespace std::string_view_literals;

TEST_SUITE(Toolchain) {

void EXPECT_FAMILY(llvm::StringRef name, toolchain::CompilerFamily family) {
    ASSERT_EQ(toolchain::driver_family(name), family);
};

TEST_CASE(Family) {
    using enum toolchain::CompilerFamily;

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

    llvm::BumpPtrAllocator a;
    llvm::StringSaver s(a);
    auto arguments = toolchain::query_toolchain({
        .arguments =
            {"g++", "-std=c++23", "-resource-dir", resource_dir().data(), "-xc++", file->c_str()},
        .callback = [&](const char* str) { return s.save(str).data(); }
    });

    ASSERT_TRUE(arguments.size() > 2);
    ASSERT_EQ(arguments[1], "-cc1"sv);

    CompilationParams params;

    params.arguments = arguments;
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

    llvm::BumpPtrAllocator a;
    llvm::StringSaver s(a);
    auto arguments = toolchain::query_toolchain({
        .arguments = {"clang++",
                      "-std=c++23", "-resource-dir",
                      resource_dir().data(),
                      "-xc++", file->c_str()},
        .callback = [&](const char* str) { return s.save(str).data(); }
    });

    ASSERT_TRUE(arguments.size() > 2);
    ASSERT_EQ(arguments[1], "-cc1"sv);

    CompilationParams params;

    params.arguments = arguments;
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

};  // TEST_SUITE(Toolchain)
}  // namespace
}  // namespace clice::testing
