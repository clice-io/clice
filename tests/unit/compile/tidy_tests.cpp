#include "test/test.h"
#include "compile/compilation.h"

namespace clice::testing {
namespace {

TEST_SUITE(ClangTidy) {

TEST_CASE(FastCheck) {
    // ASSERT_TRUE(tidy::is_fast_tidy_check("readability-misleading-indentation"));
    // ASSERT_TRUE(tidy::is_fast_tidy_check("bugprone-unused-return-value"));
    //
    // // clangd/unittests/TidyProviderTests.cpp
    // ASSERT_TRUE(tidy::is_fast_tidy_check("misc-const-correctness"));
    // ASSERT_TRUE(tidy::is_fast_tidy_check("bugprone-suspicious-include"));
    // ASSERT_EQ(tidy::is_fast_tidy_check("replay-preamble-check"), std::nullopt);
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
