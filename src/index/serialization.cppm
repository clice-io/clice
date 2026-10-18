module;

#include "modules/prelude.h"

module clice:index.serialization;

import :index.shard;
import :index.types;
import :semantic.symbol;
import :support.bitmap;

namespace clice::index {

/// A bitmap over a portable image in place: the container payloads stay
/// in `data`, which must outlive the bitmap, and the bitmap is read-only
/// (croaring aborts on a write to a frozen one). Validated without
/// trusting the bytes — bounded by the buffer, croaring's internal
/// invariants checked — plus the container offsets the image's header carries:
/// the in-place reader follows them instead of walking the payload, so a
/// corrupt one would send it outside the image.
std::optional<Bitmap> view_bitmap(const void* data, std::size_t size);

/// Encode a bitmap as its portable image — the only format with a bounded
/// deserializer.
inline std::vector<std::byte> write_bitmap(const Bitmap& bitmap) {
    std::vector<std::byte> buffer(bitmap.getSizeInBytes(true));
    bitmap.write(reinterpret_cast<char*>(buffer.data()), true);
    return buffer;
}

/// The two highest values of an id type are reserved: they were DenseMap's
/// sentinel keys when the format was defined, so no writer has ever emitted
/// them, and IPC or disk bytes carrying one are corrupt.
template <typename T>
bool reserved_key(T value) {
    static_assert(std::is_unsigned_v<T>);
    return value >= std::numeric_limits<T>::max() - 1;
}

/// A symbol's id on the command line and in JSON answers: `#<hex>`, which
/// a JavaScript number would round.
inline std::string symbol_id(SymbolHash hash) {
    return std::format("#{:016x}", hash);
}

/// On-disk index blob schema version. Every persisted blob carries it as a
/// regular field and every loader discards blobs with a different value —
/// including version-less blobs from older builds, which read back as 0.
/// Bump it whenever a persisted type's reflected layout changes, when
/// symbol-hash semantics change (stored entries keyed by old hashes would
/// silently stop matching newly computed ones), or when the meaning of
/// stored data changes (v9: preamble document links are document-ordered;
/// v10: an include location's `include` names the directive's containing
/// file, not that file's includer; v11: file-version stamps are the file
/// table's corroborated shared stamps, adopted verbatim at load, and the
/// artifact metadata blobs joined the database; v12: symbol hashes are
/// entities computed by semantic/identity; v13: symbol entries carry the bare
/// name, parent, specialization arguments, flags and canonical file, the
/// include tree is one node type in the TU index and in manifests, and module
/// names are keyed by their entity; v15: the global blob is columnar in
/// hash order with a stable path table and pins its search blob, so it
/// is read in place; v16: entity hashes follow clang 23's node kinds;
/// v17: file versions carry no stat stamps; v18: files under the
/// workspace root are named, and hashed into symbols, by their portable
/// names; v19: entity hashes follow clang 23.1.2, xclang's; v20: preamble
/// indexes carry the preamble's diagnostics; v21: aliases at namespace
/// scope are global symbols, and the entries cover weak call edges, implicit
/// calls and the names the traversal used to skip; v22: specializations
/// relate to their templates, manifests list the files their internal
/// symbols span, the global blob carries the reverse include graph,
/// module linkage is external, and C tags and C-linkage functions follow
/// C's identity rules; v23: names without linkage outside a function reach
/// as far as their scope, `defined` operands reference their macro,
/// dependent operators reference no candidates, and the include pasting a
/// fragment into a declaration carries the fragment's uses; v24: symbols a
/// module exports carry the Exported flag, and anonymous structs, unions
/// and enums the AnonymousScope flag; v25: shards mark the lines ending in
/// "\r\n", whose '\r' is no longer a column of the line; v26: TU indexes
/// carry their symbol table as columns, without a function's locals, and
/// name the variants their receiver stores by hash alone; v27: an importer
/// indexes only what it parsed, and its manifest lists what the modules it
/// imports were built from; v28: references that modify what they name
/// carry a Write relation; v29: `auto` and `decltype` keywords carry an
/// occurrence of the type they stand for, with a Deduced relation; v30:
/// entity hashes name a partition's module, not the partition; v31: an
/// implementation unit's module declaration is a Declaration of its
/// module, keywords and punctuation that call or name a declaration carry
/// an occurrence of it (`override`, `final` and `new` with an Implied
/// relation), and a smart pointer's pointee is a type definition of what
/// holds it); v32: a template's declaration extent starts at its
/// `template` keyword; v33: symbols in an anonymous namespace carry the
/// AnonymousNamespace flag; v34: template parameters carry the
/// TemplateParameter flag, an explicit instantiation's name an
/// Instantiation relation with the directive's extent, a declaration's
/// extent its leading attributes, and a symbol first reached through its
/// template or a using-declaration is described by its own declaration;
/// v35: symbols carry their semantic token class; v36: functions carry
/// their signature, a deduced type sees through a library's internal
/// aliases, an abbreviated template's `auto` names no parameter, a virtual
/// function named by qualification carries a Direct relation, and the
/// global blob lists the files whose names of internal linkage the search
/// index misses; v37: what instantiations and macro expansions resolve
/// lives in per-file instantiation blobs, manifests list the resolutions of
/// those they use, symbols the files whose instantiation blobs name them,
/// and no entry resolves through a template's only instantiation; v38: id
/// and offset columns are as narrow as their tables allow, masks as their
/// variant count, occurrences store their begins as deltas, relation payloads
/// are placed by kind bits and block counts, shards keep only the text of
/// their non-ASCII lines, the global table's file sets of one or two files
/// are raw ids, and manifests name the use set their unit shares in a file's
/// instantiation blob and encode runs of include siblings.
constexpr inline std::uint32_t index_format_version = 38;

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
/// Sentinel in a length column: the sparse escape table holds an entry
/// range's real end, a line's real length.
constexpr inline std::uint8_t length_escape = 0xff;

/// Files whose content fits 24-bit offsets use the packed range column;
/// larger files fall back to the wide begin/length columns.
constexpr inline std::uint32_t packed_range_limit = 0xffffff;

/// One entry range in the packed column: begin in the high 24 bits, length
/// in the low 8. Raw u32 order equals (begin, length) lexicographic order.
constexpr inline std::uint32_t pack_range(std::uint32_t begin, std::uint8_t length) {
    return (begin << 8) | length;
}

/// The packed spelling of the no-range sentinel pair relations carry (the
/// default LocalSourceRange, ~0u/~0u). Unambiguous: a real packed entry with
/// begin 0xffffff and an escaped length would need an end at least 255
/// past a begin that already sits at the content limit.
constexpr inline std::uint32_t packed_sentinel = 0xffffffff;

/// A column of ids into a table, back to back in the width the table's
/// size picks: u8 up to 256 entries, u16 up to 65536, u32 beyond. Readers
/// know the table, so the width is stored nowhere. End offsets into a text
/// of `n` bytes are ids into a table of `n + 1`.
using IdColumn = std::vector<std::uint8_t>;

/// Entries per block of a delta-encoded begin column.
constexpr inline std::uint32_t delta_block = 16;

/// The delta of a delta-encoded begin that does not fit: the sparse table
/// holds the begin.
constexpr inline std::uint8_t delta_escape = 0xff;

/// One side of a blob's entry storage (occurrences, relations, records).
///
/// Ranges use exactly one of three self-describing encodings:
///   - delta, for a side sorted by begin (occurrences, records): the begin
///     of every `delta_block`-th entry, then per entry the distance from the
///     previous entry's begin (0 at a block's start) as a u8 — 255
///     escaping to the sparse (entry, begin) table — and its length u8;
///   - packed, for an unsorted side of content < 16MB: `(begin << 8) |
///     length` per entry;
///   - wide: begin u32 + length u8 columns.
/// Lengths >= 255 escape to the sparse (entry, end) table in every
/// encoding.
///
/// Variant masks are absent for a single variant, then u8 / u16 / u32 /
/// u64 / concatenated roaring bitmaps by variant count.
struct RangeColumns {
    std::vector<std::uint32_t> block_begins;
    std::vector<std::uint8_t> deltas;
    std::vector<std::uint32_t> far_entries;
    std::vector<std::uint32_t> far_begins;

    std::vector<std::uint32_t> packed;

    std::vector<std::uint32_t> begins;
    std::vector<std::uint8_t> lengths;

    std::vector<std::uint32_t> long_entries;
    std::vector<std::uint32_t> long_ends;

    std::vector<std::uint8_t> masks8;
    std::vector<std::uint16_t> masks16;
    std::vector<std::uint32_t> masks32;
    std::vector<std::uint64_t> masks64;
    std::vector<std::uint32_t> roaring_offsets;
    std::vector<std::uint8_t> roaring;
};

/// Relations per block of the payload counts (ShardBlob::rel_target_ranks).
constexpr inline std::uint32_t rank_block = 64;

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
struct ShardBlob {
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
    std::vector<std::uint32_t> utf8_lines;
    std::string utf8_text;
    std::vector<std::uint32_t> utf8_text_ends;

    /// Mask bit position -> variant identity. Empty for a worker-emitted
    /// blob (one anonymous variant, identified by its own byte hash).
    std::vector<VariantHash> variants;

    /// Per-line byte lengths, up to and including the newline; the last
    /// entry runs to end of file. Lengths >= 255 escape to the sparse
    /// (line, length) table. Line starts are the prefix sums, materialized
    /// once at load.
    std::vector<std::uint8_t> line_lengths;
    std::vector<std::uint32_t> long_lines;
    std::vector<std::uint32_t> long_line_lengths;

    /// A bit for each line ending in "\r\n" — line `n` is bit `n % 64` of
    /// word `n / 64`, the words ending at the last set bit.
    std::vector<std::uint64_t> crlf_lines;

    /// Referenced symbols and the parents of the local ones, sorted by
    /// hash; the index into this table is the symbol id the entry columns
    /// use.
    std::vector<std::uint64_t> sym_hashes;

    /// Relation slice per symbol: entry i's relations occupy positions
    /// [rel_offsets[i], rel_offsets[i + 1]). Size is table size + 1.
    IdColumn rel_offsets;

    /// Symbols local to this file (FileLocal) or its TU (TULocal), whose
    /// names live nowhere else: sparse over the symbol table, ascending.
    IdColumn local_syms;
    /// Names back to back, an end offset each.
    std::string local_names;
    IdColumn local_name_ends;
    /// Arguments and signatures of the few locals having them: those
    /// locals' positions in `local_syms`, ascending, and their text back
    /// to back.
    IdColumn local_args_of;
    std::string local_args;
    IdColumn local_args_ends;
    IdColumn local_signatures_of;
    std::string local_signatures;
    IdColumn local_signature_ends;
    std::vector<std::uint8_t> local_kinds;
    std::vector<std::uint8_t> local_scopes;
    /// The parent's symbol id plus one; 0 for none.
    IdColumn local_parents;
    std::vector<std::uint16_t> local_flags;
    std::vector<std::uint8_t> local_token_kinds;
    std::vector<std::uint32_t> local_token_modifiers;

    /// Occurrences sorted by (begin, end, symbol hash), delta-encoded.
    RangeColumns occs;
    IdColumn occ_syms;

    /// Relations in symbol-table order, sorted by (kind, begin, end,
    /// payload) within each group.
    RangeColumns rels;
    /// The RelationKind, payload_bit set for a relation carrying a payload:
    /// for the kinds carrying an extent the extent of what it declares,
    /// for the others a target symbol.
    std::vector<std::uint8_t> rel_kinds;
    /// Per block of rank_block relations, the target and the extent
    /// payloads before it; with the payload bits inside the block they
    /// place a relation's payload in its column.
    IdColumn rel_target_ranks;
    IdColumn rel_extent_ranks;
    IdColumn rel_targets;
    /// An extent as the distance from its begin back to the relation's
    /// begin, and its length. One that does not fit has extent_escape for a
    /// head, 0 for a length and its bounds in the sparse (extent, begin,
    /// end) table.
    std::vector<std::uint16_t> rel_extent_heads;
    std::vector<std::uint16_t> rel_extent_lengths;
    std::vector<std::uint32_t> rel_far_extents;
    std::vector<std::uint32_t> rel_far_begins;
    std::vector<std::uint32_t> rel_far_ends;
};

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
struct InstantiationBlob {
    /// index_format_version, gated by InstantiationShard::from_buffer.
    std::uint32_t format_version = 0;

    /// The content generation of the file the records lie in
    /// (ShardBlob::content_hash), and its size, which bounds every range.
    std::uint64_t content_hash = 0;
    std::uint32_t content_size = 0;

    /// The symbols the records and the keys name, sorted by hash; an index
    /// into this table is a symbol id.
    std::vector<std::uint64_t> sym_hashes;

    /// Records sorted by (begin, end, kind, expansion, target, from) — no
    /// masks: the resolutions list their records.
    RangeColumns records;
    /// The InstantiationKind, its high bit set for a macro expansion's
    /// record.
    std::vector<std::uint8_t> record_kinds;
    IdColumn record_targets;
    /// The caller of a call, the derived class of a base, the declaration
    /// of a type definition; id 0 for the kinds without one.
    IdColumn record_froms;

    /// Resolution identities, ascending; resolution i's records are
    /// resolution_records[resolution_ends[i - 1], resolution_ends[i]), ascending.
    std::vector<std::uint64_t> resolutions;
    std::vector<std::uint32_t> resolution_ends;
    IdColumn resolution_records;

    /// The sets of resolutions the TUs use here, which manifests name by
    /// identity (use_identity), ascending; use i's resolutions are
    /// use_resolutions[use_ends[i - 1], use_ends[i]), ascending. A worker's
    /// blob has none: the master knows what its TU uses.
    std::vector<std::uint64_t> uses;
    std::vector<std::uint32_t> use_ends;
    IdColumn use_resolutions;

    /// The instantiation keys walked (the entity of the instantiated
    /// declaration), ascending: per key the template it instantiates, its
    /// resolution plus one (0: the instantiation resolves nothing here) and the
    /// run that walked it, an index into the producers. A worker's blob
    /// has no producers: the master knows the run.
    std::vector<std::uint64_t> keys;
    IdColumn key_patterns;
    IdColumn key_resolutions;
    IdColumn key_producers;

    /// The runs that walked keys, ascending: the TU (the hash of its
    /// database key) and its manifest's built_at.
    std::vector<std::uint64_t> producer_units;
    std::vector<std::uint64_t> producer_runs;
};

/// The bytes the fields of persisted blobs hold, summed over the blobs
/// measured, by field path (`occs.packed`) in declaration order — how
/// `clice index stats` splits a blob kind. Scalar fields and vector
/// headers are left to the framing, the rest of the blobs' size.
struct FieldBytes {
    llvm::SmallVector<std::pair<std::string, std::uint64_t>> fields;

    void add(llvm::StringRef name, std::uint64_t bytes) {
        auto it = llvm::find_if(fields, [&](const auto& field) { return field.first == name; });
        if(it == fields.end()) {
            fields.emplace_back(name.str(), bytes);
        } else {
            it->second += bytes;
        }
    }
};

template <typename T>
void measure_fields(const T& blob, FieldBytes& bytes, llvm::StringRef prefix = {});

template <std::size_t I, typename T>
void measure_field(const T& blob, FieldBytes& bytes, llvm::StringRef prefix) {
    auto& value = kota::meta::field_of<I>(blob);
    using Field = std::remove_cvref_t<decltype(value)>;
    auto name = std::format("{}{}", std::string_view(prefix), kota::meta::field_name<I, T>());
    if constexpr(std::is_same_v<Field, std::string>) {
        bytes.add(name, value.size());
    } else if constexpr(std::is_same_v<Field, std::vector<std::string>>) {
        // Each string lies out of line behind an offset: its length, its
        // bytes and a NUL, padded to 4.
        std::uint64_t total = 0;
        for(auto& text: value) {
            total += 4 + llvm::alignTo(4 + text.size() + 1, 4);
        }
        bytes.add(name, total);
    } else if constexpr(requires { value.data(); }) {
        bytes.add(name, value.size() * sizeof(*value.data()));
    } else if constexpr(std::is_class_v<Field>) {
        measure_fields(value, bytes, std::format("{}.", name));
    } else {
        static_assert(std::is_arithmetic_v<Field>);
    }
}

template <typename T>
void measure_fields(const T& blob, FieldBytes& bytes, llvm::StringRef prefix) {
    [&]<std::size_t... Is>(std::index_sequence<Is...>) {
        (measure_field<Is>(blob, bytes, prefix), ...);
    }(std::make_index_sequence<kota::meta::field_count<T>()>{});
}

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

/// Whether an end-offset column stays within its arena and never moves
/// back: the shape of every "entries back to back" column pair.
inline bool monotone_ends(llvm::ArrayRef<std::uint32_t> ends, std::size_t arena_size) {
    std::uint32_t last = 0;
    for(auto end: ends) {
        if(end < last || end > arena_size) {
            return false;
        }
        last = end;
    }
    return true;
}

/// monotone_ends whose entries cover the whole arena.
inline bool covering_ends(llvm::ArrayRef<std::uint32_t> ends, std::size_t arena_size) {
    return monotone_ends(ends, arena_size) &&
           (ends.empty() ? arena_size == 0 : ends.back() == arena_size);
}

/// Entry `i` of an "entries back to back" column pair: the arena between
/// the previous entry's end and its own.
template <typename Arena>
Arena back_to_back(Arena arena, llvm::ArrayRef<std::uint32_t> ends, std::uint32_t i) {
    auto begin = i == 0 ? 0 : ends[i - 1];
    return arena.drop_front(begin).take_front(ends[i] - begin);
}

}  // namespace clice::index
