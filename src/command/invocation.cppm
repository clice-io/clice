module clice:command.invocation;

import :support.prelude;

namespace clice {

/// The clang invocation of a compilation-database command — the one
/// reading every consumer of an entry (the compile, the dependency scans)
/// agrees on. A `-cc1` argument list is taken as already expanded; a
/// driver command line goes through the driver without probing for
/// precompiled headers (clang would otherwise turn `-include` into
/// `-include-pch`, see clangd#856). The entry's directory governs
/// relative paths the way an explicit -working-directory does: an
/// explicit one wins, but its own relative value resolves from the
/// entry's directory, like the real driver run from there: `vfs` is moved
/// there, so it must be one the caller owns, never the process's (whose
/// working directory is every thread's). Null when clang rejects the
/// arguments; `diagnostics` received what it said.
std::unique_ptr<clang::CompilerInvocation>
    create_compiler_invocation(llvm::ArrayRef<const char*> arguments,
                               llvm::StringRef directory,
                               llvm::IntrusiveRefCntPtr<llvm::vfs::FileSystem> vfs,
                               llvm::IntrusiveRefCntPtr<clang::DiagnosticsEngine> diagnostics);

/// What a compile and the build of a named module's PCM must agree on for
/// the compile to read it: what clang checks when it loads a module file
/// (ASTReader's checkLanguageOptions and checkTargetOptions), never the
/// macros.
struct ModuleCompatibility {
    /// The language options clang requires to match, with the target triple
    /// and ABI, hashed.
    std::uint64_t language = 0;

    /// The target features as written, sorted: an importer may add to the
    /// module's.
    std::vector<std::string> features;

    /// Whether a compile with these reads a PCM built with `module`.
    bool reads(const ModuleCompatibility& module) const {
        return language == module.language && ranges::includes(features, module.features);
    }
};

ModuleCompatibility module_compatibility(const clang::CompilerInvocation& invocation);

}  // namespace clice
