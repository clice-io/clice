module;

#include "modules/prelude.h"

#include "support/logging.macros.h"

module clice;

import :index.columns;
import :index.serialization;
import :index.shard;
import :semantic.token_class;
import :support.logging;

namespace clice::index {

/// How a blob encodes its variant masks, a strict function of the variant
/// count.
enum class MaskEncoding : std::uint8_t {
    /// One variant: no mask columns at all.
    Single,
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

/// The number of variants the blob's masks encode: the stored table's
/// size, or one for a worker-emitted blob (empty table, one anonymous
/// variant).
std::size_t variant_count_of(BlobView root) {
    auto stored = to_array_ref(root[&ShardBlob::variants]);
    return stored.empty() ? 1 : stored.size();
}

MaskEncoding encoding_of(BlobView root) {
    return encoding_of(variant_count_of(root));
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
        switch(encoding) {
            case MaskEncoding::Single: {
                return (bits & 1) != 0;
            }
            case MaskEncoding::U32: {
                return (columns->masks32[entry] & static_cast<std::uint32_t>(bits)) != 0;
            }
            case MaskEncoding::U64: {
                return (columns->masks64[entry] & bits) != 0;
            }
            case MaskEncoding::Roaring: {
                return columns->bitmap_of(entry).intersect(*big);
            }
        }
        std::unreachable();
    }
};

namespace {

/// The occurrence table: each occurrence's range and the symbol it names.
struct OccurrenceTable {
    RangeColumnsView columns;
    SymIds syms;
    llvm::ArrayRef<std::uint64_t> sym_hashes;

    static OccurrenceTable of(BlobView root) {
        return {
            .columns = columns_of(root[&ShardBlob::occs]),
            .syms = {to_array_ref(root[&ShardBlob::occ_syms8]),
                     to_array_ref(root[&ShardBlob::occ_syms16]),
                     to_array_ref(root[&ShardBlob::occ_syms32])},
            .sym_hashes = to_array_ref(root[&ShardBlob::sym_hashes]),
        };
    }

    std::size_t size() const {
        return columns.size();
    }

    Occurrence at(std::uint32_t entry) const {
        return {columns.range_of(entry), sym_hashes[syms[entry]]};
    }
};

/// The relation table: relations grouped per symbol, each with its kind,
/// range and payload. Payloads live in two sparse tables — decl/def
/// relations carry a definition range, symbol-pair relations the target's
/// id — walked by a cursor in stored order.
struct RelationTable {
    RangeColumnsView columns;
    llvm::ArrayRef<std::uint8_t> kinds;
    llvm::ArrayRef<std::uint32_t> sym_entries;
    SymIds syms;
    llvm::ArrayRef<std::uint32_t> def_entries;
    llvm::ArrayRef<std::uint32_t> def_begins;
    llvm::ArrayRef<std::uint32_t> def_ends;
    llvm::ArrayRef<std::uint64_t> sym_hashes;
    /// Entry i's relations occupy [offsets[i], offsets[i + 1]).
    llvm::ArrayRef<std::uint32_t> offsets;

    static RelationTable of(BlobView root) {
        return {
            .columns = columns_of(root[&ShardBlob::rels]),
            .kinds = to_array_ref(root[&ShardBlob::rel_kinds]),
            .sym_entries = to_array_ref(root[&ShardBlob::rel_sym_entries]),
            .syms = {to_array_ref(root[&ShardBlob::rel_sym8]),
                     to_array_ref(root[&ShardBlob::rel_sym16]),
                     to_array_ref(root[&ShardBlob::rel_sym32])},
            .def_entries = to_array_ref(root[&ShardBlob::rel_def_entries]),
            .def_begins = to_array_ref(root[&ShardBlob::rel_def_begins]),
            .def_ends = to_array_ref(root[&ShardBlob::rel_def_ends]),
            .sym_hashes = to_array_ref(root[&ShardBlob::sym_hashes]),
            .offsets = to_array_ref(root[&ShardBlob::sym_rel_offsets]),
        };
    }

    std::size_t size() const {
        return columns.size();
    }

    RelationKind kind_of(std::uint32_t entry) const {
        return static_cast<RelationKind::Kind>(kinds[entry]);
    }

    /// Positions in the two payload tables; decode() advances them, so
    /// relations must be visited in ascending order from where the cursor was
    /// seeked.
    struct Cursor {
        std::size_t sym = 0;
        std::size_t def = 0;
    };

    Cursor cursor_at(std::uint32_t entry) const {
        return {
            static_cast<std::size_t>(std::ranges::lower_bound(sym_entries, entry) -
                                     sym_entries.begin()),
            static_cast<std::size_t>(std::ranges::lower_bound(def_entries, entry) -
                                     def_entries.begin()),
        };
    }

    Relation decode(std::uint32_t entry, Cursor& cursor) const {
        while(cursor.sym < sym_entries.size() && sym_entries[cursor.sym] < entry) {
            cursor.sym += 1;
        }
        while(cursor.def < def_entries.size() && def_entries[cursor.def] < entry) {
            cursor.def += 1;
        }
        Relation relation{
            .kind = kind_of(entry),
            .range = columns.range_of(entry),
            .target_symbol = 0,
        };
        if(cursor.def < def_entries.size() && def_entries[cursor.def] == entry) {
            relation.set_definition_range({def_begins[cursor.def], def_ends[cursor.def]});
        } else if(cursor.sym < sym_entries.size() && sym_entries[cursor.sym] == entry) {
            relation.target_symbol = sym_hashes[syms[cursor.sym]];
        }
        return relation;
    }
};

/// The local-name table: the symbols whose identities live in no other
/// table, sparse over the symbol table in ascending id order.
struct LocalTable {
    BlobView root;
    llvm::ArrayRef<std::uint32_t> syms;
    llvm::ArrayRef<std::uint8_t> kinds;
    llvm::ArrayRef<std::uint8_t> scopes;
    llvm::ArrayRef<std::uint64_t> parents;
    llvm::ArrayRef<std::uint16_t> flags;
    llvm::ArrayRef<std::uint8_t> token_kinds;
    llvm::ArrayRef<std::uint32_t> token_modifiers;
    llvm::ArrayRef<std::uint64_t> sym_hashes;

    static LocalTable of(BlobView root) {
        return {
            .root = root,
            .syms = to_array_ref(root[&ShardBlob::local_syms]),
            .kinds = to_array_ref(root[&ShardBlob::local_kinds]),
            .scopes = to_array_ref(root[&ShardBlob::local_scopes]),
            .parents = to_array_ref(root[&ShardBlob::local_parents]),
            .flags = to_array_ref(root[&ShardBlob::local_flags]),
            .token_kinds = to_array_ref(root[&ShardBlob::local_token_kinds]),
            .token_modifiers = to_array_ref(root[&ShardBlob::local_token_modifiers]),
            .sym_hashes = to_array_ref(root[&ShardBlob::sym_hashes]),
        };
    }

    std::size_t size() const {
        return syms.size();
    }

    SymbolHash hash_of(std::size_t k) const {
        return sym_hashes[syms[k]];
    }

    /// The entry of symbol id `id`, if the symbol is local.
    std::optional<std::size_t> find(std::uint32_t id) const {
        auto it = std::ranges::lower_bound(syms, id);
        if(it == syms.end() || *it != id) {
            return std::nullopt;
        }
        return static_cast<std::size_t>(it - syms.begin());
    }

    SymbolIdentity at(std::size_t k) const {
        return {
            .name = to_ref(root[&ShardBlob::local_names].at(k)),
            .args = to_ref(root[&ShardBlob::local_args].at(k)),
            .signature = to_ref(root[&ShardBlob::local_signatures].at(k)),
            .parent = parents[k],
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

CheckResult content_ok(BlobView root) {
    auto content = to_ref(root[&ShardBlob::content]);
    if(content.empty()) {
        return {};
    }
    // Every freshness decision compares the advertised content hash
    // (manifest FileVersions, the merge's generation checks), so content
    // bytes corrupted under an intact structure would keep loading as
    // fresh while position mapping reads text the shard was not built
    // from.
    if(content.size() != root[&ShardBlob::content_size] ||
       llvm::xxh3_64bits(content) != root[&ShardBlob::content_hash]) {
        return std::unexpected("stored content does not match its recorded size and hash");
    }
    // Pure-ASCII content must be omitted — the canonical form.
    if(kota::ipc::lsp::is_ascii({content.data(), content.size()})) {
        return std::unexpected("pure-ASCII content stored");
    }
    return {};
}

// The line table reconstructs every line start by prefix sum, so it must
// both pair with its escape table and add up to exactly the content size
// — a drifted sum would shift every position mapping below the
// corruption. When the content is stored, the sum check is not enough: a
// table redistributing bytes between lines keeps the sum intact, so every
// start must match the one the content derives.
CheckResult line_table_ok(BlobView root) {
    auto content = to_ref(root[&ShardBlob::content]);
    auto line_lengths = to_array_ref(root[&ShardBlob::line_lengths]);
    auto long_lines = to_array_ref(root[&ShardBlob::long_lines]);
    auto long_line_lengths = to_array_ref(root[&ShardBlob::long_line_lengths]);
    if(line_lengths.empty() || !escapes_ok(line_lengths, long_lines, long_line_lengths.size()) ||
       !std::ranges::is_sorted(long_lines, std::less_equal{})) {
        return std::unexpected("line table does not pair with its escape table");
    }
    std::vector<std::uint32_t> starts;
    if(!content.empty()) {
        starts = kota::ipc::lsp::line_starts({content.data(), content.size()});
        if(starts.size() != line_lengths.size()) {
            return std::unexpected("line table does not match the stored content");
        }
    }
    // A "\r\n" ending needs a line holding at least those two bytes and a
    // line after it; content that is stored says it itself.
    auto crlf_lines = to_array_ref(root[&ShardBlob::crlf_lines]);
    if(!crlf_lines.empty() && (!content.empty() || crlf_lines.back() == 0 ||
                               crlf_lines.size() > (line_lengths.size() + 63) / 64)) {
        return std::unexpected("CRLF line table is not canonical");
    }
    auto ends_crlf = [&](std::size_t line) {
        return line / 64 < crlf_lines.size() && ((crlf_lines[line / 64] >> (line % 64)) & 1) != 0;
    };
    std::uint64_t line_sum = 0;
    std::size_t escape_cursor = 0;
    for(std::size_t line = 0; line < line_lengths.size(); line += 1) {
        if(!starts.empty() && starts[line] != line_sum) {
            return std::unexpected("line table does not match the stored content");
        }
        std::uint32_t length = line_lengths[line];
        if(length == length_escape) {
            length = long_line_lengths[escape_cursor];
            escape_cursor += 1;
            if(length < length_escape) {
                return std::unexpected("escaped line length below the escape threshold");
            }
        }
        if(ends_crlf(line) && (length < 2 || line + 1 == line_lengths.size())) {
            return std::unexpected("CRLF bit on a line that cannot end in CRLF");
        }
        line_sum += length;
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
    auto offsets = to_array_ref(root[&ShardBlob::sym_rel_offsets]);
    if(offsets.size() != sym_hashes.size() + 1) {
        return std::unexpected("relation offsets do not match the symbol table");
    }
    if(!std::ranges::is_sorted(offsets) || offsets.back() != RelationTable::of(root).size()) {
        return std::unexpected("relation offsets are not a partition of the rows");
    }
    if(!std::ranges::is_sorted(sym_hashes, std::less_equal{})) {
        return std::unexpected("symbol hashes are not strictly ascending");
    }
    return {};
}

CheckResult range_encodings_ok(BlobView root) {
    auto content_size = root[&ShardBlob::content_size];
    for(auto columns: {columns_of(root[&ShardBlob::occs]), columns_of(root[&ShardBlob::rels])}) {
        if(auto ok = range_encoding_ok(columns, content_size); !ok) {
            return ok;
        }
    }
    return {};
}

// lookup(offset) binary-searches the decoded end column and stops its
// containment walk on begin order; occurrences out of either order (a corrupt
// escaped end included) would silently miss or misresolve occurrences on
// every query, forever — reject the blob so it is rebuilt instead. Ends
// are bounded by the content size too: every decoded range is served as
// a source range into the content.
// The merge also two-way merges occurrences under the full (begin, end, sym)
// key with equal-key occurrences combined at write time, so equal ranges must
// carry strictly ascending symbols — sym_hashes is strictly sorted, so
// id order stands in for hash order.
CheckResult occurrences_ok(BlobView root) {
    auto table = OccurrenceTable::of(root);
    if(auto ok = sym_ids_ok(table.syms, table.size(), table.sym_hashes.size()); !ok) {
        return ok;
    }
    auto content_size = root[&ShardBlob::content_size];
    std::uint32_t prev_begin = 0;
    std::uint32_t prev_end = 0;
    std::uint32_t prev_sym = 0;
    for(std::uint32_t entry = 0; entry < table.size(); entry += 1) {
        auto begin = table.columns.begin_of(entry);
        auto end = table.columns.end_of(entry);
        auto sym = table.syms[entry];
        if(begin < prev_begin || end < prev_end || end < begin || end > content_size) {
            return std::unexpected("occurrence rows are out of order or past the content");
        }
        if(entry != 0 && begin == prev_begin && end == prev_end && sym <= prev_sym) {
            return std::unexpected("occurrence rows of one range are not strictly ascending");
        }
        prev_begin = begin;
        prev_end = end;
        prev_sym = sym;
    }
    return {};
}

CheckResult relations_ok(BlobView root) {
    auto table = RelationTable::of(root);
    auto content_size = root[&ShardBlob::content_size];
    if(table.kinds.size() != table.size()) {
        return std::unexpected("relation kinds do not match the row count");
    }

    // Relation ranges carry no query order to enforce, but are served as
    // source ranges all the same — bound them like the occurrence ends.
    // The exception is the default LocalSourceRange, the writer's sentinel
    // for pair relations, which carry no range of their own; every other
    // kind is written with a real range, so a sentinel there is corruption
    // that would serve an invalid source range forever.
    for(std::uint32_t entry = 0; entry < table.size(); entry += 1) {
        auto begin = table.columns.begin_of(entry);
        auto end = table.columns.end_of(entry);
        if(is_no_range(begin, end)) {
            if(!table.kind_of(entry).isBetweenSymbol()) {
                return std::unexpected("no-range sentinel on a relation that carries a range");
            }
            continue;
        }
        if(end < begin || end > content_size) {
            return std::unexpected("relation range past the content");
        }
    }

    if(!sparse_ok(table.sym_entries, table.syms.size(), table.size())) {
        return std::unexpected("relation target table is not sparse over the rows");
    }
    if(auto ok = sym_ids_ok(table.syms, table.sym_entries.size(), table.sym_hashes.size()); !ok) {
        return ok;
    }
    if(!sparse_ok(table.def_entries, table.def_begins.size(), table.size()) ||
       table.def_ends.size() != table.def_begins.size()) {
        return std::unexpected("definition range table is not sparse over the rows");
    }
    for(std::uint32_t k = 0; k < table.def_begins.size(); k += 1) {
        if(table.def_ends[k] < table.def_begins[k] || table.def_ends[k] > content_size) {
            return std::unexpected("definition range past the content");
        }
    }

    // The writer splits payloads by kind — decl/def and instantiation
    // relations carry an extent, every other payload names a target symbol — and
    // the readers decode whichever sparse table holds the relation without
    // consulting its kind. A relation in the wrong table would serve one
    // payload's bit pattern as the other (a source range as a symbol
    // hash, or vice versa), so enforce the partition, which also keeps
    // the tables disjoint.
    for(auto entry: table.sym_entries) {
        if(table.kind_of(entry).carries_extent()) {
            return std::unexpected("declaration row in the target table");
        }
    }
    for(auto entry: table.def_entries) {
        if(!table.kind_of(entry).carries_extent()) {
            return std::unexpected("non-declaration row in the definition range table");
        }
    }

    // Entries of one relation group two-way merge under the full (kind,
    // begin, end, payload) key with equal-key entries combined at write time,
    // so the key must ascend strictly within each group — out-of-order or
    // repeated entries would mis-merge silently instead of being rejected.
    RelationTable::Cursor cursor;
    for(std::size_t id = 0; id < table.sym_hashes.size(); id += 1) {
        std::tuple<std::uint8_t, std::uint32_t, std::uint32_t, std::uint64_t> prev{};
        for(auto entry = table.offsets[id]; entry < table.offsets[id + 1]; entry += 1) {
            auto relation = table.decode(entry, cursor);
            std::tuple key{table.kinds[entry],
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
    if(!sparse_ok(table.syms, table.kinds.size(), table.sym_hashes.size()) ||
       table.scopes.size() != table.size() || table.parents.size() != table.size() ||
       table.flags.size() != table.size() || table.token_kinds.size() != table.size() ||
       table.token_modifiers.size() != table.size() || !all_token_kinds(table.token_kinds) ||
       root[&ShardBlob::local_names].size() != table.size() ||
       root[&ShardBlob::local_args].size() != table.size() ||
       root[&ShardBlob::local_signatures].size() != table.size()) {
        return std::unexpected("local symbol columns do not line up");
    }
    // A local symbol's parent becomes a DenseSet key in a query's container
    // walk, where the sentinel values corrupt or assert.
    for(auto parent: table.parents) {
        if(reserved_key(parent)) {
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
    auto variant_count = variant_count_of(root);
    auto encoding = encoding_of(variant_count);
    for(auto columns: {columns_of(root[&ShardBlob::occs]), columns_of(root[&ShardBlob::rels])}) {
        auto count = columns.size();
        switch(encoding) {
            case MaskEncoding::Single: {
                if(!columns.masks32.empty() || !columns.masks64.empty() ||
                   !columns.roaring_offsets.empty() || !columns.roaring.empty()) {
                    return std::unexpected("mask columns on a single-variant blob");
                }
                break;
            }
            case MaskEncoding::U32: {
                if(columns.masks32.size() != count || !columns.masks64.empty() ||
                   !columns.roaring_offsets.empty()) {
                    return std::unexpected("mask tier is not the canonical one");
                }
                auto stray = variant_count < 32 ? ~std::uint32_t(0) << variant_count : 0;
                if(!llvm::all_of(columns.masks32, [&](std::uint32_t mask) {
                       return mask != 0 && (mask & stray) == 0;
                   })) {
                    return std::unexpected("row mask owns no stored variant");
                }
                break;
            }
            case MaskEncoding::U64: {
                if(columns.masks64.size() != count || !columns.masks32.empty() ||
                   !columns.roaring_offsets.empty()) {
                    return std::unexpected("mask tier is not the canonical one");
                }
                auto stray = variant_count < 64 ? ~std::uint64_t(0) << variant_count : 0;
                if(!llvm::all_of(columns.masks64, [&](std::uint64_t mask) {
                       return mask != 0 && (mask & stray) == 0;
                   })) {
                    return std::unexpected("row mask owns no stored variant");
                }
                break;
            }
            case MaskEncoding::Roaring: {
                if(!columns.masks32.empty() || !columns.masks64.empty() ||
                   columns.roaring_offsets.size() != count + 1 ||
                   !std::ranges::is_sorted(columns.roaring_offsets) ||
                   columns.roaring_offsets.back() != columns.roaring.size() ||
                   (count != 0 && columns.roaring_offsets.front() != 0)) {
                    return std::unexpected("mask tier is not the canonical one");
                }
                // A slice failing decode would read as an empty mask — the
                // ownerless-entry corruption above in another coat. Prove
                // each slice decodes once here so queries stay check-free
                // and the blob rebuilds instead.
                for(std::uint32_t entry = 0; entry < count; entry += 1) {
                    auto begin = columns.roaring_offsets[entry];
                    auto mask = view_bitmap(columns.roaring.data() + begin,
                                            columns.roaring_offsets[entry + 1] - begin);
                    if(!mask || mask->isEmpty() || mask->maximum() >= variant_count) {
                        return std::unexpected(
                            "row mask does not decode or owns no stored variant");
                    }
                }
                break;
            }
        }
    }
    return {};
}

/// Structural verification does not constrain field values; everything the
/// readers dereference through raw column pointers or binary-search must be
/// proven in-bounds and in order here, once, so queries stay check-free.
/// The checks are also canonicality checks: every self-describing choice
/// (range encoding, symbol-id width, content omission, mask encoding) is a strict
/// function of the data, so one logical blob has exactly one encoding and
/// its byte hash is a usable identity. Later checks decode through columns
/// earlier ones bounded, so the order matters.
CheckResult validate(BlobView root) {
    constexpr std::array checks = {
        variants_ok,
        content_ok,
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

llvm::StringRef Shard::content() const {
    if(!buffer) {
        return {};
    }
    return to_ref(root_of(*buffer)[&ShardBlob::content]);
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
    auto entries = std::views::iota(std::uint32_t(0), static_cast<std::uint32_t>(table.size()));
    auto first = std::ranges::lower_bound(entries, offset, {}, [&](std::uint32_t entry) {
        return table.columns.end_of(entry);
    });

    // A cursor between two tokens belongs to the one starting at it, so
    // occurrences the offset only touches at their right edge yield after every
    // occurrence it is properly inside — `a+b` with the cursor before `b` must
    // resolve to `b`, not `+`, while a cursor right after a lone token
    // still hits that token.
    llvm::SmallVector<std::uint32_t, 2> edge_entries;
    for(auto it = first; it != entries.end(); it += 1) {
        auto entry = *it;
        auto range = table.columns.range_of(entry);
        if(!range.touches(offset)) {
            break;
        }
        if(!is_live(entry)) {
            continue;
        }
        if(range.end == offset && range.begin != offset) {
            edge_entries.push_back(entry);
            continue;
        }
        if(!callback(table.at(entry))) {
            return;
        }
    }
    for(auto entry: edge_entries) {
        if(!callback(table.at(entry))) {
            return;
        }
    }
}

namespace {

/// Visit the relations [begin_entry, end_entry) that pass `is_live`;
/// false when the callback stopped the walk.
bool visit_relation_entries(const RelationTable& table,
                            std::uint32_t begin_entry,
                            std::uint32_t end_entry,
                            const LiveFilter& is_live,
                            llvm::function_ref<bool(const Relation&)> callback) {
    auto cursor = table.cursor_at(begin_entry);
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
    visit_relation_entries(table,
                           table.offsets[id],
                           table.offsets[id + 1],
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
    for(std::uint32_t entry = 0; entry < table.size(); entry += 1) {
        if(is_live(entry) && !callback(table.at(entry))) {
            return;
        }
    }
}

void
    Shard::for_each_relation(llvm::function_ref<bool(SymbolHash, const Relation&)> callback) const {
    if(!buffer) {
        return;
    }
    auto table = RelationTable::of(root_of(*buffer));
    auto is_live = live_filter(table.columns);
    for(std::uint32_t id = 0; id < table.sym_hashes.size(); id += 1) {
        auto hash = table.sym_hashes[id];
        bool completed = visit_relation_entries(
            table,
            table.offsets[id],
            table.offsets[id + 1],
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
    auto local = table.find(static_cast<std::uint32_t>(it - table.sym_hashes.begin()));
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
    if(!content().empty()) {
        return {content(), line_starts()};
    }
    return {content_size(), line_starts(), crlf_lines()};
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
    switch(encoding) {
        case MaskEncoding::Single: {
            apply(0);
            break;
        }
        case MaskEncoding::U32: {
            apply_bits(columns.masks32[entry]);
            break;
        }
        case MaskEncoding::U64: {
            apply_bits(columns.masks64[entry]);
            break;
        }
        case MaskEncoding::Roaring: {
            for(auto id: columns.bitmap_of(entry)) {
                apply(id);
            }
            break;
        }
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
    for(std::uint32_t entry = 0; entry < table.size(); entry += 1) {
        auto mask = mask_of(entry);
        if(mask_empty(mask)) {
            continue;
        }
        entries.push_back({table.at(entry), std::move(mask)});
    }
    return entries;
}

/// Decode one symbol's relation slice of a blob into working form.
template <typename MaskT, typename MaskOf>
std::vector<RelationEntry<MaskT>> decode_relation_group(const RelationTable& table,
                                                        std::uint32_t begin_entry,
                                                        std::uint32_t end_entry,
                                                        MaskOf mask_of) {
    std::vector<RelationEntry<MaskT>> entries;
    entries.reserve(end_entry - begin_entry);
    auto cursor = table.cursor_at(begin_entry);
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
    llvm::StringRef content;
    std::vector<std::uint8_t> line_lengths;
    std::vector<std::uint32_t> long_lines;
    std::vector<std::uint32_t> long_line_lengths;
    std::vector<std::uint64_t> crlf_lines;
};

ContentInfo content_info_of(BlobView root) {
    ContentInfo info;
    info.hash = root[&ShardBlob::content_hash];
    info.size = root[&ShardBlob::content_size];
    info.content = to_ref(root[&ShardBlob::content]);
    auto lengths = to_array_ref(root[&ShardBlob::line_lengths]);
    auto long_lines = to_array_ref(root[&ShardBlob::long_lines]);
    auto long_lengths = to_array_ref(root[&ShardBlob::long_line_lengths]);
    info.line_lengths.assign(lengths.begin(), lengths.end());
    info.long_lines.assign(long_lines.begin(), long_lines.end());
    info.long_line_lengths.assign(long_lengths.begin(), long_lengths.end());
    auto crlf_lines = to_array_ref(root[&ShardBlob::crlf_lines]);
    info.crlf_lines.assign(crlf_lines.begin(), crlf_lines.end());
    return info;
}

ContentInfo content_info_of(llvm::StringRef content) {
    ContentInfo info;
    info.hash = llvm::xxh3_64bits(content);
    info.size = static_cast<std::uint32_t>(content.size());
    bool ascii = kota::ipc::lsp::is_ascii({content.data(), content.size()});
    if(!ascii) {
        info.content = content;
    }

    auto starts = kota::ipc::lsp::line_starts({content.data(), content.size()});
    info.line_lengths.reserve(starts.size());
    for(std::size_t i = 0; i < starts.size(); i += 1) {
        auto next = i + 1 < starts.size() ? starts[i + 1] : info.size;
        auto length = next - starts[i];
        if(ascii && i + 1 < starts.size() && length >= 2 && content[next - 2] == '\r') {
            info.crlf_lines.resize(i / 64 + 1);
            info.crlf_lines[i / 64] |= std::uint64_t(1) << (i % 64);
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
/// the range encoding by content size, the mask encoding by variant count.
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
/// (range encoding, id width, mask encoding, content omission) is made here, from
/// the data, so equal inputs produce equal bytes.
template <typename MaskT>
void emit_blob(const MergedEntries<MaskT>& merged,
               const llvm::DenseMap<SymbolHash, LocalSymbol>& locals,
               std::vector<VariantHash> variants,
               const ContentInfo& content,
               llvm::raw_ostream& os) {
    auto referenced = referenced_symbols(merged);

    ShardBlob blob;
    blob.format_version = index_format_version;
    blob.content_hash = content.hash;
    blob.content_size = content.size;
    blob.content = content.content.str();
    blob.line_lengths = content.line_lengths;
    blob.long_lines = content.long_lines;
    blob.long_line_lengths = content.long_line_lengths;
    blob.crlf_lines = content.crlf_lines;
    blob.variants = std::move(variants);

    blob.sym_hashes.assign(referenced.begin(), referenced.end());
    std::ranges::sort(blob.sym_hashes);
    auto sym_id = [&](SymbolHash hash) {
        return static_cast<std::uint32_t>(std::ranges::lower_bound(blob.sym_hashes, hash) -
                                          blob.sym_hashes.begin());
    };

    llvm::SmallVector<std::pair<std::uint32_t, const LocalSymbol*>> sorted_locals;
    sorted_locals.reserve(locals.size());
    for(auto& [hash, info]: locals) {
        if(referenced.contains(hash)) {
            sorted_locals.emplace_back(sym_id(hash), &info);
        }
    }
    std::ranges::sort(sorted_locals, {}, [](const auto& entry) { return entry.first; });
    for(auto& [id, info]: sorted_locals) {
        blob.local_syms.push_back(id);
        blob.local_names.push_back(info->name);
        blob.local_kinds.push_back(info->kind.value());
        blob.local_scopes.push_back(static_cast<std::uint8_t>(info->scope));
        blob.local_args.push_back(info->args);
        blob.local_signatures.push_back(info->signature);
        blob.local_parents.push_back(info->parent);
        blob.local_flags.push_back(static_cast<std::uint16_t>(info->flags));
        blob.local_token_kinds.push_back(static_cast<std::uint8_t>(info->token_class.kind));
        blob.local_token_modifiers.push_back(info->token_class.modifiers);
    }

    auto encoding = encoding_of(blob.variants.empty() ? 1 : blob.variants.size());
    bool narrow = content.size <= packed_range_limit;
    auto width = SymIdWriter::width_for(blob.sym_hashes.size());

    SideWriter occs{
        .ranges = {.side = blob.occs, .narrow = narrow},
        .encoding = encoding,
    };
    SymIdWriter occ_syms{width, blob.occ_syms8, blob.occ_syms16, blob.occ_syms32};
    for(auto& entry: merged.occurrences) {
        occs.add(entry.occurrence.range, entry.mask);
        occ_syms.add(sym_id(entry.occurrence.target));
    }
    occs.finish();

    // Relation groups follow symbol-table order; a symbol with occurrences
    // only gets an empty slice.
    SideWriter rels{
        .ranges = {.side = blob.rels, .narrow = narrow},
        .encoding = encoding,
    };
    SymIdWriter rel_syms{width, blob.rel_sym8, blob.rel_sym16, blob.rel_sym32};
    blob.sym_rel_offsets.reserve(blob.sym_hashes.size() + 1);
    auto group = merged.relations.begin();
    for(auto hash: blob.sym_hashes) {
        blob.sym_rel_offsets.push_back(rels.count());
        if(group == merged.relations.end() || group->symbol != hash) {
            continue;
        }
        for(auto& entry: group->entries) {
            auto& relation = entry.relation;
            blob.rel_kinds.push_back(static_cast<std::uint8_t>(relation.kind));
            if(relation.target_symbol != 0) {
                if(RelationKind(relation.kind).carries_extent()) {
                    auto range = std::bit_cast<LocalSourceRange>(relation.target_symbol);
                    blob.rel_def_entries.push_back(rels.count());
                    blob.rel_def_begins.push_back(range.begin);
                    blob.rel_def_ends.push_back(range.end);
                } else {
                    blob.rel_sym_entries.push_back(rels.count());
                    rel_syms.add(sym_id(relation.target_symbol));
                }
            }
            rels.add(relation.range, entry.mask);
        }
        group += 1;
    }
    blob.sym_rel_offsets.push_back(rels.count());
    rels.finish();

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
        for(auto& group: relation_groups(table)) {
            auto entries = decode_relation_group<MaskT>(table,
                                                        group.begin_entry,
                                                        group.end_entry,
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
    auto decode_old = [&](const GroupIndex& group) {
        return decode_relation_group<MaskT>(
            old_table,
            group.begin_entry,
            group.end_entry,
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
