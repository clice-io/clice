#include "Test/Test.h"
#include "Compiler/Toolchain.h"
#include "llvm/Support/Allocator.h"
#include "llvm/Support/StringSaver.h"
#include "clang/Driver/Driver.h"

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
        expect_family("clang-20", Clang);
        expect_family("clang-20.exe", Clang);
        expect_family("clang-cl", ClangCL);
        expect_family("clang-cl-20", ClangCL);
        expect_family("clang-cl-20.exe", ClangCL);

        expect_family("cl.exe", MSVC);

        expect_family("zig", Zig);
        expect_family("zig.exe", Zig);
    };

    test("GCC") = [] {
        llvm::BumpPtrAllocator a;
        llvm::StringSaver s(a);
        auto arguments = toolchain::query_toolchain({
            .arguments = {"g++", "-xc++", "/dev/null"},
            .callback = [&](const char* str) { return s.save(str).data(); }
        });

        for(auto arg: arguments) {
            std::println("{}", arg);
        }
    };

    test("MSVC") = [] {

    };

    test("Clang") = [] {

    };

    test("Zig") = [] {

    };
};

}  // namespace

}  // namespace clice::testing
