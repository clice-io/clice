module;

#include "modules/prelude.h"

module clice:index.tu_index;

import :index.instantiation_shard;
import :index.shard;
import :index.symbol;

namespace clice::index {

/// A symbol below external scope more than one of a TU's files names,
/// with those files as the TU index's path ids.
struct PathFanout {
    SymbolHash symbol = 0;
    llvm::SmallVector<std::uint32_t, 2> paths;
};

/// Zero-copy reader over a TU index blob: the tree, the per-file blob
/// hashes and the blob bytes themselves are read straight off the
/// serialized bytes — a new variant's bytes are sliced out and written or
/// merged without ever decoding the TU index around them — and symbol
/// names are touched only when a consumer genuinely needs them. TU-wide
/// accessors on an empty reader answer empty/zero; per-element accessors
/// require an in-range position, and none is in range on an empty reader
/// (every count is 0).
class TUIndex {
public:
    TUIndex() = default;

    /// Wrap verified TU index bytes without owning them (the caller keeps
    /// the bytes alive). Verification gates the format version and bounds
    /// every path id the tree, the symbol table and the sections carry;
    /// corrupt bytes load as an empty reader.
    /// Section blob bytes are verified per section: structurally by
    /// section_shard on first use, or hash-checked and wrapped by
    /// shards_verify in one pass.
    static TUIndex from_bytes(llvm::StringRef data);

    /// Adopt an owning buffer of TU index bytes (a mapped `.pch.idx`, a
    /// session's IPC result). Same verification as from_bytes.
    static TUIndex from_buffer(std::unique_ptr<llvm::MemoryBuffer> buffer);

    /// Whether this reader holds a TU index blob.
    bool loaded() const {
        return !data.empty();
    }

    /// The TU index bytes backing this reader.
    llvm::StringRef bytes() const {
        return data;
    }

    std::int64_t built_at() const;

    /// The focus file's path is always the last id, by IncludeTree
    /// convention.
    std::uint32_t path_count() const;

    llvm::StringRef path(std::uint32_t id) const;

    std::uint64_t content_hash(std::uint32_t id) const;

    /// The places the parse's failed lookups looked that held no file.
    std::uint32_t absent_count() const;

    llvm::StringRef absent(std::uint32_t i) const;

    std::uint32_t node_count() const;

    /// One node of the include tree, its file a path id of this TU index.
    IncludeNode node(std::uint32_t i) const;

    std::uint32_t section_count() const;

    std::uint32_t section_path(std::uint32_t i) const;

    std::uint64_t section_hash(std::uint32_t i) const;

    /// One section's shard blob bytes, borrowing the TU index; empty for
    /// a variant the receiver already stores (TUIndexOptions::known_variants).
    llvm::StringRef section_blob(std::uint32_t i) const;

    /// The section holding `path_id`'s entries, or nullopt when the file had
    /// none (no entries means no contribution). Sections ascend by path id.
    std::optional<std::uint32_t> section_of(std::uint32_t path_id) const;

    /// section_shard of `path_id`'s section, or an empty shard when the
    /// file has none.
    const Shard& shard_of(std::uint32_t path_id) const;

    /// A reader over section `section`'s entries, wrapped on first use and
    /// cached for the TU index's lifetime (the wrap verifies the blob and
    /// later materializes its line table). An empty shard when its blob
    /// fails verification.
    const Shard& section_shard(std::uint32_t section) const;

    /// Wrap every section's blob in one pass, checking its bytes against
    /// the recorded section hash on top of structural verification — the
    /// load gate for persisted TU indexes, where a corrupt blob must read
    /// as "pair missing" and rebuild instead of silently serving wrong
    /// entries or nothing.
    bool shards_verify() const;

    /// The files the run's instantiations or macro expansions resolve
    /// something in or skipped a known key of, ascending by path id.
    std::uint32_t instantiation_count() const;

    std::uint32_t instantiation_path(std::uint32_t i) const;

    /// What the run resolved in section `i`'s file, borrowing the TU index;
    /// empty when it only skipped keys there.
    llvm::StringRef instantiation_blob(std::uint32_t i) const;

    /// The known keys of section `i`'s file the run skipped, ascending.
    llvm::ArrayRef<std::uint64_t> skipped_instantiations(std::uint32_t i) const;

    /// A reader over section `i`'s blob, wrapped on first use and cached
    /// like section_shard; an empty shard when the blob is empty or fails
    /// verification.
    const InstantiationShard& instantiation_shard(std::uint32_t i) const;

    /// The instantiation section of `path_id`, or nullopt.
    std::optional<std::uint32_t> instantiation_section_of(std::uint32_t path_id) const;

    /// Visit every symbol of the table that instantiation sections name, in
    /// ascending hash order, with those sections' path ids. Return false
    /// to stop.
    void iterate_instantiation_files(
        llvm::function_ref<bool(SymbolHash, llvm::ArrayRef<std::uint32_t> files)> callback) const;

    /// Visit every symbol of the table in ascending hash order: hash,
    /// identity, and its reference files as path ids. The table leaves out
    /// the file-local symbols no other file names (see find_symbol) and the
    /// external symbols only sections left empty name
    /// (TUIndexOptions::known_variants). Return false from the callback to
    /// stop.
    void iterate_symbols(
        llvm::function_ref<bool(SymbolHash,
                                const SymbolView&,
                                llvm::ArrayRef<std::uint32_t> reference_files)> callback) const;

    /// Look up one symbol's identity by hash: in the table, else among the
    /// sections' own symbols, which name the file-local ones (a
    /// function's locals, a template's parameters).
    std::optional<SymbolView> find_symbol(SymbolHash hash) const;

    /// The symbols below external scope more than one of the TU's files
    /// names (Symbol::reference_files), sorted by symbol. Nullopt when a
    /// symbol's reference files are not all among the `held` path ids.
    std::optional<std::vector<PathFanout>> local_fanout(const llvm::BitVector& held) const;

    /// Whether `text` still begins with the exact preamble this TU index
    /// was built from — the gate for serving preamble-derived state
    /// against a live buffer (the entries are offsets into that prefix).
    /// Compared by hash: the text itself is not stored. Always false for
    /// an ordinary TU index.
    bool matches_prefix(llvm::StringRef text) const;

    /// Document links of the preamble region, materialized from the
    /// TU index; empty for an ordinary one.
    std::vector<DocumentLink> links() const;

    /// Inactive regions within the preamble (flat begin/end offset
    /// pairs); empty for an ordinary TU index. Borrows the TU index.
    llvm::ArrayRef<std::uint32_t> inactive_regions() const;

    /// Conditional stack still open at the preamble bound; empty for an
    /// ordinary TU index. Borrows the TU index.
    llvm::ArrayRef<std::uint8_t> open_conditionals() const;

    /// The diagnostics the preamble's build raised, as published (a JSON
    /// array of LSP diagnostics); empty for an ordinary TU index. Borrows
    /// the TU index.
    llvm::StringRef preamble_diagnostics() const;

private:
    /// The verified TU index bytes (owned iff `owned` is set); accessors
    /// rebuild the (pointer-sized) fbs view from them on demand.
    std::unique_ptr<llvm::MemoryBuffer> owned;
    llvm::StringRef data;

    /// Lazily wrapped per-section readers; the TU index is immutable for
    /// the reader's lifetime, so the cache never invalidates.
    mutable std::vector<Shard> shards;
    mutable std::vector<InstantiationShard> instantiation_shards;
};

}  // namespace clice::index
