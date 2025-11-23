#include "Compiler/Compilation.h"
#include "Test/Test.h"
#include "Compiler/Toolchain.h"
#include "llvm/Support/Allocator.h"
#include "llvm/Support/StringSaver.h"
#include "clang/Driver/Driver.h"
#include "Support/Logging.h"

namespace clice::testing {

namespace {

suite<"Toolchain"> suite = [] {
    auto expect_family = [](llvm::StringRef name,
                            toolchain::CompilerFamily family,
                            std::source_location location = std::source_location::current()) {
        expect(eq(refl::enum_name(toolchain::driver_family(name)), refl::enum_name((family))),
               location);
    };

    test("Family") = [&] {
        using enum toolchain::CompilerFamily;

        expect_family("gcc", GCC);
        expect_family("g++", GCC);
        expect_family("x86_64-linux-gnu-g++-14", GCC);
        expect_family("arm-none-eabi-gcc", GCC);

        expect_family("clang", Clang);
        expect_family("clang++", Clang);
        expect_family("clang.exe", Clang);
        expect_family("clang++.exe", Clang);
        expect_family("clang-20", Clang);
        expect_family("clang-20.exe", Clang);
        expect_family("clang-cl", ClangCL);
        expect_family("clang-cl-20", ClangCL);
        expect_family("clang-cl-20.exe", ClangCL);

        expect_family("cl.exe", MSVC);

        expect_family("zig", Zig);
        expect_family("zig.exe", Zig);
    };

    using namespace std::string_view_literals;

    skip_unless(CIEnvironment && (Windows || Linux)) / test("GCC") = [] {
        auto file = fs::createTemporaryFile("clice", "cpp");
        if(!file) {
            LOG_ERROR_RET(void(), "{}", file.error());
        }

        llvm::BumpPtrAllocator a;
        llvm::StringSaver s(a);
        auto arguments = toolchain::query_toolchain({
            .arguments = {"g++",
                          "-std=c++23", "-resource-dir",
                          fs::resource_dir.c_str(),
                          "-xc++", file->c_str()},
            .callback = [&](const char* str) { return s.save(str).data(); }
        });

        expect(arguments.size() > 2);
        expect(eq(arguments[1], "-cc1"sv));

        CompilationParams params;
        params.arguments_from_database = true;
        params.arguments = arguments;
        params.add_remapped_file(file->c_str(), R"(
            #include <print>
            int main() {
                std::println("Hello world!");
                return 0;
            }
        )");

        auto unit = compile(params);
        expect(unit && unit->diagnostics().empty());
    };

    skip_unless(CIEnvironment) / test("MSVC") = [] {

    };

    skip_unless(CIEnvironment) / test("Clang") = [] {
        auto file = fs::createTemporaryFile("clice", "cpp");
        if(!file) {
            LOG_ERROR_RET(void(), "{}", file.error());
        }

        llvm::BumpPtrAllocator a;
        llvm::StringSaver s(a);
        auto arguments = toolchain::query_toolchain({
            .arguments = {"clang++",
                          "-std=c++23", "-resource-dir",
                          fs::resource_dir.c_str(),
                          "-xc++", file->c_str()},
            .callback = [&](const char* str) { return s.save(str).data(); }
        });

        expect(arguments.size() > 2);
        expect(eq(arguments[1], "-cc1"sv));

        CompilationParams params;
        params.arguments_from_database = true;
        params.arguments = arguments;
        params.add_remapped_file(file->c_str(), R"(
            #include <print>
            int main() {
                std::println("Hello world!");
                return 0;
            }
        )");

        auto unit = compile(params);
        expect(unit && unit->diagnostics().empty());
    };

    skip_unless(CIEnvironment) / test("Zig") = [] {

    };
};

}  // namespace

}  // namespace clice::testing
