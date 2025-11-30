#include "Test/Test.h"
#include "Compiler/Compilation.h"

#include "llvm/Support/ToolOutputFile.h"

namespace clice::testing {

namespace {

using namespace std::literals;

auto buildPCM = [](llvm::StringRef file, llvm::StringRef code) {
    llvm::SmallString<128> outPath;
    fs::createUniquePath(llvm::Twine(file) + "%%%%%%.pcm", outPath, true);

    std::string path = file.str();

    CompilationParams params;
    params.output_file = outPath;
    params.arguments = {
        "clang++",
        "-std=c++20",
        "-xc++",
        path.c_str(),
    };
    params.add_remapped_file(file, code);
    params.add_remapped_file("./test.h", "export int foo2();");

    PCMInfo pcm;
    if(!compile(params, pcm)) {
        llvm::errs() << "Failed to build PCM\n";
        std::abort();
    }

    return pcm;
};

auto scan = [](llvm::StringRef content) {
    CompilationParams params;
    params.arguments = {
        "clang++",
        "-std=c++20",
        "-xc++",
        "main.ixx",
    };
    params.add_remapped_file("main.ixx", content);
    params.add_remapped_file("./test.h", "export module A");
    auto info = scanModule(params);
    if(!info) {
        /// std::println("Fail to scan module: {}", info.error());
        std::abort();
    }
    return std::move(*info);
};

TEST_SUITE(Module) {

TEST_CASE(Scan) {
    /// Simple case.
    const char* content = R"(
export module A;
import B;
    )";
    auto info = scan(content);
    ASSERT_TRUE(info.isInterfaceUnit);
    ASSERT_EQ(info.name, "A"sv);
    ASSERT_EQ(info.mods.size(), 1U);
    ASSERT_EQ(info.mods[0], "B"sv);

    /// FIXME: Fix standard library search path(resource dir).

    /// With global module fragment and private module fragment.
    ///    content = R"(
    /// module;
    /// #include <iostream>
    /// export module A;
    /// import B;
    /// import C;
    /// module : private;
    ///)";
    ///    info = scan(content);
    ///    expect(that % info.isInterfaceUnit == true);
    ///    expect(that % info.name == "A");
    ///    expect(that % info.mods.size() == 2);
    ///    expect(that % info.mods[0] == "B");
    ///    expect(that % info.mods[1] == "C");

    /// With module partition.
    ///    content = R"(
    /// module;
    /// #include <iostream>
    /// export module A:B;
    /// import B;
    /// import C;
    /// module : private;
    ///)";
    ///    info = scan(content);
    ///    expect(that % info.isInterfaceUnit == true);
    ///    expect(that % info.name == "A:B");
    ///    expect(that % info.mods.size() == 2);
    ///    expect(that % info.mods[0] == "B");
    ///    expect(that % info.mods[1] == "C");

    content = R"(
module A;
import B;
import C;
)";
    info = scan(content);
    ASSERT_FALSE(info.isInterfaceUnit);
    ASSERT_EQ(info.name, "A"sv);
    ASSERT_EQ(info.mods.size(), 2U);
    ASSERT_EQ(info.mods[0], "B"sv);
    ASSERT_EQ(info.mods[1], "C"sv);
};

TEST_CASE(Normal) {
    const char* content = R"(
export module A;
)";
    auto pcm = buildPCM("A.ixx", content);
    // expect(that % pcm.isInterfaceUnit == true);
    // expect(that % pcm.name == "A");
    // expect(that % pcm.mods.size() == 0);
};

};  // TEST_SUITE(Module)

// TEST(Module, ScanModuleName) {
//     CompilationParams params;
//
//     /// Test module name not in condition directive.
//     params.content = "export module A;";
//     ASSERT_EQ(scanModuleName(params), "A");
//
//     params.content = "export module A.B.C.D;";
//     ASSERT_EQ(scanModuleName(params), "A.B.C.D");
//
//     params.content = "export module A:B;";
//     ASSERT_EQ(scanModuleName(params), "A:B");
//
//     params.content = R"(
// module;
// #ifdef TEST
// #include <iostream>
// #endif
// export module A;
//)";
//     ASSERT_EQ(scanModuleName(params), "A");
//
//     /// Test non-module interface unit.
//     params.content = "module A;";
//     ASSERT_EQ(scanModuleName(params), "");
//
//     params.content = "";
//     ASSERT_EQ(scanModuleName(params), "");
//
//     /// Test module name in condition directive.
//     params.content = R"(
// #ifdef TEST
// export module A;
// #else
// export module B;
// #endif
//)";
//     params.srcPath = "main.cppm";
//     params.command = "clang++ -std=c++20 -x c++ main.cppm -DTEST";
//     ASSERT_EQ(scanModuleName(params), "A");
//
//     params.command = "clang++ -std=c++20 -x c++ main.cppm";
//     ASSERT_EQ(scanModuleName(params), "B");
// }

}  // namespace

}  // namespace clice::testing
