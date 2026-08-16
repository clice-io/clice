#pragma once

/// Internal: the persisted layout of a shard blob and the encoding rules
/// shared by its writer, reader and tests. Everything else consumes shards
/// through the `Shard` reader in shard.h — include this header only to
/// build blob bytes by hand (the writer, corruption tests).

#include <cstdint>
#include <string>
#include <vector>

#include "index/shard.h"

namespace clice::index {

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

}  // namespace clice::index
