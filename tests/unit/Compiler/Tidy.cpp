#include "Test/Test.h"
#include "Compiler/Tidy.h"
#include "Compiler/Compilation.h"

namespace clice::testing {

namespace {

suite<"ClangTidy"> clang_tidy = [] {
    test("FastCheck") = [] {
        expect(tidy::is_fast_tidy_check("readability-misleading-indentation"));
        expect(tidy::is_fast_tidy_check("bugprone-unused-return-value"));

        // clangd/unittests/TidyProviderTests.cpp
        expect(tidy::is_fast_tidy_check("misc-const-correctness") == false);
        expect(tidy::is_fast_tidy_check("bugprone-suspicious-include") == true);
        expect(tidy::is_fast_tidy_check("replay-preamble-check") == std::nullopt);
    };

    test("Tidy") = [] {
        CompilationParams params;
        params.clang_tidy = true;
        params.arguments = {"clang++", "main.cpp"};
        params.add_remapped_file("main.cpp", "int main() { return 0 }");
        auto unit = compile(params);
        expect(unit.has_value());
        expect(!unit->diagnostics().empty());
    };
};

}  // namespace
}  // namespace clice::testing
