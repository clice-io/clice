module;

#include "modules/prelude.h"

module clice:syntax.scan;

import :vfs.ids;

namespace clice {

struct ScanResult {
    /// Module name (empty if not a module unit).
    std::string module_name;

    /// Whether the module declaration exports, `export module`: the primary
    /// interface or an interface partition.
    bool is_interface_unit = false;

    /// Whether this is a module implementation unit, `module M;`, which
    /// imports M implicitly. A partition never is one — an internal
    /// partition (`module M:part;`) is imported by the other units of M
    /// like any interface.
    bool is_implementation_unit() const {
        return !module_name.empty() && !is_interface_unit && !module_name.contains(':');
    }

    /// Whether this is an internal partition, `module M:part;`: importable by
    /// the units of M, but an interface unit that imports it may not export it
    /// and leaves its names unreachable to importers.
    bool is_internal_partition() const {
        return !is_interface_unit && module_name.contains(':');
    }

    /// The module importers find in this unit: the declared name, empty
    /// for an implementation unit, which must neither satisfy an import —
    /// the importer would build it as an interface — nor be given a PCM of
    /// its own.
    llvm::StringRef provided_module() const {
        return is_implementation_unit() ? llvm::StringRef() : llvm::StringRef(module_name);
    }

    /// Whether the module declaration is beyond the lexical scan — inside a
    /// conditional directive, or in text the directive scanner rejects —
    /// signaling the caller should fall back to the preprocessor-based scan.
    bool need_preprocess = false;

    struct IncludeInfo {
        /// The header name as written, without its delimiters.
        std::string path;

        /// Byte offset of the directive's `#` in the scanned content.
        std::uint32_t offset = 0;

        /// Range of the filename token including delimiters (`"..."` or
        /// `<...>`).
        std::uint32_t name_offset = 0;
        std::uint32_t name_length = 0;

        /// Whether this include is inside a conditional directive context.
        bool conditional = false;

        /// Number of #if/#ifdef/#ifndef blocks open at this directive.
        std::uint16_t conditional_depth = 0;

        /// Whether this is an angled include (<...>) vs quoted ("...").
        bool is_angled = false;

        /// Whether this is an #include_next directive.
        bool is_include_next = false;
    };

    /// The includes of the file's own text, from the lexer scan
    /// (scan_quick); the preprocessor scans leave them empty.
    std::vector<IncludeInfo> includes;

    /// Dependent module names.
    std::vector<std::string> modules;

    /// Where each of `modules` is imported, as an offset in the main file:
    /// an import in an included file sits at its #include's file name, one
    /// in a file the command line includes at 0.
    std::vector<std::uint32_t> import_offsets;

    /// The lexer scan saw an import declaration. Detection only — the
    /// names stay uncollected (`modules` empty, see scan_quick): the flag
    /// marks files worth a precise scan when import identity matters.
    bool has_import = false;

    /// An import or a module declaration: text that can make its unit
    /// depend on a module (an implementation unit on its interface).
    bool has_module_syntax() const {
        return has_import || !module_name.empty() || need_preprocess;
    }

    /// Hash of the lexer scan's directive stream, all a precise scan's
    /// result depends on: equal hashes, equal precise scans of the file
    /// under one command and disk.
    std::uint64_t directives_hash = 0;
};

/// Quick lexer-based scan, answering exactly two questions: is this file a
/// module unit (and which module does it declare), and which header names
/// does it mention? It runs over raw dependency directives without any
/// preprocessing, which is only sound because of what the standard
/// guarantees about module declarations. P1857R3 "Modules Dependency
/// Discovery" (https://wg21.link/p1857r3, adopted as a DR against C++20;
/// now [cpp.pre]/[cpp.module]) made them preprocessing directives with:
///
///   - "A module directive may only appear as the first preprocessing
///     tokens in a file (excluding the global module fragment)" — it can
///     never arrive through an #include, so scanning the file's own text
///     is sufficient.
///   - "No identifier in the pp-module-name or pp-module-partition shall
///     currently be defined as an object-like macro" ([cpp.module]/2) —
///     the declared module name is literal tokens a lexer can read.
///
/// The directive may still sit inside #if/#ifdef branches, which a lexer
/// cannot evaluate, or in text the directive scanner rejects: then
/// need_preprocess is set (module_name stays empty) and scan_module_decl()
/// resolves it.
///
/// Imports are deliberately NOT collected (`modules` stays empty): unlike
/// the module declaration, an import's tokens ARE macro-expanded
/// ("processed just as in normal text", [cpp.import]), so directive-level
/// text is not a trustworthy source of dependency edges — scan_precise()
/// is.
ScanResult scan_quick(llvm::StringRef content);

/// The lexical scans of file versions: scan_quick is a pure function of
/// the content, so a result is pinned by the version identity (fid,
/// content hash) and every consumer at that version shares one lex — the
/// startup scan feeds it, didSave rescans and CDB-reload rescans hit it
/// when only the stat moved. Keyed by the identity pair rather than a
/// version id so the scan (which runs before the persisted id space
/// loads) never allocates ids. Raw results only — a module name a
/// preprocessor run resolves is configuration output and must not enter
/// a content-keyed slot. Shared, so a scan of unchanged bytes records
/// them without a copy.
struct QuickScanCache {
    llvm::DenseMap<std::pair<Fid, std::uint64_t>, std::shared_ptr<const ScanResult>> results;

    /// The scan of exactly these bytes, whose hash the caller proved to be
    /// `content_hash` (a paired read), computed on first sight.
    std::shared_ptr<const ScanResult> scan_of(Fid fid,
                                              std::uint64_t content_hash,
                                              llvm::StringRef content);
};

/// Precise preprocessing-based scan: runs the real preprocessor, so
/// conditionals are evaluated and macros expand. This is the only
/// trustworthy source of module dependency edges — `import MACRO;` is
/// legal ([cpp.import] macro-expands an import's tokens, see
/// ImportMacroExpandedName in module_import_tests) — and it resolves a
/// partition import (`import :part;`) against the enclosing module's
/// name. Fills the module name and the imports with their offsets.
/// An engaged `content` remaps the main file to it — even when empty:
/// an emptied buffer must scan as empty, not fall back to the disk.
ScanResult scan_precise(llvm::ArrayRef<const char*> arguments,
                        llvm::StringRef directory,
                        std::optional<llvm::StringRef> content = std::nullopt,
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
                            std::optional<llvm::StringRef> content = std::nullopt,
                            llvm::IntrusiveRefCntPtr<llvm::vfs::FileSystem> vfs = nullptr);

/// The end of the preamble: the directives, and the global module
/// fragment's `module;`, that open the file.
std::uint32_t compute_preamble_bound(llvm::StringRef content);

/// The part of the preamble up to `bound` that is finished: `bound` itself,
/// or the end of the line before the first #include/#import whose "" or <>
/// is still open or C++20 module statement (import/export) still lacks its
/// ';' — the user is typing it, and it belongs with the body until done.
std::uint32_t complete_preamble_bound(llvm::StringRef content, std::uint32_t bound);

/// Whether a directive of the preamble up to `bound` defines, undefines or
/// tests a macro: the macro and its uses then sit at positions of the file
/// itself.
bool preamble_uses_macros(llvm::StringRef content, std::uint32_t bound);

}  // namespace clice
