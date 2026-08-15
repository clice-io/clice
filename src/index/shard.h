#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "index/tu_index.h"
#include "support/bitmap.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/raw_ostream.h"

namespace clice::index {

/// Identity of one preprocessing variant of a file: xxh3 over the file's
/// rows in canonical order (see FileIndex::rows_hash). Two compilation
/// contexts whose preprocessing of the file agrees produce the same value
/// and share one stored variant.
using RowsHash = std::uint64_t;

/// A file's persisted index rows: every variant produced from one content
/// generation, merged so that a row shared by several variants is stored
/// once. The blob knows nothing about which TU contributed which variant —
/// that mapping is global state (ProjectIndex) — so re-indexing a TU whose
/// rows are unchanged never touches the blob.
///
/// Columnar layout, chosen at serialize time from known bounds and
/// self-described by which columns are populated:
///   - symbol ids: u16 when the table has at most 65535 entries, else u32
///   - row ranges: begin u32 + length u8, lengths >= 255 escape to a
///     sparse (row, end) table
///   - variant masks: absent when there is one variant, u32 up to 32,
///     u64 up to 64, concatenated roaring bitmaps beyond
///
/// Relations are grouped by symbol: `sym_rel_offsets` slices the relation
/// columns per symbol-table entry, so a per-symbol lookup is a binary
/// search plus a slice walk. A relation's payload (`Relation::target_symbol`)
/// is sparse by class: a definition range for decl/def rows that recorded
/// one, a symbol reference for symbol-pair rows, nothing otherwise.
struct ShardBlob {
    /// Persisted-blob schema version (index_format_version), stamped by the
    /// writer and gated by Shard::from_bytes.
    std::uint32_t format_version = 0;

    /// xxh3 of `content`: the content generation these rows were built
    /// from. A variant produced from different bytes of the file starts a
    /// new blob instead of merging in — offsets from two generations must
    /// never share row storage.
    std::uint64_t content_hash = 0;

    /// The file's text, for position mapping. Line starts are derived at
    /// load time, never persisted.
    std::string content;

    /// Variant id (mask bit position) -> rows hash.
    std::vector<RowsHash> variants;

    /// Referenced symbols, sorted by hash; the index into this table is the
    /// symbol id the row columns use.
    std::vector<std::uint64_t> sym_hashes;

    /// Relation-column slice per symbol: entry i's relations occupy rows
    /// [sym_rel_offsets[i], sym_rel_offsets[i + 1]). Size is table size + 1.
    std::vector<std::uint32_t> sym_rel_offsets;

    /// Symbols local to this file (FileLocal) or its TU (TULocal), whose
    /// names live nowhere else: sparse over the symbol table, ascending.
    std::vector<std::uint32_t> local_syms;
    std::vector<std::string> local_names;
    std::vector<std::uint8_t> local_kinds;
    std::vector<std::uint8_t> local_scopes;

    /// Occurrences sorted by (begin, end, symbol hash).
    std::vector<std::uint32_t> occ_begins;
    std::vector<std::uint8_t> occ_lengths;
    std::vector<std::uint32_t> occ_long_rows;
    std::vector<std::uint32_t> occ_long_ends;
    std::vector<std::uint16_t> occ_syms16;
    std::vector<std::uint32_t> occ_syms32;
    std::vector<std::uint32_t> occ_masks32;
    std::vector<std::uint64_t> occ_masks64;
    std::vector<std::uint32_t> occ_roaring_offsets;
    std::vector<std::uint8_t> occ_roaring;

    /// Relations in symbol-table order, sorted by (kind, begin, end) within
    /// each group.
    std::vector<std::uint8_t> rel_kinds;
    std::vector<std::uint32_t> rel_begins;
    std::vector<std::uint8_t> rel_lengths;
    std::vector<std::uint32_t> rel_long_rows;
    std::vector<std::uint32_t> rel_long_ends;
    std::vector<std::uint32_t> rel_sym_rows;
    std::vector<std::uint16_t> rel_sym16;
    std::vector<std::uint32_t> rel_sym32;
    std::vector<std::uint32_t> rel_def_rows;
    std::vector<std::uint32_t> rel_def_begins;
    std::vector<std::uint32_t> rel_def_ends;
    std::vector<std::uint32_t> rel_masks32;
    std::vector<std::uint64_t> rel_masks64;
    std::vector<std::uint32_t> rel_roaring_offsets;
    std::vector<std::uint8_t> rel_roaring;
};

/// A variant to write into a shard blob: the rows plus a resolver for the
/// symbols they reference. Only non-External entries land in the blob's
/// local-name table (External names live in the ProjectIndex); returned
/// name refs must stay valid for the duration of the write call.
struct VariantInput {
    RowsHash hash = 0;
    const FileIndex* rows = nullptr;
    llvm::function_ref<std::optional<SymbolIdentity>(SymbolHash)> symbols;
};

/// Zero-copy reader over a shard blob, plus the live-variant mask the
/// indexer maintains: a variant whose last contributing TU was removed or
/// replaced stops serving immediately, and its rows are erased for real by
/// the next write_shard covering the blob.
class Shard {
public:
    Shard() = default;

    /// Wrap verified blob bytes without owning them (the caller keeps the
    /// bytes alive). Returns an empty shard when verification fails or the
    /// format version differs.
    static Shard from_bytes(llvm::StringRef data);

    /// Adopt an owning buffer of blob bytes (storage reads, freshly
    /// written blobs). A null buffer, corrupt bytes or a different format
    /// version load as an empty shard; the caller treats that as "not on
    /// disk".
    static Shard from_buffer(std::unique_ptr<llvm::MemoryBuffer> buffer);

    /// Whether this shard holds a blob.
    bool loaded() const {
        return buffer != nullptr;
    }

    /// The serialized blob bytes backing this shard (what save persists).
    llvm::StringRef bytes() const {
        return buffer ? buffer->getBuffer() : llvm::StringRef();
    }

    std::uint64_t content_hash() const;

    /// All variants stored in the blob, in variant-id order.
    std::vector<RowsHash> variants() const;

    bool has_variant(RowsHash hash) const;

    /// Restrict queries to the given variants (the file's live
    /// contributions). Hashes the blob does not store are ignored.
    void set_live(llvm::ArrayRef<RowsHash> live);

    /// Whether any stored variant was masked out by set_live — the signal
    /// that the next serialization of this file should compact.
    bool has_dead_variants() const;

    void lookup(std::uint32_t offset, llvm::function_ref<bool(const Occurrence&)> callback) const;

    void lookup(SymbolHash symbol,
                RelationKind kind,
                llvm::function_ref<bool(const Relation&)> callback) const;

    /// Look up a local symbol's name and kind.
    bool find_symbol(SymbolHash hash, std::string& name, SymbolKind& kind) const;

    llvm::StringRef content() const;

    /// Line start offsets for position mapping, derived from the content on
    /// first use.
    std::span<const std::uint32_t> line_starts() const;

private:
    explicit Shard(std::unique_ptr<llvm::MemoryBuffer> buffer);

    friend void write_shard(const Shard& old,
                            llvm::ArrayRef<RowsHash> keep,
                            const VariantInput& fresh,
                            llvm::StringRef content,
                            std::uint64_t content_hash,
                            llvm::raw_ostream& os);

    struct Live {
        /// Fast path: every stored variant is live, no per-row filtering.
        bool all = true;
        std::uint64_t bits = 0;
        Bitmap big;
    };

    bool row_live(bool occurrence, std::uint32_t row) const;

    std::unique_ptr<llvm::MemoryBuffer> buffer;
    Live live;
    /// Lazily derived from the blob's content; content is immutable for the
    /// shard's lifetime, so the cache never invalidates.
    mutable std::vector<std::uint32_t> line_starts_cache;
};

/// Write a shard blob for one content generation: the variants of `old`
/// whose hash is in `keep` (in stored order), then `fresh` if its rows are
/// non-null. Rows shared between variants merge; masks are re-encoded for
/// the surviving variant set. `old` may be an empty shard (fresh build) and
/// `fresh.rows` may be null (pure compaction).
void write_shard(const Shard& old,
                 llvm::ArrayRef<RowsHash> keep,
                 const VariantInput& fresh,
                 llvm::StringRef content,
                 std::uint64_t content_hash,
                 llvm::raw_ostream& os);

}  // namespace clice::index
