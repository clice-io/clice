#pragma once

#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "index/shard.h"
#include "index/types.h"
#include "semantic/symbol.h"
#include "support/bitmap.h"

#include "kota/codec/fbs/fbs.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/raw_ostream.h"

namespace clice::index {

/// Decode a serialized bitmap without trusting its bytes: bounded by the
/// buffer, nullopt on a failed parse — croaring's C++ read wrappers abort
/// on one, and blob bitmaps are untrusted disk/wire input. The caller
/// chooses what a failure means: anything feeding persisted state (the
/// project merge, blob loaders) rejects the whole input — normalized to
/// empty, reference bits would read as fresh and stay lost forever —
/// while the session-scoped full decode degrades to an empty bitmap,
/// rebuilt by the next parse.
inline std::optional<Bitmap> read_bitmap(const void* data, std::size_t size) {
    auto* decoded =
        roaring::api::roaring_bitmap_portable_deserialize_safe(static_cast<const char*>(data),
                                                               size);
    if(!decoded) {
        return std::nullopt;
    }
    // deserialize_safe only bounds the reads; the bitmap it hands back can
    // still violate internal invariants (unsorted containers), on which
    // croaring's operations are undefined.
    if(!roaring::api::roaring_bitmap_internal_validate(decoded, nullptr)) {
        roaring::api::roaring_bitmap_free(decoded);
        return std::nullopt;
    }
    return Bitmap(decoded);
}

/// Encode a bitmap as its portable image — the only format with a bounded
/// deserializer.
inline std::vector<std::byte> write_bitmap(const Bitmap& bitmap) {
    std::vector<std::byte> buffer(bitmap.getSizeInBytes(true));
    bitmap.write(reinterpret_cast<char*>(buffer.data()), true);
    return buffer;
}

}  // namespace clice::index

namespace kota::meta {

/// Roaring bitmaps travel in the portable format — the only one with a
/// bounded deserializer. Only the session-scoped full decode goes through
/// this repr (it has no failure channel, so a malformed image degrades to
/// empty); the merge path and persisted blobs read raw images and reject
/// unparseable ones.
template <>
struct repr<clice::Bitmap, codec::fbs::format> {
    using type = std::vector<std::byte>;

    static type to(const clice::Bitmap& bitmap) {
        return clice::index::write_bitmap(bitmap);
    }

    static clice::Bitmap from(const type& buffer) {
        return clice::index::read_bitmap(buffer.data(), buffer.size()).value_or(clice::Bitmap{});
    }
};

/// SymbolKind hides its enum behind constructors, which keeps it out of
/// reflection; persist the underlying value.
template <>
struct repr<clice::SymbolKind, codec::fbs::format> {
    using type = std::uint8_t;

    static type to(clice::SymbolKind kind) {
        return kind.value();
    }

    static clice::SymbolKind from(type value) {
        return clice::SymbolKind(value);
    }
};

}  // namespace kota::meta

namespace clice::index {

/// On-disk index blob schema version. Every persisted blob carries it as a
/// regular field and every loader discards blobs with a different value —
/// including version-less blobs from older builds, which read back as 0.
/// Bump it whenever a persisted type's reflected layout changes.
constexpr inline std::uint32_t index_format_version = 6;

/// Serialize a reflected index blob to `os` as a verified-readable
/// flatbuffer. Encoding only fails on structural impossibilities (e.g. more
/// fields than slots), which the persisted index types cannot hit.
template <typename T>
void serialize_blob(const T& value, llvm::raw_ostream& os) {
    auto encoded = kota::codec::fbs::to_bytes(value);
    assert(encoded.has_value());
    os.write(reinterpret_cast<const char*>(encoded->data()), encoded->size());
}

/// ---------------------------------------------------------------------
/// Persisted blob layouts. Shared by writers, load-time validation and the
/// hand-built blobs of corruption tests; everything else consumes blobs
/// through their readers.
/// Sentinel in a length column (row ranges, line lengths): the real end
/// lives in the sparse escape table.
constexpr inline std::uint8_t length_escape = 0xff;

/// Files whose content fits 24-bit offsets use the packed range column;
/// larger files fall back to the wide begin/length columns.
constexpr inline std::uint32_t packed_range_limit = 0xffffff;

/// One row range in the packed column: begin in the high 24 bits, length
/// in the low 8. Raw u32 order equals (begin, length) lexicographic order.
constexpr inline std::uint32_t pack_range(std::uint32_t begin, std::uint8_t length) {
    return (begin << 8) | length;
}

/// The packed spelling of the no-range sentinel pair relations carry (the
/// default LocalSourceRange, ~0u/~0u). Unambiguous: a real packed row with
/// begin 0xffffff and an escaped length would need an end at least 255
/// past a begin that already sits at the content limit.
constexpr inline std::uint32_t packed_sentinel = 0xffffffff;

/// One side of the blob's row storage (occurrences or relations).
///
/// Ranges use exactly one of two self-describing tiers:
///   - packed: `(begin << 8) | length` per row (content < 16MB)
///   - wide: begin u32 + length u8 columns
/// Lengths >= 255 escape to the sparse (row, end) table in either tier.
///
/// Variant masks are absent for a single variant, then u32 / u64 /
/// concatenated roaring bitmaps by variant count.
struct RowRanges {
    std::vector<std::uint32_t> packed;

    std::vector<std::uint32_t> begins;
    std::vector<std::uint8_t> lengths;

    std::vector<std::uint32_t> long_rows;
    std::vector<std::uint32_t> long_ends;

    std::vector<std::uint32_t> masks32;
    std::vector<std::uint64_t> masks64;
    std::vector<std::uint32_t> roaring_offsets;
    std::vector<std::uint8_t> roaring;
};

/// A file's index rows as one persisted blob.
///
/// The worker encodes one blob per file per TU: single variant (an empty
/// `variants` table), self-contained (content, line table, local symbol
/// names), canonical byte-for-byte — its identity is the hash of its
/// bytes, computed by whoever holds them, never stored inside. The master
/// stores first variants verbatim and merges only when a second distinct
/// variant of the same content generation arrives; merged blobs list the
/// original single-variant identities in `variants` (mask bit position ->
/// identity) and deduplicate rows shared between variants via the masks.
///
/// Symbol ids used by the row columns index `sym_hashes`; the id column
/// width is chosen from the table size (u8 / u16 / u32).
struct ShardBlob {
    /// Persisted-blob schema version (index_format_version), stamped by
    /// the writer and gated by Shard::from_bytes.
    std::uint32_t format_version = 0;

    /// xxh3 of the content bytes the indexing compile consumed — the
    /// content generation these rows were built from. Variants of
    /// different generations never share a blob.
    std::uint64_t content_hash = 0;

    /// Size of the consumed content; bounds every stored range.
    std::uint32_t content_size = 0;

    /// The file's text, for UTF-16 position mapping and text previews.
    /// Empty when the content is pure ASCII: byte offsets are already
    /// UTF-16 column offsets, so the text itself is dead weight. The form
    /// is canonical — a blob storing pure-ASCII content is invalid.
    std::string content;

    /// Mask bit position -> variant identity. Empty for a worker-emitted
    /// blob (one anonymous variant, identified by its own byte hash).
    std::vector<RowsHash> variants;

    /// Per-line byte lengths, up to and including the newline; the last
    /// entry runs to end of file. Lengths >= 255 escape to the sparse
    /// (line, length) table. Line starts are the prefix sums, materialized
    /// once at load.
    std::vector<std::uint8_t> line_lengths;
    std::vector<std::uint32_t> long_line_rows;
    std::vector<std::uint32_t> long_line_lengths;

    /// Referenced symbols, sorted by hash; the index into this table is
    /// the symbol id the row columns use.
    std::vector<std::uint64_t> sym_hashes;

    /// Relation slice per symbol: entry i's relations occupy rows
    /// [sym_rel_offsets[i], sym_rel_offsets[i + 1]). Size is table size + 1.
    std::vector<std::uint32_t> sym_rel_offsets;

    /// Symbols local to this file (FileLocal) or its TU (TULocal), whose
    /// names live nowhere else: sparse over the symbol table, ascending.
    std::vector<std::uint32_t> local_syms;
    std::vector<std::string> local_names;
    std::vector<std::uint8_t> local_kinds;
    std::vector<std::uint8_t> local_scopes;

    /// Occurrences sorted by (begin, end, symbol hash).
    RowRanges occs;
    std::vector<std::uint8_t> occ_syms8;
    std::vector<std::uint16_t> occ_syms16;
    std::vector<std::uint32_t> occ_syms32;

    /// Relations in symbol-table order, sorted by (kind, begin, end,
    /// payload) within each group.
    RowRanges rels;
    std::vector<std::uint8_t> rel_kinds;
    std::vector<std::uint32_t> rel_sym_rows;
    std::vector<std::uint8_t> rel_sym8;
    std::vector<std::uint16_t> rel_sym16;
    std::vector<std::uint32_t> rel_sym32;
    std::vector<std::uint32_t> rel_def_rows;
    std::vector<std::uint32_t> rel_def_begins;
    std::vector<std::uint32_t> rel_def_ends;
};

/// The bytes of `data` as the span every kota fbs entry point takes.
inline std::span<const std::uint8_t> blob_bytes(llvm::StringRef data) {
    return {reinterpret_cast<const std::uint8_t*>(data.data()), data.size()};
}

/// Verify and deserialize a blob into `out`. The out-parameter overload of
/// from_bytes is deliberate: index types hold llvm::DenseMap members whose
/// explicit default constructors fail std::default_initializable, which the
/// value-returning overload requires.
template <typename T>
bool deserialize_blob(llvm::StringRef data, T& out) {
    return kota::codec::fbs::from_bytes(blob_bytes(data), out).has_value();
}

inline llvm::StringRef to_ref(std::string_view text) {
    return {text.data(), text.size()};
}

/// A scalar array view as an ArrayRef borrowing the mapped blob.
template <typename T>
llvm::ArrayRef<T> to_array_ref(kota::codec::fbs::array_view<T> view) {
    if(!view.valid()) {
        return {};
    }
    return {view.raw()->data(), view.raw()->size()};
}

}  // namespace clice::index
