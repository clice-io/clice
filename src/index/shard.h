#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "index/types.h"
#include "support/bitmap.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/raw_ostream.h"

namespace clice::index {

/// Identity of one variant of a file's rows: xxh3_64 over the encoded
/// single-variant blob bytes the worker produced. Encoding is canonical
/// and deterministic, so two compilation contexts whose preprocessing of
/// the file agrees produce byte-identical blobs and share one identity.
using RowsHash = std::uint64_t;

/// Zero-copy reader over a shard blob, plus the live-variant mask the
/// indexer maintains: a variant whose last contributing TU was removed or
/// replaced stops serving immediately, and its rows are erased for real by
/// the next merge_shards covering the blob.
///
/// The same blob encoding serves every holder of a file's rows: the
/// worker's per-file product travelling in an indexing result, the disk
/// shard, a session's main-file rows and the preamble index entries.
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

    /// Swap the backing buffer for byte-identical storage (read-snapshot
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

    /// xxh3 of the content bytes the rows were built from (the content
    /// generation), not of the blob.
    std::uint64_t content_hash() const;

    /// Size of the content the rows were built from; the upper bound of
    /// every stored range.
    std::uint32_t content_size() const;

    /// The file's text. Empty for pure-ASCII content, which is not
    /// stored: byte offsets are already UTF-16 column offsets, so
    /// position mapping needs only line_starts(), and text previews
    /// re-read the file from disk under a content_hash check.
    llvm::StringRef content() const;

    /// Whether the content is pure ASCII (and therefore not stored).
    bool ascii() const;

    /// Whether `text` is the exact content the rows were built from —
    /// the freshness comparison for disk state. Compares by size and
    /// hash, so it works for ASCII blobs, whose text is not stored.
    bool matches_content(llvm::StringRef text) const;

    /// All variant identities stored in the blob, in mask-bit order. A
    /// worker-emitted blob holds one anonymous variant identified by its
    /// own byte hash.
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

    /// Visit every live occurrence in row order (sorted by range, then
    /// target hash).
    void for_each_occurrence(llvm::function_ref<bool(const Occurrence&)> callback) const;

    /// Visit every live relation, grouped by symbol in ascending hash
    /// order, rows in (kind, range, payload) order within each group.
    void for_each_relation(llvm::function_ref<bool(SymbolHash, const Relation&)> callback) const;

    /// Look up a local symbol's name and kind.
    bool find_symbol(SymbolHash hash, std::string& name, SymbolKind& kind) const;

    /// Line start offsets for position mapping, materialized from the
    /// blob's line-length columns on first use.
    std::span<const std::uint32_t> line_starts() const;

private:
    explicit Shard(std::unique_ptr<llvm::MemoryBuffer> buffer);

    struct Live {
        /// Fast path: every stored variant is live, no per-row filtering.
        bool all = true;
        std::uint64_t bits = 0;
        Bitmap big;
    };

    bool row_live(bool occurrence, std::uint32_t row) const;

    std::unique_ptr<llvm::MemoryBuffer> buffer;
    Live live;
    /// Byte hash of the blob — the identity of a worker-emitted blob's
    /// single anonymous variant. Computed once at load.
    std::uint64_t blob_hash = 0;
    /// Lazily materialized from the line-length columns; the blob is
    /// immutable for the shard's lifetime, so the cache never invalidates.
    mutable std::vector<std::uint32_t> line_starts_cache;
};

/// Encode one variant's rows as a self-contained single-variant blob —
/// the worker's per-file product. Rows are canonicalized (sorted,
/// deduplicated) and the encoding is deterministic: the blob's byte hash
/// is the variant's identity. `symbols` resolves referenced symbols so
/// non-External names land in the blob's local-name table (External names
/// live in the ProjectIndex); returned name refs must stay valid for the
/// duration of the call. `content` is the text the indexing compile
/// consumed; pure-ASCII content is hashed and measured but not stored.
void write_shard(const FileIndex& rows,
                 llvm::function_ref<std::optional<SymbolIdentity>(SymbolHash)> symbols,
                 llvm::StringRef content,
                 llvm::raw_ostream& os);

/// Merge blobs of one file and one content generation: the variants of
/// `old` whose identity is in `keep` (in stored order), then each blob of
/// `fresh` as one new variant. Rows shared between variants merge into
/// one row; masks are re-encoded for the surviving variant set. `old` may
/// be an empty shard (all-fresh merge) and `fresh` may be empty (pure
/// compaction), but at least one variant must survive overall — a blob
/// holds at least one variant, and a file whose last variant died is
/// retired by the caller, not compacted to nothing. Every fresh shard
/// must be a loaded single-variant blob of the same content generation as
/// the other inputs.
void merge_shards(const Shard& old,
                  llvm::ArrayRef<RowsHash> keep,
                  llvm::ArrayRef<Shard> fresh,
                  llvm::raw_ostream& os);

}  // namespace clice::index
