module;

#include "modules/prelude.h"

#include "support/logging.macros.h"

module clice;

import :index.columns;
import :index.format;
import :index.shard;
import :semantic.token_class;
import :support.logging;
import :syntax.position;

namespace clice::index {

/// How a blob encodes its variant masks, a strict function of the variant
/// count.
enum class MaskEncoding : std::uint8_t {
    /// One variant: no mask columns at all.
    Single,
    U8,
    U16,
    U32,
    U64,
    Roaring,
};

namespace {

using BlobView = kota::codec::fbs::table_view<ShardBlob>;

MaskEncoding encoding_of(std::size_t variant_count) {
    if(variant_count <= 1) {
        return MaskEncoding::Single;
    }
    if(variant_count <= 8) {
        return MaskEncoding::U8;
    }
    if(variant_count <= 16) {
        return MaskEncoding::U16;
    }
    if(variant_count <= 32) {
        return MaskEncoding::U32;
    }
    if(variant_count <= 64) {
        return MaskEncoding::U64;
    }
    return MaskEncoding::Roaring;
}

/// The blob was fully verified at load; per-query views skip that cost.
BlobView view_of(llvm::StringRef bytes) {
    return BlobView::from_verified_bytes(blob_bytes(bytes));
}

BlobView root_of(const llvm::MemoryBuffer& buffer) {
    return view_of(buffer.getBuffer());
}

MaskEncoding encoding_of(BlobView root) {
    return encoding_of(to_array_ref(root[&ShardBlob::variants]).size());
}

/// The mask of `entry` in an encoding of up to 64 variants.
std::uint64_t mask_bits(MaskEncoding encoding,
                        const RangeColumnsView& columns,
                        std::uint32_t entry) {
    switch(encoding) {
        case MaskEncoding::Single: return 1;
        case MaskEncoding::U8: return columns.masks8[entry];
        case MaskEncoding::U16: return columns.masks16[entry];
        case MaskEncoding::U32: return columns.masks32[entry];
        case MaskEncoding::U64: return columns.masks64[entry];
        case MaskEncoding::Roaring: break;
    }
    std::unreachable();
}

}  // namespace

/// The live mask against one side's mask columns, resolved once per query.
struct LiveFilter {
    /// Fast path: every stored variant is live, no per-entry filtering.
    bool all;
    MaskEncoding encoding;
    std::uint64_t bits;
    const Bitmap* big;
    const RangeColumnsView* columns;

    bool operator()(std::uint32_t entry) const {
        if(all) {
            return true;
        }
        if(encoding == MaskEncoding::Roaring) {
            return columns->bitmap_of(entry).intersect(*big);
        }
        return (mask_bits(encoding, *columns, entry) & bits) != 0;
    }
};

namespace {

/// The occurrence table: each occurrence's range and the symbol it names.
struct OccurrenceTable {
    RangeColumnsView columns;
    IdColumnView syms;
    llvm::ArrayRef<std::uint64_t> sym_hashes;

    static OccurrenceTable of(BlobView root) {
        auto sym_hashes = to_array_ref(root[&ShardBlob::sym_hashes]);
        return {
            .columns = columns_of(root[&ShardBlob::occs]),
            .syms = ids_of(root[&ShardBlob::occ_syms], sym_hashes.size()),
            .sym_hashes = sym_hashes,
        };
    }

    std::size_t size() const {
        return columns.size();
    }

    Occurrence at(std::uint32_t entry, LocalSourceRange range) const {
        return {range, sym_hashes[syms[entry]]};
    }
};

/// Whether an extent fits the u16 extent columns beside its relation's range:
/// as the distance back to its begin and its length.
bool fits_extent_columns(LocalSourceRange range, LocalSourceRange extent) {
    return extent.begin <= range.begin && range.begin - extent.begin < extent_escape &&
           extent.end >= extent.begin && extent.end - extent.begin <= 0xffff;
}

/// The relation table: relations grouped per symbol, each with its kind,
/// range and payload. The kinds carrying an extent hold one in the extent
/// columns, the others a target symbol's id in the target column, each
/// column listing the payloads in relation order — walked by a cursor in
/// stored order, placed by the payload counts before each block.
struct RelationTable {
    RangeColumnsView columns;
    llvm::ArrayRef<std::uint8_t> kinds;
    IdColumnView target_ranks;
    IdColumnView extent_ranks;
    IdColumnView targets;
    llvm::ArrayRef<std::uint16_t> extent_heads;
    llvm::ArrayRef<std::uint16_t> extent_lengths;
    llvm::ArrayRef<std::uint32_t> far_extents;
    llvm::ArrayRef<std::uint32_t> far_begins;
    llvm::ArrayRef<std::uint32_t> far_ends;
    llvm::ArrayRef<std::uint64_t> sym_hashes;
    /// Entry i's relations occupy [offsets[i], offsets[i + 1]).
    IdColumnView offsets;

    static RelationTable of(BlobView root) {
        auto columns = columns_of(root[&ShardBlob::rels]);
        auto sym_hashes = to_array_ref(root[&ShardBlob::sym_hashes]);
        auto targets = ids_of(root[&ShardBlob::rel_targets], sym_hashes.size());
        auto extent_heads = to_array_ref(root[&ShardBlob::rel_extent_heads]);
        return {
            .columns = columns,
            .kinds = to_array_ref(root[&ShardBlob::rel_kinds]),
            .target_ranks = ids_of(root[&ShardBlob::rel_target_ranks], targets.size() + 1),
            .extent_ranks = ids_of(root[&ShardBlob::rel_extent_ranks], extent_heads.size() + 1),
            .targets = targets,
            .extent_heads = extent_heads,
            .extent_lengths = to_array_ref(root[&ShardBlob::rel_extent_lengths]),
            .far_extents = to_array_ref(root[&ShardBlob::rel_far_extents]),
            .far_begins = to_array_ref(root[&ShardBlob::rel_far_begins]),
            .far_ends = to_array_ref(root[&ShardBlob::rel_far_ends]),
            .sym_hashes = sym_hashes,
            .offsets = ids_of(root[&ShardBlob::rel_offsets], columns.size() + 1),
        };
    }

    std::size_t size() const {
        return columns.size();
    }

    RelationKind kind_of(std::uint32_t entry) const {
        return static_cast<RelationKind::Kind>(kinds[entry] & ~payload_bit);
    }

    bool has_payload(std::uint32_t entry) const {
        return (kinds[entry] & payload_bit) != 0;
    }

    /// Positions in the two payload columns; decode() advances them, so
    /// relations must be decoded one after another from where the cursor
    /// was placed.
    struct Cursor {
        std::size_t target = 0;
        std::size_t extent = 0;
    };

    void advance(Cursor& cursor, std::uint32_t entry) const {
        if(has_payload(entry)) {
            (kind_of(entry).carries_extent() ? cursor.extent : cursor.target) += 1;
        }
    }

    Cursor cursor_at(std::uint32_t entry) const {
        if(entry == size()) {
            return {targets.size(), extent_heads.size()};
        }
        auto block = entry / rank_block;
        Cursor cursor{target_ranks[block], extent_ranks[block]};
        for(auto i = block * rank_block; i < entry; i += 1) {
            advance(cursor, i);
        }
        return cursor;
    }

    /// Extent `k`, of a relation whose range begins at `name`.
    LocalSourceRange extent_at(std::size_t k, std::uint32_t name) const {
        if(extent_heads[k] == extent_escape) {
            auto far = escape_position(far_extents, k);
            return {far_begins[far], far_ends[far]};
        }
        auto begin = name - extent_heads[k];
        return {begin, begin + extent_lengths[k]};
    }

    Relation decode(std::uint32_t entry, Cursor& cursor) const {
        Relation relation{
            .kind = kind_of(entry),
            .range = columns.range_of(entry),
            .target_symbol = 0,
        };
        if(has_payload(entry)) {
            if(RelationKind(relation.kind).carries_extent()) {
                relation.set_definition_range(extent_at(cursor.extent, relation.range.begin));
            } else {
                relation.target_symbol = sym_hashes[targets[cursor.target]];
            }
        }
        advance(cursor, entry);
        return relation;
    }
};

/// The local-name table: the symbols whose identities live in no other
/// table, sparse over the symbol table in ascending id order.
struct LocalTable {
    IdColumnView syms;
    llvm::StringRef names;
    IdColumnView name_ends;
    IdColumnView args_of;
    llvm::StringRef args;
    IdColumnView args_ends;
    IdColumnView signatures_of;
    llvm::StringRef signatures;
    IdColumnView signature_ends;
    llvm::ArrayRef<std::uint8_t> kinds;
    llvm::ArrayRef<std::uint8_t> scopes;
    IdColumnView parents;
    llvm::ArrayRef<std::uint16_t> flags;
    llvm::ArrayRef<std::uint8_t> token_kinds;
    llvm::ArrayRef<std::uint32_t> token_modifiers;
    llvm::ArrayRef<std::uint64_t> sym_hashes;

    static LocalTable of(BlobView root) {
        auto sym_hashes = to_array_ref(root[&ShardBlob::sym_hashes]);
        auto syms = ids_of(root[&ShardBlob::local_syms], sym_hashes.size());
        auto names = to_ref(root[&ShardBlob::local_names]);
        auto args = to_ref(root[&ShardBlob::local_args]);
        auto signatures = to_ref(root[&ShardBlob::local_signatures]);
        return {
            .syms = syms,
            .names = names,
            .name_ends = ids_of(root[&ShardBlob::local_name_ends], names.size() + 1),
            .args_of = ids_of(root[&ShardBlob::local_args_of], syms.size()),
            .args = args,
            .args_ends = ids_of(root[&ShardBlob::local_args_ends], args.size() + 1),
            .signatures_of = ids_of(root[&ShardBlob::local_signatures_of], syms.size()),
            .signatures = signatures,
            .signature_ends = ids_of(root[&ShardBlob::local_signature_ends], signatures.size() + 1),
            .kinds = to_array_ref(root[&ShardBlob::local_kinds]),
            .scopes = to_array_ref(root[&ShardBlob::local_scopes]),
            .parents = ids_of(root[&ShardBlob::local_parents], sym_hashes.size() + 1),
            .flags = to_array_ref(root[&ShardBlob::local_flags]),
            .token_kinds = to_array_ref(root[&ShardBlob::local_token_kinds]),
            .token_modifiers = to_array_ref(root[&ShardBlob::local_token_modifiers]),
            .sym_hashes = sym_hashes,
        };
    }

    std::size_t size() const {
        return syms.size();
    }

    SymbolHash hash_of(std::size_t k) const {
        return sym_hashes[syms[k]];
    }

    SymbolIdentity at(std::size_t k) const {
        auto position = static_cast<std::uint32_t>(k);
        auto args_at = args_of.find(position);
        auto signature_at = signatures_of.find(position);
        return {
            .name = name_ends.slice(names, k),
            .args = args_at ? args_ends.slice(args, *args_at) : llvm::StringRef(),
            .signature =
                signature_at ? signature_ends.slice(signatures, *signature_at) : llvm::StringRef(),
            .parent = parents[k] == 0 ? 0 : sym_hashes[parents[k] - 1],
            .kind = SymbolKind(kinds[k]),
            .scope = static_cast<SymbolScope>(scopes[k]),
            .flags = static_cast<SymbolFlags>(flags[k]),
            .token_class = {.kind = static_cast<SemanticTokenKind>(token_kinds[k]),
                            .modifiers = token_modifiers[k]},
        };
    }
};

// A variant is identified by its hash everywhere (set_live, the merge
// keep filter), so stored hashes must be unique: occurrences and
// relations owned only by a duplicated entry would serve and survive
// compaction with no contribution owning them.
CheckResult variants_ok(BlobView root) {
    auto variants = to_array_ref(root[&ShardBlob::variants]);
    llvm::SmallVector<VariantHash> sorted(variants.begin(), variants.end());
    std::ranges::sort(sorted);
    if(std::ranges::adjacent_find(sorted) != sorted.end()) {
        return std::unexpected("duplicate variant identity");
    }
    return {};
}

// The line table reconstructs every line start by prefix sum, so it must
// both pair with its escape table and add up to exactly the content size —
// a drifted sum would shift every position mapping below the corruption.
// Position mapping reads a listed non-ASCII line's text in place of the
// line: it must be exactly the line's text without its end, and hold a
// non-ASCII byte — a pure-ASCII line is the absent entry's to spell.
CheckResult line_table_ok(BlobView root) {
    auto line_lengths = to_array_ref(root[&ShardBlob::line_lengths]);
    auto long_lines = to_array_ref(root[&ShardBlob::long_lines]);
    auto long_line_lengths = to_array_ref(root[&ShardBlob::long_line_lengths]);
    if(line_lengths.empty() ||
       !escapes_ok(line_lengths, length_escape, long_lines, long_line_lengths.size())) {
        return std::unexpected("line table does not pair with its escape table");
    }
    // A "\r\n" ending needs a line holding at least those two bytes and a
    // line after it.
    auto crlf_lines = to_array_ref(root[&ShardBlob::crlf_lines]);
    if(!crlf_lines.empty() &&
       (crlf_lines.back() == 0 || crlf_lines.size() > (line_lengths.size() + 63) / 64)) {
        return std::unexpected("CRLF line table is not canonical");
    }
    auto utf8_lines = to_array_ref(root[&ShardBlob::utf8_lines]);
    auto utf8_text = to_ref(root[&ShardBlob::utf8_text]);
    auto utf8_ends = to_array_ref(root[&ShardBlob::utf8_text_ends]);
    if(utf8_ends.size() != utf8_lines.size() || !covering_ends(utf8_ends, utf8_text.size())) {
        return std::unexpected("non-ASCII line columns do not line up");
    }
    std::uint64_t line_sum = 0;
    std::size_t escape_cursor = 0;
    std::uint32_t utf8_cursor = 0;
    for(std::size_t line = 0; line < line_lengths.size(); line += 1) {
        std::uint32_t length = line_lengths[line];
        if(length == length_escape) {
            length = long_line_lengths[escape_cursor];
            escape_cursor += 1;
            if(length < length_escape) {
                return std::unexpected("escaped line length below the escape threshold");
            }
        }
        auto last = line + 1 == line_lengths.size();
        auto crlf = ends_crlf(crlf_lines, line);
        if(crlf && (length < 2 || last)) {
            return std::unexpected("CRLF bit on a line that cannot end in CRLF");
        }
        if(utf8_cursor < utf8_lines.size() && utf8_lines[utf8_cursor] == line) {
            auto text = back_to_back(utf8_text, utf8_ends, utf8_cursor);
            if(text.size() != (last ? length : length - (crlf ? 2 : 1)) || text.contains('\n') ||
               kota::ipc::lsp::is_ascii({text.data(), text.size()})) {
                return std::unexpected("a non-ASCII line's text does not match its line");
            }
            utf8_cursor += 1;
        }
        line_sum += length;
    }
    if(utf8_cursor != utf8_lines.size()) {
        return std::unexpected("non-ASCII lines are not ascending lines of the table");
    }
    if(line_sum != root[&ShardBlob::content_size]) {
        return std::unexpected("line lengths do not add up to the content size");
    }
    return {};
}

// Strictly sorted symbol hashes: lookups lower-bound the hash column and
// read only the first match's slices, so a duplicated hash would strand
// the later id's relations and local name unreachably.
CheckResult symbol_table_ok(BlobView root) {
    auto sym_hashes = to_array_ref(root[&ShardBlob::sym_hashes]);
    auto table = RelationTable::of(root);
    auto relations = table.size();
    if(auto ok = ascending_ids_ok(table.offsets, sym_hashes.size() + 1, relations + 1, false);
       !ok) {
        return ok;
    }
    if(table.offsets[0] != 0 || table.offsets[sym_hashes.size()] != relations) {
        return std::unexpected("relation offsets are not a partition of the relations");
    }
    if(!std::ranges::is_sorted(sym_hashes, std::less_equal{})) {
        return std::unexpected("symbol hashes are not strictly ascending");
    }
    return {};
}

CheckResult range_encodings_ok(BlobView root) {
    auto content_size = root[&ShardBlob::content_size];
    if(auto ok = range_encoding_ok(columns_of(root[&ShardBlob::occs]),
                                   content_size,
                                   RangeEncoding::Delta);
       !ok) {
        return ok;
    }
    return range_encoding_ok(columns_of(root[&ShardBlob::rels]),
                             content_size,
                             unsorted_encoding(content_size));
}

// lookup(offset) binary-searches the decoded end column and stops its
// containment walk on begin order, which the delta encoding proves; ends out
// of order (a corrupt escaped end included) would silently miss or
// misresolve occurrences on every query, forever — reject the blob so it is
// rebuilt instead.
// The merge also two-way merges occurrences under the full (begin, end, sym)
// key with equal-key occurrences combined at write time, so equal ranges must
// carry strictly ascending symbols — sym_hashes is strictly sorted, so
// id order stands in for hash order.
CheckResult occurrences_ok(BlobView root) {
    auto table = OccurrenceTable::of(root);
    if(auto ok = ids_ok(table.syms, table.size(), table.sym_hashes.size()); !ok) {
        return ok;
    }
    LocalSourceRange prev{0, 0};
    std::uint32_t prev_sym = 0;
    CheckResult result;
    table.columns.for_each_range([&](std::uint32_t entry, LocalSourceRange range) {
        auto sym = table.syms[entry];
        if(range.end < prev.end) {
            result = std::unexpected("occurrence ends are out of order");
        } else if(entry != 0 && range == prev && sym <= prev_sym) {
            result = std::unexpected("occurrences of one range are not strictly ascending");
        }
        prev = range;
        prev_sym = sym;
        return result.has_value();
    });
    return result;
}

CheckResult relations_ok(BlobView root) {
    auto table = RelationTable::of(root);
    auto content_size = root[&ShardBlob::content_size];
    if(table.kinds.size() != table.size()) {
        return std::unexpected("relation kinds do not match the row count");
    }

    // Readers place a payload by its block's counts and the payload bits
    // before it, trusting both: one wrong count would hand every later
    // relation of the block another relation's payload.
    auto blocks = (table.size() + rank_block - 1) / rank_block;
    if(auto ok = ids_ok(table.target_ranks, blocks, table.targets.size() + 1); !ok) {
        return ok;
    }
    if(auto ok = ids_ok(table.extent_ranks, blocks, table.extent_heads.size() + 1); !ok) {
        return ok;
    }
    RelationTable::Cursor counted;
    for(std::uint32_t entry = 0; entry < table.size(); entry += 1) {
        if(entry % rank_block == 0 && (table.target_ranks[entry / rank_block] != counted.target ||
                                       table.extent_ranks[entry / rank_block] != counted.extent)) {
            return std::unexpected("relation payload counts are wrong");
        }
        table.advance(counted, entry);
    }
    if(auto ok = ids_ok(table.targets, counted.target, table.sym_hashes.size()); !ok) {
        return ok;
    }
    if(table.extent_heads.size() != counted.extent ||
       table.extent_lengths.size() != counted.extent ||
       table.far_ends.size() != table.far_begins.size() ||
       !escapes_ok(table.extent_heads, extent_escape, table.far_extents, table.far_begins.size())) {
        return std::unexpected("relation extent columns do not line up");
    }

    // Entries of one relation group two-way merge under the full (kind,
    // begin, end, payload) key with equal-key entries combined at write time,
    // so the key must ascend strictly within each group — out-of-order or
    // repeated entries would mis-merge silently instead of being rejected.
    RelationTable::Cursor cursor;
    for(std::size_t id = 0; id < table.sym_hashes.size(); id += 1) {
        std::tuple<std::uint8_t, std::uint32_t, std::uint32_t, std::uint64_t> prev{};
        for(auto entry = table.offsets[id]; entry < table.offsets[id + 1]; entry += 1) {
            auto k = cursor.extent;
            auto relation = table.decode(entry, cursor);
            // The default LocalSourceRange is the writer's sentinel for pair
            // relations, which carry no range of their own; every other kind
            // is written with a real range, so a sentinel there is corruption
            // that would serve an invalid source range forever.
            if(is_no_range(relation.range.begin, relation.range.end) &&
               !table.kind_of(entry).isBetweenSymbol()) {
                return std::unexpected("no-range sentinel on a relation that carries a range");
            }
            if(k != cursor.extent) {
                auto extent = relation.definition_range();
                bool escaped = table.extent_heads[k] == extent_escape;
                if(escaped
                       ? table.extent_lengths[k] != 0 || fits_extent_columns(relation.range, extent)
                       : table.extent_heads[k] > relation.range.begin) {
                    return std::unexpected("relation extent is not the canonical one");
                }
                if(extent.end < extent.begin || extent.end > content_size) {
                    return std::unexpected("definition range past the content");
                }
            }
            std::tuple key{static_cast<std::uint8_t>(relation.kind),
                           relation.range.begin,
                           relation.range.end,
                           relation.target_symbol};
            if(entry != table.offsets[id] && key <= prev) {
                return std::unexpected("relation rows of one symbol are not strictly ascending");
            }
            prev = key;
        }
    }
    return {};
}

CheckResult locals_ok(BlobView root) {
    auto table = LocalTable::of(root);
    auto count = table.size();
    auto table_size = table.sym_hashes.size();
    if(auto ok = ascending_ids_ok(table.syms, count, table_size, true); !ok) {
        return ok;
    }
    if(table.kinds.size() != count || table.scopes.size() != count || table.flags.size() != count ||
       table.token_kinds.size() != count || table.token_modifiers.size() != count ||
       !all_token_kinds(table.token_kinds)) {
        return std::unexpected("local symbol columns do not line up");
    }
    if(auto ok = ends_ok(table.name_ends, count, table.names.size()); !ok) {
        return ok;
    }
    // The few locals with arguments or a signature: ascending positions,
    // each text non-empty — an empty one is the absent entry's to spell.
    auto texts_ok =
        [&](const IdColumnView& of, llvm::StringRef text, const IdColumnView& ends) -> CheckResult {
        if(auto ok = ascending_ids_ok(of, of.size(), count, true); !ok) {
            return ok;
        }
        if(auto ok = ends_ok(ends, of.size(), text.size()); !ok) {
            return ok;
        }
        for(std::size_t i = 0; i < of.size(); i += 1) {
            if(ends.slice(text, i).empty()) {
                return std::unexpected("an empty local argument or signature stored");
            }
        }
        return {};
    };
    if(auto ok = texts_ok(table.args_of, table.args, table.args_ends); !ok) {
        return ok;
    }
    if(auto ok = texts_ok(table.signatures_of, table.signatures, table.signature_ends); !ok) {
        return ok;
    }
    if(auto ok = ids_ok(table.parents, count, table_size + 1); !ok) {
        return ok;
    }
    // A local symbol's parent becomes a DenseSet key in a query's container
    // walk, where the sentinel values corrupt or assert.
    for(std::size_t k = 0; k < count; k += 1) {
        if(table.parents[k] != 0 && reserved_key(table.sym_hashes[table.parents[k] - 1])) {
            return std::unexpected("reserved local parent hash");
        }
    }
    return {};
}

// Beyond the per-encoding column shape, every mask must own at least one
// stored variant and no bits past the variant table: an ownerless entry
// serves unconditionally while every stored variant is live (the
// LiveFilter fast path skips the mask), vanishes once any variant dies,
// and the next compaction erases it for real — every manifest still
// fresh throughout.
CheckResult masks_ok(BlobView root) {
    auto variant_count = to_array_ref(root[&ShardBlob::variants]).size();
    auto encoding = encoding_of(variant_count);
    for(auto columns: {columns_of(root[&ShardBlob::occs]), columns_of(root[&ShardBlob::rels])}) {
        auto count = columns.size();
        auto expected = [&](MaskEncoding column) -> std::size_t {
            if(column != encoding) {
                return 0;
            }
            return column == MaskEncoding::Roaring ? count + 1 : count;
        };
        if(columns.masks8.size() != expected(MaskEncoding::U8) ||
           columns.masks16.size() != expected(MaskEncoding::U16) ||
           columns.masks32.size() != expected(MaskEncoding::U32) ||
           columns.masks64.size() != expected(MaskEncoding::U64) ||
           columns.roaring_offsets.size() != expected(MaskEncoding::Roaring) ||
           (encoding != MaskEncoding::Roaring && !columns.roaring.empty())) {
            return std::unexpected("mask encoding is not the canonical one");
        }
        if(encoding == MaskEncoding::Single) {
            continue;
        }
        if(encoding != MaskEncoding::Roaring) {
            auto stray = variant_count < 64 ? ~std::uint64_t(0) << variant_count : 0;
            for(std::uint32_t entry = 0; entry < count; entry += 1) {
                auto mask = mask_bits(encoding, columns, entry);
                if(mask == 0 || (mask & stray) != 0) {
                    return std::unexpected("entry mask owns no stored variant");
                }
            }
            continue;
        }
        if(!std::ranges::is_sorted(columns.roaring_offsets) ||
           columns.roaring_offsets.back() != columns.roaring.size() ||
           (count != 0 && columns.roaring_offsets.front() != 0)) {
            return std::unexpected("mask encoding is not the canonical one");
        }
        // A slice failing decode would read as an empty mask — the
        // ownerless-entry corruption above in another coat. Prove each
        // slice decodes once here so queries stay check-free and the blob
        // rebuilds instead.
        for(std::uint32_t entry = 0; entry < count; entry += 1) {
            auto begin = columns.roaring_offsets[entry];
            auto mask = view_bitmap(columns.roaring.data() + begin,
                                    columns.roaring_offsets[entry + 1] - begin);
            if(!mask || mask->isEmpty() || mask->maximum() >= variant_count) {
                return std::unexpected("entry mask does not decode or owns no stored variant");
            }
        }
    }
    return {};
}

/// Structural verification does not constrain field values; everything the
/// readers dereference through raw column pointers or binary-search must be
/// proven in-bounds and in order here, once, so queries stay check-free.
/// The checks are also canonicality checks: every self-describing choice
/// (range encoding, id width, non-ASCII lines, mask encoding) is a strict
/// function of the data, so one logical blob has exactly one encoding and
/// its byte hash is a usable identity. Later checks decode through columns
/// earlier ones bounded, so the order matters.
CheckResult validate(BlobView root) {
    constexpr std::array checks = {
        variants_ok,
        line_table_ok,
        symbol_table_ok,
        range_encodings_ok,
        occurrences_ok,
        relations_ok,
        locals_ok,
        masks_ok,
    };
    for(auto check: checks) {
        if(auto ok = check(root); !ok) {
            return ok;
        }
    }
    return {};
}

}  // namespace

Shard::Shard(std::unique_ptr<llvm::MemoryBuffer> buffer) : buffer(std::move(buffer)) {
    blob_hash = llvm::xxh3_64bits(this->buffer->getBuffer());
}

Shard Shard::from_bytes(llvm::StringRef data) {
    return from_buffer(llvm::MemoryBuffer::getMemBuffer(data, "", false));
}

bool Shard::rebind(std::unique_ptr<llvm::MemoryBuffer> replacement) {
    if(!buffer || !replacement || replacement->getBufferSize() != buffer->getBufferSize()) {
        return false;
    }
    buffer = std::move(replacement);
    return true;
}

Shard Shard::from_buffer(std::unique_ptr<llvm::MemoryBuffer> buffer) {
    if(!buffer) {
        return {};
    }

    // Stale or corrupt bytes (an older build's cache directory) must never
    // crash the server or be misread: deep structural verification first,
    // then the format-version gate, then the cross-field checks the raw
    // column readers rely on. Anything failing loads as "not on disk" and
    // the background indexer rebuilds it.
    auto root = BlobView::from_bytes(blob_bytes(buffer->getBuffer()));
    if(!root.valid()) {
        LOG_DEBUG("Rejecting shard blob: structural verification failed");
        return {};
    }
    if(root[&ShardBlob::format_version] != index_format_version) {
        LOG_DEBUG("Rejecting shard blob: format version {}, this build reads {}",
                  root[&ShardBlob::format_version],
                  index_format_version);
        return {};
    }
    if(auto ok = validate(root); !ok) {
        LOG_DEBUG("Rejecting shard blob: {}", ok.error());
        return {};
    }
    return Shard(std::move(buffer));
}

std::uint64_t Shard::content_hash() const {
    if(!buffer) {
        return 0;
    }
    return root_of(*buffer)[&ShardBlob::content_hash];
}

std::uint32_t Shard::content_size() const {
    if(!buffer) {
        return 0;
    }
    return root_of(*buffer)[&ShardBlob::content_size];
}

bool Shard::matches_content(llvm::StringRef text) const {
    return matches_content(text.size(), llvm::xxh3_64bits(text));
}

bool Shard::matches_content(std::uint64_t size, std::uint64_t hash) const {
    return loaded() && size == content_size() && hash == content_hash();
}

std::vector<VariantHash> Shard::variants() const {
    if(!buffer) {
        return {};
    }
    auto stored = to_array_ref(root_of(*buffer)[&ShardBlob::variants]);
    if(stored.empty()) {
        return {blob_hash};
    }
    return {stored.begin(), stored.end()};
}

bool Shard::has_variant(VariantHash hash) const {
    if(!buffer) {
        return false;
    }
    auto stored = to_array_ref(root_of(*buffer)[&ShardBlob::variants]);
    if(stored.empty()) {
        return hash == blob_hash;
    }
    return llvm::is_contained(stored, hash);
}

void Shard::set_live(llvm::ArrayRef<VariantHash> live_hashes) {
    live = {};
    if(!buffer) {
        return;
    }

    auto stored = variants();
    std::size_t matched = 0;
    Live next;
    for(std::uint32_t id = 0; id < stored.size(); id += 1) {
        if(!llvm::is_contained(live_hashes, stored[id])) {
            continue;
        }
        matched += 1;
        if(id < 64) {
            next.bits |= std::uint64_t(1) << id;
        }
        next.big.add(id);
    }
    next.all = matched == stored.size();
    live = std::move(next);
}

bool Shard::has_dead_variants() const {
    return loaded() && !live.all;
}

LiveFilter Shard::live_filter(const RangeColumnsView& columns) const {
    return {
        .all = live.all,
        .encoding = encoding_of(root_of(*buffer)),
        .bits = live.bits,
        .big = &live.big,
        .columns = &columns,
    };
}

void Shard::lookup(std::uint32_t offset,
                   llvm::function_ref<bool(const Occurrence&)> callback) const {
    if(!buffer) {
        return;
    }
    auto table = OccurrenceTable::of(root_of(*buffer));
    auto is_live = live_filter(table.columns);

    // Binary search the first occurrence whose end reaches the offset, then walk
    // while occurrences contain it. Occurrence ranges are name-token spans,
    // pairwise disjoint or identical, so under (begin, end) order the end
    // column is monotonic too.
    auto first =
        table.columns.partition_point([&](LocalSourceRange range) { return range.end < offset; });

    // A cursor between two tokens belongs to the one starting at it, so
    // occurrences the offset only touches at their right edge yield after every
    // occurrence it is properly inside — `a+b` with the cursor before `b` must
    // resolve to `b`, not `+`, while a cursor right after a lone token
    // still hits that token.
    llvm::SmallVector<Occurrence, 2> edges;
    bool stopped = false;
    table.columns.for_each_range(first, [&](std::uint32_t entry, LocalSourceRange range) {
        if(!range.touches(offset)) {
            return false;
        }
        if(!is_live(entry)) {
            return true;
        }
        if(range.end == offset && range.begin != offset) {
            edges.push_back(table.at(entry, range));
            return true;
        }
        stopped = !callback(table.at(entry, range));
        return !stopped;
    });
    if(stopped) {
        return;
    }
    for(auto& edge: edges) {
        if(!callback(edge)) {
            return;
        }
    }
}

namespace {

/// Visit the relations [begin_entry, end_entry) that pass `is_live`, `cursor`
/// placed at the first; false when the callback stopped the walk.
bool visit_relation_entries(const RelationTable& table,
                            std::uint32_t begin_entry,
                            std::uint32_t end_entry,
                            RelationTable::Cursor& cursor,
                            const LiveFilter& is_live,
                            llvm::function_ref<bool(const Relation&)> callback) {
    for(auto entry = begin_entry; entry < end_entry; entry += 1) {
        auto relation = table.decode(entry, cursor);
        if(is_live(entry) && !callback(relation)) {
            return false;
        }
    }
    return true;
}

}  // namespace

void Shard::lookup(SymbolHash symbol,
                   RelationKind kind,
                   llvm::function_ref<bool(const Relation&)> callback) const {
    if(!buffer) {
        return;
    }
    auto table = RelationTable::of(root_of(*buffer));
    auto it = std::ranges::lower_bound(table.sym_hashes, symbol);
    if(it == table.sym_hashes.end() || *it != symbol) [[unlikely]] {
        return;
    }
    auto id = static_cast<std::uint32_t>(it - table.sym_hashes.begin());
    auto cursor = table.cursor_at(table.offsets[id]);
    visit_relation_entries(table,
                           table.offsets[id],
                           table.offsets[id + 1],
                           cursor,
                           live_filter(table.columns),
                           [&](const Relation& relation) {
                               if(!(RelationKind(relation.kind) & kind)) {
                                   return true;
                               }
                               return callback(relation);
                           });
}

void Shard::for_each_occurrence(llvm::function_ref<bool(const Occurrence&)> callback) const {
    if(!buffer) {
        return;
    }
    auto table = OccurrenceTable::of(root_of(*buffer));
    auto is_live = live_filter(table.columns);
    table.columns.for_each_range([&](std::uint32_t entry, LocalSourceRange range) {
        return !is_live(entry) || callback(table.at(entry, range));
    });
}

void
    Shard::for_each_relation(llvm::function_ref<bool(SymbolHash, const Relation&)> callback) const {
    if(!buffer) {
        return;
    }
    auto table = RelationTable::of(root_of(*buffer));
    auto is_live = live_filter(table.columns);
    RelationTable::Cursor cursor;
    for(std::uint32_t id = 0; id < table.sym_hashes.size(); id += 1) {
        auto hash = table.sym_hashes[id];
        bool completed = visit_relation_entries(
            table,
            table.offsets[id],
            table.offsets[id + 1],
            cursor,
            is_live,
            [&](const Relation& relation) { return callback(hash, relation); });
        if(!completed) {
            return;
        }
    }
}

std::optional<SymbolIdentity> Shard::find_symbol(SymbolHash hash) const {
    if(!buffer) {
        return std::nullopt;
    }
    auto table = LocalTable::of(root_of(*buffer));
    auto it = std::ranges::lower_bound(table.sym_hashes, hash);
    if(it == table.sym_hashes.end() || *it != hash) {
        return std::nullopt;
    }
    auto local = table.syms.find(static_cast<std::uint32_t>(it - table.sym_hashes.begin()));
    if(!local) {
        return std::nullopt;
    }
    return table.at(*local);
}

void Shard::for_each_local_symbol(
    llvm::function_ref<bool(SymbolHash, const SymbolIdentity&)> callback) const {
    if(!buffer) {
        return;
    }
    auto table = LocalTable::of(root_of(*buffer));
    for(std::size_t k = 0; k < table.size(); k += 1) {
        if(!callback(table.hash_of(k), table.at(k))) {
            return;
        }
    }
}

std::span<const std::uint32_t> Shard::line_starts() const {
    if(!buffer) {
        return {};
    }
    if(line_starts_cache.empty()) {
        auto root = root_of(*buffer);
        auto lengths = to_array_ref(root[&ShardBlob::line_lengths]);
        auto long_lengths = to_array_ref(root[&ShardBlob::long_line_lengths]);
        line_starts_cache.reserve(lengths.size());
        std::uint32_t start = 0;
        std::size_t escape_cursor = 0;
        for(auto length: lengths) {
            line_starts_cache.push_back(start);
            if(length == length_escape) {
                start += long_lengths[escape_cursor];
                escape_cursor += 1;
            } else {
                start += length;
            }
        }
    }
    return line_starts_cache;
}

std::span<const std::uint64_t> Shard::crlf_lines() const {
    if(!buffer) {
        return {};
    }
    return to_array_ref(root_of(*buffer)[&ShardBlob::crlf_lines]);
}

PositionMap Shard::positions() const {
    if(!buffer) {
        return {0, {}, {}};
    }
    auto root = root_of(*buffer);
    return {content_size(),
            line_starts(),
            crlf_lines(),
            to_array_ref(root[&ShardBlob::utf8_lines]),
            to_ref(root[&ShardBlob::utf8_text]),
            to_array_ref(root[&ShardBlob::utf8_text_ends])};
}

namespace {

/// Working entries during a write: the entry as the readers hand it out plus
/// its variant mask. Masks stay wide; the encoding is chosen at emit time from
/// the final variant count.
template <typename MaskT>
struct OccurrenceEntry {
    Occurrence occurrence;
    MaskT mask;
};

template <typename MaskT>
struct RelationEntry {
    Relation relation;
    MaskT mask;
};

/// One symbol's relation entries, sorted by (kind, begin, end, payload).
template <typename MaskT>
struct RelGroup {
    SymbolHash symbol;
    std::vector<RelationEntry<MaskT>> entries;
};

/// Everything a write accumulates before choosing column encodings.
template <typename MaskT>
struct MergedEntries {
    std::vector<OccurrenceEntry<MaskT>> occurrences;
    /// Groups sorted by symbol hash.
    std::vector<RelGroup<MaskT>> relations;
};

constexpr auto occ_key = [](const auto& entry) {
    return std::tuple(entry.occurrence.range.begin,
                      entry.occurrence.range.end,
                      entry.occurrence.target);
};

/// The kind as the column stores it: the serialized order the readers rely on.
constexpr auto rel_key = [](const auto& entry) {
    return std::tuple(static_cast<std::uint8_t>(entry.relation.kind),
                      entry.relation.range.begin,
                      entry.relation.range.end,
                      entry.relation.target_symbol);
};

constexpr auto group_key = [](const auto& group) {
    return group.symbol;
};

template <typename MaskT>
MaskT single_bit(std::uint32_t id) {
    if constexpr(std::same_as<MaskT, std::uint64_t>) {
        return std::uint64_t(1) << id;
    } else {
        MaskT mask;
        mask.add(id);
        return mask;
    }
}

template <typename MaskT>
bool mask_empty(const MaskT& mask) {
    if constexpr(std::same_as<MaskT, std::uint64_t>) {
        return mask == 0;
    } else {
        return mask.isEmpty();
    }
}

/// The old blob's mask of one entry, remapped through old-id -> new-id (a
/// dropped variant's bit vanishes; an all-dropped entry reads as empty and
/// is skipped by the caller).
template <typename MaskT>
MaskT remap_mask(MaskEncoding encoding,
                 const RangeColumnsView& columns,
                 std::uint32_t entry,
                 llvm::ArrayRef<std::int64_t> id_map) {
    MaskT result{};
    auto apply = [&](std::uint32_t old_id) {
        if(id_map[old_id] >= 0) {
            result |= single_bit<MaskT>(static_cast<std::uint32_t>(id_map[old_id]));
        }
    };
    auto apply_bits = [&](std::uint64_t bits) {
        while(bits != 0) {
            apply(static_cast<std::uint32_t>(std::countr_zero(bits)));
            bits &= bits - 1;
        }
    };
    if(encoding == MaskEncoding::Roaring) {
        for(auto id: columns.bitmap_of(entry)) {
            apply(id);
        }
    } else {
        apply_bits(mask_bits(encoding, columns, entry));
    }
    return result;
}

/// Two-way merge of runs sorted under `key`; entries with equal keys are
/// one entry and OR their masks — the cross-variant dedup.
template <typename Entry, typename Key>
void merge_sorted(std::vector<Entry> old_entries,
                  std::vector<Entry> fresh_entries,
                  Key key,
                  std::vector<Entry>& out) {
    out.reserve(old_entries.size() + fresh_entries.size());
    auto lhs = old_entries.begin();
    auto rhs = fresh_entries.begin();
    while(lhs != old_entries.end() || rhs != fresh_entries.end()) {
        if(rhs == fresh_entries.end() || (lhs != old_entries.end() && key(*lhs) < key(*rhs))) {
            out.push_back(std::move(*lhs));
            lhs += 1;
        } else if(lhs == old_entries.end() || key(*rhs) < key(*lhs)) {
            out.push_back(std::move(*rhs));
            rhs += 1;
        } else {
            lhs->mask |= rhs->mask;
            out.push_back(std::move(*lhs));
            lhs += 1;
            rhs += 1;
        }
    }
}

/// Combine adjacent equal-key entries of a sorted run into one entry with
/// OR-ed masks.
template <typename Entry, typename Key>
void combine_equal(std::vector<Entry>& entries, Key key) {
    std::size_t out = 0;
    for(std::size_t i = 0; i < entries.size(); i += 1) {
        if(out != 0 && key(entries[out - 1]) == key(entries[i])) {
            entries[out - 1].mask |= entries[i].mask;
        } else {
            if(out != i) {
                entries[out] = std::move(entries[i]);
            }
            out += 1;
        }
    }
    entries.resize(out);
}

/// Sort a run and combine its equal-key entries.
template <typename Entry, typename Key>
void canonicalize(std::vector<Entry>& entries, Key key) {
    std::ranges::sort(entries, {}, key);
    combine_equal(entries, key);
}

/// The symbol table a set of merged entries requires: occurrence targets,
/// relation group keys, and symbol payloads.
template <typename MaskT>
llvm::DenseSet<SymbolHash> referenced_symbols(const MergedEntries<MaskT>& merged) {
    llvm::DenseSet<SymbolHash> referenced;
    for(auto& entry: merged.occurrences) {
        referenced.insert(entry.occurrence.target);
    }
    for(auto& group: merged.relations) {
        referenced.insert(group.symbol);
        for(auto& entry: group.entries) {
            auto& relation = entry.relation;
            if(relation.target_symbol != 0 && !RelationKind(relation.kind).carries_extent()) {
                referenced.insert(relation.target_symbol);
            }
        }
    }
    return referenced;
}

/// Decode a blob's occurrences into working form; `mask_of` returns the
/// occurrence's mask in the output id space (empty drops the occurrence).
template <typename MaskT, typename MaskOf>
std::vector<OccurrenceEntry<MaskT>> decode_occurrences(const OccurrenceTable& table,
                                                       MaskOf mask_of) {
    std::vector<OccurrenceEntry<MaskT>> entries;
    entries.reserve(table.size());
    table.columns.for_each_range([&](std::uint32_t entry, LocalSourceRange range) {
        auto mask = mask_of(entry);
        if(!mask_empty(mask)) {
            entries.push_back({table.at(entry, range), std::move(mask)});
        }
        return true;
    });
    return entries;
}

/// Decode one symbol's relation slice of a blob into working form, `cursor`
/// placed at its first relation.
template <typename MaskT, typename MaskOf>
std::vector<RelationEntry<MaskT>> decode_relation_group(const RelationTable& table,
                                                        std::uint32_t begin_entry,
                                                        std::uint32_t end_entry,
                                                        RelationTable::Cursor& cursor,
                                                        MaskOf mask_of) {
    std::vector<RelationEntry<MaskT>> entries;
    entries.reserve(end_entry - begin_entry);
    for(auto entry = begin_entry; entry < end_entry; entry += 1) {
        auto relation = table.decode(entry, cursor);
        auto mask = mask_of(entry);
        if(mask_empty(mask)) {
            continue;
        }
        entries.push_back({relation, std::move(mask)});
    }
    return entries;
}

/// One blob's relation groups in symbol-hash order, decoded lazily by the
/// group merge.
struct GroupIndex {
    SymbolHash hash;
    std::uint32_t begin_entry;
    std::uint32_t end_entry;
};

std::vector<GroupIndex> relation_groups(const RelationTable& table) {
    std::vector<GroupIndex> groups;
    for(std::uint32_t id = 0; id < table.sym_hashes.size(); id += 1) {
        if(table.offsets[id] != table.offsets[id + 1]) {
            groups.push_back({table.sym_hashes[id], table.offsets[id], table.offsets[id + 1]});
        }
    }
    return groups;
}

/// The content identity and line table one blob carries, copied verbatim
/// between blobs of the same generation.
struct ContentInfo {
    std::uint64_t hash = 0;
    std::uint32_t size = 0;
    std::vector<std::uint8_t> line_lengths;
    std::vector<std::uint32_t> long_lines;
    std::vector<std::uint32_t> long_line_lengths;
    std::vector<std::uint64_t> crlf_lines;
    std::vector<std::uint32_t> utf8_lines;
    std::string utf8_text;
    std::vector<std::uint32_t> utf8_text_ends;
};

ContentInfo content_info_of(BlobView root) {
    auto copy = [](auto column) {
        auto values = to_array_ref(column);
        return std::vector(values.begin(), values.end());
    };
    return {
        .hash = root[&ShardBlob::content_hash],
        .size = root[&ShardBlob::content_size],
        .line_lengths = copy(root[&ShardBlob::line_lengths]),
        .long_lines = copy(root[&ShardBlob::long_lines]),
        .long_line_lengths = copy(root[&ShardBlob::long_line_lengths]),
        .crlf_lines = copy(root[&ShardBlob::crlf_lines]),
        .utf8_lines = copy(root[&ShardBlob::utf8_lines]),
        .utf8_text = to_ref(root[&ShardBlob::utf8_text]).str(),
        .utf8_text_ends = copy(root[&ShardBlob::utf8_text_ends]),
    };
}

ContentInfo content_info_of(llvm::StringRef content) {
    ContentInfo info;
    info.hash = llvm::xxh3_64bits(content);
    info.size = static_cast<std::uint32_t>(content.size());

    auto starts = kota::ipc::lsp::line_starts({content.data(), content.size()});
    info.line_lengths.reserve(starts.size());
    for(std::size_t i = 0; i < starts.size(); i += 1) {
        auto last = i + 1 == starts.size();
        auto next = last ? info.size : starts[i + 1];
        auto length = next - starts[i];
        auto crlf = !last && length >= 2 && content[next - 2] == '\r';
        if(crlf) {
            info.crlf_lines.resize(i / 64 + 1);
            info.crlf_lines[i / 64] |= std::uint64_t(1) << (i % 64);
        }
        auto text = content.slice(starts[i], next - (last ? 0 : crlf ? 2 : 1));
        if(!kota::ipc::lsp::is_ascii({text.data(), text.size()})) {
            info.utf8_lines.push_back(static_cast<std::uint32_t>(i));
            info.utf8_text += text;
            info.utf8_text_ends.push_back(static_cast<std::uint32_t>(info.utf8_text.size()));
        }
        if(length >= length_escape) {
            info.line_lengths.push_back(length_escape);
            info.long_lines.push_back(static_cast<std::uint32_t>(i));
            info.long_line_lengths.push_back(length);
        } else {
            info.line_lengths.push_back(static_cast<std::uint8_t>(length));
        }
    }
    return info;
}

/// A local symbol's identity, owned for the duration of a write.
struct LocalSymbol {
    std::string name;
    std::string args;
    std::string signature;
    SymbolHash parent;
    SymbolKind kind;
    SymbolScope scope;
    SymbolFlags flags;
    TokenClass token_class;
};

LocalSymbol own(const SymbolIdentity& identity) {
    return {
        .name = identity.name.str(),
        .args = identity.args.str(),
        .signature = identity.signature.str(),
        .parent = identity.parent,
        .kind = identity.kind,
        .scope = identity.scope,
        .flags = identity.flags,
        .token_class = identity.token_class,
    };
}

/// Collect one blob's local symbols; symbols the merged entries no longer
/// reference are filtered at emit time.
void collect_locals(BlobView root, llvm::DenseMap<SymbolHash, LocalSymbol>& locals) {
    auto table = LocalTable::of(root);
    for(std::size_t k = 0; k < table.size(); k += 1) {
        auto identity = table.at(k);
        auto [it, inserted] = locals.try_emplace(table.hash_of(k), own(identity));
        // Variants may see different facts of one symbol (a definition
        // behind `#ifdef`); like the project table, the union is kept, and
        // the token kind a defining variant gives.
        if(!inserted) {
            if(has_flag(identity.flags, SymbolFlags::HasDefinition)) {
                it->second.token_class.kind = identity.token_class.kind;
            }
            it->second.flags |= identity.flags;
            it->second.token_class.modifiers |= identity.token_class.modifiers;
        }
    }
}

/// Appends one side's entries in the encodings chosen for the whole blob:
/// the range encoding by the side's order and the content size, the mask
/// encoding by variant count.
struct SideWriter {
    RangeWriter ranges;
    MaskEncoding encoding;

    template <typename MaskT>
    void add(LocalSourceRange range, const MaskT& mask) {
        ranges.add(range);
        add_mask(mask);
    }

    std::uint32_t count() const {
        return ranges.count;
    }

    /// The roaring encoding's offset column ends with the size of the slices.
    void finish() {
        if(encoding == MaskEncoding::Roaring) {
            auto& side = ranges.side;
            side.roaring_offsets.push_back(static_cast<std::uint32_t>(side.roaring.size()));
        }
    }

    template <typename MaskT>
    void add_mask(const MaskT& mask) {
        auto& side = ranges.side;
        switch(encoding) {
            case MaskEncoding::Single: {
                break;
            }
            case MaskEncoding::U8: {
                if constexpr(std::same_as<MaskT, std::uint64_t>) {
                    side.masks8.push_back(static_cast<std::uint8_t>(mask));
                }
                break;
            }
            case MaskEncoding::U16: {
                if constexpr(std::same_as<MaskT, std::uint64_t>) {
                    side.masks16.push_back(static_cast<std::uint16_t>(mask));
                }
                break;
            }
            case MaskEncoding::U32: {
                if constexpr(std::same_as<MaskT, std::uint64_t>) {
                    side.masks32.push_back(static_cast<std::uint32_t>(mask));
                }
                break;
            }
            case MaskEncoding::U64: {
                if constexpr(std::same_as<MaskT, std::uint64_t>) {
                    side.masks64.push_back(mask);
                }
                break;
            }
            case MaskEncoding::Roaring: {
                if constexpr(std::same_as<MaskT, Bitmap>) {
                    auto size = mask.getSizeInBytes(true);
                    auto offset = side.roaring.size();
                    side.roaring.resize(offset + size);
                    mask.write(reinterpret_cast<char*>(side.roaring.data() + offset), true);
                    side.roaring_offsets.push_back(static_cast<std::uint32_t>(offset));
                }
                break;
            }
        }
    }
};

/// Encode merged entries, locals and content into canonical blob bytes. The
/// only entry point that writes a ShardBlob: every self-describing choice
/// (range encoding, id width, mask encoding) is made here, from
/// the data, so equal inputs produce equal bytes.
template <typename MaskT>
void emit_blob(const MergedEntries<MaskT>& merged,
               const llvm::DenseMap<SymbolHash, LocalSymbol>& locals,
               std::vector<VariantHash> variants,
               const ContentInfo& content,
               llvm::raw_ostream& os) {
    // The locals the entries name; their parents join the symbol table,
    // which their parent column indexes.
    auto symbols = referenced_symbols(merged);
    llvm::SmallVector<std::pair<SymbolHash, const LocalSymbol*>> kept;
    for(auto& [hash, info]: locals) {
        if(symbols.contains(hash)) {
            kept.emplace_back(hash, &info);
        }
    }
    std::ranges::sort(kept, {}, [](const auto& entry) { return entry.first; });
    for(auto& entry: kept) {
        if(entry.second->parent != 0) {
            symbols.insert(entry.second->parent);
        }
    }

    ShardBlob blob;
    blob.format_version = index_format_version;
    blob.content_hash = content.hash;
    blob.content_size = content.size;
    blob.utf8_lines = content.utf8_lines;
    blob.utf8_text = content.utf8_text;
    blob.utf8_text_ends = content.utf8_text_ends;
    blob.line_lengths = content.line_lengths;
    blob.long_lines = content.long_lines;
    blob.long_line_lengths = content.long_line_lengths;
    blob.crlf_lines = content.crlf_lines;
    blob.variants = std::move(variants);

    blob.sym_hashes.assign(symbols.begin(), symbols.end());
    std::ranges::sort(blob.sym_hashes);
    auto sym_id = [&](SymbolHash hash) {
        return static_cast<std::uint32_t>(std::ranges::lower_bound(blob.sym_hashes, hash) -
                                          blob.sym_hashes.begin());
    };

    std::vector<std::uint32_t> local_ids;
    std::vector<std::uint32_t> name_ends;
    std::vector<std::uint32_t> args_of;
    std::vector<std::uint32_t> args_ends;
    std::vector<std::uint32_t> signatures_of;
    std::vector<std::uint32_t> signature_ends;
    std::vector<std::uint32_t> parents;
    for(std::uint32_t k = 0; k < kept.size(); k += 1) {
        auto& [hash, info] = kept[k];
        local_ids.push_back(sym_id(hash));
        blob.local_names += info->name;
        name_ends.push_back(static_cast<std::uint32_t>(blob.local_names.size()));
        if(!info->args.empty()) {
            args_of.push_back(k);
            blob.local_args += info->args;
            args_ends.push_back(static_cast<std::uint32_t>(blob.local_args.size()));
        }
        if(!info->signature.empty()) {
            signatures_of.push_back(k);
            blob.local_signatures += info->signature;
            signature_ends.push_back(static_cast<std::uint32_t>(blob.local_signatures.size()));
        }
        parents.push_back(info->parent == 0 ? 0 : sym_id(info->parent) + 1);
        blob.local_kinds.push_back(info->kind.value());
        blob.local_scopes.push_back(static_cast<std::uint8_t>(info->scope));
        blob.local_flags.push_back(static_cast<std::uint16_t>(info->flags));
        blob.local_token_kinds.push_back(static_cast<std::uint8_t>(info->token_class.kind));
        blob.local_token_modifiers.push_back(info->token_class.modifiers);
    }
    write_ids(blob.local_syms, local_ids, blob.sym_hashes.size());
    write_ids(blob.local_name_ends, name_ends, blob.local_names.size() + 1);
    write_ids(blob.local_args_of, args_of, kept.size());
    write_ids(blob.local_args_ends, args_ends, blob.local_args.size() + 1);
    write_ids(blob.local_signatures_of, signatures_of, kept.size());
    write_ids(blob.local_signature_ends, signature_ends, blob.local_signatures.size() + 1);
    write_ids(blob.local_parents, parents, blob.sym_hashes.size() + 1);

    auto encoding = encoding_of(blob.variants.size());

    SideWriter occs{
        .ranges = {.side = blob.occs, .encoding = RangeEncoding::Delta},
        .encoding = encoding,
    };
    auto occ_syms = id_writer(blob.occ_syms, blob.sym_hashes.size());
    for(auto& entry: merged.occurrences) {
        occs.add(entry.occurrence.range, entry.mask);
        occ_syms.add(sym_id(entry.occurrence.target));
    }
    occs.finish();

    // Relation groups follow symbol-table order; a symbol with occurrences
    // only gets an empty slice.
    SideWriter rels{
        .ranges = {.side = blob.rels, .encoding = unsorted_encoding(content.size)},
        .encoding = encoding,
    };
    std::vector<std::uint32_t> rel_offsets;
    rel_offsets.reserve(blob.sym_hashes.size() + 1);
    std::vector<std::uint32_t> targets;
    std::vector<std::uint32_t> target_ranks;
    std::vector<std::uint32_t> extent_ranks;
    auto group = merged.relations.begin();
    for(auto hash: blob.sym_hashes) {
        rel_offsets.push_back(rels.count());
        if(group == merged.relations.end() || group->symbol != hash) {
            continue;
        }
        for(auto& entry: group->entries) {
            auto& relation = entry.relation;
            if(rels.count() % rank_block == 0) {
                target_ranks.push_back(static_cast<std::uint32_t>(targets.size()));
                extent_ranks.push_back(static_cast<std::uint32_t>(blob.rel_extent_heads.size()));
            }
            auto kind = static_cast<std::uint8_t>(relation.kind);
            if(relation.target_symbol != 0) {
                kind |= payload_bit;
                if(RelationKind(relation.kind).carries_extent()) {
                    auto extent = relation.definition_range();
                    if(fits_extent_columns(relation.range, extent)) {
                        blob.rel_extent_heads.push_back(
                            static_cast<std::uint16_t>(relation.range.begin - extent.begin));
                        blob.rel_extent_lengths.push_back(
                            static_cast<std::uint16_t>(extent.end - extent.begin));
                    } else {
                        blob.rel_far_extents.push_back(
                            static_cast<std::uint32_t>(blob.rel_extent_heads.size()));
                        blob.rel_far_begins.push_back(extent.begin);
                        blob.rel_far_ends.push_back(extent.end);
                        blob.rel_extent_heads.push_back(extent_escape);
                        blob.rel_extent_lengths.push_back(0);
                    }
                } else {
                    targets.push_back(sym_id(relation.target_symbol));
                }
            }
            blob.rel_kinds.push_back(kind);
            rels.add(relation.range, entry.mask);
        }
        group += 1;
    }
    rel_offsets.push_back(rels.count());
    rels.finish();
    write_ids(blob.rel_offsets, rel_offsets, rels.count() + 1);
    write_ids(blob.rel_targets, targets, blob.sym_hashes.size());
    write_ids(blob.rel_target_ranks, target_ranks, targets.size() + 1);
    write_ids(blob.rel_extent_ranks, extent_ranks, blob.rel_extent_heads.size() + 1);

    serialize_blob(blob, os);
}

template <typename MaskT>
void merge_shards_impl(BlobView old_root,
                       llvm::ArrayRef<std::int64_t> id_map,
                       llvm::ArrayRef<Shard> fresh,
                       std::uint32_t fresh_base,
                       std::vector<VariantHash> variants,
                       const ContentInfo& content,
                       llvm::raw_ostream& os) {
    MergedEntries<MaskT> merged;

    // Occurrences: concatenate every fresh blob's entries (each marked with
    // its new bit), sort, combine equal entries, then merge with the old
    // entries. Fresh runs are individually sorted already; one sort over the
    // concatenation keeps the merge two-way.
    std::vector<OccurrenceEntry<MaskT>> fresh_occs;
    for(std::uint32_t i = 0; i < fresh.size(); i += 1) {
        auto bit = single_bit<MaskT>(fresh_base + i);
        auto entries = decode_occurrences<MaskT>(OccurrenceTable::of(view_of(fresh[i].bytes())),
                                                 [&](std::uint32_t) { return bit; });
        fresh_occs.insert(fresh_occs.end(),
                          std::make_move_iterator(entries.begin()),
                          std::make_move_iterator(entries.end()));
    }
    canonicalize(fresh_occs, occ_key);

    std::vector<OccurrenceEntry<MaskT>> old_occs;
    if(old_root.valid()) {
        auto table = OccurrenceTable::of(old_root);
        auto encoding = encoding_of(old_root);
        old_occs = decode_occurrences<MaskT>(table, [&](std::uint32_t entry) {
            return remap_mask<MaskT>(encoding, table.columns, entry, id_map);
        });
    }
    merge_sorted(std::move(old_occs), std::move(fresh_occs), occ_key, merged.occurrences);

    // Relations: gather fresh groups per symbol across all fresh blobs,
    // then two-way merge with the old blob's groups in hash order.
    llvm::DenseMap<SymbolHash, std::vector<RelationEntry<MaskT>>> fresh_group_map;
    for(std::uint32_t i = 0; i < fresh.size(); i += 1) {
        auto table = RelationTable::of(view_of(fresh[i].bytes()));
        auto bit = single_bit<MaskT>(fresh_base + i);
        RelationTable::Cursor cursor;
        for(auto& group: relation_groups(table)) {
            auto entries = decode_relation_group<MaskT>(table,
                                                        group.begin_entry,
                                                        group.end_entry,
                                                        cursor,
                                                        [&](std::uint32_t) { return bit; });
            auto& into = fresh_group_map[group.hash];
            into.insert(into.end(),
                        std::make_move_iterator(entries.begin()),
                        std::make_move_iterator(entries.end()));
        }
    }
    std::vector<RelGroup<MaskT>> fresh_groups;
    fresh_groups.reserve(fresh_group_map.size());
    for(auto& [hash, entries]: fresh_group_map) {
        canonicalize(entries, rel_key);
        fresh_groups.push_back({hash, std::move(entries)});
    }
    std::ranges::sort(fresh_groups, {}, group_key);

    std::vector<GroupIndex> old_groups;
    RelationTable old_table;
    MaskEncoding old_encoding = MaskEncoding::Single;
    if(old_root.valid()) {
        old_table = RelationTable::of(old_root);
        old_encoding = encoding_of(old_root);
        old_groups = relation_groups(old_table);
    }
    // The merge below decodes every old group once, in order.
    RelationTable::Cursor old_cursor;
    auto decode_old = [&](const GroupIndex& group) {
        return decode_relation_group<MaskT>(
            old_table,
            group.begin_entry,
            group.end_entry,
            old_cursor,
            [&](std::uint32_t entry) {
                return remap_mask<MaskT>(old_encoding, old_table.columns, entry, id_map);
            });
    };

    auto lhs = old_groups.begin();
    auto rhs = fresh_groups.begin();
    while(lhs != old_groups.end() || rhs != fresh_groups.end()) {
        if(rhs == fresh_groups.end() || (lhs != old_groups.end() && lhs->hash < rhs->symbol)) {
            auto entries = decode_old(*lhs);
            if(!entries.empty()) {
                merged.relations.push_back({lhs->hash, std::move(entries)});
            }
            lhs += 1;
        } else if(lhs == old_groups.end() || rhs->symbol < lhs->hash) {
            merged.relations.push_back(std::move(*rhs));
            rhs += 1;
        } else {
            std::vector<RelationEntry<MaskT>> combined;
            merge_sorted(decode_old(*lhs), std::move(rhs->entries), rel_key, combined);
            if(!combined.empty()) {
                merged.relations.push_back({lhs->hash, std::move(combined)});
            }
            lhs += 1;
            rhs += 1;
        }
    }

    // Local names union: every input blob is self-contained, so the merge
    // needs no external symbol resolver. First writer wins — identities of
    // one symbol agree across blobs of one file.
    llvm::DenseMap<SymbolHash, LocalSymbol> locals;
    if(old_root.valid()) {
        collect_locals(old_root, locals);
    }
    for(auto& shard: fresh) {
        collect_locals(view_of(shard.bytes()), locals);
    }

    emit_blob(merged, locals, std::move(variants), content, os);
}

}  // namespace

void write_shard(const FileIndex& file_index,
                 llvm::function_ref<std::optional<SymbolIdentity>(SymbolHash)> symbols,
                 llvm::StringRef content,
                 llvm::raw_ostream& os) {
    // Canonicalize straight from the in-memory entries: sorted, deduplicated.
    // MaskT is irrelevant for a single variant (no mask columns) — use the
    // cheap one.
    MergedEntries<std::uint64_t> merged;
    merged.occurrences.reserve(file_index.occurrences.size());
    for(auto& occurrence: file_index.occurrences) {
        merged.occurrences.push_back({occurrence, 1});
    }
    canonicalize(merged.occurrences, occ_key);

    merged.relations.reserve(file_index.relations.size());
    for(auto& [hash, relations]: file_index.relations) {
        std::vector<RelationEntry<std::uint64_t>> group;
        group.reserve(relations.size());
        for(auto& relation: relations) {
            group.push_back({relation, 1});
        }
        canonicalize(group, rel_key);
        merged.relations.push_back({hash, std::move(group)});
    }
    std::ranges::sort(merged.relations, {}, group_key);

    llvm::DenseMap<SymbolHash, LocalSymbol> locals;
    if(symbols) {
        for(auto hash: referenced_symbols(merged)) {
            auto found = symbols(hash);
            if(found && found->scope != SymbolScope::External) {
                locals.try_emplace(hash, own(*found));
            }
        }
    }

    emit_blob(merged, locals, {}, content_info_of(content), os);
}

void merge_shards(const Shard& old,
                  llvm::ArrayRef<VariantHash> keep,
                  llvm::ArrayRef<Shard> fresh,
                  llvm::raw_ostream& os) {
    auto old_variants = old.variants();

    // old-id -> new-id; -1 drops the variant.
    llvm::SmallVector<std::int64_t> id_map(old_variants.size(), -1);
    std::vector<VariantHash> variants;
    for(std::uint32_t id = 0; id < old_variants.size(); id += 1) {
        if(llvm::is_contained(keep, old_variants[id])) {
            id_map[id] = static_cast<std::int64_t>(variants.size());
            variants.push_back(old_variants[id]);
        }
    }

    BlobView old_root;
    if(!variants.empty()) {
        old_root = view_of(old.bytes());
    }

    auto fresh_base = static_cast<std::uint32_t>(variants.size());
    for(auto& shard: fresh) {
        assert(shard.loaded() && "fresh shards must hold a blob");
        auto identity = shard.variants();
        assert(identity.size() == 1 && "fresh shards are single-variant worker blobs");
        assert(!llvm::is_contained(variants, identity.front()) &&
               "a variant already stored must not be re-appended");
        variants.push_back(identity.front());
    }
    assert(!variants.empty() && "a shard blob holds at least one variant");

    // Content and line table travel verbatim from any input — all inputs
    // share one content generation. Offsets from different generations
    // must never share entry storage, hence the assert.
    ContentInfo content = old_root.valid() ? content_info_of(old_root)
                                           : content_info_of(view_of(fresh.front().bytes()));
    for([[maybe_unused]] auto& shard: fresh) {
        assert(shard.content_hash() == content.hash &&
               "merge inputs must share one content generation");
    }

    if(variants.size() <= 64) {
        merge_shards_impl<std::uint64_t>(old_root,
                                         id_map,
                                         fresh,
                                         fresh_base,
                                         std::move(variants),
                                         content,
                                         os);
    } else {
        merge_shards_impl<Bitmap>(old_root,
                                  id_map,
                                  fresh,
                                  fresh_base,
                                  std::move(variants),
                                  content,
                                  os);
    }
}

}  // namespace clice::index
