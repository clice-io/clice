export module clice:compile.implement;

import stdlib;
import llvm;
import clang;
import :compile.compilation_unit;
import :compile.diagnostic;
import :compile.directive;
import :compile.tidy_checker;
import :semantic.resolver;

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

    /// Cache for file path. It is used to avoid multiple file path lookup.
    llvm::DenseMap<clang::FileEntryRef, llvm::StringRef> path_cache;

    /// Cache for symbol id.
    llvm::DenseMap<const void*, std::uint64_t> symbol_hash_cache;

    /// Cache for line starts of the interested file.
    std::vector<std::uint32_t> line_starts_cache;

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
