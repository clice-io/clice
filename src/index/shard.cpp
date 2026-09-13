#include "index/shard.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cassert>
#include <expected>
#include <ranges>
#include <tuple>
#include <utility>

#include "index/serialization.h"
#include "support/logging.h"

#include "kota/ipc/lsp/text.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/xxhash.h"

namespace clice::index {

/// How a blob encodes its variant masks, a strict function of the variant
/// count.
enum class MaskTier : std::uint8_t {
    /// One variant: no mask columns at all.
    Single,
    U32,
    U64,
    Roaring,
};

namespace {

using BlobView = kota::codec::fbs::table_view<ShardBlob>;

/// Load-time verification outcome: the violated invariant, spelled for the
/// log, or success.
using Verdict = std::expected<void, llvm::StringRef>;

MaskTier tier_of(std::size_t variant_count) {
    if(variant_count <= 1) {
        return MaskTier::Single;
    }
    if(variant_count <= 32) {
        return MaskTier::U32;
    }
    if(variant_count <= 64) {
        return MaskTier::U64;
    }
    return MaskTier::Roaring;
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

MaskTier tier_of(BlobView root) {
    return tier_of(variant_count_of(root));
}

/// The no-range sentinel of pair relations: the default LocalSourceRange.
bool is_no_range(std::uint32_t begin, std::uint32_t end) {
    return LocalSourceRange{begin, end} == LocalSourceRange{};
}

bool is_ascii(llvm::StringRef text) {
    return llvm::all_of(text, [](char c) { return static_cast<unsigned char>(c) < 0x80; });
}

/// The symbol-id column of one row table in whichever width the blob
/// stores; validation proves exactly one width is present.
struct SymIds {
    llvm::ArrayRef<std::uint8_t> ids8;
    llvm::ArrayRef<std::uint16_t> ids16;
    llvm::ArrayRef<std::uint32_t> ids32;

    std::size_t size() const {
        return ids8.size() + ids16.size() + ids32.size();
    }

    std::uint32_t operator[](std::size_t i) const {
        if(!ids8.empty()) {
            return ids8[i];
        }
        if(!ids16.empty()) {
            return ids16[i];
        }
        return ids32[i];
    }
};

}  // namespace

/// One side of the blob's row storage as contiguous column refs,
/// tier-agnostic: the range accessors read whichever range tier the blob
/// stores, the mask columns whichever mask tier.
struct RowColumns {
    llvm::ArrayRef<std::uint32_t> packed;
    llvm::ArrayRef<std::uint32_t> begins;
    llvm::ArrayRef<std::uint8_t> lengths;
    llvm::ArrayRef<std::uint32_t> long_rows;
    llvm::ArrayRef<std::uint32_t> long_ends;
    llvm::ArrayRef<std::uint32_t> masks32;
    llvm::ArrayRef<std::uint64_t> masks64;
    llvm::ArrayRef<std::uint32_t> roaring_offsets;
    llvm::ArrayRef<std::uint8_t> roaring;

    std::size_t size() const {
        return packed.empty() ? begins.size() : packed.size();
    }

    std::uint32_t begin_of(std::uint32_t row) const {
        if(packed.empty()) {
            return begins[row];
        }
        return packed[row] == packed_sentinel ? ~std::uint32_t(0) : packed[row] >> 8;
    }

    std::uint32_t end_of(std::uint32_t row) const {
        if(!packed.empty() && packed[row] == packed_sentinel) {
            return ~std::uint32_t(0);
        }
        auto length = packed.empty() ? lengths[row] : static_cast<std::uint8_t>(packed[row] & 0xff);
        if(length == length_escape) {
            // validate() proves every sentinel owns exactly one escape
            // entry, so the search always lands.
            auto it = std::ranges::lower_bound(long_rows, row);
            return long_ends[it - long_rows.begin()];
        }
        return begin_of(row) + length;
    }

    LocalSourceRange range_of(std::uint32_t row) const {
        return {begin_of(row), end_of(row)};
    }

    /// The slice bounds were validated monotonic and in-bounds at load, and
    /// every slice proven to decode, so this cannot fail.
    Bitmap bitmap_of(std::uint32_t row) const {
        auto begin = roaring_offsets[row];
        return *read_bitmap(roaring.data() + begin, roaring_offsets[row + 1] - begin);
    }
};

/// The live mask against one side's mask columns, resolved once per query.
struct LiveFilter {
    /// Fast path: every stored variant is live, no per-row filtering.
    bool all;
    MaskTier tier;
    std::uint64_t bits;
    const Bitmap* big;
    const RowColumns* columns;

    bool operator()(std::uint32_t row) const {
        if(all) {
            return true;
        }
        switch(tier) {
            case MaskTier::Single: {
                return (bits & 1) != 0;
            }
            case MaskTier::U32: {
                return (columns->masks32[row] & static_cast<std::uint32_t>(bits)) != 0;
            }
            case MaskTier::U64: {
                return (columns->masks64[row] & bits) != 0;
            }
            case MaskTier::Roaring: {
                return columns->bitmap_of(row).intersect(*big);
            }
        }
        std::unreachable();
    }
};

namespace {

RowColumns columns_of(kota::codec::fbs::table_view<RowRanges> view) {
    if(!view.valid()) {
        return {};
    }
    return {
        to_array_ref(view[&RowRanges::packed]),
        to_array_ref(view[&RowRanges::begins]),
        to_array_ref(view[&RowRanges::lengths]),
        to_array_ref(view[&RowRanges::long_rows]),
        to_array_ref(view[&RowRanges::long_ends]),
        to_array_ref(view[&RowRanges::masks32]),
        to_array_ref(view[&RowRanges::masks64]),
        to_array_ref(view[&RowRanges::roaring_offsets]),
        to_array_ref(view[&RowRanges::roaring]),
    };
}

/// The occurrence table: each row's range and the symbol it names.
struct OccurrenceTable {
    RowColumns rows;
    SymIds syms;
    llvm::ArrayRef<std::uint64_t> sym_hashes;

    static OccurrenceTable of(BlobView root) {
        return {
            .rows = columns_of(root[&ShardBlob::occs]),
            .syms = {to_array_ref(root[&ShardBlob::occ_syms8]),
                     to_array_ref(root[&ShardBlob::occ_syms16]),
                     to_array_ref(root[&ShardBlob::occ_syms32])},
            .sym_hashes = to_array_ref(root[&ShardBlob::sym_hashes]),
        };
    }

    std::size_t size() const {
        return rows.size();
    }

    Occurrence at(std::uint32_t row) const {
        return {rows.range_of(row), sym_hashes[syms[row]]};
    }
};

/// The relation table: rows grouped per symbol, each with its kind, range
/// and payload. Payloads live in two sparse tables — decl/def rows carry a
/// definition range, symbol-pair rows the target's id — walked by a cursor
/// in row order.
struct RelationTable {
    RowColumns rows;
    llvm::ArrayRef<std::uint8_t> kinds;
    llvm::ArrayRef<std::uint32_t> sym_rows;
    SymIds syms;
    llvm::ArrayRef<std::uint32_t> def_rows;
    llvm::ArrayRef<std::uint32_t> def_begins;
    llvm::ArrayRef<std::uint32_t> def_ends;
    llvm::ArrayRef<std::uint64_t> sym_hashes;
    /// Entry i's rows occupy [offsets[i], offsets[i + 1]).
    llvm::ArrayRef<std::uint32_t> offsets;

    static RelationTable of(BlobView root) {
        return {
            .rows = columns_of(root[&ShardBlob::rels]),
            .kinds = to_array_ref(root[&ShardBlob::rel_kinds]),
            .sym_rows = to_array_ref(root[&ShardBlob::rel_sym_rows]),
            .syms = {to_array_ref(root[&ShardBlob::rel_sym8]),
                     to_array_ref(root[&ShardBlob::rel_sym16]),
                     to_array_ref(root[&ShardBlob::rel_sym32])},
            .def_rows = to_array_ref(root[&ShardBlob::rel_def_rows]),
            .def_begins = to_array_ref(root[&ShardBlob::rel_def_begins]),
            .def_ends = to_array_ref(root[&ShardBlob::rel_def_ends]),
            .sym_hashes = to_array_ref(root[&ShardBlob::sym_hashes]),
            .offsets = to_array_ref(root[&ShardBlob::sym_rel_offsets]),
        };
    }

    std::size_t size() const {
        return rows.size();
    }

    RelationKind kind_of(std::uint32_t row) const {
        return static_cast<RelationKind::Kind>(kinds[row]);
    }

    /// Positions in the two payload tables; decode() advances them, so
    /// rows must be visited in ascending order from where the cursor was
    /// seeked.
    struct Cursor {
        std::size_t sym = 0;
        std::size_t def = 0;
    };

    Cursor cursor_at(std::uint32_t row) const {
        return {
            static_cast<std::size_t>(std::ranges::lower_bound(sym_rows, row) - sym_rows.begin()),
            static_cast<std::size_t>(std::ranges::lower_bound(def_rows, row) - def_rows.begin()),
        };
    }

    Relation decode(std::uint32_t row, Cursor& cursor) const {
        while(cursor.sym < sym_rows.size() && sym_rows[cursor.sym] < row) {
            cursor.sym += 1;
        }
        while(cursor.def < def_rows.size() && def_rows[cursor.def] < row) {
            cursor.def += 1;
        }
        Relation relation{
            .kind = kind_of(row),
            .range = rows.range_of(row),
            .target_symbol = 0,
        };
        if(cursor.def < def_rows.size() && def_rows[cursor.def] == row) {
            relation.set_definition_range({def_begins[cursor.def], def_ends[cursor.def]});
        } else if(cursor.sym < sym_rows.size() && sym_rows[cursor.sym] == row) {
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
    llvm::ArrayRef<std::uint64_t> sym_hashes;

    static LocalTable of(BlobView root) {
        return {
            .root = root,
            .syms = to_array_ref(root[&ShardBlob::local_syms]),
            .kinds = to_array_ref(root[&ShardBlob::local_kinds]),
            .scopes = to_array_ref(root[&ShardBlob::local_scopes]),
            .parents = to_array_ref(root[&ShardBlob::local_parents]),
            .flags = to_array_ref(root[&ShardBlob::local_flags]),
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
            .parent = parents[k],
            .kind = SymbolKind(kinds[k]),
            .scope = static_cast<SymbolScope>(scopes[k]),
            .flags = static_cast<SymbolFlags>(flags[k]),
        };
    }
};

/// The escape table must pair one-to-one, in row order, with the sentinel
/// lengths: readers trust the pairing, and a sentinel missing its entry
/// (or a stray entry masking one elsewhere) can pass every range bound
/// while serving a wrong value forever.
bool escapes_ok(llvm::ArrayRef<std::uint8_t> lengths,
                llvm::ArrayRef<std::uint32_t> escape_rows,
                std::size_t escape_values) {
    if(escape_values != escape_rows.size()) {
        return false;
    }
    std::size_t cursor = 0;
    for(std::uint32_t row = 0; row < lengths.size(); row += 1) {
        if(lengths[row] != length_escape) {
            continue;
        }
        if(cursor == escape_rows.size() || escape_rows[cursor] != row) {
            return false;
        }
        cursor += 1;
    }
    return cursor == escape_rows.size();
}

/// Sentinel lengths inside the packed column, extracted for escapes_ok.
/// The no-range sentinel word shares the escape byte pattern but owns no
/// escape entry — report it as a plain length.
llvm::SmallVector<std::uint8_t> packed_lengths(llvm::ArrayRef<std::uint32_t> packed) {
    llvm::SmallVector<std::uint8_t> lengths;
    lengths.reserve(packed.size());
    for(auto value: packed) {
        lengths.push_back(value == packed_sentinel ? 0 : static_cast<std::uint8_t>(value & 0xff));
    }
    return lengths;
}

/// A sparse row table: one value per listed row, rows strictly ascending
/// and inside the row count.
bool sparse_ok(llvm::ArrayRef<std::uint32_t> rows, std::size_t values, std::size_t count) {
    return rows.size() == values && std::ranges::is_sorted(rows, std::less_equal{}) &&
           (rows.empty() || rows.back() < count);
}

/// Exactly one id width, chosen by the symbol table's size, one id per row
/// and every id inside the table.
Verdict sym_ids_ok(const SymIds& ids, std::size_t count, std::size_t table_size) {
    if(ids.size() != count) {
        return std::unexpected("symbol id column does not match the row count");
    }
    bool width_ok = table_size <= 0x100     ? ids.ids16.empty() && ids.ids32.empty()
                    : table_size <= 0x10000 ? ids.ids8.empty() && ids.ids32.empty()
                                            : ids.ids8.empty() && ids.ids16.empty();
    if(!width_ok) {
        return std::unexpected("symbol id width is not the canonical one");
    }
    auto in_table = [&](auto column) {
        return llvm::all_of(column, [&](std::uint32_t id) { return id < table_size; });
    };
    if(!in_table(ids.ids8) || !in_table(ids.ids16) || !in_table(ids.ids32)) {
        return std::unexpected("symbol id past the symbol table");
    }
    return {};
}

// A variant is identified by its hash everywhere (set_live, the merge
// keep filter), so stored hashes must be unique: rows owned only by a
// duplicated entry would serve and survive compaction with no
// contribution owning them.
Verdict variants_ok(BlobView root) {
    auto variants = to_array_ref(root[&ShardBlob::variants]);
    llvm::SmallVector<RowsHash> sorted(variants.begin(), variants.end());
    std::ranges::sort(sorted);
    if(std::ranges::adjacent_find(sorted) != sorted.end()) {
        return std::unexpected("duplicate variant identity");
    }
    return {};
}

Verdict content_ok(BlobView root) {
    auto content = to_ref(root[&ShardBlob::content]);
    if(content.empty()) {
        return {};
    }
    // Every freshness decision compares the advertised content hash
    // (manifest FileVersions, the merge's generation checks), so content
    // bytes corrupted under an intact structure would keep loading as
    // fresh while position mapping reads text the rows were not built
    // from.
    if(content.size() != root[&ShardBlob::content_size] ||
       llvm::xxh3_64bits(content) != root[&ShardBlob::content_hash]) {
        return std::unexpected("stored content does not match its recorded size and hash");
    }
    // Pure-ASCII content must be omitted — the canonical form.
    if(is_ascii(content)) {
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
Verdict line_table_ok(BlobView root) {
    auto content = to_ref(root[&ShardBlob::content]);
    auto line_lengths = to_array_ref(root[&ShardBlob::line_lengths]);
    auto long_line_rows = to_array_ref(root[&ShardBlob::long_line_rows]);
    auto long_line_lengths = to_array_ref(root[&ShardBlob::long_line_lengths]);
    if(line_lengths.empty() ||
       !escapes_ok(line_lengths, long_line_rows, long_line_lengths.size()) ||
       !std::ranges::is_sorted(long_line_rows, std::less_equal{})) {
        return std::unexpected("line table does not pair with its escape table");
    }
    std::vector<std::uint32_t> starts;
    if(!content.empty()) {
        starts =
            kota::ipc::lsp::build_line_starts(std::string_view(content.data(), content.size()));
        if(starts.size() != line_lengths.size()) {
            return std::unexpected("line table does not match the stored content");
        }
    }
    std::uint64_t line_sum = 0;
    std::size_t escape_cursor = 0;
    for(std::size_t row = 0; row < line_lengths.size(); row += 1) {
        if(!starts.empty() && starts[row] != line_sum) {
            return std::unexpected("line table does not match the stored content");
        }
        auto length = line_lengths[row];
        if(length == length_escape) {
            auto value = long_line_lengths[escape_cursor];
            escape_cursor += 1;
            if(value < length_escape) {
                return std::unexpected("escaped line length below the escape threshold");
            }
            line_sum += value;
        } else {
            line_sum += length;
        }
    }
    if(line_sum != root[&ShardBlob::content_size]) {
        return std::unexpected("line lengths do not add up to the content size");
    }
    return {};
}

// Strictly sorted symbol hashes: lookups lower-bound the hash column and
// read only the first match's slices, so a duplicated hash would strand
// the later id's relations and local name unreachably.
Verdict symbol_table_ok(BlobView root) {
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

// Exactly one range tier, chosen by the content size, with its escape
// table paired on both sides.
Verdict range_tiers_ok(BlobView root) {
    auto content_size = root[&ShardBlob::content_size];
    for(auto columns: {columns_of(root[&ShardBlob::occs]), columns_of(root[&ShardBlob::rels])}) {
        bool tier_ok =
            content_size <= packed_range_limit
                ? columns.begins.empty() && columns.lengths.empty()
                : columns.packed.empty() && columns.lengths.size() == columns.begins.size();
        if(!tier_ok) {
            return std::unexpected("range tier is not the canonical one");
        }
        bool escaped_ok =
            columns.packed.empty()
                ? escapes_ok(columns.lengths, columns.long_rows, columns.long_ends.size())
                : escapes_ok(packed_lengths(columns.packed),
                             columns.long_rows,
                             columns.long_ends.size());
        if(!escaped_ok) {
            return std::unexpected("range escape table does not pair with its rows");
        }
    }
    return {};
}

// lookup(offset) binary-searches the decoded end column and stops its
// containment walk on begin order; rows out of either order (a corrupt
// escaped end included) would silently miss or misresolve occurrences on
// every query, forever — reject the blob so it is rebuilt instead. Ends
// are bounded by the content size too: every decoded range is served as
// a source range into the content.
// The merge also two-way merges rows under the full (begin, end, sym)
// key with equal-key rows combined at write time, so equal ranges must
// carry strictly ascending symbols — sym_hashes is strictly sorted, so
// id order stands in for hash order.
Verdict occurrences_ok(BlobView root) {
    auto table = OccurrenceTable::of(root);
    if(auto ok = sym_ids_ok(table.syms, table.size(), table.sym_hashes.size()); !ok) {
        return ok;
    }
    auto content_size = root[&ShardBlob::content_size];
    std::uint32_t prev_begin = 0;
    std::uint32_t prev_end = 0;
    std::uint32_t prev_sym = 0;
    for(std::uint32_t row = 0; row < table.size(); row += 1) {
        auto begin = table.rows.begin_of(row);
        auto end = table.rows.end_of(row);
        auto sym = table.syms[row];
        if(begin < prev_begin || end < prev_end || end < begin || end > content_size) {
            return std::unexpected("occurrence rows are out of order or past the content");
        }
        if(row != 0 && begin == prev_begin && end == prev_end && sym <= prev_sym) {
            return std::unexpected("occurrence rows of one range are not strictly ascending");
        }
        prev_begin = begin;
        prev_end = end;
        prev_sym = sym;
    }
    return {};
}

Verdict relations_ok(BlobView root) {
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
    for(std::uint32_t row = 0; row < table.size(); row += 1) {
        auto begin = table.rows.begin_of(row);
        auto end = table.rows.end_of(row);
        if(is_no_range(begin, end)) {
            if(!table.kind_of(row).isBetweenSymbol()) {
                return std::unexpected("no-range sentinel on a relation that carries a range");
            }
            continue;
        }
        if(end < begin || end > content_size) {
            return std::unexpected("relation range past the content");
        }
    }

    if(!sparse_ok(table.sym_rows, table.syms.size(), table.size())) {
        return std::unexpected("relation target table is not sparse over the rows");
    }
    if(auto ok = sym_ids_ok(table.syms, table.sym_rows.size(), table.sym_hashes.size()); !ok) {
        return ok;
    }
    if(!sparse_ok(table.def_rows, table.def_begins.size(), table.size()) ||
       table.def_ends.size() != table.def_begins.size()) {
        return std::unexpected("definition range table is not sparse over the rows");
    }
    for(std::uint32_t k = 0; k < table.def_begins.size(); k += 1) {
        if(table.def_ends[k] < table.def_begins[k] || table.def_ends[k] > content_size) {
            return std::unexpected("definition range past the content");
        }
    }

    // The writer splits payloads by kind — decl/def rows carry a
    // definition range, every other payload names a target symbol — and
    // the readers decode whichever sparse table holds the row without
    // consulting its kind. A row in the wrong table would serve one
    // payload's bit pattern as the other (a source range as a symbol
    // hash, or vice versa), so enforce the partition, which also keeps
    // the tables disjoint.
    for(auto row: table.sym_rows) {
        if(table.kind_of(row).isDeclOrDef()) {
            return std::unexpected("declaration row in the target table");
        }
    }
    for(auto row: table.def_rows) {
        if(!table.kind_of(row).isDeclOrDef()) {
            return std::unexpected("non-declaration row in the definition range table");
        }
    }

    // Rows of one relation group two-way merge under the full (kind,
    // begin, end, payload) key with equal-key rows combined at write time,
    // so the key must ascend strictly within each group — out-of-order or
    // repeated rows would mis-merge silently instead of being rejected.
    RelationTable::Cursor cursor;
    for(std::size_t id = 0; id < table.sym_hashes.size(); id += 1) {
        std::tuple<std::uint8_t, std::uint32_t, std::uint32_t, std::uint64_t> prev{};
        for(auto row = table.offsets[id]; row < table.offsets[id + 1]; row += 1) {
            auto relation = table.decode(row, cursor);
            std::tuple key{table.kinds[row],
                           relation.range.begin,
                           relation.range.end,
                           relation.target_symbol};
            if(row != table.offsets[id] && key <= prev) {
                return std::unexpected("relation rows of one symbol are not strictly ascending");
            }
            prev = key;
        }
    }
    return {};
}

Verdict locals_ok(BlobView root) {
    auto table = LocalTable::of(root);
    if(!sparse_ok(table.syms, table.kinds.size(), table.sym_hashes.size()) ||
       table.scopes.size() != table.size() || table.parents.size() != table.size() ||
       table.flags.size() != table.size() || root[&ShardBlob::local_names].size() != table.size() ||
       root[&ShardBlob::local_args].size() != table.size()) {
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

// Beyond the per-tier column shape, every mask must own at least one
// stored variant and no bits past the variant table: an ownerless row
// serves unconditionally while every stored variant is live (the
// LiveFilter fast path skips the mask), vanishes once any variant dies,
// and the next compaction erases it for real — every manifest still
// fresh throughout.
Verdict masks_ok(BlobView root) {
    auto variant_count = variant_count_of(root);
    auto tier = tier_of(variant_count);
    for(auto columns: {columns_of(root[&ShardBlob::occs]), columns_of(root[&ShardBlob::rels])}) {
        auto count = columns.size();
        switch(tier) {
            case MaskTier::Single: {
                if(!columns.masks32.empty() || !columns.masks64.empty() ||
                   !columns.roaring_offsets.empty() || !columns.roaring.empty()) {
                    return std::unexpected("mask columns on a single-variant blob");
                }
                break;
            }
            case MaskTier::U32: {
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
            case MaskTier::U64: {
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
            case MaskTier::Roaring: {
                if(!columns.masks32.empty() || !columns.masks64.empty() ||
                   columns.roaring_offsets.size() != count + 1 ||
                   !std::ranges::is_sorted(columns.roaring_offsets) ||
                   columns.roaring_offsets.back() != columns.roaring.size() ||
                   (count != 0 && columns.roaring_offsets.front() != 0)) {
                    return std::unexpected("mask tier is not the canonical one");
                }
                // A slice failing decode would read as an empty mask — the
                // ownerless-row corruption above in another coat. Prove
                // each slice decodes once here so queries stay check-free
                // and the blob rebuilds instead.
                for(std::uint32_t row = 0; row < count; row += 1) {
                    auto begin = columns.roaring_offsets[row];
                    auto mask = read_bitmap(columns.roaring.data() + begin,
                                            columns.roaring_offsets[row + 1] - begin);
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
/// (range tier, symbol-id width, content omission, mask tier) is a strict
/// function of the data, so one logical blob has exactly one encoding and
/// its byte hash is a usable identity. Later checks decode through columns
/// earlier ones bounded, so the order matters.
Verdict validate(BlobView root) {
    constexpr std::array checks = {
        variants_ok,
        content_ok,
        line_table_ok,
        symbol_table_ok,
        range_tiers_ok,
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

std::vector<RowsHash> Shard::variants() const {
    if(!buffer) {
        return {};
    }
    auto stored = to_array_ref(root_of(*buffer)[&ShardBlob::variants]);
    if(stored.empty()) {
        return {blob_hash};
    }
    return {stored.begin(), stored.end()};
}

bool Shard::has_variant(RowsHash hash) const {
    if(!buffer) {
        return false;
    }
    auto stored = to_array_ref(root_of(*buffer)[&ShardBlob::variants]);
    if(stored.empty()) {
        return hash == blob_hash;
    }
    return llvm::is_contained(stored, hash);
}

void Shard::set_live(llvm::ArrayRef<RowsHash> live_hashes) {
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

LiveFilter Shard::live_filter(const RowColumns& columns) const {
    return {
        .all = live.all,
        .tier = tier_of(root_of(*buffer)),
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
    auto is_live = live_filter(table.rows);

    // Binary search the first row whose end reaches the offset, then walk
    // while rows contain it. Occurrence ranges are name-token spans,
    // pairwise disjoint or identical, so under (begin, end) order the end
    // column is monotonic too.
    auto rows = std::views::iota(std::uint32_t(0), static_cast<std::uint32_t>(table.size()));
    auto first = std::ranges::lower_bound(rows, offset, {}, [&](std::uint32_t row) {
        return table.rows.end_of(row);
    });

    // A cursor between two tokens belongs to the one starting at it, so
    // rows the offset only touches at their right edge yield after every
    // row it is properly inside — `a+b` with the cursor before `b` must
    // resolve to `b`, not `+`, while a cursor right after a lone token
    // still hits that token.
    llvm::SmallVector<std::uint32_t, 2> edge_rows;
    for(auto it = first; it != rows.end(); it += 1) {
        auto row = *it;
        auto range = table.rows.range_of(row);
        if(!range.contains(offset)) {
            break;
        }
        if(!is_live(row)) {
            continue;
        }
        if(range.end == offset && range.begin != offset) {
            edge_rows.push_back(row);
            continue;
        }
        if(!callback(table.at(row))) {
            return;
        }
    }
    for(auto row: edge_rows) {
        if(!callback(table.at(row))) {
            return;
        }
    }
}

namespace {

/// Visit the relation rows [begin_row, end_row) that pass `is_live`;
/// false when the callback stopped the walk.
bool visit_relation_rows(const RelationTable& table,
                         std::uint32_t begin_row,
                         std::uint32_t end_row,
                         const LiveFilter& is_live,
                         llvm::function_ref<bool(const Relation&)> callback) {
    auto cursor = table.cursor_at(begin_row);
    for(auto row = begin_row; row < end_row; row += 1) {
        auto relation = table.decode(row, cursor);
        if(is_live(row) && !callback(relation)) {
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
    visit_relation_rows(table,
                        table.offsets[id],
                        table.offsets[id + 1],
                        live_filter(table.rows),
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
    auto is_live = live_filter(table.rows);
    for(std::uint32_t row = 0; row < table.size(); row += 1) {
        if(is_live(row) && !callback(table.at(row))) {
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
    auto is_live = live_filter(table.rows);
    for(std::uint32_t id = 0; id < table.sym_hashes.size(); id += 1) {
        auto hash = table.sym_hashes[id];
        bool completed =
            visit_relation_rows(table,
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

namespace {

/// Working rows during a write: the row as the readers hand it out plus
/// its variant mask. Masks stay wide; the tier is chosen at emit time from
/// the final variant count.
template <typename MaskT>
struct OccRow {
    Occurrence occurrence;
    MaskT mask;
};

template <typename MaskT>
struct RelRow {
    Relation relation;
    MaskT mask;
};

/// One symbol's relation rows, sorted by (kind, begin, end, payload).
template <typename MaskT>
struct RelGroup {
    SymbolHash symbol;
    std::vector<RelRow<MaskT>> rows;
};

/// Everything a write accumulates before choosing column tiers.
template <typename MaskT>
struct MergedRows {
    std::vector<OccRow<MaskT>> occurrences;
    /// Groups sorted by symbol hash.
    std::vector<RelGroup<MaskT>> relations;
};

constexpr auto occ_key = [](const auto& row) {
    return std::tuple(row.occurrence.range.begin, row.occurrence.range.end, row.occurrence.target);
};

/// The kind as the column stores it: the wire order the readers rely on.
constexpr auto rel_key = [](const auto& row) {
    return std::tuple(static_cast<std::uint8_t>(row.relation.kind),
                      row.relation.range.begin,
                      row.relation.range.end,
                      row.relation.target_symbol);
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

/// The old blob's mask of one row, remapped through old-id -> new-id (a
/// dropped variant's bit vanishes; an all-dropped row reads as empty and
/// is skipped by the caller).
template <typename MaskT>
MaskT remap_mask(MaskTier tier,
                 const RowColumns& columns,
                 std::uint32_t row,
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
    switch(tier) {
        case MaskTier::Single: {
            apply(0);
            break;
        }
        case MaskTier::U32: {
            apply_bits(columns.masks32[row]);
            break;
        }
        case MaskTier::U64: {
            apply_bits(columns.masks64[row]);
            break;
        }
        case MaskTier::Roaring: {
            for(auto id: columns.bitmap_of(row)) {
                apply(id);
            }
            break;
        }
    }
    return result;
}

/// Two-way merge of runs sorted under `key`; rows with equal keys are one
/// row and OR their masks — the cross-variant dedup.
template <typename Row, typename Key>
void merge_sorted(std::vector<Row> old_rows,
                  std::vector<Row> fresh_rows,
                  Key key,
                  std::vector<Row>& out) {
    out.reserve(old_rows.size() + fresh_rows.size());
    auto lhs = old_rows.begin();
    auto rhs = fresh_rows.begin();
    while(lhs != old_rows.end() || rhs != fresh_rows.end()) {
        if(rhs == fresh_rows.end() || (lhs != old_rows.end() && key(*lhs) < key(*rhs))) {
            out.push_back(std::move(*lhs));
            lhs += 1;
        } else if(lhs == old_rows.end() || key(*rhs) < key(*lhs)) {
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

/// Combine adjacent equal-key rows of a sorted run into one row with
/// OR-ed masks.
template <typename Row, typename Key>
void combine_equal(std::vector<Row>& rows, Key key) {
    std::size_t out = 0;
    for(std::size_t i = 0; i < rows.size(); i += 1) {
        if(out != 0 && key(rows[out - 1]) == key(rows[i])) {
            rows[out - 1].mask |= rows[i].mask;
        } else {
            if(out != i) {
                rows[out] = std::move(rows[i]);
            }
            out += 1;
        }
    }
    rows.resize(out);
}

/// Sort a run and combine its equal-key rows.
template <typename Row, typename Key>
void canonicalize(std::vector<Row>& rows, Key key) {
    std::ranges::sort(rows, {}, key);
    combine_equal(rows, key);
}

/// The symbol table a set of merged rows requires: occurrence targets,
/// relation group keys, and symbol payloads.
template <typename MaskT>
llvm::DenseSet<SymbolHash> referenced_symbols(const MergedRows<MaskT>& merged) {
    llvm::DenseSet<SymbolHash> referenced;
    for(auto& row: merged.occurrences) {
        referenced.insert(row.occurrence.target);
    }
    for(auto& group: merged.relations) {
        referenced.insert(group.symbol);
        for(auto& row: group.rows) {
            auto& relation = row.relation;
            if(relation.target_symbol != 0 && !RelationKind(relation.kind).isDeclOrDef()) {
                referenced.insert(relation.target_symbol);
            }
        }
    }
    return referenced;
}

/// Decode a blob's occurrence rows into working form; `mask_of` returns
/// the row's mask in the output id space (empty drops the row).
template <typename MaskT, typename MaskOf>
std::vector<OccRow<MaskT>> decode_occurrences(const OccurrenceTable& table, MaskOf mask_of) {
    std::vector<OccRow<MaskT>> rows;
    rows.reserve(table.size());
    for(std::uint32_t row = 0; row < table.size(); row += 1) {
        auto mask = mask_of(row);
        if(mask_empty(mask)) {
            continue;
        }
        rows.push_back({table.at(row), std::move(mask)});
    }
    return rows;
}

/// Decode one symbol's relation slice of a blob into working form.
template <typename MaskT, typename MaskOf>
std::vector<RelRow<MaskT>> decode_relation_group(const RelationTable& table,
                                                 std::uint32_t begin_row,
                                                 std::uint32_t end_row,
                                                 MaskOf mask_of) {
    std::vector<RelRow<MaskT>> rows;
    rows.reserve(end_row - begin_row);
    auto cursor = table.cursor_at(begin_row);
    for(auto row = begin_row; row < end_row; row += 1) {
        auto relation = table.decode(row, cursor);
        auto mask = mask_of(row);
        if(mask_empty(mask)) {
            continue;
        }
        rows.push_back({relation, std::move(mask)});
    }
    return rows;
}

/// One blob's relation groups in symbol-hash order, decoded lazily by the
/// group merge.
struct GroupIndex {
    SymbolHash hash;
    std::uint32_t begin_row;
    std::uint32_t end_row;
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
    std::vector<std::uint32_t> long_line_rows;
    std::vector<std::uint32_t> long_line_lengths;
};

ContentInfo content_info_of(BlobView root) {
    ContentInfo info;
    info.hash = root[&ShardBlob::content_hash];
    info.size = root[&ShardBlob::content_size];
    info.content = to_ref(root[&ShardBlob::content]);
    auto lengths = to_array_ref(root[&ShardBlob::line_lengths]);
    auto long_rows = to_array_ref(root[&ShardBlob::long_line_rows]);
    auto long_lengths = to_array_ref(root[&ShardBlob::long_line_lengths]);
    info.line_lengths.assign(lengths.begin(), lengths.end());
    info.long_line_rows.assign(long_rows.begin(), long_rows.end());
    info.long_line_lengths.assign(long_lengths.begin(), long_lengths.end());
    return info;
}

ContentInfo content_info_of(llvm::StringRef content) {
    ContentInfo info;
    info.hash = llvm::xxh3_64bits(content);
    info.size = static_cast<std::uint32_t>(content.size());
    if(!is_ascii(content)) {
        info.content = content;
    }

    auto starts =
        kota::ipc::lsp::build_line_starts(std::string_view(content.data(), content.size()));
    info.line_lengths.reserve(starts.size());
    for(std::size_t i = 0; i < starts.size(); i += 1) {
        auto next = i + 1 < starts.size() ? starts[i + 1] : info.size;
        auto length = next - starts[i];
        if(length >= length_escape) {
            info.line_lengths.push_back(length_escape);
            info.long_line_rows.push_back(static_cast<std::uint32_t>(i));
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
    SymbolHash parent;
    SymbolKind kind;
    SymbolScope scope;
    SymbolFlags flags;
};

LocalSymbol own(const SymbolIdentity& identity) {
    return {
        .name = identity.name.str(),
        .args = identity.args.str(),
        .parent = identity.parent,
        .kind = identity.kind,
        .scope = identity.scope,
        .flags = identity.flags,
    };
}

/// Collect one blob's local symbols; symbols the merged rows no longer
/// reference are filtered at emit time.
void collect_locals(BlobView root, llvm::DenseMap<SymbolHash, LocalSymbol>& locals) {
    auto table = LocalTable::of(root);
    for(std::size_t k = 0; k < table.size(); k += 1) {
        auto identity = table.at(k);
        auto [it, inserted] = locals.try_emplace(table.hash_of(k), own(identity));
        // Variants may see different facts of one symbol (a definition
        // behind `#ifdef`); like the project table, the union is kept.
        if(!inserted) {
            it->second.flags |= identity.flags;
        }
    }
}

/// Appends one side's rows in the tiers chosen for the whole blob: the
/// range tier by content size, the mask tier by variant count.
struct SideWriter {
    RowRanges& side;
    bool narrow;
    MaskTier tier;
    std::uint32_t row = 0;

    template <typename MaskT>
    void add(LocalSourceRange range, const MaskT& mask) {
        add_range(range);
        add_mask(mask);
        row += 1;
    }

    /// The roaring tier's offset column ends with the size of the slices.
    void finish() {
        if(tier == MaskTier::Roaring) {
            side.roaring_offsets.push_back(static_cast<std::uint32_t>(side.roaring.size()));
        }
    }

    void add_range(LocalSourceRange range) {
        if(is_no_range(range.begin, range.end)) {
            // The no-range sentinel of pair relations; the wide columns hold
            // it natively, the packed column spells it as the reserved word.
            if(narrow) {
                side.packed.push_back(packed_sentinel);
            } else {
                side.begins.push_back(range.begin);
                side.lengths.push_back(0);
            }
            return;
        }
        auto length = range.end - range.begin;
        std::uint8_t stored =
            length >= length_escape ? length_escape : static_cast<std::uint8_t>(length);
        if(stored == length_escape) {
            side.long_rows.push_back(row);
            side.long_ends.push_back(range.end);
        }
        if(narrow) {
            side.packed.push_back(pack_range(range.begin, stored));
        } else {
            side.begins.push_back(range.begin);
            side.lengths.push_back(stored);
        }
    }

    template <typename MaskT>
    void add_mask(const MaskT& mask) {
        switch(tier) {
            case MaskTier::Single: {
                break;
            }
            case MaskTier::U32: {
                if constexpr(std::same_as<MaskT, std::uint64_t>) {
                    side.masks32.push_back(static_cast<std::uint32_t>(mask));
                }
                break;
            }
            case MaskTier::U64: {
                if constexpr(std::same_as<MaskT, std::uint64_t>) {
                    side.masks64.push_back(mask);
                }
                break;
            }
            case MaskTier::Roaring: {
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

/// Appends symbol ids in the width chosen for the whole blob from the
/// symbol table's size.
struct SymIdWriter {
    enum class Width : std::uint8_t { U8, U16, U32 };

    Width width;
    std::vector<std::uint8_t>& ids8;
    std::vector<std::uint16_t>& ids16;
    std::vector<std::uint32_t>& ids32;

    static Width width_for(std::size_t table_size) {
        return table_size <= 0x100 ? Width::U8 : table_size <= 0x10000 ? Width::U16 : Width::U32;
    }

    void add(std::uint32_t id) {
        switch(width) {
            case Width::U8: ids8.push_back(static_cast<std::uint8_t>(id)); break;
            case Width::U16: ids16.push_back(static_cast<std::uint16_t>(id)); break;
            case Width::U32: ids32.push_back(id); break;
        }
    }
};

/// Encode merged rows, locals and content into canonical blob bytes. The
/// only entry point that writes a ShardBlob: every self-describing choice
/// (range tier, id width, mask tier, content omission) is made here, from
/// the data, so equal inputs produce equal bytes.
template <typename MaskT>
void emit_blob(const MergedRows<MaskT>& merged,
               const llvm::DenseMap<SymbolHash, LocalSymbol>& locals,
               std::vector<RowsHash> variants,
               const ContentInfo& content,
               llvm::raw_ostream& os) {
    auto referenced = referenced_symbols(merged);

    ShardBlob blob;
    blob.format_version = index_format_version;
    blob.content_hash = content.hash;
    blob.content_size = content.size;
    blob.content = content.content.str();
    blob.line_lengths = content.line_lengths;
    blob.long_line_rows = content.long_line_rows;
    blob.long_line_lengths = content.long_line_lengths;
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
        blob.local_parents.push_back(info->parent);
        blob.local_flags.push_back(static_cast<std::uint16_t>(info->flags));
    }

    auto tier = tier_of(blob.variants.empty() ? 1 : blob.variants.size());
    bool narrow = content.size <= packed_range_limit;
    auto width = SymIdWriter::width_for(blob.sym_hashes.size());

    SideWriter occs{blob.occs, narrow, tier};
    SymIdWriter occ_syms{width, blob.occ_syms8, blob.occ_syms16, blob.occ_syms32};
    for(auto& row: merged.occurrences) {
        occs.add(row.occurrence.range, row.mask);
        occ_syms.add(sym_id(row.occurrence.target));
    }
    occs.finish();

    // Relation groups follow symbol-table order; a symbol with occurrences
    // only gets an empty slice.
    SideWriter rels{blob.rels, narrow, tier};
    SymIdWriter rel_syms{width, blob.rel_sym8, blob.rel_sym16, blob.rel_sym32};
    blob.sym_rel_offsets.reserve(blob.sym_hashes.size() + 1);
    auto group = merged.relations.begin();
    for(auto hash: blob.sym_hashes) {
        blob.sym_rel_offsets.push_back(rels.row);
        if(group == merged.relations.end() || group->symbol != hash) {
            continue;
        }
        for(auto& row: group->rows) {
            auto& relation = row.relation;
            blob.rel_kinds.push_back(static_cast<std::uint8_t>(relation.kind));
            if(relation.target_symbol != 0) {
                if(RelationKind(relation.kind).isDeclOrDef()) {
                    auto range = std::bit_cast<LocalSourceRange>(relation.target_symbol);
                    blob.rel_def_rows.push_back(rels.row);
                    blob.rel_def_begins.push_back(range.begin);
                    blob.rel_def_ends.push_back(range.end);
                } else {
                    blob.rel_sym_rows.push_back(rels.row);
                    rel_syms.add(sym_id(relation.target_symbol));
                }
            }
            rels.add(relation.range, row.mask);
        }
        group += 1;
    }
    blob.sym_rel_offsets.push_back(rels.row);
    rels.finish();

    serialize_blob(blob, os);
}

template <typename MaskT>
void merge_shards_impl(BlobView old_root,
                       llvm::ArrayRef<std::int64_t> id_map,
                       llvm::ArrayRef<Shard> fresh,
                       std::uint32_t fresh_base,
                       std::vector<RowsHash> variants,
                       const ContentInfo& content,
                       llvm::raw_ostream& os) {
    MergedRows<MaskT> merged;

    // Occurrences: concatenate every fresh blob's rows (each stamped with
    // its new bit), sort, combine equal rows, then merge with the old
    // rows. Fresh runs are individually sorted already; one sort over the
    // concatenation keeps the merge two-way.
    std::vector<OccRow<MaskT>> fresh_occs;
    for(std::uint32_t i = 0; i < fresh.size(); i += 1) {
        auto bit = single_bit<MaskT>(fresh_base + i);
        auto rows = decode_occurrences<MaskT>(OccurrenceTable::of(view_of(fresh[i].bytes())),
                                              [&](std::uint32_t) { return bit; });
        fresh_occs.insert(fresh_occs.end(),
                          std::make_move_iterator(rows.begin()),
                          std::make_move_iterator(rows.end()));
    }
    canonicalize(fresh_occs, occ_key);

    std::vector<OccRow<MaskT>> old_occs;
    if(old_root.valid()) {
        auto table = OccurrenceTable::of(old_root);
        auto tier = tier_of(old_root);
        old_occs = decode_occurrences<MaskT>(table, [&](std::uint32_t row) {
            return remap_mask<MaskT>(tier, table.rows, row, id_map);
        });
    }
    merge_sorted(std::move(old_occs), std::move(fresh_occs), occ_key, merged.occurrences);

    // Relations: gather fresh groups per symbol across all fresh blobs,
    // then two-way merge with the old blob's groups in hash order.
    llvm::DenseMap<SymbolHash, std::vector<RelRow<MaskT>>> fresh_group_map;
    for(std::uint32_t i = 0; i < fresh.size(); i += 1) {
        auto table = RelationTable::of(view_of(fresh[i].bytes()));
        auto bit = single_bit<MaskT>(fresh_base + i);
        for(auto& group: relation_groups(table)) {
            auto rows = decode_relation_group<MaskT>(table,
                                                     group.begin_row,
                                                     group.end_row,
                                                     [&](std::uint32_t) { return bit; });
            auto& into = fresh_group_map[group.hash];
            into.insert(into.end(),
                        std::make_move_iterator(rows.begin()),
                        std::make_move_iterator(rows.end()));
        }
    }
    std::vector<RelGroup<MaskT>> fresh_groups;
    fresh_groups.reserve(fresh_group_map.size());
    for(auto& [hash, rows]: fresh_group_map) {
        canonicalize(rows, rel_key);
        fresh_groups.push_back({hash, std::move(rows)});
    }
    std::ranges::sort(fresh_groups, {}, group_key);

    std::vector<GroupIndex> old_groups;
    RelationTable old_table;
    MaskTier old_tier = MaskTier::Single;
    if(old_root.valid()) {
        old_table = RelationTable::of(old_root);
        old_tier = tier_of(old_root);
        old_groups = relation_groups(old_table);
    }
    auto decode_old = [&](const GroupIndex& group) {
        return decode_relation_group<MaskT>(
            old_table,
            group.begin_row,
            group.end_row,
            [&](std::uint32_t row) {
                return remap_mask<MaskT>(old_tier, old_table.rows, row, id_map);
            });
    };

    auto lhs = old_groups.begin();
    auto rhs = fresh_groups.begin();
    while(lhs != old_groups.end() || rhs != fresh_groups.end()) {
        if(rhs == fresh_groups.end() || (lhs != old_groups.end() && lhs->hash < rhs->symbol)) {
            auto rows = decode_old(*lhs);
            if(!rows.empty()) {
                merged.relations.push_back({lhs->hash, std::move(rows)});
            }
            lhs += 1;
        } else if(lhs == old_groups.end() || rhs->symbol < lhs->hash) {
            merged.relations.push_back(std::move(*rhs));
            rhs += 1;
        } else {
            std::vector<RelRow<MaskT>> combined;
            merge_sorted(decode_old(*lhs), std::move(rhs->rows), rel_key, combined);
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

void write_shard(const FileIndex& rows,
                 llvm::function_ref<std::optional<SymbolIdentity>(SymbolHash)> symbols,
                 llvm::StringRef content,
                 llvm::raw_ostream& os) {
    // Canonicalize straight from the in-memory rows: sorted, deduplicated.
    // MaskT is irrelevant for a single variant (no mask columns) — use the
    // cheap one.
    MergedRows<std::uint64_t> merged;
    merged.occurrences.reserve(rows.occurrences.size());
    for(auto& occurrence: rows.occurrences) {
        merged.occurrences.push_back({occurrence, 1});
    }
    canonicalize(merged.occurrences, occ_key);

    merged.relations.reserve(rows.relations.size());
    for(auto& [hash, relations]: rows.relations) {
        std::vector<RelRow<std::uint64_t>> group;
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
                  llvm::ArrayRef<RowsHash> keep,
                  llvm::ArrayRef<Shard> fresh,
                  llvm::raw_ostream& os) {
    auto old_variants = old.variants();

    // old-id -> new-id; -1 drops the variant.
    llvm::SmallVector<std::int64_t> id_map(old_variants.size(), -1);
    std::vector<RowsHash> variants;
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
    // must never share row storage, hence the assert.
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
