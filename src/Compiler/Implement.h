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

struct CompilationUnitRef::Self {
    CompilationKind kind;

    CompilationStatus status;

    std::shared_ptr<std::atomic_bool> stop;

    llvm::StringMap<std::unique_ptr<llvm::MemoryBuffer>> remapped_buffers;

    /// The frontend action used to build the unit.
    std::unique_ptr<clang::FrontendAction> action;

    /// Compiler instance, responsible for performing the actual compilation and managing the
    /// lifecycle of all objects during the compilation process.
    std::unique_ptr<clang::CompilerInstance> instance;

    /// The template resolver used to resolve dependent name.
    std::optional<TemplateResolver> resolver;

    /// Token information collected during the preprocessing.
    std::optional<clang::syntax::TokenBuffer> buffer;

    /// All directive information collected during the preprocessing.
    llvm::DenseMap<clang::FileID, Directive> directives;

    llvm::DenseSet<clang::FileID> all_files;

    /// Cache for file path. It is used to avoid multiple file path lookup.
    llvm::DenseMap<clang::FileID, llvm::StringRef> path_cache;

    /// Cache for symbol id.
    llvm::DenseMap<const void*, std::uint64_t> symbol_hash_cache;

    llvm::BumpPtrAllocator path_storage;

    std::vector<Diagnostic> diagnostics;

    std::vector<clang::Decl*> top_level_decls;

    std::unique_ptr<tidy::ClangTidyChecker> checker;

    std::chrono::milliseconds build_at;
    std::chrono::milliseconds build_duration;

    auto& SM() {
        return instance->getSourceManager();
    }

public:
    ~Self();

    std::unique_ptr<clang::DiagnosticConsumer> create_diagnostic();

    /// create a `clang::CompilerInvocation` for compilation, it set and reset
    /// all necessary arguments and flags for clice compilation.
    std::unique_ptr<clang::CompilerInvocation>
        create_invocation(this Self& self,
                          CompilationParams& params,
                          clang::DiagnosticConsumer* consumer);

    void collect_directives();

    void configure_tidy(tidy::TidyParams tidy_params);

    // Must be called before EndSourceFile because the ast context can be destroyed later.
    void run_tidy();

    CompilationStatus run_clang(this Self& self,
                                CompilationParams& params,
                                std::unique_ptr<clang::FrontendAction> action,
                                llvm::function_ref<void(clang::CompilerInstance&)> before_execute);
};

}  // namespace clice
