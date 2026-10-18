module;

#include "modules/prelude.h"

module clice:index.shard;

import :index.symbol;
import :support.bitmap;
import :syntax.position;

namespace clice::index {

struct RangeColumnsView;
struct LiveFilter;

/// Zero-copy reader over a shard blob, plus the live-variant mask the
/// indexer maintains: a variant whose last contributing TU was removed or
/// replaced stops serving immediately, and its entries are erased for real by
/// the next merge_shards covering the blob.
///
/// The same blob encoding serves every holder of a file's index: the
/// worker's per-file product travelling in an indexing result, the disk
/// shard, a session's focus-file index and the preamble index entries.
class Shard {
public:
    Shard() = default;

    /// Wrap verified blob bytes without owning them (the caller keeps the
    /// bytes alive). Returns an empty shard when verification fails or the
    /// format version differs.
    static Shard from_bytes(llvm::StringRef data);

    /// Adopt an owning buffer of blob bytes (database reads, freshly
    /// written blobs). A null buffer, corrupt bytes or a different format
    /// version load as an empty shard; the caller treats that as "not on
    /// disk".
    static Shard from_buffer(std::unique_ptr<llvm::MemoryBuffer> buffer);

    /// Swap the backing buffer for a byte-identical one (read-snapshot
    /// migration after a save). Verification, the live mask and the
    /// line-start cache all describe the bytes, not the address, so they
    /// carry over. Byte identity is the caller's contract (the exclusive
    /// writer lock keeps every key the batch left alone byte-identical);
    /// only the size is checked, so migrating a resident shard never
    /// faults its content pages in. Returns false — and keeps the current
    /// buffer — when the replacement is missing or its size differs.
    bool rebind(std::unique_ptr<llvm::MemoryBuffer> replacement);

    /// Whether this shard holds a blob.
    bool loaded() const {
        return buffer != nullptr;
    }

    /// The serialized blob bytes backing this shard (what save persists).
    llvm::StringRef bytes() const {
        return buffer ? buffer->getBuffer() : llvm::StringRef();
    }

    /// xxh3 of the content bytes the shard was built from (the content
    /// generation), not of the blob.
    std::uint64_t content_hash() const;

    /// Size of the content the shard was built from; the upper bound of
    /// every stored range.
    std::uint32_t content_size() const;

    /// Whether `text` is the exact content the shard was built from —
    /// the freshness comparison for disk state. Compares by size and
    /// hash: the shard does not keep the content.
    bool matches_content(llvm::StringRef text) const;

    /// The same comparison for a caller that already holds the disk
    /// content's size and hash (a FileTable observation) — no read.
    bool matches_content(std::uint64_t size, std::uint64_t hash) const;

    /// All variant identities stored in the blob, in mask-bit order. A
    /// worker-emitted blob holds one anonymous variant identified by its
    /// own byte hash.
    std::vector<VariantHash> variants() const;

    bool has_variant(VariantHash hash) const;

    /// Restrict queries to the given variants (the file's live
    /// contributions). Hashes the blob does not store are ignored.
    void set_live(llvm::ArrayRef<VariantHash> live);

    /// Whether any stored variant was masked out by set_live — the signal
    /// that the next serialization of this file should compact.
    bool has_dead_variants() const;

    void occurrences_at(std::uint32_t offset, llvm::function_ref<bool(const Occurrence&)> callback) const;

    void relations_of(SymbolHash symbol,
                RelationKind kind,
                llvm::function_ref<bool(const Relation&)> callback) const;

    /// Visit every live occurrence in stored order (sorted by range, then
    /// target hash).
    void for_each_occurrence(llvm::function_ref<bool(const Occurrence&)> callback) const;

    /// Visit every live relation, grouped by symbol in ascending hash
    /// order, relations in (kind, range, payload) order within each group.
    void for_each_relation(llvm::function_ref<bool(SymbolHash, const Relation&)> callback) const;

    /// Look up a local symbol's identity; the strings borrow the blob.
    std::optional<SymbolView> find_symbol(SymbolHash hash) const;

    /// Visit every local symbol the blob names, with its identity.
    void for_each_local_symbol(
        llvm::function_ref<bool(SymbolHash, const SymbolView&)> callback) const;

    /// Line start offsets for position mapping, materialized from the
    /// blob's line-length columns on first use.
    std::span<const std::uint32_t> line_starts() const;

    /// Which lines end in "\r\n" (ShardBlob::crlf_lines), read in place.
    std::span<const std::uint64_t> crlf_lines() const;

    /// The positions of the text the shard was built from, over its line
    /// table and the text of its lines holding non-ASCII bytes. Text
    /// previews read the file from disk under a content_hash check.
    PositionMap positions() const;

private:
    explicit Shard(std::unique_ptr<llvm::MemoryBuffer> buffer);

    struct Live {
        /// Fast path: every stored variant is live, no per-entry filtering.
        bool all = true;
        std::uint64_t bits = 0;
        Bitmap big;
    };

    /// The live mask resolved against one side's mask columns, built once
    /// per query.
    LiveFilter live_filter(const RangeColumnsView& columns) const;

    std::unique_ptr<llvm::MemoryBuffer> buffer;
    Live live;
    /// Byte hash of the blob — the identity of a worker-emitted blob's
    /// single anonymous variant. Computed once at load.
    std::uint64_t blob_hash = 0;
    /// Lazily materialized from the line-length columns; the blob is
    /// immutable for the shard's lifetime, so the cache never invalidates.
    mutable std::vector<std::uint32_t> line_starts_cache;
};

/// Encode one variant's index as a self-contained single-variant blob —
/// the worker's per-file product. Entries are canonicalized (sorted,
/// deduplicated) and the encoding is deterministic: the blob's byte hash
/// is the variant's identity. `symbols` resolves referenced symbols so
/// non-External names end up in the blob's local-name table (External names
/// live in the ProjectIndex); returned name refs must stay valid for the
/// duration of the call. `content` is the text the indexing compile
/// consumed; the blob keeps its hash, size and line table, and the text of
/// its non-ASCII lines only.
void write_shard(const FileIndex& file_index,
                 llvm::function_ref<std::optional<SymbolView>(SymbolHash)> symbols,
                 llvm::StringRef content,
                 llvm::raw_ostream& os);

/// Merge blobs of one file and one content generation: the variants of
/// `old` whose identity is in `keep` (in stored order), then each blob of
/// `fresh` as one new variant. Entries shared between variants merge into
/// one entry; masks are re-encoded for the surviving variant set. `old` may
/// be an empty shard (all-fresh merge) and `fresh` may be empty (pure
/// compaction), but at least one variant must survive overall — a blob
/// holds at least one variant, and a file whose last variant died is
/// retired by the caller, not compacted to nothing. Every fresh shard
/// must be a loaded single-variant blob of the same content generation as
/// the other inputs.
void merge_shards(const Shard& old,
                  llvm::ArrayRef<VariantHash> keep,
                  llvm::ArrayRef<Shard> fresh,
                  llvm::raw_ostream& os);

}  // namespace clice::index
