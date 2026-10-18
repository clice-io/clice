module;

#include "modules/prelude.h"

#include "support/logging.macros.h"

/// The persisted index format: every blob layout the index writes, to the
/// database or between processes, and the version gating them. Writers,
/// load-time validation and the hand-built blobs of corruption tests share
/// the layouts; everything else consumes blobs through their readers.

module clice:index.format;

import :index.symbol;
import :support.logging;

namespace clice::index {

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
/// names are keyed by their entity; v15: the project table is columnar in
/// hash order with a stable path table and pins its search blob, so it
/// is read in place; v16: entity hashes follow clang 23's node kinds;
/// v17: file versions carry no stat stamps; v18: files under the
/// workspace root are named, and hashed into symbols, by their portable
/// names; v19: entity hashes follow clang 23.1.2, xclang's; v20: preamble
/// indexes carry the preamble's diagnostics; v21: aliases at namespace
/// scope are global symbols, and the entries cover weak call edges, implicit
/// calls and the names the traversal used to skip; v22: specializations
/// relate to their templates, manifests list the files their internal
/// symbols span, the project table carries the reverse include graph,
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
/// project table lists the files whose names of internal linkage the search
/// index misses; v37: what instantiations and macro expansions resolve
/// lives in per-file instantiation blobs, manifests list the resolutions of
/// those they use, symbols the files whose instantiation blobs name them,
/// and no entry resolves through a template's only instantiation; v38: a
/// friend class declaration is a reference to the class, no declaration;
/// v39: id and offset columns are as narrow as their tables allow, masks as
/// their variant count, occurrences store their begins as deltas, relation
/// payloads are placed by kind bits and block counts, shards keep only the
/// text of their non-ASCII lines, the project table's file sets of one or two
/// files are raw ids, and manifests name the use set their unit shares in a
/// file's instantiation blob and encode runs of include siblings; v40: a
/// shared local names its files by version, including files the unit reads
/// through a precompiled header or a module, function-local symbols other
/// files name join the TU index's table and the shared locals, and the project
/// table lists the files the shared locals name per symbol.
constexpr inline std::uint32_t index_format_version = 40;

/// Serialize a reflected index blob to `os` as a verified-readable
/// flatbuffer. Encoding only fails on structural impossibilities (e.g. more
/// fields than slots), which the persisted index types cannot hit.
template <typename T>
void serialize_blob(const T& value, llvm::raw_ostream& os) {
    auto encoded = kota::codec::fbs::to_bytes(value);
    assert(encoded.has_value());
    os.write(reinterpret_cast<const char*>(encoded->data()), encoded->size());
}

/// The bytes of `data` as the span every kota fbs entry point takes.
inline std::span<const std::uint8_t> blob_bytes(llvm::StringRef data) {
    return {reinterpret_cast<const std::uint8_t*>(data.data()), data.size()};
}

/// The root of a blob of layout `Blob` once its bytes pass the structural
/// check and carry this build's format version; nullopt, logged as the
/// rejection of `what`, otherwise. The cross-field checks are the reader's.
template <typename Blob>
std::optional<kota::codec::fbs::table_view<Blob>> verify_root(llvm::StringRef bytes,
                                                              llvm::StringRef what) {
    auto root = kota::codec::fbs::table_view<Blob>::from_bytes(blob_bytes(bytes));
    if(!root.valid()) {
        LOG_DEBUG("Rejecting {}: structural verification failed", what);
        return std::nullopt;
    }
    if(root[&Blob::format_version] != index_format_version) {
        LOG_DEBUG("Rejecting {}: format version {}, this build reads {}",
                  what,
                  root[&Blob::format_version],
                  index_format_version);
        return std::nullopt;
    }
    return root;
}

/// The root of a blob a load verified, read in place without verifying
/// again.
template <typename Blob>
kota::codec::fbs::table_view<Blob> verified_root(llvm::StringRef bytes) {
    return kota::codec::fbs::table_view<Blob>::from_verified_bytes(blob_bytes(bytes));
}

/// Swap a reader's backing buffer for byte-identical bytes elsewhere (a
/// newer read snapshot): what the reader verified and derived describes
/// the bytes, not their address. False, keeping `buffer`, when the sizes
/// differ.
inline bool rebind_buffer(std::unique_ptr<llvm::MemoryBuffer>& buffer,
                          std::unique_ptr<llvm::MemoryBuffer> replacement) {
    if(!buffer || !replacement || replacement->getBufferSize() != buffer->getBufferSize()) {
        return false;
    }
    buffer = std::move(replacement);
    return true;
}

/// Verify and deserialize a blob into `out`. The out-parameter overload of
/// from_bytes is deliberate: index types hold llvm::DenseMap members whose
/// explicit default constructors fail std::default_initializable, which the
/// value-returning overload requires.
template <typename T>
bool deserialize_blob(llvm::StringRef data, T& out) {
    return kota::codec::fbs::from_bytes(blob_bytes(data), out).has_value();
}

/// The two highest values of an id type are reserved: they were DenseMap's
/// sentinel keys when the format was defined, so no writer has ever emitted
/// them, and IPC or disk bytes carrying one are corrupt.
template <typename T>
bool reserved_key(T value) {
    static_assert(std::is_unsigned_v<T>);
    return value >= std::numeric_limits<T>::max() - 1;
}

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

/// One side of a blob's entries (occurrences, relations, records).
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

/// Relations per block of the payload counts (ShardBlob::relation_target_ranks).
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
    std::vector<std::uint64_t> symbol_hashes;

    /// Relation slice per symbol: entry i's relations occupy positions
    /// [relation_offsets[i], relation_offsets[i + 1]). Size is table size + 1.
    IdColumn relation_offsets;

    /// Symbols local to this file (FunctionLocal) or its TU (Internal), whose
    /// names live nowhere else: sparse over the symbol table, ascending.
    IdColumn local_symbols;
    /// Names back to back, an end offset each.
    std::string local_names;
    IdColumn local_name_ends;
    /// Arguments and signatures of the few locals having them: those
    /// locals' positions in `local_symbols`, ascending, and their text back
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
    RangeColumns occurrences;
    IdColumn occurrence_symbols;

    /// Relations in symbol-table order, sorted by (kind, begin, end,
    /// payload) within each group.
    RangeColumns relations;
    /// The RelationKind, payload_bit set for a relation carrying a payload:
    /// for the kinds carrying an extent the extent of what it declares,
    /// for the others a target symbol.
    std::vector<std::uint8_t> relation_kinds;
    /// Per block of rank_block relations, the target and the extent
    /// payloads before it; with the payload bits inside the block they
    /// place a relation's payload in its column.
    IdColumn relation_target_ranks;
    IdColumn relation_extent_ranks;
    IdColumn relation_targets;
    /// An extent as the distance from its begin back to the relation's
    /// begin, and its length. One that does not fit has extent_escape for a
    /// head, 0 for a length and its bounds in the sparse (extent, begin,
    /// end) table.
    std::vector<std::uint16_t> relation_extent_heads;
    std::vector<std::uint16_t> relation_extent_lengths;
    std::vector<std::uint32_t> relation_far_extents;
    std::vector<std::uint32_t> relation_far_begins;
    std::vector<std::uint32_t> relation_far_ends;
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
struct InstantiationShardBlob {
    /// index_format_version, gated by InstantiationShard::from_buffer.
    std::uint32_t format_version = 0;

    /// The content generation of the file the records lie in
    /// (ShardBlob::content_hash), and its size, which bounds every range.
    std::uint64_t content_hash = 0;
    std::uint32_t content_size = 0;

    /// The symbols the records and the keys name, sorted by hash; an index
    /// into this table is a symbol id.
    std::vector<std::uint64_t> symbol_hashes;

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
    /// identity (use_hash), ascending; use i's resolutions are
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

/// One file's index in the TU index blob: a self-contained single-variant shard
/// blob (index/shard.cppm). `hash` is xxh3 of `blob` — the variant's
/// identity — so the master can skip blobs it already stores without
/// touching their bytes; the blob is left empty for a variant the master
/// said it stores.
struct FileSection {
    std::uint32_t path_id = 0;

    std::uint64_t hash = 0;

    std::vector<std::uint8_t> blob;
};

/// One file's instantiations in the TU index: the instantiation blob of what
/// the run resolved there, empty when it resolved nothing, and the known keys
/// whose definition the file holds that the run skipped, ascending.
struct InstantiationSection {
    std::uint32_t path_id = 0;

    std::vector<std::uint8_t> blob;

    std::vector<std::uint64_t> skipped;
};

/// The TU index's serialized layout. Only the indexer materializes it (and
/// tests planting shapes it never emits); every consumer reads the bytes
/// through the TUIndex reader.
struct TUIndexBlob {
    /// Serialized schema version (index_format_version), gated by
    /// TUIndex::from_bytes.
    /// A worker respawned after the binary on disk changed can be one
    /// build ahead of the server, and a layout change need not be
    /// structurally detectable.
    std::uint32_t format_version = 0;

    /// Milliseconds since epoch, sampled before the build started.
    std::int64_t built_at = 0;

    /// The include tree (IncludeTree's persisted vectors): the path table,
    /// the consumed-content hash per path, and every include edge of the
    /// parse.
    std::vector<std::string> paths;
    std::vector<std::uint64_t> content_hashes;
    std::vector<IncludeNode> nodes;

    /// The symbols the entries name, sorted by hash, except the file-local
    /// ones (a function's locals, a template's parameters) — only their
    /// own file can mention one, and that file's section names it — and
    /// the external symbols only sections left empty name. Names, arguments and signatures are
    /// back to back with one end offset per entry; so are the reference files, as path ids.
    std::vector<std::uint64_t> symbol_hashes;
    std::string symbol_names;
    std::vector<std::uint32_t> symbol_name_ends;
    std::string symbol_args;
    std::vector<std::uint32_t> symbol_args_ends;
    std::string symbol_signatures;
    std::vector<std::uint32_t> symbol_signature_ends;
    std::vector<std::uint64_t> symbol_parents;
    std::vector<std::uint8_t> symbol_kinds;
    std::vector<std::uint8_t> symbol_scopes;
    std::vector<std::uint16_t> symbol_flags;
    std::vector<std::uint8_t> symbol_token_kinds;
    std::vector<std::uint32_t> symbol_token_modifiers;
    /// Path ids, `no_file` for a symbol without a declaring relation.
    std::vector<std::uint32_t> symbol_files;
    std::vector<std::uint32_t> symbol_reference_ends;
    std::vector<std::uint32_t> symbol_references;

    /// Per symbol, the files whose instantiation section names it, back to
    /// back as path ids.
    std::vector<std::uint32_t> symbol_instantiation_ends;
    std::vector<std::uint32_t> symbol_instantiations;

    /// One entry per file with an index, ascending by path id.
    std::vector<FileSection> sections;

    /// One entry per file the run's instantiations or macro expansions
    /// resolve something in or skipped a known key of, ascending by path id.
    std::vector<InstantiationSection> instantiations;

    /// Preamble ride-alongs, empty on ordinary TU indexes: identity of the
    /// exact preamble text the PCH was built from (matches_preamble), and
    /// the PCH-derived feature state spliced into focus-file results. The
    /// refs borrow the builder's inputs — encode-only, like the section
    /// blobs are for readers.
    std::uint64_t preamble_hash = 0;
    std::uint32_t preamble_size = 0;
    llvm::ArrayRef<DocumentLink> links;
    llvm::ArrayRef<std::uint32_t> inactive_regions;
    llvm::ArrayRef<std::uint8_t> open_conditionals;

    /// The places the parse's failed lookups looked that held no file
    /// (CompilationUnit::absent).
    std::vector<std::string> absent;

    /// Preamble ride-along: the diagnostics the preamble's build raised,
    /// as published (a JSON array of LSP diagnostics) — a compile that
    /// consumes the PCH never raises them again.
    std::string diagnostics;
};

/// The project table's persisted form, read in place: the FileVersion table
/// as parallel columns, the symbol table as parallel columns in hash order
/// with its names, arguments and reference bitmaps in arenas, and the path
/// table those columns index.
struct ProjectTableBlob {
    std::uint32_t format_version = 0;

    /// See ProjectIndex::table_generation.
    std::uint64_t generation = 0;

    std::uint32_t next_version_id = 0;

    std::vector<std::uint32_t> version_ids;
    std::vector<std::string> version_paths;
    std::vector<std::uint64_t> version_hashes;

    /// Path id -> path for every id the symbol columns reference. Ids
    /// are dense and stable across writes: a path keeps its id for as
    /// long as the blob lineage lives, so a base entry's bitmap image is
    /// copied into the next blob verbatim.
    std::vector<std::string> paths;

    /// The external symbol table, sorted by hash. A symbol's reference
    /// files are a file set (append_file_set) in the arena, one end offset
    /// per symbol.
    std::vector<std::uint64_t> symbol_hashes;
    std::string symbol_names;
    std::vector<std::uint32_t> symbol_name_ends;
    /// The arguments of the few symbols having them: those symbols,
    /// ascending, and their text back to back.
    std::vector<std::uint32_t> symbol_args_entries;
    std::string symbol_args;
    std::vector<std::uint32_t> symbol_args_ends;
    std::string symbol_signatures;
    std::vector<std::uint32_t> symbol_signature_ends;
    /// The parent's position in the table plus one; 0 for none, or for one
    /// outside the table, which the sparse (symbol, parent hash) table names.
    IdColumn symbol_parents;
    std::vector<std::uint32_t> symbol_far_parents;
    std::vector<std::uint64_t> symbol_far_parent_hashes;
    std::vector<std::uint8_t> symbol_kinds;
    std::vector<std::uint16_t> symbol_flags;
    std::vector<std::uint8_t> symbol_token_kinds;
    std::vector<std::uint32_t> symbol_token_modifiers;
    /// Path ids plus one, 0 for a symbol without a declaring relation.
    IdColumn symbol_files;
    /// The files referencing each symbol, counted: at most the path count,
    /// so they take the width of path ids.
    IdColumn symbol_reference_counts;
    std::vector<std::uint32_t> symbol_bitmap_ends;
    std::vector<std::uint8_t> symbol_bitmaps;
    /// The files whose instantiation shards name a symbol, as file sets for
    /// the few symbols some instantiation names, ascending.
    std::vector<std::uint32_t> symbol_instantiation_entries;
    std::vector<std::uint32_t> symbol_instantiation_ends;
    std::vector<std::uint8_t> symbol_instantiations;

    /// tu_version -> generation of every manifest current at this save.
    /// The loader adopts a manifest blob only on an exact generation match, so
    /// a manifest whose write failed while the project table was persisted (or one that
    /// outran a lost project table write) reads as lost and its TU reindexes,
    /// instead of an older on-disk manifest serving as current.
    std::vector<std::uint32_t> manifest_versions;
    std::vector<std::uint64_t> manifest_generations;

    /// The generation of the search blob this table was saved with, and
    /// the entries and the files whose names of internal linkage (path
    /// ids, ascending) changed since that blob was built.
    std::uint64_t search_generation = 0;
    std::vector<std::uint64_t> search_pending;
    std::vector<std::uint32_t> search_pending_files;

    /// The reverse include graph, for readers holding no manifests: every
    /// file some TU contributed a variant to (path ids, ascending), and those
    /// TUs' path ids as file sets back to back.
    std::vector<std::uint32_t> contributed_files;
    std::vector<std::uint32_t> contributor_ends;
    std::vector<std::uint8_t> contributors;

    /// The symbols below external scope some TU's shared locals list
    /// (TUManifest::shared_locals), ascending; the end of each symbol's run
    /// of file sets; and the sets, file sets back to back.
    std::vector<std::uint64_t> shared_local_symbols;
    std::vector<std::uint32_t> shared_local_set_ends;
    std::vector<std::uint32_t> shared_local_file_ends;
    std::vector<std::uint8_t> shared_local_files;
};

/// Layout and scoring version of the search blob, apart from the index
/// format it is derived from: a bump rebuilds the search index alone.
constexpr std::uint32_t search_format_version = 3;

/// The persisted search index. Entries are in quality order (ties by hash),
/// which is the entry id order every bitmap speaks; the arenas hold the
/// names back to back with one end offset per entry; posting arenas hold
/// posting lists over the entries (write_postings) the same way.
struct SearchIndexBlob {
    std::uint32_t format_version = 0;
    std::uint32_t search_version = 0;
    /// See SearchSnapshot::generation.
    std::uint64_t generation = 0;

    std::vector<std::uint64_t> hashes;
    std::string names;
    std::vector<std::uint32_t> name_ends;
    std::string args;
    std::vector<std::uint32_t> args_ends;
    /// The entry of the nearest enclosing symbol that is not an inline
    /// namespace, or no_entry at the translation unit or when that symbol
    /// is not in the index.
    std::vector<std::uint32_t> parents;
    std::vector<float> qualities;
    /// Entries sorted by lowercase name, then name, then entry.
    std::vector<std::uint32_t> by_lower_name;
    /// Entries sorted by hash.
    std::vector<std::uint32_t> by_hash;

    std::vector<std::uint32_t> token_keys;
    std::vector<std::uint32_t> token_ends;
    std::vector<std::uint8_t> token_postings;

    /// Entries with members, sorted; their members as bitmaps.
    std::vector<std::uint32_t> container_entries;
    std::vector<std::uint32_t> children_ends;
    std::vector<std::uint8_t> children_postings;
    /// The entries with no parent in the index, one posting list.
    std::vector<std::uint8_t> top_level;

    /// The entries whose name runs past the token bound (name_bound), one
    /// posting list: their tail carries no token, so every name lookup scans
    /// them.
    std::vector<std::uint8_t> long_names;

    /// Namespace entries, sorted; everything below each as bitmaps.
    std::vector<std::uint32_t> subtree_entries;
    std::vector<std::uint32_t> subtree_ends;
    std::vector<std::uint8_t> subtree_postings;

    /// One bitmap per SymbolKind value.
    std::vector<std::uint32_t> kind_ends;
    std::vector<std::uint8_t> kind_postings;

    /// The files of the entries, one bitmap each.
    std::vector<std::string> paths;
    std::vector<std::uint32_t> file_ends;
    std::vector<std::uint8_t> file_postings;

    /// The entries of internal linkage, sorted, and the path each one's
    /// entries live in.
    std::vector<std::uint32_t> local_entries;
    std::vector<std::uint32_t> local_paths;
};

/// The manifest's persisted form. Node and contribution payloads are
/// varint-packed by hand: a big TU enters thousands of files, and LEB128
/// on the small ids and lines roughly halves the blob against fixed-width
/// columns.
struct TUManifestBlob {
    std::uint32_t format_version = 0;

    std::uint64_t table_generation = 0;

    std::uint64_t built_at = 0;

    std::uint32_t tu_version = 0;

    std::uint32_t node_count = 0;

    std::uint32_t contribution_count = 0;

    /// node_count × (varint version, varint parent, varint line): the parent 0
    /// for the previous node's, 1 for the root, else its index plus 2; the
    /// line shifted left by two over a bit for a skipped node and a bit for
    /// a line stored as its distance past the previous node's — only
    /// beside the previous node's parent, at or past its line.
    std::vector<std::uint8_t> nodes;

    /// contribution_count × (varint version, 8-byte little-endian variant hash —
    /// hashes are random bits, varint would only inflate them)
    std::vector<std::uint8_t> contributions;

    std::vector<std::uint32_t> absent;

    std::vector<std::uint32_t> imports;

    /// TUManifest::shared_locals as columns: the symbols ascending, each
    /// one's file versions back to back.
    std::vector<std::uint64_t> local_symbols;
    std::vector<std::uint32_t> local_file_ends;
    std::vector<std::uint32_t> local_files;

    /// TUManifest::instantiations as columns: per file its version and its
    /// use.
    std::vector<std::uint32_t> instantiation_files;
    std::vector<std::uint64_t> instantiation_uses;
};

/// The bytes the fields of persisted blobs hold, summed over the blobs
/// measured, by field path (`occurrences.packed`) in declaration order — how
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

}  // namespace clice::index
