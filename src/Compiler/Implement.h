#pragma once

#include "Compiler/CompilationUnit.h"
#include "Compiler/Diagnostic.h"

#include "clang/Frontend/CompilerInstance.h"
#include "clang/Frontend/FrontendActions.h"
#include "clang-tidy/ClangTidyCheck.h"
#include "clang-tidy/ClangTidyModuleRegistry.h"
#include "clang-tidy/ClangTidyOptions.h"

namespace clice::tidy {

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

namespace clice {

constexpr static auto no_hook = [](auto& /*ignore*/) {
};

struct CompilationParams;

enum class BuildStatus {
    Success,

    FailToCreateCompilationInvocation,

    FailToCreateTarget,

    FailToBeginSource,

    FailToExecuteAction,

    Cancelled,
};

struct CompilationUnitRef::Self {
    /// The interested file ID.
    clang::FileID interested;

    std::string error_message;

    BuildStatus status;

    clang::SourceManager* src_mgr;

    /// The frontend action used to build the unit.
    std::unique_ptr<clang::FrontendAction> action;

    /// Compiler instance, responsible for performing the actual compilation and managing the
    /// lifecycle of all objects during the compilation process.
    std::unique_ptr<clang::CompilerInstance> instance;

    /// The template resolver used to resolve dependent name.
    std::optional<TemplateResolver> resolver;

    /// Token information collected during the preprocessing.
    std::optional<clang::syntax::TokenBuffer> buffer;

    /// All diretive information collected during the preprocessing.
    llvm::DenseMap<clang::FileID, Directive> directives;

    llvm::DenseSet<clang::FileID> all_files;

    /// Cache for file path. It is used to avoid multiple file path lookup.
    llvm::DenseMap<clang::FileID, llvm::StringRef> path_cache;

    /// Cache for symbol id.
    llvm::DenseMap<const void*, std::uint64_t> symbol_hash_cache;

    llvm::BumpPtrAllocator path_storage;

    std::vector<Diagnostic> diagnostics;

    std::vector<clang::Decl*> top_level_decls;

    std::chrono::milliseconds build_at;

    std::chrono::milliseconds build_duration;
};

class DiagnosticCollector : public clang::DiagnosticConsumer {
public:
    tidy::ClangTidyChecker* checker = nullptr;
};

std::unique_ptr<DiagnosticCollector> create_diagnostic(CompilationUnitRef unit);

}  // namespace clice
