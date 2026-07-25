module;

#include "clang-tidy/ClangTidyCheck.h"
#include "clang-tidy/ClangTidyModuleRegistry.h"
#include "clang-tidy/ClangTidyOptions.h"

export module clice.compile:tidy_checker;

// No `import stdlib`: the clang-tidy headers in the global module fragment pull
// std names textually. Like the clang/llvm wrappers, this unit stays out of any
// stdlib-importing graph — so it does not import clice partitions either, and
// clice::Diagnostic is forward-declared instead.
import llvm;
import clang;

namespace clice {

struct Diagnostic;

}

export namespace clice::tidy {

using namespace clang::tidy;

bool is_registered_tidy_check(llvm::StringRef check);

std::optional<bool> is_fast_tidy_check(llvm::StringRef check);

struct TidyParams {};

class ClangTidyChecker;

/// Configure to run clang-tidy on the given file.
std::unique_ptr<ClangTidyChecker> configure(clang::CompilerInstance& instance,
                                            const TidyParams& params);

class ClangTidyChecker {
public:
    /// The context of the clang-tidy checker.
    ClangTidyContext context;

    /// The instances of checks that are enabled for the current Language.
    std::vector<std::unique_ptr<ClangTidyCheck>> checks;

    /// The match finder to run clang-tidy on ASTs.
    clang::ast_matchers::MatchFinder finder;

    ClangTidyChecker(std::unique_ptr<ClangTidyOptionsProvider> provider);

    clang::DiagnosticsEngine::Level adjust_level(clang::DiagnosticsEngine::Level level,
                                                 const clang::Diagnostic& diag);
    void adjust_diag(Diagnostic& diag);
};

}  // namespace clice::tidy
