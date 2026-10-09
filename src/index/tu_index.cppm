module;

#include "modules/prelude.h"

module clice:index.tu_index;

import :index.include_tree;
import :index.manifest;
import :index.shard;
import :index.types;

namespace clice {

class CompilationUnitRef;

}

namespace clice::index {

/// Which level of the multi-level symbol table stores a declaration's
/// symbol.
SymbolScope classify_scope(const clang::NamedDecl* decl);

/// How build_tu_index shapes the envelope.
struct TUIndexOptions {
    /// Keep only the rows in the main file.
    bool main_file_only = false;

    /// Variant identities the receiver already stores, sorted. A section
    /// whose hash is among them travels as the bare hash, and an external
    /// symbol only such sections name stays out of the table: the
    /// receiver took both in with the variant.
    llvm::ArrayRef<std::uint64_t> known_variants;
};

/// Index one TU and encode the result as its envelope bytes: the include
/// tree (remapped into a manifest), the TU's symbol table with
/// per-symbol reference files (merged into the project table), and one
/// self-contained shard blob per file that received rows (stored or
/// merged into the file's disk shard). Rows of a header entered several
/// times are one union blob. The envelope travels worker→server over IPC
/// and is dismantled into the three persistent layers on arrival.
std::string build_tu_index(CompilationUnitRef unit, const TUIndexOptions& options = {});

/// The preamble variant: a preamble is a TU cut off at the preamble
/// bound, and its index is the same envelope — persisted verbatim as the
/// PCH's `.pch.idx` pair — plus the preamble-specific fields ordinary
/// envelopes leave empty: the identity of the exact preamble text, and
/// the PCH-derived feature state spliced into main-file results
/// (document links, inactive regions, the open conditional stack at the
/// bound, the diagnostics as published).
std::string build_preamble_index(CompilationUnitRef unit,
                                 llvm::ArrayRef<DocumentLink> links,
                                 llvm::ArrayRef<std::uint32_t> inactive_regions,
                                 llvm::ArrayRef<std::uint8_t> open_conditionals,
                                 llvm::StringRef diagnostics);

/// Zero-copy reader over an envelope: the tree, the per-file blob hashes
/// and the blob bytes themselves are read straight off the wire — a new
/// variant's bytes are sliced out and written or merged without ever
/// decoding the envelope around them — and symbol names are touched only
/// when a consumer genuinely needs them. Whole-envelope accessors on an
/// empty reader answer empty/zero; per-element accessors require a valid
/// index, and no index is valid on an empty reader (every count is 0).
class TUIndex {
public:
    TUIndex() = default;

    /// Wrap verified envelope bytes without owning them (the caller keeps
    /// the bytes alive). Verification gates the format version and bounds
    /// every path id the tree, the symbol table and the sections carry;
    /// corrupt bytes load as an empty reader.
    /// Section blob bytes are verified per section: structurally by
    /// section_shard on first use, or hash-checked and wrapped by
    /// shards_verify in one pass.
    static TUIndex from_bytes(llvm::StringRef data);

    /// Adopt an owning buffer of envelope bytes (a mapped `.pch.idx`, a
    /// session's IPC result). Same verification as from_bytes.
    static TUIndex from_buffer(std::unique_ptr<llvm::MemoryBuffer> buffer);

    /// Whether this reader holds an envelope.
    bool loaded() const {
        return !data.empty();
    }

    /// The envelope bytes backing this reader.
    llvm::StringRef bytes() const {
        return data;
    }

    std::int64_t built_at() const;

    /// The main file's path is always the last id, by IncludeTree
    /// convention.
    std::uint32_t path_count() const;

    llvm::StringRef path(std::uint32_t id) const;

    std::uint64_t path_hash(std::uint32_t id) const;

    /// The places the parse's failed lookups looked that held no file.
    std::uint32_t absent_count() const;

    llvm::StringRef absent(std::uint32_t i) const;

    std::uint32_t node_count() const;

    /// One node of the include tree, its file a path id of this envelope.
    IncludeNode node(std::uint32_t i) const;

    std::uint32_t section_count() const;

    std::uint32_t section_path(std::uint32_t i) const;

    std::uint64_t section_hash(std::uint32_t i) const;

    /// One section's shard blob bytes, borrowing the envelope; empty for
    /// a variant the receiver already stores (TUIndexOptions::known_variants).
    llvm::StringRef section_blob(std::uint32_t i) const;

    /// The section holding `path_id`'s rows, or nullopt when the file had
    /// none (no rows means no contribution). Sections ascend by path id.
    std::optional<std::uint32_t> section_of(std::uint32_t path_id) const;

    /// section_shard of `path_id`'s section, or an empty shard when the
    /// file has none.
    const Shard& shard_of(std::uint32_t path_id) const;

    /// A reader over section `section`'s rows, wrapped on first use and
    /// cached for the envelope's lifetime (the wrap verifies the blob and
    /// later materializes its line table). An empty shard when its blob
    /// fails verification.
    const Shard& section_shard(std::uint32_t section) const;

    /// Wrap every section's blob in one pass, checking its bytes against
    /// the recorded section hash on top of structural verification — the
    /// load gate for persisted envelopes, where a corrupt blob must read
    /// as "pair missing" and rebuild instead of silently serving wrong
    /// rows or nothing.
    bool shards_verify() const;

    /// Visit every symbol of the table in ascending hash order: hash,
    /// identity, and its reference files as path ids. The table leaves out
    /// file-local symbols (see find_symbol) and the external symbols only
    /// sections left empty name (TUIndexOptions::known_variants). Return
    /// false from the callback to stop.
    void iterate_symbols(
        llvm::function_ref<bool(SymbolHash,
                                const SymbolIdentity&,
                                llvm::ArrayRef<std::uint32_t> reference_files)> callback) const;

    /// Look up one symbol's identity by hash: in the table, else among the
    /// sections' own symbols, which name the file-local ones (a
    /// function's locals, a template's parameters).
    std::optional<SymbolIdentity> find_symbol(SymbolHash hash) const;

    /// The internal-linkage symbols more than one of the TU's files names
    /// (Symbol::reference_files), sorted by symbol, their files as indices
    /// into `contribution_paths` — the path ids of the manifest's
    /// contributions, in order. Nullopt when a symbol's reference files are
    /// not all among them.
    std::optional<std::vector<LocalFanout>>
        local_fanout(llvm::ArrayRef<std::uint32_t> contribution_paths) const;

    /// Whether `text` still begins with the exact preamble this envelope
    /// was built from — the gate for serving preamble-derived state
    /// against a live buffer (the rows are offsets into that prefix).
    /// Compared by hash: the text itself is not stored. Always false for
    /// an ordinary envelope.
    bool matches_prefix(llvm::StringRef text) const;

    /// Document links of the preamble region, materialized from the
    /// envelope; empty for an ordinary one.
    std::vector<DocumentLink> links() const;

    /// Inactive regions within the preamble (flat begin/end offset
    /// pairs); empty for an ordinary envelope. Borrows the envelope.
    llvm::ArrayRef<std::uint32_t> inactive_regions() const;

    /// Conditional stack still open at the preamble bound; empty for an
    /// ordinary envelope. Borrows the envelope.
    llvm::ArrayRef<std::uint8_t> open_conditionals() const;

    /// The diagnostics the preamble's build raised, as published (a JSON
    /// array of LSP diagnostics); empty for an ordinary envelope. Borrows
    /// the envelope.
    llvm::StringRef preamble_diagnostics() const;

private:
    /// The verified envelope bytes (owned iff `owned` is set); accessors
    /// rebuild the (pointer-sized) fbs view from them on demand.
    std::unique_ptr<llvm::MemoryBuffer> owned;
    llvm::StringRef data;

    /// Lazily wrapped per-section readers; the envelope is immutable for
    /// the reader's lifetime, so the cache never invalidates.
    mutable std::vector<Shard> shards;
};

}  // namespace clice::index
