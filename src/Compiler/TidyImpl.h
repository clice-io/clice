#pragma once

#include <memory>

#include "clang-tidy/ClangTidyModuleRegistry.h"
#include "clang-tidy/ClangTidyOptions.h"
#include "clang-tidy/ClangTidyCheck.h"

#include "Compiler/Diagnostic.h"
#include "Compiler/Tidy.h"

namespace clice::tidy {

using namespace clang::tidy;

class ClangTidyChecker : public DiagnosticTransform {

public:
    /// The context of the clang-tidy checker.
    ClangTidyContext context;
    /// The instances of checks that are enabled for the current Language.
    std::vector<std::unique_ptr<ClangTidyCheck>> checks;
    /// The match finder to run clang-tidy on ASTs.
    clang::ast_matchers::MatchFinder finder;

    ClangTidyChecker(std::unique_ptr<ClangTidyOptionsProvider> provider);

    clang::DiagnosticsEngine::Level adjust_level(clang::DiagnosticsEngine::Level level,
                                                 const clang::Diagnostic& diag) override;
    void adjust_diag(Diagnostic& diag) override;
};

}  // namespace clice::tidy

namespace clice {}  // namespace clice
