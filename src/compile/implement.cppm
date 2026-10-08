module;

#include "modules/prelude.h"

module clice:compile.implement;

import :compile.compilation;
import :compile.compilation_unit;
import :compile.diagnostic;
import :compile.identity;
import :compile.semantics;
import :compile.tokens;

namespace clice::tidy {

using namespace clang::tidy;

bool is_registered_tidy_check(llvm::StringRef check);

std::optional<bool> is_fast_tidy_check(llvm::StringRef check);

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

    /// See TidyParams::batch.
    bool batch = false;

    /// The check names the unit's diagnostics borrow.
    llvm::BumpPtrAllocator allocator;
    llvm::StringSaver names{allocator};

    ClangTidyChecker(std::unique_ptr<ClangTidyOptionsProvider> provider,
                     clang::ast_matchers::MatchFinder::MatchFinderOptions options);

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

    /// See CompilationParams::workspace.
    std::string workspace;

    llvm::StringMap<std::unique_ptr<llvm::MemoryBuffer>> remapped_buffers;

    /// See CompilationParams::synthesized.
    llvm::StringMap<SynthesizedOrigin> synthesized;

    /// Every place a failed include or `__has_include` lookup looked, see
    /// CompilationUnitRef::absent.
    llvm::StringSet<> absent;

    /// Memo of CompilationUnitRef::from_context.
    llvm::DenseMap<clang::FileID, bool> context_files;

    /// Memo of the host path CompilationUnitRef::host_source compares
    /// against.
    std::optional<std::string> host;

    /// The frontend action used to build the unit.
    std::unique_ptr<clang::FrontendAction> action;

    /// Compiler instance, responsible for performing the actual compilation and managing the
    /// lifecycle of all objects during the compilation process.
    std::unique_ptr<clang::CompilerInstance> instance;

    /// The template resolver used to resolve dependent name.
    std::optional<types::TemplateResolver> resolver;

    /// Lazily built semantic map, see CompilationUnitRef::semantics().
    std::unique_ptr<Semantics> semantics_cache;

    /// The tokens a Content compile collects for the features.
    std::optional<TokenMap> tokens;

    /// All directive information collected during the preprocessing.
    llvm::DenseMap<clang::FileID, Directive> directives;

    /// Cache for file path. It is used to avoid multiple file path lookup.
    llvm::DenseMap<clang::FileEntryRef, llvm::StringRef> path_cache;

    /// Lazily built, see CompilationUnitRef::entity().
    std::unique_ptr<EntityTable> entities;

    /// Cache for line starts of the main file.
    std::vector<std::uint32_t> line_starts_cache;

    /// Cache for the main file's non-ASCII lines; an ASCII file has none,
    /// so only nullopt means not built yet.
    std::optional<std::vector<std::uint64_t>> non_ascii_cache;

    llvm::BumpPtrAllocator path_storage;

    std::vector<Diagnostic> diagnostics;

    std::vector<clang::Decl*> top_level_decls;

    std::unique_ptr<tidy::ClangTidyChecker> checker;

    std::chrono::milliseconds build_at;

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

    // Must be called before EndSourceFile because the ast context can be destroyed later.
    void run_tidy();

    CompilationStatus run_clang(this Self& self,
                                CompilationParams& params,
                                std::unique_ptr<clang::FrontendAction> action,
                                llvm::function_ref<void(clang::CompilerInstance&)> before_execute);
};

}  // namespace clice
