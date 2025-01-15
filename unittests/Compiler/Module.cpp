#include "Test/Test.h"
#include "Compiler/Compiler.h"
#include "llvm/Support/ToolOutputFile.h"

namespace clice::testing {

namespace {

PCMInfo buildPCM(llvm::StringRef file, llvm::StringRef code) {
    llvm::SmallString<128> outPath;
    fs::createUniquePath(llvm::Twine(file) + "%%%%%%.pcm", outPath, true);

    CompilationParams params;
    params.content = code;
    params.srcPath = file;
    params.outPath = outPath;
    params.command = "clang++ -std=c++20 -x c++ " + file.str();
    params.remappedFiles.emplace_back("./test.h", "export int foo2();");

    PCMInfo pcm;
    if(!compile(params, pcm)) {
        llvm::errs() << "Failed to build PCM\n";
        std::terminate();
    }

    return pcm;
}

ModuleInfo scan(llvm::StringRef content) {
    CompilationParams params;
    params.content = content;
    params.srcPath = "main.ixx";
    params.command = "clang++ -std=c++20 -x c++ main.ixx";
    params.remappedFiles.emplace_back("./test.h", "export module A");
    auto info = scanModule(params);
    if(!info) {
        llvm::errs() << "Failed to scan module\n";
        std::terminate();
    }
    return std::move(*info);
}

TEST(Module, Scan) {
    /// Simple case.
    const char* content = R"(
export module A;
import B;    
    )";
    auto info = scan(content);
    ASSERT_EQ(info.isInterfaceUnit, true);
    ASSERT_EQ(info.name, "A");
    ASSERT_EQ(info.mods.size(), 1);
    ASSERT_EQ(info.mods[0], "B");

    /// With global module fragment and private module fragment.
    content = R"(
module;
#include <iostream>
export module A;
import B;    
import C;
module : private;
)";
    info = scan(content);
    ASSERT_EQ(info.isInterfaceUnit, true);
    ASSERT_EQ(info.name, "A");
    ASSERT_EQ(info.mods.size(), 2);
    ASSERT_EQ(info.mods[0], "B");
    ASSERT_EQ(info.mods[1], "C");

    /// With module partition.
    content = R"(
module;
#include <iostream>
export module A:B;
import B;    
import C;
module : private;
)";
    info = scan(content);
    ASSERT_EQ(info.isInterfaceUnit, true);
    ASSERT_EQ(info.name, "A:B");
    ASSERT_EQ(info.mods.size(), 2);
    ASSERT_EQ(info.mods[0], "B");
    ASSERT_EQ(info.mods[1], "C");

    content = R"(
module A;
import B;    
import C;
)";
    info = scan(content);
    ASSERT_EQ(info.isInterfaceUnit, false);
    ASSERT_EQ(info.name, "A");
    ASSERT_EQ(info.mods.size(), 2);
    ASSERT_EQ(info.mods[0], "B");
    ASSERT_EQ(info.mods[1], "C");
}

TEST(Module, Normal) {
    const char* content = R"(
export module A;
)";
    auto pcm = buildPCM("A.ixx", content);
    // ASSERT_EQ(pcm.isInterfaceUnit, true);
    // ASSERT_EQ(pcm.name, "A");
    // ASSERT_EQ(pcm.mods.size(), 0);
}

TEST(Module, ScanModuleName) {
    CompilationParams params;

    /// Test module name not in condition directive.
    params.content = "export module A;";
    ASSERT_EQ(scanModuleName(params), "A");

    params.content = "export module A.B.C.D;";
    ASSERT_EQ(scanModuleName(params), "A.B.C.D");

    params.content = "export module A:B;";
    ASSERT_EQ(scanModuleName(params), "A:B");

    params.content = R"(
module;
#ifdef TEST
#include <iostream>
#endif
export module A;
)";
    ASSERT_EQ(scanModuleName(params), "A");

    /// Test non-module interface unit.
    params.content = "module A;";
    ASSERT_EQ(scanModuleName(params), "");

    params.content = "";
    ASSERT_EQ(scanModuleName(params), "");

    /// Test module name in condition directive.
    params.content = R"(
#ifdef TEST
export module A;
#else
export module B;
#endif
)";
    params.srcPath = "main.cppm";
    params.command = "clang++ -std=c++20 -x c++ main.cppm -DTEST";
    ASSERT_EQ(scanModuleName(params), "A");

    params.command = "clang++ -std=c++20 -x c++ main.cppm";
    ASSERT_EQ(scanModuleName(params), "B");
}

}  // namespace

}  // namespace clice
