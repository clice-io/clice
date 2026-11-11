/// The persisted layouts of a file's index: its shard and its instantiation
/// shard.

module clice:index.shard.layout;

import :index.columns;
import :index.symbol;
import :support.prelude;

namespace clice::index {

/// Relations per block of the counts placing a relation's payload, range
/// code and explicit range (ShardBlob::relation_target_ranks and the other
/// ranks); an occurrence code's distance restarts at each block.
constexpr inline std::uint32_t rank_block = 64;

/// The odd codes of a relation's range (ShardBlob::relation_range_codes): the
/// relation has no range, or its range is the next of the explicit ranges.
constexpr inline std::uint64_t range_none = 1;
constexpr inline std::uint64_t range_explicit = 3;

/// The bit of a stored relation kind saying the relation carries a payload.
constexpr inline std::uint8_t payload_bit = 0x80;

/// The head of an extent that does not fit the u16 extent columns.
constexpr inline std::uint16_t extent_escape = 0xffff;

/// A file's index as one persisted blob.
///
/// The worker encodes one blob per file per TU: single variant (an empty
/// `variants` table), self-contained (line table, the text of the lines
/// holding non-ASCII bytes, local symbol names), canonical byte-for-byte —
/// its identity is the hash of its bytes, computed by whoever holds them,
/// never stored inside. The master stores first variants verbatim and
/// merges only when a second distinct variant of the same content
/// generation arrives; merged blobs list the original single-variant
/// identities in `variants` (mask bit position -> identity) and deduplicate
/// entries shared between variants via the masks.
template <template <typename> typename Seq>
struct ShardBlobOf {
    /// Persisted-blob schema version (index_format_version), set by
    /// the writer and gated by Shard::from_bytes.
    std::uint32_t format_version = 0;

    /// xxh3 of the content bytes the indexing compile consumed — the
    /// content generation this shard was built from. Variants of
    /// different generations never share a blob.
    std::uint64_t content_hash = 0;

    /// Size of the consumed content; bounds every stored range.
    std::uint32_t content_size = 0;

    /// The lines holding non-ASCII bytes, ascending, and their text without
    /// the line end, back to back: all UTF-16 and UTF-32 position mapping
    /// reads, since on every other line a byte is a unit. Text previews
    /// read the file from disk under a content-hash check.
    Seq<std::uint32_t> utf8_lines;
    Seq<char> utf8_text;
    Seq<std::uint32_t> utf8_text_ends;

    /// Mask bit position -> variant identity. Empty for a worker-emitted
    /// blob (one anonymous variant, identified by its own byte hash).
    Seq<VariantHash> variants;

    /// Per-line byte lengths, up to and including the newline; the last
    /// entry runs to end of file. Lengths >= 255 escape to the sparse
    /// (line, length) table. Line starts are the prefix sums, materialized
    /// once at load.
    Seq<std::uint8_t> line_lengths;
    Seq<std::uint32_t> long_lines;
    Seq<std::uint32_t> long_line_lengths;

    /// A bit for each line ending in "\r\n" — line `n` is bit `n % 64` of
    /// word `n / 64`, the words ending at the last set bit.
    Seq<std::uint64_t> crlf_lines;

    /// Referenced symbols and the parents of the local ones, sorted by
    /// hash; the index into this table is the symbol id the entry columns
    /// use.
    Seq<std::uint64_t> symbol_hashes;

    /// Relation slice per symbol: entry i's relations occupy positions
    /// [relation_offsets[i], relation_offsets[i + 1]). Size is table size + 1.
    IdColumnOf<Seq> relation_offsets;

    /// Symbols local to this file (FunctionLocal) or its TU (Internal), whose
    /// names live nowhere else: sparse over the symbol table, ascending.
    IdColumnOf<Seq> local_symbols;
    /// Names back to back, an end offset each.
    Seq<char> local_names;
    IdColumnOf<Seq> local_name_ends;
    /// Arguments and signatures of the few locals having them: those
    /// locals' positions in `local_symbols`, ascending, and their text back
    /// to back.
    IdColumnOf<Seq> local_args_of;
    Seq<char> local_args;
    IdColumnOf<Seq> local_args_ends;
    IdColumnOf<Seq> local_signatures_of;
    Seq<char> local_signatures;
    IdColumnOf<Seq> local_signature_ends;
    Seq<std::uint8_t> local_kinds;
    Seq<std::uint8_t> local_scopes;
    /// The parent's symbol id plus one; 0 for none.
    IdColumnOf<Seq> local_parents;
    Seq<std::uint16_t> local_flags;
    Seq<std::uint8_t> local_token_kinds;
    Seq<std::uint32_t> local_token_modifiers;

    /// Occurrences sorted by (begin, end, symbol hash), delta-encoded.
    RangeColumnsOf<Seq> occurrences;
    MaskColumnsOf<Seq> occurrence_masks;
    IdColumnOf<Seq> occurrence_symbols;

    /// Relations in symbol-table order, sorted by (kind, begin, end,
    /// payload) within each group. Most relations span the range of an
    /// occurrence: per relation a varint code, an even one naming the
    /// first occurrence of that range — its zigzag distance from the
    /// occurrence the previous even code of the rank block named (0 at the
    /// block's start), shifted left by one — or range_none, or
    /// range_explicit for a range no occurrence has, the next of the
    /// explicit ranges. Per rank block, the code bytes and the explicit
    /// ranges before it.
    Seq<std::uint8_t> relation_range_codes;
    IdColumnOf<Seq> relation_code_ranks;
    RangeColumnsOf<Seq> relation_explicit_ranges;
    IdColumnOf<Seq> relation_explicit_ranks;
    MaskColumnsOf<Seq> relation_masks;
    /// The RelationKind, payload_bit set for a relation carrying a payload:
    /// for the kinds carrying an extent the extent of what it declares,
    /// for the others a target symbol.
    Seq<std::uint8_t> relation_kinds;
    /// Per block of rank_block relations, the target and the extent
    /// payloads before it; with the payload bits inside the block they
    /// place a relation's payload in its column.
    IdColumnOf<Seq> relation_target_ranks;
    IdColumnOf<Seq> relation_extent_ranks;
    IdColumnOf<Seq> relation_targets;
    /// An extent as the distance from its begin back to the relation's
    /// begin, and its length. One that does not fit has extent_escape for a
    /// head, 0 for a length and its bounds in the sparse (extent, begin,
    /// end) table.
    Seq<std::uint16_t> relation_extent_heads;
    Seq<std::uint16_t> relation_extent_lengths;
    Seq<std::uint32_t> relation_far_extents;
    Seq<std::uint32_t> relation_far_begins;
    Seq<std::uint32_t> relation_far_ends;
};

using ShardBlob = ShardBlobOf<codec::blob::Owned>;

/// A file's instantiation shard as one persisted blob: what the template
/// instantiations and the macro expansions of the indexed TUs resolve in the
/// file's text, which its shard leaves open — the calls and member
/// accesses of a template's dependent code, the declarations an `auto`
/// deduces, the bases a dependent base specifier names, the names a
/// `#define` body spells.
///
/// One instantiated definition or macro expansion resolves a set of records,
/// its resolution; resolutions are identified by the hash of the file's content
/// generation and their records, so the TUs agreeing on one share it, and a
/// record several resolutions hold is stored once. A worker emits one blob per
/// file with its TU's resolutions there and the keys it walked; the
/// master merges them into the file's blob, which records who walked each
/// key.
template <template <typename> typename Seq>
struct InstantiationShardBlobOf {
    /// index_format_version, gated by InstantiationShard::from_buffer.
    std::uint32_t format_version = 0;

    /// The content generation of the file the records lie in
    /// (ShardBlob::content_hash), and its size, which bounds every range.
    std::uint64_t content_hash = 0;
    std::uint32_t content_size = 0;

    /// The symbols the records and the keys name, sorted by hash; an index
    /// into this table is a symbol id.
    Seq<std::uint64_t> symbol_hashes;

    /// Records sorted by (begin, end, kind, expansion, target, from) — no
    /// masks: the resolutions list their records.
    RangeColumnsOf<Seq> records;
    /// The InstantiationKind, its high bit set for a macro expansion's
    /// record.
    Seq<std::uint8_t> record_kinds;
    IdColumnOf<Seq> record_targets;
    /// The caller of a call, the derived class of a base, the declaration
    /// of a type definition; id 0 for the kinds without one.
    IdColumnOf<Seq> record_froms;

    /// Resolution identities, ascending; resolution i's records are
    /// resolution_records[resolution_ends[i - 1], resolution_ends[i]), ascending.
    Seq<std::uint64_t> resolutions;
    Seq<std::uint32_t> resolution_ends;
    IdColumnOf<Seq> resolution_records;

    /// The sets of resolutions the TUs use here, which manifests name by
    /// identity (use_hash), ascending; use i's resolutions are
    /// use_resolutions[use_ends[i - 1], use_ends[i]), ascending. A worker's
    /// blob has none: the master knows what its TU uses.
    Seq<std::uint64_t> uses;
    Seq<std::uint32_t> use_ends;
    IdColumnOf<Seq> use_resolutions;

    /// The instantiation keys walked (the entity of the instantiated
    /// declaration), ascending: per key the template it instantiates, its
    /// resolution plus one (0: the instantiation resolves nothing here) and the
    /// run that walked it, an index into the producers. A worker's blob
    /// has no producers: the master knows the run.
    Seq<std::uint64_t> keys;
    IdColumnOf<Seq> key_patterns;
    IdColumnOf<Seq> key_resolutions;
    IdColumnOf<Seq> key_producers;

    /// The runs that walked keys, ascending: the TU (its path_hash) and its
    /// manifest's built_at.
    Seq<std::uint64_t> producer_units;
    Seq<std::uint64_t> producer_runs;
};

using InstantiationShardBlob = InstantiationShardBlobOf<codec::blob::Owned>;

}  // namespace clice::index
