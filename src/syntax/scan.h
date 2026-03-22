#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/IntrusiveRefCntPtr.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/VirtualFileSystem.h"
#include "clang/Lex/DependencyDirectivesScanner.h"

namespace clice {

struct ScanResult {
    /// Module name (empty if not a module unit).
    std::string module_name;

    /// Whether this is an interface unit (has `export module`).
    bool is_interface_unit = false;

    /// Whether module declaration is inside conditional directive,
    /// signaling caller should fall back to preprocessor-based scan.
    bool need_preprocess = false;

    struct IncludeInfo {
        /// Resolved file path (fuzzy/precise scan) or raw header name (lexer scan).
        std::string path;

        /// Whether this include is inside a conditional directive context.
        bool conditional = false;

        /// Whether the included file was not found during resolution.
        bool not_found = false;
    };

    /// Include file names.
    /// From lexer scan these are the raw header names;
    /// from preprocessor scan these are resolved file paths.
    std::vector<IncludeInfo> includes;

    /// Dependent module names.
    std::vector<std::string> modules;
};

/// Shared cache for dependency directives across multiple scan invocations.
struct SharedScanCache {
    struct CachedEntry {
        /// The source content of the file (kept alive for token references).
        std::string source;

        /// Scanned tokens.
        llvm::SmallVector<clang::dependency_directives_scan::Token> tokens;

        /// Scanned directives (referencing tokens above).
        llvm::SmallVector<clang::dependency_directives_scan::Directive> directives;

        /// Whether each pp_include directive is inside a conditional block,
        /// computed from the raw directive structure before filtering.
        std::vector<bool> include_is_conditional;
    };

    /// path -> cached scan result.
    llvm::StringMap<CachedEntry> entries;
};

/// Quick lexer-based scan for module name and include file names.
/// If module declaration is inside #if/#ifdef, sets need_preprocess=true
/// and module_name will be empty.
ScanResult scan(llvm::StringRef content);

/// Fuzzy preprocessing-based scan. Strips #define and conditional directives
/// so ALL #include are processed unconditionally. Each include is marked
/// with its structural conditional status from the raw directive scan.
/// Returns per-file results (main file + all transitively included files).
llvm::StringMap<ScanResult>
    scan_fuzzy(llvm::ArrayRef<const char*> arguments,
               llvm::StringRef directory,
               llvm::StringRef content = {},
               SharedScanCache* cache = nullptr,
               llvm::IntrusiveRefCntPtr<llvm::vfs::FileSystem> vfs = nullptr);

/// Precise preprocessing-based scan. Keeps all directives including #define
/// and conditionals. Used for lazy module dependency resolution.
ScanResult scan_precise(llvm::ArrayRef<const char*> arguments,
                        llvm::StringRef directory,
                        llvm::StringRef content = {},
                        SharedScanCache* cache = nullptr,
                        llvm::IntrusiveRefCntPtr<llvm::vfs::FileSystem> vfs = nullptr);

/// Compute preamble bound (moved from compile/preamble).
std::uint32_t compute_preamble_bound(llvm::StringRef content);

std::vector<std::uint32_t> compute_preamble_bounds(llvm::StringRef content);

}  // namespace clice
