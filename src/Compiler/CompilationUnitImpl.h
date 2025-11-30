#pragma once

#include "Compiler/CompilationUnit.h"
#include "Compiler/Diagnostic.h"

#include "clang/Frontend/CompilerInstance.h"
#include "clang/Frontend/FrontendActions.h"

namespace clice {

struct CompilationUnit::Impl {
    /// The interested file ID.
    clang::FileID interested;

    clang::SourceManager& src_mgr;

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

    std::shared_ptr<std::vector<Diagnostic>> diagnostics;

    std::vector<clang::Decl*> top_level_decls;

    std::chrono::milliseconds build_at;

    std::chrono::milliseconds build_duration;
};

}  // namespace clice
