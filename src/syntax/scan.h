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

        /// Whether this is an angled include (<...>) vs quoted ("...").
        bool is_angled = false;

        /// Whether this is an #include_next directive.
        bool is_include_next = false;
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
    };

    /// path -> cached scan result.
    llvm::StringMap<CachedEntry> entries;
};

/// Quick lexer-based scan for module name and include file names.
/// If module declaration is inside #if/#ifdef, sets need_preprocess=true
/// and module_name will be empty.
ScanResult scan(llvm::StringRef content);

/// Precise preprocessing-based scan. Keeps all directives including #define
/// and conditionals. Used for lazy module dependency resolution.
ScanResult scan_precise(llvm::ArrayRef<const char*> arguments,
                        llvm::StringRef directory,
                        llvm::StringRef content = {},
                        SharedScanCache* cache = nullptr,
                        llvm::IntrusiveRefCntPtr<llvm::vfs::FileSystem> vfs = nullptr);

/// Lightweight preprocessor-based fallback for resolving module declarations
/// inside conditional directives (#if/#ifdef). When the quick `scan()` detects
/// `need_preprocess=true`, this function runs clang's preprocessor to evaluate
/// the conditions and extract the actual module name.
///
/// Much cheaper than `scan_precise()`: stops lexing as soon as the module
/// declaration is found, so it only processes the file preamble (global module
/// fragment + conditionals around the module declaration). Only populates
/// `module_name` and `is_interface_unit` in the returned ScanResult.
ScanResult scan_module_decl(llvm::ArrayRef<const char*> arguments,
                            llvm::StringRef directory,
                            llvm::StringRef content = {},
                            SharedScanCache* cache = nullptr,
                            llvm::IntrusiveRefCntPtr<llvm::vfs::FileSystem> vfs = nullptr);

/// Compute preamble bound (moved from compile/preamble).
std::uint32_t compute_preamble_bound(llvm::StringRef content);

std::vector<std::uint32_t> compute_preamble_bounds(llvm::StringRef content);

/// Check if the preamble region contains only syntactically complete directives.
/// Returns false if any #include/#import has an unclosed "" or <>, or any
/// C++20 module statement (import/export) is missing a trailing ';',
/// indicating the user is still typing and PCH/PCM rebuild should be deferred.
bool is_preamble_complete(llvm::StringRef content, std::uint32_t bound);

}  // namespace clice
