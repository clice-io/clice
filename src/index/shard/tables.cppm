/// The typed views over a shard blob's columns and its mask encoding, which reading and writing a
/// shard share.

module clice:index.shard.tables;

import :index.columns;
import :index.shard.layout;
import :index.symbol;
import :semantic.symbol;
import :semantic.token_class;
import :support.prelude;
import :support.text;
import :syntax.token;

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

using Root = codec::blob::view_t<ShardBlob>;

MaskEncoding encoding_of(std::uint32_t variant_count) {
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

const Root& view_of(llvm::StringRef bytes) {
    return codec::blob::view_verified<ShardBlob>(as_bytes(bytes));
}

MaskEncoding encoding_of(const Root& root) {
    return encoding_of(root.variants.size());
}

/// The mask of `entry` in an encoding of up to 64 variants.
std::uint64_t mask_bits(MaskEncoding encoding, const MaskColumnsView& masks, std::uint32_t entry) {
    switch(encoding) {
        case MaskEncoding::Single: return 1;
        case MaskEncoding::U8: return masks.masks8[entry];
        case MaskEncoding::U16: return masks.masks16[entry];
        case MaskEncoding::U32: return masks.masks32[entry];
        case MaskEncoding::U64: return masks.masks64[entry];
        case MaskEncoding::Roaring: break;
    }
    std::unreachable();
}

/// The occurrence table: each occurrence's range and the symbol it names.
struct OccurrenceTable {
    const Root& root;
    const RangeColumnsView& columns;
    const MaskColumnsView& masks;
    IdColumnView symbols;

    static OccurrenceTable of(const Root& root) {
        return {
            .root = root,
            .columns = root.occurrences,
            .masks = root.occurrence_masks,
            .symbols = ids_of(root.occurrence_symbols, root.symbol_hashes.size()),
        };
    }

    std::uint32_t size() const {
        return entry_count(columns);
    }

    Occurrence at(std::uint32_t entry, LocalSourceRange range) const {
        return {range, root.symbol_hashes[symbols[entry]]};
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
/// column listing the payloads in relation order; a range is a code naming
/// the occurrence that spans it or an explicit range. All of them are walked
/// by a cursor in stored order, placed by the counts before each block.
struct RelationTable {
    const Root& root;
    const RangeColumnsView& occurrences;
    llvm::ArrayRef<std::uint8_t> codes;
    const RangeColumnsView& explicit_ranges;
    const MaskColumnsView& masks;
    IdColumnView target_ranks;
    IdColumnView extent_ranks;
    IdColumnView range_ranks;
    IdColumnView explicit_ranks;
    IdColumnView targets;
    /// Entry i's relations occupy [offsets[i], offsets[i + 1]).
    IdColumnView offsets;

    static RelationTable of(const Root& root) {
        auto targets = ids_of(root.relation_targets, root.symbol_hashes.size());
        auto& explicit_ranges = root.relation_explicit_ranges;
        return {
            .root = root,
            .occurrences = root.occurrences,
            .codes = {root.relation_range_codes.data(), root.relation_range_codes.size()},
            .explicit_ranges = explicit_ranges,
            .masks = root.relation_masks,
            .target_ranks = ids_of(root.relation_target_ranks, targets.size() + 1),
            .extent_ranks =
                ids_of(root.relation_extent_ranks, root.relation_extent_heads.size() + 1),
            .range_ranks = ids_of(root.relation_code_ranks, root.relation_range_codes.size() + 1),
            .explicit_ranks =
                ids_of(root.relation_explicit_ranks, entry_count(explicit_ranges) + 1),
            .targets = targets,
            .offsets = ids_of(root.relation_offsets, root.relation_kinds.size() + 1),
        };
    }

    std::uint32_t size() const {
        return root.relation_kinds.size();
    }

    RelationKind kind_of(std::uint32_t entry) const {
        return static_cast<RelationKind::Kind>(root.relation_kinds[entry] & ~payload_bit);
    }

    bool has_payload(std::uint32_t entry) const {
        return (root.relation_kinds[entry] & payload_bit) != 0;
    }

    /// Whether the stored relations of symbol `id`, of any variant, hold a
    /// definition: a group starts with its declarations, then its
    /// definitions.
    bool defines(std::uint32_t id) const {
        for(auto entry = offsets[id]; entry < offsets[id + 1]; entry += 1) {
            auto kind = kind_of(entry);
            if(!kind.is_one_of(RelationKind::Declaration)) {
                return kind.is_one_of(RelationKind::Definition);
            }
        }
        return false;
    }

    /// Positions in the payload columns, the range codes and the explicit
    /// ranges, and the occurrence the block's last occurrence code named;
    /// advance() moves them, so relations must be decoded one after another
    /// from where the cursor was placed.
    struct Cursor {
        std::uint32_t target = 0;
        std::uint32_t extent = 0;
        std::uint32_t code = 0;
        std::uint32_t explicit_range = 0;
        std::uint32_t occurrence = 0;
    };

    /// Where a relation's range is: `code` range_none, range_explicit with
    /// the explicit range at `index`, or 0 with the occurrence at `index`.
    struct RangeAt {
        std::uint64_t code = 0;
        std::uint32_t index = 0;
    };

    /// Move the cursor past `entry`, returning where its range is; nullopt
    /// for bytes that hold no range code.
    std::optional<RangeAt> advance(Cursor& cursor, std::uint32_t entry) const {
        if(has_payload(entry)) {
            (kind_of(entry).carries_extent() ? cursor.extent : cursor.target) += 1;
        }
        if(entry % rank_block == 0) {
            cursor.occurrence = 0;
        }
        // Most codes are one byte, which needs no varint decode.
        std::optional<std::uint64_t> code;
        if(cursor.code < codes.size() && codes[cursor.code] < 0x80) {
            code = codes[cursor.code];
            cursor.code += 1;
        } else {
            code = read_varint(codes, cursor.code);
        }
        if(!code) {
            return std::nullopt;
        }
        if(*code == range_none) {
            return RangeAt{.code = range_none};
        }
        if(*code == range_explicit) {
            cursor.explicit_range += 1;
            return RangeAt{.code = range_explicit, .index = cursor.explicit_range - 1};
        }
        if(*code % 2 != 0) {
            return std::nullopt;
        }
        auto zigzag = *code >> 1;
        auto distance =
            static_cast<std::int64_t>(zigzag >> 1) ^ -static_cast<std::int64_t>(zigzag & 1);
        auto occurrence = cursor.occurrence + distance;
        if(occurrence < 0 || occurrence > std::numeric_limits<std::uint32_t>::max()) {
            return std::nullopt;
        }
        cursor.occurrence = static_cast<std::uint32_t>(occurrence);
        return RangeAt{.index = cursor.occurrence};
    }

    LocalSourceRange range_at(RangeAt at) const {
        switch(at.code) {
            case range_none: return {};
            case range_explicit: return range_of(explicit_ranges, at.index);
            default: return range_of(occurrences, at.index);
        }
    }

    Cursor cursor_at(std::uint32_t entry) const {
        if(entry == size()) {
            return {
                .target = targets.size(),
                .extent = static_cast<std::uint32_t>(root.relation_extent_heads.size()),
                .code = static_cast<std::uint32_t>(codes.size()),
                .explicit_range = entry_count(explicit_ranges),
            };
        }
        auto block = entry / rank_block;
        Cursor cursor{
            .target = target_ranks[block],
            .extent = extent_ranks[block],
            .code = range_ranks[block],
            .explicit_range = explicit_ranks[block],
        };
        for(auto i = block * rank_block; i < entry; i += 1) {
            advance(cursor, i);
        }
        return cursor;
    }

    /// Extent `k`, of a relation whose range begins at `name`.
    LocalSourceRange extent_at(std::uint32_t k, std::uint32_t name) const {
        auto head = root.relation_extent_heads[k];
        if(head == extent_escape) {
            auto far = escape_position(root.relation_far_extents, k);
            return {root.relation_far_begins[far], root.relation_far_ends[far]};
        }
        auto begin = name - head;
        return {begin, begin + root.relation_extent_lengths[k]};
    }

    Relation decode(std::uint32_t entry, Cursor& cursor) const {
        auto payload = cursor;
        return with_payload(entry, payload, range_at(*advance(cursor, entry)));
    }

    /// The relation at `entry` of range `range`, its payload at `payload`.
    Relation with_payload(std::uint32_t entry,
                          const Cursor& payload,
                          LocalSourceRange range) const {
        Relation relation{.kind = kind_of(entry), .range = range, .target = 0};
        if(has_payload(entry)) {
            if(RelationKind(relation.kind).carries_extent()) {
                relation.set_extent(extent_at(payload.extent, relation.range.begin));
            } else {
                relation.target = root.symbol_hashes[targets[payload.target]];
            }
        }
        return relation;
    }
};

/// The local-name table: the symbols whose identities live in no other
/// table, sparse over the symbol table in ascending id order.
struct LocalTable {
    const Root& root;
    IdColumnView symbols;
    IdColumnView name_ends;
    IdColumnView args_of;
    IdColumnView args_ends;
    IdColumnView signatures_of;
    IdColumnView signature_ends;
    IdColumnView parents;

    static LocalTable of(const Root& root) {
        auto& symbol_hashes = root.symbol_hashes;
        auto symbols = ids_of(root.local_symbols, symbol_hashes.size());
        return {
            .root = root,
            .symbols = symbols,
            .name_ends = ids_of(root.local_name_ends, root.local_names.size() + 1),
            .args_of = ids_of(root.local_args_of, symbols.size()),
            .args_ends = ids_of(root.local_args_ends, root.local_args.size() + 1),
            .signatures_of = ids_of(root.local_signatures_of, symbols.size()),
            .signature_ends = ids_of(root.local_signature_ends, root.local_signatures.size() + 1),
            .parents = ids_of(root.local_parents, symbol_hashes.size() + 1),
        };
    }

    std::uint32_t size() const {
        return symbols.size();
    }

    SymbolHash hash_of(std::uint32_t k) const {
        return root.symbol_hashes[symbols[k]];
    }

    SymbolView at(std::uint32_t k) const {
        auto args_at = args_of.find(k);
        auto signature_at = signatures_of.find(k);
        return {
            .name = name_ends.slice(root.local_names, k),
            .args = args_at ? args_ends.slice(root.local_args, *args_at) : llvm::StringRef(),
            .signature = signature_at ? signature_ends.slice(root.local_signatures, *signature_at)
                                      : llvm::StringRef(),
            .parent = parents[k] == 0 ? 0 : root.symbol_hashes[parents[k] - 1],
            .kind = SymbolKind(root.local_kinds[k]),
            .scope = static_cast<SymbolScope>(root.local_scopes[k]),
            .flags = static_cast<SymbolFlags>(root.local_flags[k]),
            .token_class = {.kind = static_cast<SemanticTokenKind>(root.local_token_kinds[k]),
                            .modifiers = root.local_token_modifiers[k]},
        };
    }
};

}  // namespace clice::index
