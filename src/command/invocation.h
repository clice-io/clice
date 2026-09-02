#pragma once

#include <memory>

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/IntrusiveRefCntPtr.h"
#include "llvm/ADT/StringRef.h"

namespace clang {

class CompilerInvocation;
class DiagnosticsEngine;

}  // namespace clang

namespace llvm::vfs {

class FileSystem;

}  // namespace llvm::vfs

namespace clice {

/// The clang invocation of a compilation-database command — the one
/// reading every consumer of an entry (the compile, the dependency scans)
/// agrees on. A `-cc1` argument list is taken as already expanded; a
/// driver command line goes through the driver without probing for
/// precompiled headers (clang would otherwise turn `-include` into
/// `-include-pch`, see clangd#856). The entry's directory governs
/// relative paths the way an explicit -working-directory does: an
/// explicit one wins, but its own relative value resolves from the
/// entry's directory, like the real driver run from there. Null when
/// clang rejects the arguments; `diagnostics` received what it said.
std::unique_ptr<clang::CompilerInvocation>
    create_compiler_invocation(llvm::ArrayRef<const char*> arguments,
                               llvm::StringRef directory,
                               llvm::IntrusiveRefCntPtr<llvm::vfs::FileSystem> vfs,
                               llvm::IntrusiveRefCntPtr<clang::DiagnosticsEngine> diagnostics);

}  // namespace clice
