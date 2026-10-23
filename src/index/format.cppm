module;

#include "support/logging.macros.h"
#include "support/prelude.h"

/// The persisted index format: every blob layout the index writes, to the
/// database or between processes, and the version gating them. Writers,
/// load-time validation and the hand-built blobs of corruption tests share
/// the layouts; everything else consumes blobs through their readers.

module clice:index.format;

import :index.symbol;
import :support.bytes;
import :support.logging;
import :syntax.token;

namespace clice::index {

/// On-disk index blob schema version. Every persisted blob carries it as a
/// regular field and every loader discards blobs with a different value; a
/// blob of another layout fails the blob codec's check before that.
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
/// table lists the files the shared locals name per symbol; v41: the names
/// a focus file's doc comments document with `@param` and `@tparam` are
/// references to those parameters, an abbreviated parameter pack carries
/// its type definitions, and a precompiled header's diagnostics know an
/// imported module unit from an included file; v42: no blob is a
/// flatbuffer any more; v43: every blob is kotatsu's (codec::blob),
/// whose header differs; v44: the project table is one record per symbol,
/// per file and per path behind a header, manifests name their files by
/// path id and content hash, and producers name their unit by its path
/// hash; v45: a manifest keeps the memory its unit's worker reached.
constexpr inline std::uint32_t index_format_version = 45;

/// The bytes of `blob`, the writer form of a blob layout. A blob holds less
/// than 4 GiB; index blobs stay far below it (LLVM's search blob, the
/// largest, is about 75 MB), so one past it stops the process.
template <typename Blob>
std::vector<std::byte> encode(const Blob& blob) {
    auto bytes = codec::blob::to_bytes(blob);
    if(!bytes) {
        LOG_FATAL("Cannot encode an index blob: {}", bytes.error().to_string());
    }
    return std::move(*bytes);
}

/// Whether `bytes` sit where a blob is read in place: a database or a
/// message hands out bytes at whatever address it keeps them.
inline bool readable_in_place(llvm::StringRef bytes) {
    return reinterpret_cast<std::uintptr_t>(bytes.data()) % codec::blob::alignment == 0;
}

/// `buffer` itself when its bytes are readable_in_place, else an aligned
/// copy of them.
inline std::unique_ptr<llvm::MemoryBuffer>
    in_place_buffer(std::unique_ptr<llvm::MemoryBuffer> buffer) {
    if(buffer && !readable_in_place(buffer->getBuffer())) {
        return llvm::MemoryBuffer::getMemBufferCopy(buffer->getBuffer());
    }
    return buffer;
}

/// The root of a blob of layout `Blob`, read in place, once its bytes pass
/// the structural check and carry this build's format version; null, logged
/// as the rejection of `what`, otherwise. The cross-field checks are the
/// reader's.
template <typename Blob>
const codec::blob::view_t<Blob>* verify_root(llvm::StringRef bytes, llvm::StringRef what) {
    auto root = codec::blob::view<Blob>(as_bytes(bytes));
    if(!root) {
        LOG_DEBUG("Rejecting {}: {}", what, root.error().to_string());
        return nullptr;
    }
    if((*root)->format_version != index_format_version) {
        LOG_DEBUG("Rejecting {}: format version {}, this build reads {}",
                  what,
                  (*root)->format_version,
                  index_format_version);
        return nullptr;
    }
    return *root;
}

/// Swap a reader's backing buffer for byte-identical bytes elsewhere (a
/// newer read snapshot) that are readable_in_place, as a database read
/// gives: what the reader verified and derived describes the
/// bytes, which carry over. False, keeping `buffer`, when the sizes differ.
inline bool rebind_buffer(std::unique_ptr<llvm::MemoryBuffer>& buffer,
                          std::unique_ptr<llvm::MemoryBuffer> replacement) {
    if(!buffer || !replacement || replacement->getBufferSize() != buffer->getBufferSize()) {
        return false;
    }
    buffer = std::move(replacement);
    return true;
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
template <template <typename> typename Seq>
using IdColumnOf = Seq<std::uint8_t>;

using IdColumn = IdColumnOf<codec::blob::Owned>;

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
template <template <typename> typename Seq>
struct RangeColumnsOf {
    Seq<std::uint32_t> block_begins;
    Seq<std::uint8_t> deltas;
    Seq<std::uint32_t> far_entries;
    Seq<std::uint32_t> far_begins;

    Seq<std::uint32_t> packed;

    Seq<std::uint32_t> begins;
    Seq<std::uint8_t> lengths;

    Seq<std::uint32_t> long_entries;
    Seq<std::uint32_t> long_ends;

    Seq<std::uint8_t> masks8;
    Seq<std::uint16_t> masks16;
    Seq<std::uint32_t> masks32;
    Seq<std::uint64_t> masks64;
    Seq<std::uint32_t> roaring_offsets;
    Seq<std::uint8_t> roaring;
};

using RangeColumns = RangeColumnsOf<codec::blob::Owned>;

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
    IdColumnOf<Seq> occurrence_symbols;

    /// Relations in symbol-table order, sorted by (kind, begin, end,
    /// payload) within each group.
    RangeColumnsOf<Seq> relations;
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

/// One file's index in the TU index blob: a self-contained single-variant shard
/// blob (index/shard.cppm). `hash` is xxh3 of `blob` — the variant's
/// identity — so the master can skip blobs it already stores without
/// touching their bytes; the blob is left empty for a variant the master
/// said it stores.
template <template <typename> typename Seq>
struct FileSectionOf {
    std::uint32_t path_id = 0;

    std::uint64_t hash = 0;

    Seq<std::byte> blob;
};

using FileSection = FileSectionOf<codec::blob::Owned>;

/// One file's instantiations in the TU index: the instantiation blob of what
/// the run resolved there, empty when it resolved nothing, and the known keys
/// whose definition the file holds that the run skipped, ascending.
template <template <typename> typename Seq>
struct InstantiationSectionOf {
    std::uint32_t path_id = 0;

    Seq<std::byte> blob;

    Seq<std::uint64_t> skipped;
};

using InstantiationSection = InstantiationSectionOf<codec::blob::Owned>;

/// A DocumentLink as the TU index of a preamble persists it.
template <template <typename> typename Seq>
struct LinkOf {
    LocalSourceRange range;
    Seq<char> target;
};

/// The TU index's serialized layout. Only the indexer materializes it (and
/// tests planting shapes it never emits); every consumer reads the bytes
/// through the TUIndex reader.
template <template <typename> typename Seq>
struct TUIndexBlobOf {
    /// Serialized schema version (index_format_version), gated by
    /// TUIndex::from_bytes.
    /// A worker respawned after the binary on disk changed can be one
    /// build ahead of the server, and a change of meaning keeps the layout.
    std::uint32_t format_version = 0;

    /// Milliseconds since epoch, sampled before the build started.
    std::int64_t built_at = 0;

    /// The include tree (IncludeTree's persisted vectors): the path table,
    /// the consumed-content hash per path, and every include edge of the
    /// parse.
    Seq<Seq<char>> paths;
    Seq<std::uint64_t> content_hashes;
    Seq<IncludeNode> nodes;

    /// The symbols the entries name, sorted by hash, except the file-local
    /// ones (a function's locals, a template's parameters) — only their
    /// own file can mention one, and that file's section names it — and
    /// the external symbols only sections left empty name. Names, arguments and signatures are
    /// back to back with one end offset per entry; so are the reference files, as path ids.
    Seq<std::uint64_t> symbol_hashes;
    Seq<char> symbol_names;
    Seq<std::uint32_t> symbol_name_ends;
    Seq<char> symbol_args;
    Seq<std::uint32_t> symbol_args_ends;
    Seq<char> symbol_signatures;
    Seq<std::uint32_t> symbol_signature_ends;
    Seq<std::uint64_t> symbol_parents;
    Seq<std::uint8_t> symbol_kinds;
    Seq<std::uint8_t> symbol_scopes;
    Seq<std::uint16_t> symbol_flags;
    Seq<std::uint8_t> symbol_token_kinds;
    Seq<std::uint32_t> symbol_token_modifiers;
    /// Path ids, `no_file` for a symbol without a declaring relation.
    Seq<std::uint32_t> symbol_files;
    Seq<std::uint32_t> symbol_reference_ends;
    Seq<std::uint32_t> symbol_references;

    /// Per symbol, the files whose instantiation section names it, back to
    /// back as path ids.
    Seq<std::uint32_t> symbol_instantiation_ends;
    Seq<std::uint32_t> symbol_instantiations;

    /// One entry per file with an index, ascending by path id.
    Seq<FileSectionOf<Seq>> sections;

    /// One entry per file the run's instantiations or macro expansions
    /// resolve something in or skipped a known key of, ascending by path id.
    Seq<InstantiationSectionOf<Seq>> instantiations;

    /// Preamble ride-alongs, empty on ordinary TU indexes: identity of the
    /// exact preamble text the PCH was built from (matches_preamble), and
    /// the PCH-derived feature state spliced into focus-file results.
    std::uint64_t preamble_hash = 0;
    std::uint32_t preamble_size = 0;
    Seq<LinkOf<Seq>> links;
    Seq<std::uint32_t> inactive_regions;
    Seq<std::uint8_t> open_conditionals;

    /// The places the parse's failed lookups looked that held no file
    /// (CompilationUnit::absent).
    Seq<Seq<char>> absent;

    /// Preamble ride-along: the diagnostics the preamble's build raised,
    /// as published (a JSON array of LSP diagnostics) — a compile that
    /// consumes the PCH never raises them again.
    Seq<char> diagnostics;
};

using TUIndexBlob = TUIndexBlobOf<codec::blob::Owned>;

/// The index database's header, written by every save. Its format version
/// covers every record: a database of another one is discarded whole.
template <template <typename> typename Seq>
struct HeaderBlobOf {
    std::uint32_t format_version = 0;

    /// The path id the next file the database names receives.
    std::uint32_t next_path = 0;

    /// The generation of the search blob saved with this header, and the
    /// symbols and the files (path ids) whose names of internal linkage
    /// changed since that blob was built, ascending.
    std::uint64_t search_generation = 0;
    Seq<std::uint64_t> search_pending;
    Seq<std::uint32_t> search_pending_files;

    /// The units (path ids) owed a reindex because a rebuild of a file they
    /// contribute to discarded their variants, ascending: their files' content
    /// reads fresh, so no start-up scan would find them.
    Seq<std::uint32_t> owed_units;
};

using HeaderBlob = HeaderBlobOf<codec::blob::Owned>;

/// What the database knows of one file, small enough for the start-up
/// scan to read for every file: its path id, the contents the units read,
/// what its shard stores and, for a unit, its current run.
template <template <typename> typename Seq>
struct FileBlobOf {
    std::uint32_t path = 0;

    /// Some unit's failed lookup looked here and found no file.
    bool absent = false;

    /// The content hashes the units entering the file, or reading it
    /// through a module or a precompiled header, consumed, ascending.
    Seq<std::uint64_t> consumed;

    /// The content generation of the file's shard, 0 for none, and the
    /// variants it stores in mask order — what a merge or a worker may
    /// take as stored without reading the shard.
    std::uint64_t generation = 0;
    Seq<std::uint64_t> stored;

    /// For a unit, its manifest's built_at; 0 when the file is no unit.
    std::uint64_t run = 0;
};

using FileBlob = FileBlobOf<codec::blob::Owned>;

/// A set of units (path ids, as a file set: append_file_set) under a key.
template <template <typename> typename Seq>
struct UnitSetOf {
    std::uint64_t key = 0;
    Seq<std::uint8_t> units;
};

/// How a set of units resolved a file's own `#include` lines at one
/// content: the directive lines and their targets (path ids), in line
/// order.
template <template <typename> typename Seq>
struct IncludeResolutionOf {
    std::uint64_t content = 0;
    Seq<std::uint32_t> lines;
    Seq<std::uint32_t> targets;
    Seq<std::uint8_t> units;
};

/// The units depending on one file, each list ascending by key.
template <template <typename> typename Seq>
struct DependentsBlobOf {
    /// Per consumed content hash, the units that entered the file at it.
    Seq<UnitSetOf<Seq>> readers;
    /// Per variant, the units that contributed it.
    Seq<UnitSetOf<Seq>> contributors;
    /// The units whose failed lookups looked here.
    Seq<std::uint8_t> absent;
    /// Per content hash, the units reading the file through a module they
    /// import or the precompiled header they load.
    Seq<UnitSetOf<Seq>> importers;
    /// Per instantiation use, the units using it.
    Seq<UnitSetOf<Seq>> uses;
    /// The distinct resolutions of the file's own `#include` lines.
    Seq<IncludeResolutionOf<Seq>> resolutions;
};

using DependentsBlob = DependentsBlobOf<codec::blob::Owned>;

/// The names of internal linkage a file's shard declares, for the search
/// index, ascending by hash.
template <template <typename> typename Seq>
struct InternalsBlobOf {
    Seq<std::uint64_t> hashes;
    Seq<char> names;
    Seq<std::uint32_t> name_ends;
    Seq<char> args;
    Seq<std::uint32_t> args_ends;
    Seq<std::uint64_t> parents;
    Seq<std::uint8_t> kinds;
    Seq<std::uint16_t> flags;
};

using InternalsBlob = InternalsBlobOf<codec::blob::Owned>;

/// One external symbol's record.
template <template <typename> typename Seq>
struct SymbolBlobOf {
    /// The name, the arguments and the signature back to back.
    Seq<char> text;
    std::uint32_t name_size = 0;
    std::uint32_t args_size = 0;

    std::uint64_t parent = 0;

    /// The canonical declaration's path id plus one, 0 for none.
    std::uint32_t file = 0;

    std::uint32_t token_modifiers = 0;
    std::uint16_t flags = 0;
    std::uint8_t kind = 0;
    std::uint8_t token_kind = 0;

    /// The file sets of the files referencing the symbol and of those whose
    /// instantiation shards name it, back to back.
    Seq<std::uint8_t> files;
    std::uint32_t references_size = 0;
};

using SymbolBlob = SymbolBlobOf<codec::blob::Owned>;

/// A file set (append_file_set) and how many units list it.
template <template <typename> typename Seq>
struct ListedFilesOf {
    std::uint32_t units = 0;
    Seq<std::uint8_t> files;
};

/// A symbol below external scope several files hold: the file sets the
/// units list for it (TUManifest::shared_locals), each once.
template <template <typename> typename Seq>
struct SharedLocalBlobOf {
    Seq<ListedFilesOf<Seq>> sets;
};

using SharedLocalBlob = SharedLocalBlobOf<codec::blob::Owned>;

/// Layout and scoring version of the search blob, apart from the index
/// format it is derived from: a bump rebuilds the search index alone.
constexpr std::uint32_t search_format_version = 3;

/// The persisted search index. Entries are in quality order (ties by hash),
/// which is the entry id order every bitmap speaks; the arenas hold the
/// names back to back with one end offset per entry; posting arenas hold
/// posting lists over the entries (write_postings) the same way.
template <template <typename> typename Seq>
struct SearchIndexBlobOf {
    std::uint32_t format_version = 0;
    std::uint32_t search_version = 0;
    /// See SearchSnapshot::generation.
    std::uint64_t generation = 0;

    Seq<std::uint64_t> hashes;
    Seq<char> names;
    Seq<std::uint32_t> name_ends;
    Seq<char> args;
    Seq<std::uint32_t> args_ends;
    /// The entry of the nearest enclosing symbol that is not an inline
    /// namespace, or no_entry at the translation unit or when that symbol
    /// is not in the index.
    Seq<std::uint32_t> parents;
    Seq<float> qualities;
    /// Entries sorted by lowercase name, then name, then entry.
    Seq<std::uint32_t> by_lower_name;
    /// Entries sorted by hash.
    Seq<std::uint32_t> by_hash;

    Seq<std::uint32_t> token_keys;
    Seq<std::uint32_t> token_ends;
    Seq<std::uint8_t> token_postings;

    /// Entries with members, sorted; their members as bitmaps.
    Seq<std::uint32_t> container_entries;
    Seq<std::uint32_t> children_ends;
    Seq<std::uint8_t> children_postings;
    /// The entries with no parent in the index, one posting list.
    Seq<std::uint8_t> top_level;

    /// The entries whose name runs past the token bound (name_bound), one
    /// posting list: their tail carries no token, so every name lookup scans
    /// them.
    Seq<std::uint8_t> long_names;

    /// Namespace entries, sorted; everything below each as bitmaps.
    Seq<std::uint32_t> subtree_entries;
    Seq<std::uint32_t> subtree_ends;
    Seq<std::uint8_t> subtree_postings;

    /// One bitmap per SymbolKind value.
    Seq<std::uint32_t> kind_ends;
    Seq<std::uint8_t> kind_postings;

    /// The files of the entries, one bitmap each.
    Seq<Seq<char>> paths;
    Seq<std::uint32_t> file_ends;
    Seq<std::uint8_t> file_postings;

    /// The entries of internal linkage, sorted, and the path each one's
    /// entries live in.
    Seq<std::uint32_t> local_entries;
    Seq<std::uint32_t> local_paths;
};

using SearchIndexBlob = SearchIndexBlobOf<codec::blob::Owned>;

/// The manifest's persisted form. Its file versions are a table of its
/// own, (path id, content hash) each, which every other field indexes; a
/// path appears twice when the modules a unit imports were built from two
/// versions of it. Node and contribution payloads are varint-packed by
/// hand: a big TU enters thousands of files, and LEB128 on the small ids
/// and lines roughly halves the blob against fixed-width columns.
template <template <typename> typename Seq>
struct TUManifestBlobOf {
    std::uint32_t format_version = 0;

    std::uint64_t built_at = 0;

    std::uint64_t peak = 0;

    Seq<std::uint32_t> version_paths;
    Seq<std::uint64_t> version_hashes;

    std::uint32_t tu_version = 0;

    std::uint32_t node_count = 0;

    std::uint32_t contribution_count = 0;

    /// node_count × (varint version, varint parent, varint line): the parent 0
    /// for the previous node's, 1 for the root, else its index plus 2; the
    /// line shifted left by two over a bit for a skipped node and a bit for
    /// a line stored as its distance past the previous node's — only
    /// beside the previous node's parent, at or past its line.
    Seq<std::uint8_t> nodes;

    /// contribution_count × (varint version, 8-byte little-endian variant hash —
    /// hashes are random bits, varint would only inflate them)
    Seq<std::uint8_t> contributions;

    Seq<std::uint32_t> absent;

    Seq<std::uint32_t> imports;

    /// TUManifest::shared_locals as columns: the symbols ascending, each
    /// one's file versions back to back.
    Seq<std::uint64_t> local_symbols;
    Seq<std::uint32_t> local_file_ends;
    Seq<std::uint32_t> local_files;

    /// TUManifest::instantiations as columns: per file its version and its
    /// use.
    Seq<std::uint32_t> instantiation_files;
    Seq<std::uint64_t> instantiation_uses;
};

using TUManifestBlob = TUManifestBlobOf<codec::blob::Owned>;

/// The bytes the fields of persisted blobs hold, summed over the blobs
/// measured, by field path (`occurrences.packed`) in declaration order — how
/// `clice index stats` splits a blob kind. Scalar fields, the pairs placing
/// strings and sequences, and padding are left to the framing, the rest of
/// the blobs' size.
struct FieldBytes {
    llvm::SmallVector<std::pair<std::string, std::uint64_t>> fields;

    void add(llvm::StringRef name, std::uint64_t bytes) {
        auto it = ranges::find_if(fields, [&](const auto& field) { return field.first == name; });
        if(it == fields.end()) {
            fields.emplace_back(name.str(), bytes);
        } else {
            it->second += bytes;
        }
    }
};

/// Add the fields of a blob of layout `Blob` to `bytes`; nothing for bytes
/// that do not verify as one.
template <typename Blob>
void measure_blob(llvm::StringRef data, FieldBytes& bytes) {
    if(auto root = codec::blob::view<Blob>(as_bytes(data))) {
        codec::blob::measure(**root, [&](llvm::StringRef path, std::uint64_t size) {
            bytes.add(path, size);
        });
    }
}

}  // namespace clice::index
