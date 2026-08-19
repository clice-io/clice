#include "index/shard.h"

#include <algorithm>
#include <bit>
#include <cassert>
#include <tuple>
#include <utility>

#include "index/serialization.h"

#include "kota/ipc/lsp/text.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/xxhash.h"

namespace clice::index {

namespace {

using BlobView = kota::codec::fbs::table_view<ShardBlob>;

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

/// One side of the blob's row storage as contiguous column refs,
/// tier-agnostic: `begin_of`/`end_of` read whichever range tier the blob
/// stores.
struct Ranges {
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
};

Ranges ranges_of(kota::codec::fbs::table_view<RowRanges> view) {
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

Ranges occ_ranges(BlobView root) {
    return ranges_of(root[&ShardBlob::occs]);
}

Ranges rel_ranges(BlobView root) {
    return ranges_of(root[&ShardBlob::rels]);
}

/// The slice bounds were validated monotonic and in-bounds at load, and
/// every slice proven to decode, so this cannot fail.
Bitmap read_row_bitmap(const Ranges& columns, std::uint32_t row) {
    auto begin = columns.roaring_offsets[row];
    return *read_bitmap(columns.roaring.data() + begin, columns.roaring_offsets[row + 1] - begin);
}

std::uint32_t occ_sym_id(BlobView root, std::uint32_t row) {
    if(auto syms8 = to_array_ref(root[&ShardBlob::occ_syms8]); !syms8.empty()) {
        return syms8[row];
    }
    if(auto syms16 = to_array_ref(root[&ShardBlob::occ_syms16]); !syms16.empty()) {
        return syms16[row];
    }
    return to_array_ref(root[&ShardBlob::occ_syms32])[row];
}

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

/// Structural verification does not constrain field values; everything the
/// readers dereference through raw column pointers or binary-search must be
/// proven in-bounds and in order here, once, so queries stay check-free.
/// The checks are also canonicality checks: every self-describing choice
/// (range tier, symbol-id width, content omission, mask tier) is a strict
/// function of the data, so one logical blob has exactly one encoding and
/// its byte hash is a usable identity.
bool validate(BlobView root) {
    auto variants = to_array_ref(root[&ShardBlob::variants]);
    auto sym_hashes = to_array_ref(root[&ShardBlob::sym_hashes]);
    auto offsets = to_array_ref(root[&ShardBlob::sym_rel_offsets]);

    // A variant is identified by its hash everywhere (set_live, the merge
    // keep filter), so stored hashes must be unique: rows owned only by a
    // duplicated entry would serve and survive compaction with no
    // contribution owning them.
    llvm::SmallVector<RowsHash> sorted_variants(variants.begin(), variants.end());
    std::ranges::sort(sorted_variants);
    if(std::ranges::adjacent_find(sorted_variants) != sorted_variants.end()) {
        return false;
    }

    auto content = to_ref(root[&ShardBlob::content]);
    auto content_size = root[&ShardBlob::content_size];
    if(!content.empty()) {
        // Every freshness decision compares the advertised content hash
        // (manifest FileVersions, the merge's generation checks), so
        // content bytes corrupted under an intact structure would keep
        // loading as fresh while position mapping reads text the rows were
        // not built from.
        if(content.size() != content_size ||
           llvm::xxh3_64bits(content) != root[&ShardBlob::content_hash]) {
            return false;
        }
        // Pure-ASCII content must be omitted — the canonical form.
        if(llvm::all_of(content, [](char c) { return static_cast<unsigned char>(c) < 0x80; })) {
            return false;
        }
    }

    // The line table reconstructs every line start by prefix sum, so it
    // must both pair with its escape table and add up to exactly the
    // content size — a drifted sum would shift every position mapping
    // below the corruption. When the content is stored, the sum check is
    // not enough: a table redistributing bytes between lines keeps the
    // sum intact, so every start must match the one the content derives.
    auto line_lengths = to_array_ref(root[&ShardBlob::line_lengths]);
    auto long_line_rows = to_array_ref(root[&ShardBlob::long_line_rows]);
    auto long_line_lengths = to_array_ref(root[&ShardBlob::long_line_lengths]);
    if(line_lengths.empty() ||
       !escapes_ok(line_lengths, long_line_rows, long_line_lengths.size()) ||
       !std::ranges::is_sorted(long_line_rows, std::less_equal{})) {
        return false;
    }
    std::vector<std::uint32_t> starts;
    if(!content.empty()) {
        starts =
            kota::ipc::lsp::build_line_starts(std::string_view(content.data(), content.size()));
        if(starts.size() != line_lengths.size()) {
            return false;
        }
    }
    std::uint64_t line_sum = 0;
    std::size_t line_escape_cursor = 0;
    for(std::size_t row = 0; row < line_lengths.size(); row += 1) {
        if(!starts.empty() && starts[row] != line_sum) {
            return false;
        }
        auto length = line_lengths[row];
        if(length == length_escape) {
            auto value = long_line_lengths[line_escape_cursor];
            line_escape_cursor += 1;
            if(value < length_escape) {
                return false;
            }
            line_sum += value;
        } else {
            line_sum += length;
        }
    }
    if(line_sum != content_size) {
        return false;
    }

    auto occ = occ_ranges(root);
    auto rel = rel_ranges(root);
    auto occ_count = occ.size();
    auto rel_count = rel.size();

    // Exactly one range tier, chosen by the content size.
    auto tier_ok = [&](const Ranges& columns) {
        if(content_size <= packed_range_limit) {
            return columns.begins.empty() && columns.lengths.empty();
        }
        return columns.packed.empty() && columns.lengths.size() == columns.begins.size();
    };
    if(!tier_ok(occ) || !tier_ok(rel)) {
        return false;
    }
    auto range_escapes_ok = [](const Ranges& columns) {
        if(columns.packed.empty()) {
            return escapes_ok(columns.lengths, columns.long_rows, columns.long_ends.size());
        }
        return escapes_ok(packed_lengths(columns.packed),
                          columns.long_rows,
                          columns.long_ends.size());
    };
    if(!range_escapes_ok(occ) || !range_escapes_ok(rel)) {
        return false;
    }

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
    // Exactly one id width, chosen by the table size.
    auto sym_width_ok = [&](std::size_t count,
                            llvm::ArrayRef<std::uint8_t> ids8,
                            llvm::ArrayRef<std::uint16_t> ids16,
                            llvm::ArrayRef<std::uint32_t> ids32) {
        if(ids8.size() + ids16.size() + ids32.size() != count) {
            return false;
        }
        if(sym_hashes.size() <= 0x100) {
            return ids16.empty() && ids32.empty();
        }
        if(sym_hashes.size() <= 0x10000) {
            return ids8.empty() && ids32.empty();
        }
        return ids8.empty() && ids16.empty();
    };
    auto occ_syms8 = to_array_ref(root[&ShardBlob::occ_syms8]);
    auto occ_syms16 = to_array_ref(root[&ShardBlob::occ_syms16]);
    auto occ_syms32 = to_array_ref(root[&ShardBlob::occ_syms32]);
    if(!sym_width_ok(occ_count, occ_syms8, occ_syms16, occ_syms32)) {
        return false;
    }
    if(!sym_ids_ok(occ_syms8) || !sym_ids_ok(occ_syms16) || !sym_ids_ok(occ_syms32)) {
        return false;
    }

    auto rel_kinds = to_array_ref(root[&ShardBlob::rel_kinds]);
    if(rel_kinds.size() != rel_count) {
        return false;
    }

    // lookup(offset) binary-searches the decoded end column and stops its
    // containment walk on begin order; rows out of either order (a corrupt
    // escaped end included) would silently miss or misresolve occurrences
    // on every query, forever — reject the blob so it is rebuilt instead.
    // Ends are bounded by the content size too: every decoded range is
    // served as a source range into the content.
    // The merge also two-way merges rows under the full (begin, end, sym)
    // key with equal-key rows combined at write time, so equal ranges must
    // carry strictly ascending symbols — sym_hashes is strictly sorted, so
    // id order stands in for hash order.
    std::uint32_t prev_begin = 0;
    std::uint32_t prev_end = 0;
    std::uint32_t prev_sym = 0;
    for(std::uint32_t row = 0; row < occ_count; row += 1) {
        auto begin = occ.begin_of(row);
        auto end = occ.end_of(row);
        auto sym = occ_sym_id(root, row);
        if(begin < prev_begin || end < prev_end || end < begin || end > content_size) {
            return false;
        }
        if(row != 0 && begin == prev_begin && end == prev_end && sym <= prev_sym) {
            return false;
        }
        prev_begin = begin;
        prev_end = end;
        prev_sym = sym;
    }
    // Relation ranges carry no query order to enforce, but are served as
    // source ranges all the same — bound them like the occurrence ends.
    // The exception is the default LocalSourceRange, the writer's sentinel
    // for pair relations, which carry no range of their own; every other
    // kind is written with a real range, so a sentinel there is corruption
    // that would serve an invalid source range forever.
    for(std::uint32_t row = 0; row < rel_count; row += 1) {
        auto begin = rel.begin_of(row);
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

    auto sparse_ok = [](llvm::ArrayRef<std::uint32_t> rows, std::size_t values, std::size_t count) {
        return rows.size() == values && std::ranges::is_sorted(rows, std::less_equal{}) &&
               (rows.empty() || rows.back() < count);
    };
    auto rel_sym_rows = to_array_ref(root[&ShardBlob::rel_sym_rows]);
    auto rel_sym8 = to_array_ref(root[&ShardBlob::rel_sym8]);
    auto rel_sym16 = to_array_ref(root[&ShardBlob::rel_sym16]);
    auto rel_sym32 = to_array_ref(root[&ShardBlob::rel_sym32]);
    if(!sparse_ok(rel_sym_rows, rel_sym8.size() + rel_sym16.size() + rel_sym32.size(), rel_count)) {
        return false;
    }
    if(!sym_width_ok(rel_sym_rows.size(), rel_sym8, rel_sym16, rel_sym32)) {
        return false;
    }
    if(!sym_ids_ok(rel_sym8) || !sym_ids_ok(rel_sym16) || !sym_ids_ok(rel_sym32)) {
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

    // The writer splits payloads by kind — decl/def rows carry a
    // definition range, every other payload names a target symbol — and
    // the readers decode whichever sparse table holds the row without
    // consulting its kind. A row in the wrong table would serve one
    // payload's bit pattern as the other (a source range as a symbol
    // hash, or vice versa), so enforce the partition, which also keeps
    // the tables disjoint.
    auto kind_of = [&](std::uint32_t row) {
        return RelationKind(static_cast<RelationKind::Kind>(rel_kinds[row]));
    };
    for(auto row: rel_sym_rows) {
        if(kind_of(row).isDeclOrDef()) {
            return false;
        }
    }
    for(auto row: rel_def_rows) {
        if(!kind_of(row).isDeclOrDef()) {
            return false;
        }
    }

    // Rows of one relation group two-way merge under the full (kind,
    // begin, end, payload) key with equal-key rows combined at write time,
    // so the key must ascend strictly within each group — out-of-order or
    // repeated rows would mis-merge silently instead of being rejected.
    // The payload mirrors decode_relation_group: a def range, a target
    // symbol's hash, or 0.
    {
        std::size_t sym_cursor = 0;
        std::size_t def_cursor = 0;
        auto payload_of = [&](std::uint32_t row) -> std::uint64_t {
            while(sym_cursor < rel_sym_rows.size() && rel_sym_rows[sym_cursor] < row) {
                sym_cursor += 1;
            }
            while(def_cursor < rel_def_rows.size() && rel_def_rows[def_cursor] < row) {
                def_cursor += 1;
            }
            if(def_cursor < rel_def_rows.size() && rel_def_rows[def_cursor] == row) {
                return std::bit_cast<std::uint64_t>(
                    LocalSourceRange{rel_def_begins[def_cursor], rel_def_ends[def_cursor]});
            }
            if(sym_cursor < rel_sym_rows.size() && rel_sym_rows[sym_cursor] == row) {
                auto id = !rel_sym8.empty()    ? rel_sym8[sym_cursor]
                          : !rel_sym16.empty() ? rel_sym16[sym_cursor]
                                               : rel_sym32[sym_cursor];
                return sym_hashes[id];
            }
            return 0;
        };
        for(std::size_t id = 0; id < sym_hashes.size(); id += 1) {
            std::tuple<std::uint8_t, std::uint32_t, std::uint32_t, std::uint64_t> prev{};
            for(auto row = offsets[id]; row < offsets[id + 1]; row += 1) {
                std::tuple key{rel_kinds[row], rel.begin_of(row), rel.end_of(row), payload_of(row)};
                if(row != offsets[id] && key <= prev) {
                    return false;
                }
                prev = key;
            }
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
    auto variant_count = variant_count_of(root);
    auto masks_ok = [&](const Ranges& columns, std::size_t count) {
        switch(tier_of(variant_count)) {
            case MaskTier::Single: {
                return columns.masks32.empty() && columns.masks64.empty() &&
                       columns.roaring_offsets.empty() && columns.roaring.empty();
            }
            case MaskTier::U32: {
                if(columns.masks32.size() != count || !columns.masks64.empty() ||
                   !columns.roaring_offsets.empty()) {
                    return false;
                }
                auto stray = variant_count < 32 ? ~std::uint32_t(0) << variant_count : 0;
                return llvm::all_of(columns.masks32, [&](std::uint32_t mask) {
                    return mask != 0 && (mask & stray) == 0;
                });
            }
            case MaskTier::U64: {
                if(columns.masks64.size() != count || !columns.masks32.empty() ||
                   !columns.roaring_offsets.empty()) {
                    return false;
                }
                auto stray = variant_count < 64 ? ~std::uint64_t(0) << variant_count : 0;
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
                    if(!mask || mask->isEmpty() || mask->maximum() >= variant_count) {
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

bool Shard::ascii() const {
    return loaded() && content().empty();
}

bool Shard::matches_content(llvm::StringRef text) const {
    return loaded() && text.size() == content_size() && llvm::xxh3_64bits(text) == content_hash();
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

bool Shard::row_live(bool occurrence, std::uint32_t row) const {
    if(live.all) {
        return true;
    }
    auto root = root_of(*buffer);
    auto columns = occurrence ? occ_ranges(root) : rel_ranges(root);
    switch(tier_of(variant_count_of(root))) {
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
    auto columns = occ_ranges(root);
    auto sym_hashes = to_array_ref(root[&ShardBlob::sym_hashes]);

    // Binary search the first row whose end reaches the offset, then walk
    // while rows contain it. Occurrence ranges are name-token spans,
    // pairwise disjoint or identical, so under (begin, end) order the end
    // column is monotonic too.
    std::size_t lo = 0;
    std::size_t hi = columns.size();
    while(lo < hi) {
        auto mid = lo + (hi - lo) / 2;
        if(columns.end_of(mid) < offset) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }

    for(; lo < columns.size(); lo += 1) {
        auto row = static_cast<std::uint32_t>(lo);
        LocalSourceRange range{columns.begin_of(row), columns.end_of(row)};
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

namespace {

/// Reconstruct and visit the relation rows [begin_row, end_row); `live`
/// filters dead rows, the callback's false stops the walk.
void visit_relation_rows(BlobView root,
                         std::uint32_t begin_row,
                         std::uint32_t end_row,
                         llvm::function_ref<bool(std::uint32_t)> live,
                         llvm::function_ref<bool(const Relation&)> callback) {
    auto columns = rel_ranges(root);
    auto sym_hashes = to_array_ref(root[&ShardBlob::sym_hashes]);
    auto kinds = to_array_ref(root[&ShardBlob::rel_kinds]);
    auto sym_rows = to_array_ref(root[&ShardBlob::rel_sym_rows]);
    auto sym8 = to_array_ref(root[&ShardBlob::rel_sym8]);
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

        if(!live(row)) {
            continue;
        }

        Relation relation{
            .kind = static_cast<RelationKind::Kind>(kinds[row]),
            .range = {columns.begin_of(row), columns.end_of(row)},
            .target_symbol = 0,
        };
        if(def_cursor < static_cast<std::ptrdiff_t>(def_rows.size()) &&
           def_rows[def_cursor] == row) {
            relation.set_definition_range({def_begins[def_cursor], def_ends[def_cursor]});
        } else if(sym_cursor < static_cast<std::ptrdiff_t>(sym_rows.size()) &&
                  sym_rows[sym_cursor] == row) {
            auto payload = !sym8.empty()    ? sym8[sym_cursor]
                           : !sym16.empty() ? sym16[sym_cursor]
                                            : sym32[sym_cursor];
            relation.target_symbol = sym_hashes[payload];
        }

        if(!callback(relation)) {
            return;
        }
    }
}

}  // namespace

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
    visit_relation_rows(
        root,
        offsets[id],
        offsets[id + 1],
        [&](std::uint32_t row) { return row_live(false, row); },
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
    auto root = root_of(*buffer);
    auto columns = occ_ranges(root);
    auto sym_hashes = to_array_ref(root[&ShardBlob::sym_hashes]);
    for(std::uint32_t row = 0; row < columns.size(); row += 1) {
        if(!row_live(true, row)) {
            continue;
        }
        Occurrence occurrence{
            {columns.begin_of(row), columns.end_of(row)},
            sym_hashes[occ_sym_id(root, row)]
        };
        if(!callback(occurrence)) {
            return;
        }
    }
}

void
    Shard::for_each_relation(llvm::function_ref<bool(SymbolHash, const Relation&)> callback) const {
    if(!buffer) {
        return;
    }
    auto root = root_of(*buffer);
    auto sym_hashes = to_array_ref(root[&ShardBlob::sym_hashes]);
    auto offsets = to_array_ref(root[&ShardBlob::sym_rel_offsets]);
    for(std::uint32_t id = 0; id < sym_hashes.size(); id += 1) {
        bool stopped = false;
        visit_relation_rows(
            root,
            offsets[id],
            offsets[id + 1],
            [&](std::uint32_t row) { return row_live(false, row); },
            [&](const Relation& relation) {
                if(!callback(sym_hashes[id], relation)) {
                    stopped = true;
                    return false;
                }
                return true;
            });
        if(stopped) {
            return;
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
MaskT remap_mask(BlobView root,
                 const Ranges& columns,
                 std::uint32_t row,
                 llvm::ArrayRef<std::int64_t> id_map) {
    MaskT result{};
    auto apply = [&](std::uint32_t old_id) {
        if(id_map[old_id] >= 0) {
            mask_or(result, single_bit<MaskT>(static_cast<std::uint32_t>(id_map[old_id])));
        }
    };
    switch(tier_of(variant_count_of(root))) {
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

/// Everything a write accumulates before choosing column tiers.
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

/// Combine adjacent equal-key rows of a sorted run into one row with
/// OR-ed masks.
template <typename Row, typename Key>
void combine_equal(std::vector<Row>& rows, Key key) {
    std::size_t out = 0;
    for(std::size_t i = 0; i < rows.size(); i += 1) {
        if(out != 0 && key(rows[out - 1]) == key(rows[i])) {
            mask_or(rows[out - 1].mask, rows[i].mask);
        } else {
            if(out != i) {
                rows[out] = std::move(rows[i]);
            }
            out += 1;
        }
    }
    rows.resize(out);
}

/// The symbol table a set of merged rows requires: occurrence targets,
/// relation group keys, and symbol payloads.
template <typename MaskT>
llvm::DenseSet<std::uint64_t> referenced_symbols(const MergedRows<MaskT>& merged) {
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
    return referenced;
}

constexpr auto occ_key = [](const auto& row) {
    return std::tuple(row.begin, row.end, row.sym);
};
constexpr auto rel_key = [](const auto& row) {
    return std::tuple(row.kind, row.begin, row.end, row.payload);
};

/// Decode one blob's occurrence rows into working form; `mask_of` returns
/// the row's mask in the output id space (empty drops the row).
template <typename MaskT, typename MaskOf>
std::vector<OccRow<MaskT>> decode_occurrences(BlobView root, MaskOf mask_of) {
    auto columns = occ_ranges(root);
    auto sym_hashes = to_array_ref(root[&ShardBlob::sym_hashes]);
    std::vector<OccRow<MaskT>> rows;
    rows.reserve(columns.size());
    for(std::uint32_t row = 0; row < columns.size(); row += 1) {
        auto mask = mask_of(columns, row);
        if(mask_empty(mask)) {
            continue;
        }
        rows.push_back({columns.begin_of(row),
                        columns.end_of(row),
                        sym_hashes[occ_sym_id(root, row)],
                        std::move(mask)});
    }
    return rows;
}

/// Decode one symbol's relation slice of a blob into working form.
template <typename MaskT, typename MaskOf>
std::vector<RelRow<MaskT>> decode_relation_group(BlobView root,
                                                 const Ranges& columns,
                                                 std::uint32_t begin_row,
                                                 std::uint32_t end_row,
                                                 MaskOf mask_of) {
    auto sym_hashes = to_array_ref(root[&ShardBlob::sym_hashes]);
    auto kinds = to_array_ref(root[&ShardBlob::rel_kinds]);
    auto sym_rows = to_array_ref(root[&ShardBlob::rel_sym_rows]);
    auto sym8 = to_array_ref(root[&ShardBlob::rel_sym8]);
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

        auto mask = mask_of(columns, row);
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
            auto id = !sym8.empty()    ? sym8[sym_cursor]
                      : !sym16.empty() ? sym16[sym_cursor]
                                       : sym32[sym_cursor];
            payload = sym_hashes[id];
        }

        rows.push_back(
            {kinds[row], columns.begin_of(row), columns.end_of(row), payload, std::move(mask)});
    }
    return rows;
}

/// One blob's relation groups in symbol-hash order, decoded lazily by the
/// group merge.
struct GroupIndex {
    std::uint64_t hash;
    std::uint32_t begin_row;
    std::uint32_t end_row;
};

std::vector<GroupIndex> relation_groups(BlobView root) {
    std::vector<GroupIndex> groups;
    auto sym_hashes = to_array_ref(root[&ShardBlob::sym_hashes]);
    auto offsets = to_array_ref(root[&ShardBlob::sym_rel_offsets]);
    for(std::uint32_t id = 0; id < sym_hashes.size(); id += 1) {
        if(offsets[id] != offsets[id + 1]) {
            groups.push_back({sym_hashes[id], offsets[id], offsets[id + 1]});
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
    bool is_ascii =
        llvm::all_of(content, [](char c) { return static_cast<unsigned char>(c) < 0x80; });
    if(!is_ascii) {
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

/// A local symbol's identity as stored in a blob's local-name table.
struct LocalInfo {
    std::string name;
    std::uint8_t kind;
    std::uint8_t scope;
};

/// Collect one blob's local symbols; symbols the merged rows no longer
/// reference are filtered at emit time.
void collect_locals(BlobView root, llvm::DenseMap<std::uint64_t, LocalInfo>& locals) {
    auto sym_hashes = to_array_ref(root[&ShardBlob::sym_hashes]);
    auto local_syms = to_array_ref(root[&ShardBlob::local_syms]);
    auto kinds = to_array_ref(root[&ShardBlob::local_kinds]);
    auto scopes = to_array_ref(root[&ShardBlob::local_scopes]);
    auto names = root[&ShardBlob::local_names];
    for(std::uint32_t k = 0; k < local_syms.size(); k += 1) {
        auto hash = sym_hashes[local_syms[k]];
        locals.try_emplace(hash, LocalInfo{std::string(names.at(k)), kinds[k], scopes[k]});
    }
}

void emit_row_range(RowRanges& side,
                    bool narrow,
                    std::uint32_t row,
                    std::uint32_t begin,
                    std::uint32_t end) {
    if((LocalSourceRange{begin, end}) == LocalSourceRange{}) {
        // The no-range sentinel of pair relations; the wide columns hold
        // it natively, the packed column spells it as the reserved word.
        if(narrow) {
            side.packed.push_back(packed_sentinel);
        } else {
            side.begins.push_back(begin);
            side.lengths.push_back(0);
        }
        return;
    }
    auto length = end - begin;
    std::uint8_t stored =
        length >= length_escape ? length_escape : static_cast<std::uint8_t>(length);
    if(stored == length_escape) {
        side.long_rows.push_back(row);
        side.long_ends.push_back(end);
    }
    if(narrow) {
        side.packed.push_back(pack_range(begin, stored));
    } else {
        side.begins.push_back(begin);
        side.lengths.push_back(stored);
    }
}

template <typename MaskT>
void emit_mask(RowRanges& side, MaskTier tier, const MaskT& mask) {
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

/// Encode merged rows, locals and content into canonical blob bytes. The
/// only entry point that writes a ShardBlob: every self-describing choice
/// (range tier, id width, mask tier, content omission) is made here, from
/// the data, so equal inputs produce equal bytes.
template <typename MaskT>
void emit_blob(MergedRows<MaskT>& merged,
               llvm::DenseMap<std::uint64_t, LocalInfo>& locals,
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
    auto sym_id = [&](std::uint64_t hash) {
        return static_cast<std::uint32_t>(std::ranges::lower_bound(blob.sym_hashes, hash) -
                                          blob.sym_hashes.begin());
    };

    llvm::SmallVector<std::pair<std::uint32_t, const LocalInfo*>> sorted_locals;
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
        blob.local_kinds.push_back(info->kind);
        blob.local_scopes.push_back(info->scope);
    }

    auto tier = tier_of(blob.variants.empty() ? 1 : blob.variants.size());
    bool narrow = content.size <= packed_range_limit;
    enum class SymWidth : std::uint8_t { U8, U16, U32 };
    auto width = blob.sym_hashes.size() <= 0x100     ? SymWidth::U8
                 : blob.sym_hashes.size() <= 0x10000 ? SymWidth::U16
                                                     : SymWidth::U32;
    auto emit_sym_id = [&](std::uint32_t id,
                           std::vector<std::uint8_t>& ids8,
                           std::vector<std::uint16_t>& ids16,
                           std::vector<std::uint32_t>& ids32) {
        switch(width) {
            case SymWidth::U8: ids8.push_back(static_cast<std::uint8_t>(id)); break;
            case SymWidth::U16: ids16.push_back(static_cast<std::uint16_t>(id)); break;
            case SymWidth::U32: ids32.push_back(id); break;
        }
    };

    for(std::uint32_t row = 0; row < merged.occurrences.size(); row += 1) {
        auto& occurrence = merged.occurrences[row];
        emit_row_range(blob.occs, narrow, row, occurrence.begin, occurrence.end);
        emit_sym_id(sym_id(occurrence.sym), blob.occ_syms8, blob.occ_syms16, blob.occ_syms32);
        emit_mask(blob.occs, tier, occurrence.mask);
    }
    if(tier == MaskTier::Roaring) {
        blob.occs.roaring_offsets.push_back(static_cast<std::uint32_t>(blob.occs.roaring.size()));
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
            emit_row_range(blob.rels, narrow, rel_row, row.begin, row.end);
            if(row.payload != 0) {
                if(RelationKind(static_cast<RelationKind::Kind>(row.kind)).isDeclOrDef()) {
                    auto range = std::bit_cast<LocalSourceRange>(row.payload);
                    blob.rel_def_rows.push_back(rel_row);
                    blob.rel_def_begins.push_back(range.begin);
                    blob.rel_def_ends.push_back(range.end);
                } else {
                    blob.rel_sym_rows.push_back(rel_row);
                    emit_sym_id(sym_id(row.payload), blob.rel_sym8, blob.rel_sym16, blob.rel_sym32);
                }
            }
            emit_mask(blob.rels, tier, row.mask);
            rel_row += 1;
        }
        group += 1;
    }
    blob.sym_rel_offsets.push_back(rel_row);
    if(tier == MaskTier::Roaring) {
        blob.rels.roaring_offsets.push_back(static_cast<std::uint32_t>(blob.rels.roaring.size()));
    }

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
    auto old_mask = [&](const Ranges& columns, std::uint32_t row) {
        return remap_mask<MaskT>(old_root, columns, row, id_map);
    };

    MergedRows<MaskT> merged;

    // Occurrences: concatenate every fresh blob's rows (each stamped with
    // its new bit), sort, combine equal rows, then merge with the old
    // rows. Fresh runs are individually sorted already; one sort over the
    // concatenation keeps the merge two-way.
    std::vector<OccRow<MaskT>> fresh_occs;
    for(std::uint32_t i = 0; i < fresh.size(); i += 1) {
        auto root = view_of(fresh[i].bytes());
        auto bit = single_bit<MaskT>(fresh_base + i);
        auto rows =
            decode_occurrences<MaskT>(root, [&](const Ranges&, std::uint32_t) { return bit; });
        fresh_occs.insert(fresh_occs.end(),
                          std::make_move_iterator(rows.begin()),
                          std::make_move_iterator(rows.end()));
    }
    std::ranges::sort(fresh_occs, {}, occ_key);
    combine_equal(fresh_occs, occ_key);

    std::vector<OccRow<MaskT>> old_occs;
    if(old_root.valid()) {
        old_occs = decode_occurrences<MaskT>(old_root, old_mask);
    }
    merge_sorted(std::move(old_occs), std::move(fresh_occs), occ_key, merged.occurrences);

    // Relations: gather fresh groups per symbol across all fresh blobs,
    // then two-way merge with the old blob's groups in hash order.
    llvm::DenseMap<std::uint64_t, std::vector<RelRow<MaskT>>> fresh_group_map;
    for(std::uint32_t i = 0; i < fresh.size(); i += 1) {
        auto root = view_of(fresh[i].bytes());
        auto bit = single_bit<MaskT>(fresh_base + i);
        auto columns = rel_ranges(root);
        for(auto& group: relation_groups(root)) {
            auto rows =
                decode_relation_group<MaskT>(root,
                                             columns,
                                             group.begin_row,
                                             group.end_row,
                                             [&](const Ranges&, std::uint32_t) { return bit; });
            auto& into = fresh_group_map[group.hash];
            into.insert(into.end(),
                        std::make_move_iterator(rows.begin()),
                        std::make_move_iterator(rows.end()));
        }
    }
    std::vector<std::pair<std::uint64_t, std::vector<RelRow<MaskT>>>> fresh_groups;
    fresh_groups.reserve(fresh_group_map.size());
    for(auto& [hash, rows]: fresh_group_map) {
        std::ranges::sort(rows, {}, rel_key);
        combine_equal(rows, rel_key);
        fresh_groups.emplace_back(hash, std::move(rows));
    }
    std::ranges::sort(fresh_groups, {}, [](const auto& group) { return group.first; });

    std::vector<GroupIndex> old_groups;
    Ranges old_columns;
    if(old_root.valid()) {
        old_columns = rel_ranges(old_root);
        old_groups = relation_groups(old_root);
    }

    auto lhs = old_groups.begin();
    auto rhs = fresh_groups.begin();
    while(lhs != old_groups.end() || rhs != fresh_groups.end()) {
        if(rhs == fresh_groups.end() || (lhs != old_groups.end() && lhs->hash < rhs->first)) {
            auto rows = decode_relation_group<MaskT>(old_root,
                                                     old_columns,
                                                     lhs->begin_row,
                                                     lhs->end_row,
                                                     old_mask);
            if(!rows.empty()) {
                merged.relations.emplace_back(lhs->hash, std::move(rows));
            }
            lhs += 1;
        } else if(lhs == old_groups.end() || rhs->first < lhs->hash) {
            merged.relations.emplace_back(rhs->first, std::move(rhs->second));
            rhs += 1;
        } else {
            auto old_rows = decode_relation_group<MaskT>(old_root,
                                                         old_columns,
                                                         lhs->begin_row,
                                                         lhs->end_row,
                                                         old_mask);
            std::vector<RelRow<MaskT>> combined;
            merge_sorted(std::move(old_rows), std::move(rhs->second), rel_key, combined);
            if(!combined.empty()) {
                merged.relations.emplace_back(lhs->hash, std::move(combined));
            }
            lhs += 1;
            rhs += 1;
        }
    }

    // Local names union: every input blob is self-contained, so the merge
    // needs no external symbol resolver. First writer wins — identities of
    // one symbol agree across blobs of one file.
    llvm::DenseMap<std::uint64_t, LocalInfo> locals;
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
        merged.occurrences.push_back(
            {occurrence.range.begin, occurrence.range.end, occurrence.target, 1});
    }
    std::ranges::sort(merged.occurrences, {}, occ_key);
    combine_equal(merged.occurrences, occ_key);

    merged.relations.reserve(rows.relations.size());
    for(auto& [hash, relations]: rows.relations) {
        std::vector<RelRow<std::uint64_t>> group;
        group.reserve(relations.size());
        for(auto& relation: relations) {
            group.push_back({static_cast<std::uint8_t>(relation.kind),
                             relation.range.begin,
                             relation.range.end,
                             relation.target_symbol,
                             1});
        }
        std::ranges::sort(group, {}, rel_key);
        combine_equal(group, rel_key);
        merged.relations.emplace_back(hash, std::move(group));
    }
    std::ranges::sort(merged.relations, {}, [](const auto& group) { return group.first; });

    llvm::DenseMap<std::uint64_t, LocalInfo> locals;
    if(symbols) {
        for(auto hash: referenced_symbols(merged)) {
            auto found = symbols(hash);
            if(!found || found->scope == SymbolScope::External) {
                continue;
            }
            locals.try_emplace(hash,
                               LocalInfo{std::string(found->name),
                                         found->kind.value(),
                                         static_cast<std::uint8_t>(found->scope)});
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
