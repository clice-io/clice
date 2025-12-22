#pragma once

#include <memory>

#include "Compiler/Diagnostic.h"
#include "Compiler/Tidy.h"

#include "clang-tidy/ClangTidyCheck.h"
#include "clang-tidy/ClangTidyModuleRegistry.h"
#include "clang-tidy/ClangTidyOptions.h"

namespace clice::tidy {

using namespace clang::tidy;

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
