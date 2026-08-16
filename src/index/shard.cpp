#include "index/shard.h"

#include <algorithm>
#include <bit>
#include <cassert>
#include <tuple>
#include <utility>

#include "index/serialization.h"

#include "kota/ipc/lsp/position.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/xxhash.h"

namespace clice::index {

namespace {

using ShardView = kota::codec::fbs::table_view<ShardBlob>;

/// Sentinel in the length column: the real end lives in the sparse
/// (row, end) escape table.
constexpr std::uint8_t length_escape = 0xff;

enum class MaskTier : std::uint8_t {
    /// One variant: no mask columns at all.
    Single,
    U32,
    U64,
    Roaring,
};

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
ShardView root_of(const llvm::MemoryBuffer& buffer) {
    return ShardView::from_verified_bytes(blob_bytes(buffer.getBuffer()));
}

/// One side of the blob's row storage (occurrences or relations) as
/// contiguous column refs.
struct RowColumns {
    llvm::ArrayRef<std::uint32_t> begins;
    llvm::ArrayRef<std::uint8_t> lengths;
    llvm::ArrayRef<std::uint32_t> long_rows;
    llvm::ArrayRef<std::uint32_t> long_ends;
    llvm::ArrayRef<std::uint32_t> masks32;
    llvm::ArrayRef<std::uint64_t> masks64;
    llvm::ArrayRef<std::uint32_t> roaring_offsets;
    llvm::ArrayRef<std::uint8_t> roaring;

    std::uint32_t end_of(std::uint32_t row) const {
        auto length = lengths[row];
        if(length == length_escape) {
            // validate() proves every sentinel owns exactly one escape
            // entry, so the search always lands.
            auto it = std::ranges::lower_bound(long_rows, row);
            return long_ends[it - long_rows.begin()];
        }
        return begins[row] + length;
    }
};

RowColumns occ_columns(ShardView root) {
    return {
        to_array_ref(root[&ShardBlob::occ_begins]),
        to_array_ref(root[&ShardBlob::occ_lengths]),
        to_array_ref(root[&ShardBlob::occ_long_rows]),
        to_array_ref(root[&ShardBlob::occ_long_ends]),
        to_array_ref(root[&ShardBlob::occ_masks32]),
        to_array_ref(root[&ShardBlob::occ_masks64]),
        to_array_ref(root[&ShardBlob::occ_roaring_offsets]),
        to_array_ref(root[&ShardBlob::occ_roaring]),
    };
}

RowColumns rel_columns(ShardView root) {
    return {
        to_array_ref(root[&ShardBlob::rel_begins]),
        to_array_ref(root[&ShardBlob::rel_lengths]),
        to_array_ref(root[&ShardBlob::rel_long_rows]),
        to_array_ref(root[&ShardBlob::rel_long_ends]),
        to_array_ref(root[&ShardBlob::rel_masks32]),
        to_array_ref(root[&ShardBlob::rel_masks64]),
        to_array_ref(root[&ShardBlob::rel_roaring_offsets]),
        to_array_ref(root[&ShardBlob::rel_roaring]),
    };
}

/// The slice bounds were validated monotonic and in-bounds at load, and
/// every slice proven to decode, so this cannot fail.
Bitmap read_row_bitmap(const RowColumns& columns, std::uint32_t row) {
    auto begin = columns.roaring_offsets[row];
    return *read_bitmap(columns.roaring.data() + begin, columns.roaring_offsets[row + 1] - begin);
}

/// Structural verification does not constrain field values; everything the
/// readers dereference through raw column pointers or binary-search must be
/// proven in-bounds and in order here, once, so queries stay check-free.
bool validate(ShardView root) {
    auto variants = to_array_ref(root[&ShardBlob::variants]);
    auto sym_hashes = to_array_ref(root[&ShardBlob::sym_hashes]);
    auto offsets = to_array_ref(root[&ShardBlob::sym_rel_offsets]);

    if(variants.empty()) {
        return false;
    }
    // A variant is identified by its rows hash everywhere (set_live,
    // write_shard's keep filter), so hashes must be unique: rows owned only
    // by a duplicated entry would serve and survive compaction with no
    // contribution owning them.
    llvm::SmallVector<RowsHash> sorted_variants(variants.begin(), variants.end());
    std::ranges::sort(sorted_variants);
    if(std::ranges::adjacent_find(sorted_variants) != sorted_variants.end()) {
        return false;
    }

    // Every freshness decision compares the advertised content hash
    // (manifest FileVersions, the merge's generation checks), so content
    // bytes corrupted under an intact structure would keep loading as fresh
    // while position mapping reads text the rows were not built from.
    if(llvm::xxh3_64bits(to_ref(root[&ShardBlob::content])) != root[&ShardBlob::content_hash]) {
        return false;
    }

    auto occ = occ_columns(root);
    auto rel = rel_columns(root);
    auto occ_count = occ.begins.size();
    auto rel_count = rel.begins.size();

    if(offsets.size() != sym_hashes.size() + 1) {
        return false;
    }
    if(!std::ranges::is_sorted(offsets) || offsets.back() != rel_count) {
        return false;
    }
    // Strictly: symbol lookups lower-bound the hash column and read only
    // the first match's slices, so a duplicated hash would strand the later
    // id's relations and local name unreachably.
    if(!std::ranges::is_sorted(sym_hashes, std::less_equal{})) {
        return false;
    }

    auto sym_ids_ok = [&](auto ids) {
        return llvm::all_of(ids, [&](std::uint32_t id) { return id < sym_hashes.size(); });
    };
    auto occ_syms16 = to_array_ref(root[&ShardBlob::occ_syms16]);
    auto occ_syms32 = to_array_ref(root[&ShardBlob::occ_syms32]);
    if(occ.lengths.size() != occ_count) {
        return false;
    }
    if(occ_syms16.size() + occ_syms32.size() != occ_count ||
       (!occ_syms16.empty() && !occ_syms32.empty())) {
        return false;
    }
    if(!sym_ids_ok(occ_syms16) || !sym_ids_ok(occ_syms32)) {
        return false;
    }

    auto rel_kinds = to_array_ref(root[&ShardBlob::rel_kinds]);
    if(rel_kinds.size() != rel_count || rel.lengths.size() != rel_count) {
        return false;
    }

    auto sparse_ok = [](llvm::ArrayRef<std::uint32_t> rows, std::size_t values, std::size_t count) {
        return rows.size() == values && std::ranges::is_sorted(rows, std::less_equal{}) &&
               (rows.empty() || rows.back() < count);
    };
    // The escape table must pair one-to-one, in row order, with the
    // sentinel lengths: end_of trusts the pairing, and a sentinel missing
    // its entry (or a stray entry masking one elsewhere) can pass every
    // range bound below while serving a wrong end forever.
    auto escapes_ok = [](llvm::ArrayRef<std::uint8_t> lengths,
                         llvm::ArrayRef<std::uint32_t> long_rows,
                         llvm::ArrayRef<std::uint32_t> long_ends) {
        if(long_ends.size() != long_rows.size()) {
            return false;
        }
        std::size_t cursor = 0;
        for(std::uint32_t row = 0; row < lengths.size(); row += 1) {
            if(lengths[row] != length_escape) {
                continue;
            }
            if(cursor == long_rows.size() || long_rows[cursor] != row) {
                return false;
            }
            cursor += 1;
        }
        return cursor == long_rows.size();
    };
    if(!escapes_ok(occ.lengths, occ.long_rows, occ.long_ends)) {
        return false;
    }
    // lookup(offset) binary-searches the decoded end column and stops its
    // containment walk on begin order; rows out of either order (a corrupt
    // escaped end included) would silently miss or misresolve occurrences
    // on every query, forever — reject the blob so it is rebuilt instead.
    // Ends are bounded by the stored content too: every decoded range is
    // served as a source range into it.
    auto content_size = root[&ShardBlob::content].size();
    std::uint32_t prev_begin = 0;
    std::uint32_t prev_end = 0;
    for(std::uint32_t row = 0; row < occ_count; row += 1) {
        auto begin = occ.begins[row];
        auto end = occ.end_of(row);
        if(begin < prev_begin || end < prev_end || end < begin || end > content_size) {
            return false;
        }
        prev_begin = begin;
        prev_end = end;
    }
    if(!escapes_ok(rel.lengths, rel.long_rows, rel.long_ends)) {
        return false;
    }
    // Relation ranges carry no query order to enforce, but are served as
    // source ranges all the same — bound them like the occurrence ends.
    // The exception is the default LocalSourceRange, the writer's sentinel
    // for pair relations, which carry no range of their own; every other
    // kind is written with a real range, so a sentinel there is corruption
    // that would serve an invalid source range forever.
    for(std::uint32_t row = 0; row < rel_count; row += 1) {
        auto begin = rel.begins[row];
        auto end = rel.end_of(row);
        if((LocalSourceRange{begin, end}) == LocalSourceRange{}) {
            if(!RelationKind(static_cast<RelationKind::Kind>(rel_kinds[row])).isBetweenSymbol()) {
                return false;
            }
            continue;
        }
        if(end < begin || end > content_size) {
            return false;
        }
    }

    auto rel_sym_rows = to_array_ref(root[&ShardBlob::rel_sym_rows]);
    auto rel_sym16 = to_array_ref(root[&ShardBlob::rel_sym16]);
    auto rel_sym32 = to_array_ref(root[&ShardBlob::rel_sym32]);
    if(!sparse_ok(rel_sym_rows, rel_sym16.size() + rel_sym32.size(), rel_count) ||
       (!rel_sym16.empty() && !rel_sym32.empty())) {
        return false;
    }
    if(!sym_ids_ok(rel_sym16) || !sym_ids_ok(rel_sym32)) {
        return false;
    }
    auto rel_def_rows = to_array_ref(root[&ShardBlob::rel_def_rows]);
    auto rel_def_begins = to_array_ref(root[&ShardBlob::rel_def_begins]);
    auto rel_def_ends = to_array_ref(root[&ShardBlob::rel_def_ends]);
    if(!sparse_ok(rel_def_rows, rel_def_begins.size(), rel_count) ||
       rel_def_ends.size() != rel_def_begins.size()) {
        return false;
    }
    for(std::uint32_t k = 0; k < rel_def_begins.size(); k += 1) {
        if(rel_def_ends[k] < rel_def_begins[k] || rel_def_ends[k] > content_size) {
            return false;
        }
    }

    auto local_syms = to_array_ref(root[&ShardBlob::local_syms]);
    auto local_kinds = to_array_ref(root[&ShardBlob::local_kinds]);
    auto local_scopes = to_array_ref(root[&ShardBlob::local_scopes]);
    if(!sparse_ok(local_syms, local_kinds.size(), sym_hashes.size()) ||
       local_scopes.size() != local_kinds.size() ||
       root[&ShardBlob::local_names].size() != local_kinds.size()) {
        return false;
    }

    // Beyond the per-tier column shape, every mask must own at least one
    // stored variant and no bits past the variant table: an ownerless row
    // serves unconditionally while every stored variant is live (row_live's
    // live.all fast path skips the mask), vanishes once any variant dies,
    // and the next compaction erases it for real — every manifest still
    // fresh throughout.
    auto masks_ok = [&](const RowColumns& columns, std::size_t count) {
        switch(tier_of(variants.size())) {
            case MaskTier::Single: {
                return columns.masks32.empty() && columns.masks64.empty() &&
                       columns.roaring_offsets.empty() && columns.roaring.empty();
            }
            case MaskTier::U32: {
                if(columns.masks32.size() != count || !columns.masks64.empty() ||
                   !columns.roaring_offsets.empty()) {
                    return false;
                }
                auto stray = variants.size() < 32 ? ~std::uint32_t(0) << variants.size() : 0;
                return llvm::all_of(columns.masks32, [&](std::uint32_t mask) {
                    return mask != 0 && (mask & stray) == 0;
                });
            }
            case MaskTier::U64: {
                if(columns.masks64.size() != count || !columns.masks32.empty() ||
                   !columns.roaring_offsets.empty()) {
                    return false;
                }
                auto stray = variants.size() < 64 ? ~std::uint64_t(0) << variants.size() : 0;
                return llvm::all_of(columns.masks64, [&](std::uint64_t mask) {
                    return mask != 0 && (mask & stray) == 0;
                });
            }
            case MaskTier::Roaring: {
                if(!columns.masks32.empty() || !columns.masks64.empty() ||
                   columns.roaring_offsets.size() != count + 1 ||
                   !std::ranges::is_sorted(columns.roaring_offsets) ||
                   columns.roaring_offsets.back() != columns.roaring.size() ||
                   (count != 0 && columns.roaring_offsets.front() != 0)) {
                    return false;
                }
                // A slice failing decode would read as an empty mask — the
                // ownerless-row corruption above in another coat. Prove
                // each slice decodes once here so queries stay check-free
                // and the blob rebuilds instead.
                for(std::uint32_t row = 0; row < count; row += 1) {
                    auto begin = columns.roaring_offsets[row];
                    auto mask = read_bitmap(columns.roaring.data() + begin,
                                            columns.roaring_offsets[row + 1] - begin);
                    if(!mask || mask->isEmpty() || mask->maximum() >= variants.size()) {
                        return false;
                    }
                }
                return true;
            }
        }
        std::unreachable();
    };
    return masks_ok(occ, occ_count) && masks_ok(rel, rel_count);
}

std::uint32_t occ_sym_id(ShardView root, std::uint32_t row) {
    auto syms16 = to_array_ref(root[&ShardBlob::occ_syms16]);
    if(!syms16.empty()) {
        return syms16[row];
    }
    return to_array_ref(root[&ShardBlob::occ_syms32])[row];
}

}  // namespace

Shard::Shard(std::unique_ptr<llvm::MemoryBuffer> buffer) : buffer(std::move(buffer)) {}

Shard Shard::from_bytes(llvm::StringRef data) {
    return from_buffer(llvm::MemoryBuffer::getMemBuffer(data, "", false));
}

Shard Shard::from_buffer(std::unique_ptr<llvm::MemoryBuffer> buffer) {
    if(!buffer) {
        return {};
    }

    // Stale or corrupt bytes (an older build's cache directory) must never
    // crash the server or be misread: deep structural verification first,
    // then the format-version gate, then the cross-field size checks the
    // raw column readers rely on. Anything failing loads as "not on disk"
    // and the background indexer rebuilds it.
    auto root = ShardView::from_bytes(blob_bytes(buffer->getBuffer()));
    if(!root.valid() || root[&ShardBlob::format_version] != index_format_version ||
       !validate(root)) {
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

std::vector<RowsHash> Shard::variants() const {
    if(!buffer) {
        return {};
    }
    auto stored = to_array_ref(root_of(*buffer)[&ShardBlob::variants]);
    return {stored.begin(), stored.end()};
}

bool Shard::has_variant(RowsHash hash) const {
    if(!buffer) {
        return false;
    }
    return llvm::is_contained(to_array_ref(root_of(*buffer)[&ShardBlob::variants]), hash);
}

void Shard::set_live(llvm::ArrayRef<RowsHash> live_hashes) {
    live = {};
    if(!buffer) {
        return;
    }

    auto stored = to_array_ref(root_of(*buffer)[&ShardBlob::variants]);
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

bool Shard::row_live(bool occurrence, std::uint32_t row) const {
    if(live.all) {
        return true;
    }
    auto root = root_of(*buffer);
    auto columns = occurrence ? occ_columns(root) : rel_columns(root);
    switch(tier_of(to_array_ref(root[&ShardBlob::variants]).size())) {
        case MaskTier::Single: {
            return (live.bits & 1) != 0;
        }
        case MaskTier::U32: {
            return (columns.masks32[row] & static_cast<std::uint32_t>(live.bits)) != 0;
        }
        case MaskTier::U64: {
            return (columns.masks64[row] & live.bits) != 0;
        }
        case MaskTier::Roaring: {
            return read_row_bitmap(columns, row).intersect(live.big);
        }
    }
    std::unreachable();
}

void Shard::lookup(std::uint32_t offset,
                   llvm::function_ref<bool(const Occurrence&)> callback) const {
    if(!buffer) {
        return;
    }
    auto root = root_of(*buffer);
    auto columns = occ_columns(root);
    auto sym_hashes = to_array_ref(root[&ShardBlob::sym_hashes]);

    // Binary search the first row whose end reaches the offset, then walk
    // while rows contain it. Occurrence ranges are name-token spans,
    // pairwise disjoint or identical, so under (begin, end) order the end
    // column is monotonic too.
    std::size_t lo = 0;
    std::size_t hi = columns.begins.size();
    while(lo < hi) {
        auto mid = lo + (hi - lo) / 2;
        if(columns.end_of(mid) < offset) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }

    for(; lo < columns.begins.size(); lo += 1) {
        auto row = static_cast<std::uint32_t>(lo);
        LocalSourceRange range{columns.begins[row], columns.end_of(row)};
        if(!range.contains(offset)) {
            break;
        }
        if(!row_live(true, row)) {
            continue;
        }
        Occurrence result{range, sym_hashes[occ_sym_id(root, row)]};
        if(!callback(result)) {
            break;
        }
    }
}

void Shard::lookup(SymbolHash symbol,
                   RelationKind kind,
                   llvm::function_ref<bool(const Relation&)> callback) const {
    if(!buffer) {
        return;
    }
    auto root = root_of(*buffer);
    auto sym_hashes = to_array_ref(root[&ShardBlob::sym_hashes]);
    auto it = std::ranges::lower_bound(sym_hashes, symbol);
    if(it == sym_hashes.end() || *it != symbol) [[unlikely]] {
        return;
    }
    auto id = static_cast<std::uint32_t>(it - sym_hashes.begin());

    auto offsets = to_array_ref(root[&ShardBlob::sym_rel_offsets]);
    auto begin_row = offsets[id];
    auto end_row = offsets[id + 1];

    auto columns = rel_columns(root);
    auto kinds = to_array_ref(root[&ShardBlob::rel_kinds]);
    auto sym_rows = to_array_ref(root[&ShardBlob::rel_sym_rows]);
    auto sym16 = to_array_ref(root[&ShardBlob::rel_sym16]);
    auto sym32 = to_array_ref(root[&ShardBlob::rel_sym32]);
    auto def_rows = to_array_ref(root[&ShardBlob::rel_def_rows]);
    auto def_begins = to_array_ref(root[&ShardBlob::rel_def_begins]);
    auto def_ends = to_array_ref(root[&ShardBlob::rel_def_ends]);

    auto sym_cursor = std::ranges::lower_bound(sym_rows, begin_row) - sym_rows.begin();
    auto def_cursor = std::ranges::lower_bound(def_rows, begin_row) - def_rows.begin();

    for(auto row = begin_row; row < end_row; row += 1) {
        while(sym_cursor < static_cast<std::ptrdiff_t>(sym_rows.size()) &&
              sym_rows[sym_cursor] < row) {
            sym_cursor += 1;
        }
        while(def_cursor < static_cast<std::ptrdiff_t>(def_rows.size()) &&
              def_rows[def_cursor] < row) {
            def_cursor += 1;
        }

        auto row_kind = static_cast<RelationKind::Kind>(kinds[row]);
        if(!(RelationKind(row_kind) & kind)) {
            continue;
        }
        if(!row_live(false, row)) {
            continue;
        }

        Relation relation{
            .kind = row_kind,
            .range = {columns.begins[row], columns.end_of(row)},
            .target_symbol = 0,
        };
        if(def_cursor < static_cast<std::ptrdiff_t>(def_rows.size()) &&
           def_rows[def_cursor] == row) {
            relation.set_definition_range({def_begins[def_cursor], def_ends[def_cursor]});
        } else if(sym_cursor < static_cast<std::ptrdiff_t>(sym_rows.size()) &&
                  sym_rows[sym_cursor] == row) {
            auto payload = sym16.empty() ? sym32[sym_cursor] : sym16[sym_cursor];
            relation.target_symbol = sym_hashes[payload];
        }

        if(!callback(relation)) {
            break;
        }
    }
}

bool Shard::find_symbol(SymbolHash hash, std::string& name, SymbolKind& kind) const {
    if(!buffer) {
        return false;
    }
    auto root = root_of(*buffer);
    auto sym_hashes = to_array_ref(root[&ShardBlob::sym_hashes]);
    auto it = std::ranges::lower_bound(sym_hashes, hash);
    if(it == sym_hashes.end() || *it != hash) {
        return false;
    }
    auto id = static_cast<std::uint32_t>(it - sym_hashes.begin());

    auto local_syms = to_array_ref(root[&ShardBlob::local_syms]);
    auto local_it = std::ranges::lower_bound(local_syms, id);
    if(local_it == local_syms.end() || *local_it != id) {
        return false;
    }
    auto local = local_it - local_syms.begin();
    name = std::string(root[&ShardBlob::local_names].at(local));
    kind = SymbolKind(to_array_ref(root[&ShardBlob::local_kinds])[local]);
    return true;
}

llvm::StringRef Shard::content() const {
    if(!buffer) {
        return {};
    }
    return to_ref(root_of(*buffer)[&ShardBlob::content]);
}

std::span<const std::uint32_t> Shard::line_starts() const {
    if(!buffer) {
        return {};
    }
    if(line_starts_cache.empty()) {
        line_starts_cache = kota::ipc::lsp::build_line_starts(content());
    }
    return line_starts_cache;
}

namespace {

/// Working row forms during a write; masks stay wide, the tier is chosen
/// at emit time from the final variant count.
template <typename MaskT>
struct OccRow {
    std::uint32_t begin;
    std::uint32_t end;
    std::uint64_t sym;
    MaskT mask;
};

template <typename MaskT>
struct RelRow {
    std::uint8_t kind;
    std::uint32_t begin;
    std::uint32_t end;
    /// Raw Relation::target_symbol bits; whether they mean a definition
    /// range, a symbol hash or nothing is decided by `kind` (mirroring the
    /// in-memory encoding).
    std::uint64_t payload;
    MaskT mask;
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

template <typename MaskT>
void mask_or(MaskT& into, const MaskT& from) {
    into |= from;
}

/// The old blob's mask of one row, remapped through old-id -> new-id (a
/// dropped variant's bit vanishes; an all-dropped row reads as empty and
/// is skipped by the caller).
template <typename MaskT>
MaskT remap_mask(ShardView root,
                 const RowColumns& columns,
                 std::uint32_t row,
                 llvm::ArrayRef<std::int64_t> id_map) {
    MaskT result{};
    auto apply = [&](std::uint32_t old_id) {
        if(id_map[old_id] >= 0) {
            mask_or(result, single_bit<MaskT>(static_cast<std::uint32_t>(id_map[old_id])));
        }
    };
    switch(tier_of(to_array_ref(root[&ShardBlob::variants]).size())) {
        case MaskTier::Single: {
            apply(0);
            break;
        }
        case MaskTier::U32: {
            auto bits = columns.masks32[row];
            while(bits != 0) {
                auto id = static_cast<std::uint32_t>(std::countr_zero(bits));
                apply(id);
                bits &= bits - 1;
            }
            break;
        }
        case MaskTier::U64: {
            auto bits = columns.masks64[row];
            while(bits != 0) {
                auto id = static_cast<std::uint32_t>(std::countr_zero(bits));
                apply(id);
                bits &= bits - 1;
            }
            break;
        }
        case MaskTier::Roaring: {
            for(auto id: read_row_bitmap(columns, row)) {
                apply(id);
            }
            break;
        }
    }
    return result;
}

/// Everything write_shard accumulates before choosing column tiers.
template <typename MaskT>
struct MergedRows {
    std::vector<OccRow<MaskT>> occurrences;
    /// Groups sorted by symbol hash; rows sorted by (kind, begin, end,
    /// payload) within each.
    std::vector<std::pair<std::uint64_t, std::vector<RelRow<MaskT>>>> relations;
};

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
            mask_or(lhs->mask, rhs->mask);
            out.push_back(std::move(*lhs));
            lhs += 1;
            rhs += 1;
        }
    }
}

template <typename MaskT>
void merge_occurrences(ShardView old_root,
                       llvm::ArrayRef<std::int64_t> id_map,
                       const VariantInput& fresh,
                       std::int64_t fresh_id,
                       std::vector<OccRow<MaskT>>& out) {
    std::vector<OccRow<MaskT>> old_rows;
    if(old_root.valid()) {
        auto columns = occ_columns(old_root);
        auto sym_hashes = to_array_ref(old_root[&ShardBlob::sym_hashes]);
        old_rows.reserve(columns.begins.size());
        for(std::uint32_t row = 0; row < columns.begins.size(); row += 1) {
            auto mask = remap_mask<MaskT>(old_root, columns, row, id_map);
            if(mask_empty(mask)) {
                continue;
            }
            old_rows.push_back({columns.begins[row],
                                columns.end_of(row),
                                sym_hashes[occ_sym_id(old_root, row)],
                                std::move(mask)});
        }
    }

    std::vector<OccRow<MaskT>> fresh_rows;
    if(fresh.rows) {
        fresh_rows.reserve(fresh.rows->occurrences.size());
        auto bit = single_bit<MaskT>(static_cast<std::uint32_t>(fresh_id));
        for(auto& occurrence: fresh.rows->occurrences) {
            fresh_rows.push_back(
                {occurrence.range.begin, occurrence.range.end, occurrence.target, bit});
        }
        std::ranges::sort(fresh_rows, [](const auto& lhs, const auto& rhs) {
            return std::tuple(lhs.begin, lhs.end, lhs.sym) <
                   std::tuple(rhs.begin, rhs.end, rhs.sym);
        });
    }

    merge_sorted(
        std::move(old_rows),
        std::move(fresh_rows),
        [](const auto& row) { return std::tuple(row.begin, row.end, row.sym); },
        out);
}

template <typename MaskT>
std::vector<RelRow<MaskT>> decode_relation_group(ShardView root,
                                                 const RowColumns& columns,
                                                 std::uint32_t begin_row,
                                                 std::uint32_t end_row,
                                                 llvm::ArrayRef<std::int64_t> id_map) {
    auto sym_hashes = to_array_ref(root[&ShardBlob::sym_hashes]);
    auto kinds = to_array_ref(root[&ShardBlob::rel_kinds]);
    auto sym_rows = to_array_ref(root[&ShardBlob::rel_sym_rows]);
    auto sym16 = to_array_ref(root[&ShardBlob::rel_sym16]);
    auto sym32 = to_array_ref(root[&ShardBlob::rel_sym32]);
    auto def_rows = to_array_ref(root[&ShardBlob::rel_def_rows]);
    auto def_begins = to_array_ref(root[&ShardBlob::rel_def_begins]);
    auto def_ends = to_array_ref(root[&ShardBlob::rel_def_ends]);

    auto sym_cursor = std::ranges::lower_bound(sym_rows, begin_row) - sym_rows.begin();
    auto def_cursor = std::ranges::lower_bound(def_rows, begin_row) - def_rows.begin();

    std::vector<RelRow<MaskT>> rows;
    rows.reserve(end_row - begin_row);
    for(auto row = begin_row; row < end_row; row += 1) {
        while(sym_cursor < static_cast<std::ptrdiff_t>(sym_rows.size()) &&
              sym_rows[sym_cursor] < row) {
            sym_cursor += 1;
        }
        while(def_cursor < static_cast<std::ptrdiff_t>(def_rows.size()) &&
              def_rows[def_cursor] < row) {
            def_cursor += 1;
        }

        auto mask = remap_mask<MaskT>(root, columns, row, id_map);
        if(mask_empty(mask)) {
            continue;
        }

        std::uint64_t payload = 0;
        if(def_cursor < static_cast<std::ptrdiff_t>(def_rows.size()) &&
           def_rows[def_cursor] == row) {
            payload = std::bit_cast<std::uint64_t>(
                LocalSourceRange{def_begins[def_cursor], def_ends[def_cursor]});
        } else if(sym_cursor < static_cast<std::ptrdiff_t>(sym_rows.size()) &&
                  sym_rows[sym_cursor] == row) {
            auto id = sym16.empty() ? sym32[sym_cursor] : sym16[sym_cursor];
            payload = sym_hashes[id];
        }

        rows.push_back(
            {kinds[row], columns.begins[row], columns.end_of(row), payload, std::move(mask)});
    }
    return rows;
}

template <typename MaskT>
void merge_relation_rows(std::vector<RelRow<MaskT>> old_rows,
                         std::vector<RelRow<MaskT>> fresh_rows,
                         std::vector<RelRow<MaskT>>& out) {
    merge_sorted(
        std::move(old_rows),
        std::move(fresh_rows),
        [](const auto& row) { return std::tuple(row.kind, row.begin, row.end, row.payload); },
        out);
}

template <typename MaskT>
void merge_relations(ShardView old_root,
                     llvm::ArrayRef<std::int64_t> id_map,
                     const VariantInput& fresh,
                     std::int64_t fresh_id,
                     std::vector<std::pair<std::uint64_t, std::vector<RelRow<MaskT>>>>& out) {
    // Fresh groups, sorted by symbol hash; rows in a group follow the
    // builder's canonical (kind, begin, end, payload) order already, but a
    // hand-built FileIndex (tests) may not — sort defensively, it is cheap
    // relative to the merge.
    std::vector<std::pair<std::uint64_t, std::vector<RelRow<MaskT>>>> fresh_groups;
    if(fresh.rows) {
        auto bit = single_bit<MaskT>(static_cast<std::uint32_t>(fresh_id));
        fresh_groups.reserve(fresh.rows->relations.size());
        for(auto& [hash, relations]: fresh.rows->relations) {
            std::vector<RelRow<MaskT>> rows;
            rows.reserve(relations.size());
            for(auto& relation: relations) {
                rows.push_back({static_cast<std::uint8_t>(relation.kind),
                                relation.range.begin,
                                relation.range.end,
                                relation.target_symbol,
                                bit});
            }
            std::ranges::sort(rows, [](const auto& lhs, const auto& rhs) {
                return std::tuple(lhs.kind, lhs.begin, lhs.end, lhs.payload) <
                       std::tuple(rhs.kind, rhs.begin, rhs.end, rhs.payload);
            });
            fresh_groups.emplace_back(hash, std::move(rows));
        }
        std::ranges::sort(fresh_groups, {}, [](const auto& group) { return group.first; });
    }

    struct OldGroup {
        std::uint64_t hash;
        std::uint32_t begin_row;
        std::uint32_t end_row;
    };

    std::vector<OldGroup> old_groups;
    RowColumns old_columns;
    if(old_root.valid()) {
        old_columns = rel_columns(old_root);
        auto sym_hashes = to_array_ref(old_root[&ShardBlob::sym_hashes]);
        auto offsets = to_array_ref(old_root[&ShardBlob::sym_rel_offsets]);
        for(std::uint32_t id = 0; id < sym_hashes.size(); id += 1) {
            if(offsets[id] != offsets[id + 1]) {
                old_groups.push_back({sym_hashes[id], offsets[id], offsets[id + 1]});
            }
        }
    }

    auto lhs = old_groups.begin();
    auto rhs = fresh_groups.begin();
    while(lhs != old_groups.end() || rhs != fresh_groups.end()) {
        if(rhs == fresh_groups.end() || (lhs != old_groups.end() && lhs->hash < rhs->first)) {
            auto rows = decode_relation_group<MaskT>(old_root,
                                                     old_columns,
                                                     lhs->begin_row,
                                                     lhs->end_row,
                                                     id_map);
            if(!rows.empty()) {
                out.emplace_back(lhs->hash, std::move(rows));
            }
            lhs += 1;
        } else if(lhs == old_groups.end() || rhs->first < lhs->hash) {
            out.emplace_back(rhs->first, std::move(rhs->second));
            rhs += 1;
        } else {
            auto old_rows = decode_relation_group<MaskT>(old_root,
                                                         old_columns,
                                                         lhs->begin_row,
                                                         lhs->end_row,
                                                         id_map);
            std::vector<RelRow<MaskT>> merged;
            merge_relation_rows(std::move(old_rows), std::move(rhs->second), merged);
            if(!merged.empty()) {
                out.emplace_back(lhs->hash, std::move(merged));
            }
            lhs += 1;
            rhs += 1;
        }
    }
}

template <typename MaskT>
void emit_mask(ShardBlob& blob, bool occurrence, MaskTier tier, const MaskT& mask) {
    auto& masks32 = occurrence ? blob.occ_masks32 : blob.rel_masks32;
    auto& masks64 = occurrence ? blob.occ_masks64 : blob.rel_masks64;
    auto& roaring_offsets = occurrence ? blob.occ_roaring_offsets : blob.rel_roaring_offsets;
    auto& roaring = occurrence ? blob.occ_roaring : blob.rel_roaring;

    switch(tier) {
        case MaskTier::Single: {
            break;
        }
        case MaskTier::U32: {
            if constexpr(std::same_as<MaskT, std::uint64_t>) {
                masks32.push_back(static_cast<std::uint32_t>(mask));
            }
            break;
        }
        case MaskTier::U64: {
            if constexpr(std::same_as<MaskT, std::uint64_t>) {
                masks64.push_back(mask);
            }
            break;
        }
        case MaskTier::Roaring: {
            if constexpr(std::same_as<MaskT, Bitmap>) {
                auto size = mask.getSizeInBytes(true);
                auto offset = roaring.size();
                roaring.resize(offset + size);
                mask.write(reinterpret_cast<char*>(roaring.data() + offset), true);
                roaring_offsets.push_back(static_cast<std::uint32_t>(offset));
            }
            break;
        }
    }
}

void emit_range(std::vector<std::uint8_t>& lengths,
                std::vector<std::uint32_t>& long_rows,
                std::vector<std::uint32_t>& long_ends,
                std::uint32_t row,
                std::uint32_t begin,
                std::uint32_t end) {
    auto length = end - begin;
    if(length >= length_escape) {
        lengths.push_back(length_escape);
        long_rows.push_back(row);
        long_ends.push_back(end);
    } else {
        lengths.push_back(static_cast<std::uint8_t>(length));
    }
}

template <typename MaskT>
void write_shard_impl(ShardView old_root,
                      llvm::ArrayRef<std::int64_t> id_map,
                      const VariantInput& fresh,
                      std::int64_t fresh_id,
                      std::vector<RowsHash> variants,
                      llvm::StringRef content,
                      std::uint64_t content_hash,
                      llvm::raw_ostream& os) {
    MergedRows<MaskT> merged;
    merge_occurrences<MaskT>(old_root, id_map, fresh, fresh_id, merged.occurrences);
    merge_relations<MaskT>(old_root, id_map, fresh, fresh_id, merged.relations);

    // The symbol table covers exactly what the merged rows reference:
    // occurrence targets, relation group keys, and symbol payloads.
    llvm::DenseSet<std::uint64_t> referenced;
    for(auto& row: merged.occurrences) {
        referenced.insert(row.sym);
    }
    for(auto& [hash, rows]: merged.relations) {
        referenced.insert(hash);
        for(auto& row: rows) {
            if(row.payload != 0 &&
               !RelationKind(static_cast<RelationKind::Kind>(row.kind)).isDeclOrDef()) {
                referenced.insert(row.payload);
            }
        }
    }

    ShardBlob blob;
    blob.format_version = index_format_version;
    blob.content_hash = content_hash;
    blob.content = content.str();
    blob.variants = std::move(variants);

    blob.sym_hashes.assign(referenced.begin(), referenced.end());
    std::ranges::sort(blob.sym_hashes);
    auto sym_id = [&](std::uint64_t hash) {
        return static_cast<std::uint32_t>(std::ranges::lower_bound(blob.sym_hashes, hash) -
                                          blob.sym_hashes.begin());
    };

    // Local symbol names: survivors from the old blob, plus the fresh
    // variant's non-External symbols its rows referenced. External names
    // live in the ProjectIndex and are never stored here.
    struct LocalInfo {
        std::string name;
        std::uint8_t kind;
        std::uint8_t scope;
    };

    llvm::DenseMap<std::uint64_t, LocalInfo> locals;
    if(old_root.valid()) {
        auto old_sym_hashes = to_array_ref(old_root[&ShardBlob::sym_hashes]);
        auto old_local_syms = to_array_ref(old_root[&ShardBlob::local_syms]);
        auto old_kinds = to_array_ref(old_root[&ShardBlob::local_kinds]);
        auto old_scopes = to_array_ref(old_root[&ShardBlob::local_scopes]);
        auto old_names = old_root[&ShardBlob::local_names];
        for(std::uint32_t k = 0; k < old_local_syms.size(); k += 1) {
            auto hash = old_sym_hashes[old_local_syms[k]];
            if(referenced.contains(hash)) {
                locals.try_emplace(
                    hash,
                    LocalInfo{std::string(old_names.at(k)), old_kinds[k], old_scopes[k]});
            }
        }
    }
    if(fresh.symbols) {
        for(auto hash: referenced) {
            auto found = fresh.symbols(hash);
            if(!found || found->scope == SymbolScope::External) {
                continue;
            }
            locals.try_emplace(hash,
                               LocalInfo{std::string(found->name),
                                         found->kind.value(),
                                         static_cast<std::uint8_t>(found->scope)});
        }
    }

    llvm::SmallVector<std::pair<std::uint32_t, const LocalInfo*>> sorted_locals;
    sorted_locals.reserve(locals.size());
    for(auto& [hash, info]: locals) {
        sorted_locals.emplace_back(sym_id(hash), &info);
    }
    std::ranges::sort(sorted_locals, {}, [](const auto& entry) { return entry.first; });
    for(auto& [id, info]: sorted_locals) {
        blob.local_syms.push_back(id);
        blob.local_names.push_back(info->name);
        blob.local_kinds.push_back(info->kind);
        blob.local_scopes.push_back(info->scope);
    }

    auto tier = tier_of(blob.variants.size());
    bool wide_syms = blob.sym_hashes.size() > 0xffff;

    for(std::uint32_t row = 0; row < merged.occurrences.size(); row += 1) {
        auto& occurrence = merged.occurrences[row];
        blob.occ_begins.push_back(occurrence.begin);
        emit_range(blob.occ_lengths,
                   blob.occ_long_rows,
                   blob.occ_long_ends,
                   row,
                   occurrence.begin,
                   occurrence.end);
        auto id = sym_id(occurrence.sym);
        if(wide_syms) {
            blob.occ_syms32.push_back(id);
        } else {
            blob.occ_syms16.push_back(static_cast<std::uint16_t>(id));
        }
        emit_mask(blob, true, tier, occurrence.mask);
    }
    if(tier == MaskTier::Roaring) {
        blob.occ_roaring_offsets.push_back(static_cast<std::uint32_t>(blob.occ_roaring.size()));
    }

    // Relation groups follow symbol-table order; a symbol with occurrences
    // only gets an empty slice.
    blob.sym_rel_offsets.reserve(blob.sym_hashes.size() + 1);
    auto group = merged.relations.begin();
    std::uint32_t rel_row = 0;
    for(auto hash: blob.sym_hashes) {
        blob.sym_rel_offsets.push_back(rel_row);
        if(group == merged.relations.end() || group->first != hash) {
            continue;
        }
        for(auto& row: group->second) {
            blob.rel_kinds.push_back(row.kind);
            blob.rel_begins.push_back(row.begin);
            emit_range(blob.rel_lengths,
                       blob.rel_long_rows,
                       blob.rel_long_ends,
                       rel_row,
                       row.begin,
                       row.end);
            if(row.payload != 0) {
                if(RelationKind(static_cast<RelationKind::Kind>(row.kind)).isDeclOrDef()) {
                    auto range = std::bit_cast<LocalSourceRange>(row.payload);
                    blob.rel_def_rows.push_back(rel_row);
                    blob.rel_def_begins.push_back(range.begin);
                    blob.rel_def_ends.push_back(range.end);
                } else {
                    auto id = sym_id(row.payload);
                    blob.rel_sym_rows.push_back(rel_row);
                    if(wide_syms) {
                        blob.rel_sym32.push_back(id);
                    } else {
                        blob.rel_sym16.push_back(static_cast<std::uint16_t>(id));
                    }
                }
            }
            emit_mask(blob, false, tier, row.mask);
            rel_row += 1;
        }
        group += 1;
    }
    blob.sym_rel_offsets.push_back(rel_row);
    if(tier == MaskTier::Roaring) {
        blob.rel_roaring_offsets.push_back(static_cast<std::uint32_t>(blob.rel_roaring.size()));
    }

    serialize_blob(blob, os);
}

}  // namespace

void write_shard(const Shard& old,
                 llvm::ArrayRef<RowsHash> keep,
                 const VariantInput& fresh,
                 llvm::StringRef content,
                 std::uint64_t content_hash,
                 llvm::raw_ostream& os) {
    ShardView old_root;
    std::vector<RowsHash> old_variants = old.variants();

    // old-id -> new-id; -1 drops the variant.
    llvm::SmallVector<std::int64_t> id_map(old_variants.size(), -1);
    std::vector<RowsHash> variants;
    for(std::uint32_t id = 0; id < old_variants.size(); id += 1) {
        if(llvm::is_contained(keep, old_variants[id])) {
            id_map[id] = static_cast<std::int64_t>(variants.size());
            variants.push_back(old_variants[id]);
        }
    }
    if(!variants.empty()) {
        old_root = root_of(*old.buffer);
    }

    std::int64_t fresh_id = -1;
    if(fresh.rows) {
        assert(!llvm::is_contained(variants, fresh.hash) &&
               "a variant already stored must not be re-appended");
        fresh_id = static_cast<std::int64_t>(variants.size());
        variants.push_back(fresh.hash);
    }
    assert(!variants.empty() && "a shard blob holds at least one variant");

    if(variants.size() <= 64) {
        write_shard_impl<std::uint64_t>(old_root,
                                        id_map,
                                        fresh,
                                        fresh_id,
                                        std::move(variants),
                                        content,
                                        content_hash,
                                        os);
    } else {
        write_shard_impl<Bitmap>(old_root,
                                 id_map,
                                 fresh,
                                 fresh_id,
                                 std::move(variants),
                                 content,
                                 content_hash,
                                 os);
    }
}

}  // namespace clice::index
