#include "Test/Test.h"
#include "Compiler/Compilation.h"
#include "Compiler/Tidy.h"

namespace clice::testing {
namespace {

TEST_SUITE(ClangTidy) {

TEST_CASE(FastCheck) {
    ASSERT_TRUE(tidy::is_fast_tidy_check("readability-misleading-indentation"));
    ASSERT_TRUE(tidy::is_fast_tidy_check("bugprone-unused-return-value"));

    // clangd/unittests/TidyProviderTests.cpp
    ASSERT_TRUE(tidy::is_fast_tidy_check("misc-const-correctness"));
    ASSERT_TRUE(tidy::is_fast_tidy_check("bugprone-suspicious-include"));
    ASSERT_EQ(tidy::is_fast_tidy_check("replay-preamble-check"), std::nullopt);
}

TEST_CASE(Tidy) {
    CompilationParams params;
    params.clang_tidy = true;
    params.arguments = {"clang++", "main.cpp"};
    params.add_remapped_file("main.cpp", "int main() { return 0 }");
    auto unit = compile(params);
    ASSERT_TRUE(unit.has_value());
    ASSERT_FALSE(unit->diagnostics().empty());
}

};  // TEST_SUITE(ClangTidy)
}  // namespace
}  // namespace clice::testing
