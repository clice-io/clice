#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/IntrusiveRefCntPtr.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/VirtualFileSystem.h"

namespace clice {

struct ScanResult {
    /// Module name (empty if not a module unit).
    std::string module_name;

    /// Whether this is an interface unit (has `export module`).
    bool is_interface_unit = false;

    /// Whether module declaration is inside conditional directive,
    /// signaling caller should fall back to scan_with_preprocessor.
    bool need_preprocess = false;

    /// Include file names (spelling without <>/quotes, e.g. "vector", "foo/bar.h").
    /// From lexer scan these are the raw header names;
    /// from preprocessor scan these are resolved file paths.
    std::vector<std::string> includes;

    /// Dependent module names (only populated by scan_with_preprocessor).
    std::vector<std::string> modules;
};

/// Quick lexer-based scan for module name and include file names.
/// If module declaration is inside #if/#ifdef, sets need_preprocess=true
/// and module_name will be empty.
ScanResult scan(llvm::StringRef content);

/// Full preprocessing-based scan. Uses hooked VFS to rapidly traverse
/// include chains. Returns module name, resolved include paths, and
/// module dependencies.
ScanResult scan_with_preprocessor(llvm::ArrayRef<const char*> arguments,
                                  llvm::StringRef directory,
                                  bool arguments_from_database,
                                  llvm::IntrusiveRefCntPtr<llvm::vfs::FileSystem> vfs = nullptr);

/// Compute preamble bound (moved from compile/preamble).
std::uint32_t compute_preamble_bound(llvm::StringRef content);
std::vector<std::uint32_t> compute_preamble_bounds(llvm::StringRef content);

}  // namespace clice
