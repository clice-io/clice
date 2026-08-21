#include "test/test.h"
#include "compile/compilation.h"

#include "clang-tidy/ClangTidyModuleRegistry.h"

namespace clice::testing {
namespace {

TEST_SUITE(ClangTidy) {

TEST_CASE(ModulesLinked) {
    llvm::StringSet<> expected = {
        "abseil-module",      "altera-module",
        "android-module",     "boost-module",
        "bugprone-module",    "cert-module",
        "concurrency-module", "cppcoreguidelines-module",
        "darwin-module",      "fuchsia-module",
        "google-module",      "hicpp-module",
        "linux-module",       "llvm-module",
        "llvmlibc-module",    "misc-module",
        "modernize-module",   "mpi-module",
        "objc-module",        "openmp-module",
        "performance-module", "portability-module",
        "readability-module", "zircon-module",
    };

    for(auto& entry: clang::tidy::ClangTidyModuleRegistry::entries()) {
        expected.erase(entry.getName());
    }
    // Debug links shared libs (all 24 modules); Release uses static libs
    // where --gc-sections strips mpi-module (CLANG_TIDY_ENABLE_STATIC_ANALYZER=0).
    expected.erase("mpi-module");
    ASSERT_TRUE(expected.empty());
}

TEST_CASE(Tidy) {
    auto vfs = llvm::makeIntrusiveRefCnt<TestVFS>();
    vfs->add("main.cpp", "int main() { return 0 }");

    std::string main_path = TestVFS::path("main.cpp");
    CompilationParams params;
    params.clang_tidy = true;
    params.vfs = vfs;
    params.arguments = {"clang++", "-ffreestanding", "-Xclang", "-undef", main_path.c_str()};
    auto unit = compile(params);
    ASSERT_TRUE(unit.completed());
    ASSERT_FALSE(unit.diagnostics().empty());
}

};  // TEST_SUITE(ClangTidy)
}  // namespace
}  // namespace clice::testing
