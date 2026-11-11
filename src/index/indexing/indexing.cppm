/// The index of one compiled unit: its AST, semantic map and preprocessor
/// directives projected into the entries of each file it indexes, the
/// symbols they name and what its instantiations and macro expansions
/// resolve, encoded as a TU index blob.

module clice:index.indexing;

import :compile.compilation;
import :index.symbol;
import :support.prelude;

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

    /// The variants the receiver already stores, each as its file knows it
    /// (known_at), sorted. A section whose hash is among them travels as
    /// the bare hash, and an external symbol only such sections name stays
    /// out of the table: the receiver took both in with the variant.
    llvm::ArrayRef<std::uint64_t> known_variants;

    /// The keys of the instantiations the receiver already holds, each as
    /// its file knows it (known_at), sorted: an instantiation whose key is
    /// among them is not walked, and its file's section lists the key as
    /// skipped.
    llvm::ArrayRef<std::uint64_t> known_instantiations;

    /// Walk the standard library's own instantiations and macro expansions
    /// too (decls::in_standard_library): what `std::sort` resolves in the
    /// library's text. Without, they are neither walked nor keyed.
    bool standard_library_instantiations = false;

    /// Index the bodies of the functions library headers define too
    /// (SemanticsOptions::library_bodies): the names they use inside.
    /// Without, a library header's index holds its declarations alone —
    /// the focus file's own text is always indexed whole.
    bool library_bodies = false;
};

/// Index one TU and encode the result as its TU index blob: the include
/// tree (remapped into a manifest), the TU's symbol table with
/// per-symbol reference files (merged into the symbol records), and one
/// self-contained shard blob per file that received entries (stored or
/// merged into the file's disk shard). Entries of a header entered several
/// times are one union blob. The TU index blob travels worker→server over IPC
/// and is dismantled into the three persistent layers on arrival.
std::vector<std::byte> build_tu_index(CompilationUnit& unit, const TUIndexOptions& options = {});

}  // namespace clice::index
