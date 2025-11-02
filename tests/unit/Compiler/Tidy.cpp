#include "Test/Test.h"
#include "Compiler/Tidy.h"
#include "Compiler/Compilation.h"

namespace clice::testing {

namespace {

suite<"ClangTidy"> clang_tidy = [] {
    test("FaskCheck") = [] {
        expect(that % tidy::is_fast_tidy_check("readability-misleading-indentation"));
        expect(that % tidy::is_fast_tidy_check("bugprone-unused-return-value"));

        // clangd/unittests/TidyProviderTests.cpp
        expect(that % tidy::is_fast_tidy_check("misc-const-correctness") == false);
        expect(that % tidy::is_fast_tidy_check("bugprone-suspicious-include") == true);
        expect(that % tidy::is_fast_tidy_check("replay-preamble-check") == std::nullopt);
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
