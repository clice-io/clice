module;

#include "modules/prelude.h"

/// The index of one compiled unit: its AST, semantic map and preprocessor
/// directives projected into the entries of each file it indexes, the
/// symbols they name and what its instantiations and macro expansions
/// resolve, encoded as a TU index blob.

module clice:index.indexer;

import :compile.compilation;
import :index.symbol;

namespace clice::index {

/// Which level of the multi-level symbol table stores a declaration's
/// symbol.
SymbolScope scope_of(const clang::NamedDecl* decl);

/// How build_tu_index shapes the TU index blob.
struct TUIndexOptions {
    /// Index every file a Content compile entered, not its focus file
    /// alone: `clice inspect`'s stand-in for the server's project index.
    /// A Whole compile always indexes every file.
    bool all_files = false;

    /// Variant identities the receiver already stores, sorted. A section
    /// whose hash is among them travels as the bare hash, and an external
    /// symbol only such sections name stays out of the table: the
    /// receiver took both in with the variant.
    llvm::ArrayRef<std::uint64_t> known_variants;

    /// The keys of the instantiations the receiver already holds, sorted:
    /// an instantiation whose key is among them is not walked, and its
    /// file's section lists the key as skipped.
    llvm::ArrayRef<std::uint64_t> known_instantiations;

    /// Walk the standard library's own instantiations and macro expansions
    /// too (decls::in_standard_library): what `std::sort` resolves in the
    /// library's text. Without, they are neither walked nor keyed.
    bool standard_library_instantiations = false;
};

/// Index one TU and encode the result as its TU index blob: the include
/// tree (remapped into a manifest), the TU's symbol table with
/// per-symbol reference files (merged into the project table), and one
/// self-contained shard blob per file that received entries (stored or
/// merged into the file's disk shard). Entries of a header entered several
/// times are one union blob. The TU index blob travels worker→server over IPC
/// and is dismantled into the three persistent layers on arrival.
std::vector<std::byte> build_tu_index(CompilationUnit& unit, const TUIndexOptions& options = {});

/// The preamble variant: a preamble is a TU cut off at the preamble
/// bound, and its index is the same TU index blob — persisted verbatim as
/// the PCH's `.pch.idx` pair — plus the preamble-specific fields ordinary
/// TU index blobs leave empty: the identity of the exact preamble text, and
/// the PCH-derived feature state spliced into focus-file results
/// (document links, inactive regions, the open conditional stack at the
/// bound, the diagnostics as published).
std::vector<std::byte> build_preamble_index(CompilationUnit& unit,
                                            llvm::ArrayRef<DocumentLink> links,
                                            llvm::ArrayRef<std::uint32_t> inactive_regions,
                                            llvm::ArrayRef<std::uint8_t> open_conditionals,
                                            llvm::StringRef diagnostics);

}  // namespace clice::index
