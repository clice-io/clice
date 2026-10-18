module;

#include "modules/prelude.h"

/// The column encodings the blobs share: entry ranges in whichever
/// encoding the side and the content size pick (shard and instantiation
/// blobs), id columns in whichever width their table picks (those and the
/// global blob), and the load-time checks that let readers trust them.

module clice:index.columns;

import :index.serialization;
import :support.bitmap;
import :syntax.token;

namespace clice::index {

/// Load-time verification outcome: the violated invariant, spelled for the
/// log, or success.
using CheckResult = std::expected<void, llvm::StringRef>;

/// The no-range sentinel of pair relations: the default LocalSourceRange.
inline bool is_no_range(std::uint32_t begin, std::uint32_t end) {
    return LocalSourceRange{begin, end} == LocalSourceRange{};
}

/// The delta of a delta-encoded begin that does not fit: the sparse table
/// holds the begin.
constexpr inline std::uint8_t delta_escape = 0xff;

/// The width in bytes of ids into a table of `table_size` entries.
inline std::uint8_t id_width(std::size_t table_size) {
    return table_size <= 0x100 ? 1 : table_size <= 0x10000 ? 2 : 4;
}

/// An id column read in the width its table picks (IdColumn).
struct IdColumnView {
    llvm::ArrayRef<std::uint8_t> bytes;
    std::uint8_t width = 1;

    std::size_t size() const {
        return bytes.size() / width;
    }

    std::uint32_t operator[](std::size_t i) const {
        auto at = bytes.data() + i * width;
        switch(width) {
            case 1: return *at;
            case 2: {
                std::uint16_t id;
                std::memcpy(&id, at, sizeof(id));
                return id;
            }
            default: {
                std::uint32_t id;
                std::memcpy(&id, at, sizeof(id));
                return id;
            }
        }
    }

    /// Whether an ascending column holds `id`, and where.
    std::optional<std::size_t> find(std::uint32_t id) const {
        auto positions = std::views::iota(std::size_t(0), size());
        auto it = std::ranges::lower_bound(positions, id, {}, [&](std::size_t position) {
            return (*this)[position];
        });
        if(it == positions.end() || (*this)[*it] != id) {
            return std::nullopt;
        }
        return *it;
    }

    /// Entry `i` of a text whose entries end at the offsets of this column,
    /// back to back.
    llvm::StringRef slice(llvm::StringRef text, std::size_t i) const {
        auto begin = i == 0 ? 0 : (*this)[i - 1];
        return text.slice(begin, (*this)[i]);
    }
};

/// One side of a blob's entry storage as contiguous column refs,
/// encoding-agnostic: the range accessors read whichever range encoding the
/// side stores, the mask columns whichever mask encoding.
struct RangeColumnsView {
    llvm::ArrayRef<std::uint32_t> block_begins;
    llvm::ArrayRef<std::uint8_t> deltas;
    llvm::ArrayRef<std::uint32_t> far_entries;
    llvm::ArrayRef<std::uint32_t> far_begins;
    llvm::ArrayRef<std::uint32_t> packed;
    llvm::ArrayRef<std::uint32_t> begins;
    llvm::ArrayRef<std::uint8_t> lengths;
    llvm::ArrayRef<std::uint32_t> long_entries;
    llvm::ArrayRef<std::uint32_t> long_ends;
    llvm::ArrayRef<std::uint8_t> masks8;
    llvm::ArrayRef<std::uint16_t> masks16;
    llvm::ArrayRef<std::uint32_t> masks32;
    llvm::ArrayRef<std::uint64_t> masks64;
    llvm::ArrayRef<std::uint32_t> roaring_offsets;
    llvm::ArrayRef<std::uint8_t> roaring;

    std::size_t size() const {
        return packed.empty() ? lengths.size() : packed.size();
    }

    /// range_encoding_ok() proves every escape owns exactly one entry of its
    /// sparse table, so the search always finds it.
    std::uint32_t far_begin(std::uint32_t entry) const {
        auto it = std::ranges::lower_bound(far_entries, entry);
        return far_begins[it - far_entries.begin()];
    }

    /// The begin of `entry` given the begin of the entry before it, in the
    /// delta encoding.
    std::uint32_t next_begin(std::uint32_t entry, std::uint32_t previous) const {
        if(entry % delta_block == 0) {
            return block_begins[entry / delta_block];
        }
        return deltas[entry] == delta_escape ? far_begin(entry) : previous + deltas[entry];
    }

    std::uint32_t begin_of(std::uint32_t entry) const {
        if(!packed.empty()) {
            return packed[entry] == packed_sentinel ? ~std::uint32_t(0) : packed[entry] >> 8;
        }
        if(deltas.empty()) {
            return begins[entry];
        }
        std::uint32_t begin = 0;
        for(auto i = entry - entry % delta_block; i <= entry; i += 1) {
            begin = next_begin(i, begin);
        }
        return begin;
    }

    /// The end of `entry`, which begins at `begin`.
    std::uint32_t end_of(std::uint32_t entry, std::uint32_t begin) const {
        if(!packed.empty() && packed[entry] == packed_sentinel) {
            return ~std::uint32_t(0);
        }
        auto length =
            packed.empty() ? lengths[entry] : static_cast<std::uint8_t>(packed[entry] & 0xff);
        if(length == length_escape) {
            auto it = std::ranges::lower_bound(long_entries, entry);
            return long_ends[it - long_entries.begin()];
        }
        return begin + length;
    }

    std::uint32_t end_of(std::uint32_t entry) const {
        return end_of(entry, begin_of(entry));
    }

    LocalSourceRange range_of(std::uint32_t entry) const {
        auto begin = begin_of(entry);
        return {begin, end_of(entry, begin)};
    }

    /// Visit every entry's range in entry order, the delta encoding decoded
    /// in one pass; `visit` returns false to stop.
    template <typename Visit>
    void for_each_range(Visit&& visit) const {
        std::uint32_t begin = 0;
        for(std::uint32_t entry = 0; entry < size(); entry += 1) {
            begin = deltas.empty() ? begin_of(entry) : next_begin(entry, begin);
            if(!visit(entry, LocalSourceRange{begin, end_of(entry, begin)})) {
                return;
            }
        }
    }

    /// The slice bounds were validated monotonic and in-bounds at load, and
    /// every slice proven to decode, so this cannot fail.
    Bitmap bitmap_of(std::uint32_t entry) const {
        auto begin = roaring_offsets[entry];
        return *view_bitmap(roaring.data() + begin, roaring_offsets[entry + 1] - begin);
    }
};

inline RangeColumnsView columns_of(kota::codec::fbs::table_view<RangeColumns> view) {
    if(!view.valid()) {
        return {};
    }
    return {
        to_array_ref(view[&RangeColumns::block_begins]),
        to_array_ref(view[&RangeColumns::deltas]),
        to_array_ref(view[&RangeColumns::far_entries]),
        to_array_ref(view[&RangeColumns::far_begins]),
        to_array_ref(view[&RangeColumns::packed]),
        to_array_ref(view[&RangeColumns::begins]),
        to_array_ref(view[&RangeColumns::lengths]),
        to_array_ref(view[&RangeColumns::long_entries]),
        to_array_ref(view[&RangeColumns::long_ends]),
        to_array_ref(view[&RangeColumns::masks8]),
        to_array_ref(view[&RangeColumns::masks16]),
        to_array_ref(view[&RangeColumns::masks32]),
        to_array_ref(view[&RangeColumns::masks64]),
        to_array_ref(view[&RangeColumns::roaring_offsets]),
        to_array_ref(view[&RangeColumns::roaring]),
    };
}

/// The ids of a column into a table of `table_size` entries.
inline IdColumnView ids_of(kota::codec::fbs::array_view<std::uint8_t> view,
                           std::size_t table_size) {
    return {to_array_ref(view), id_width(table_size)};
}

/// The escape table must pair one-to-one, in column order, with the sentinel
/// values: readers trust the pairing, and a sentinel missing its entry
/// (or a stray entry masking one elsewhere) can pass every range bound
/// while serving a wrong value forever.
template <typename T>
bool escapes_ok(llvm::ArrayRef<T> values,
                T escape,
                llvm::ArrayRef<std::uint32_t> escaped,
                std::size_t escape_values) {
    if(escape_values != escaped.size()) {
        return false;
    }
    std::size_t cursor = 0;
    for(std::uint32_t entry = 0; entry < values.size(); entry += 1) {
        if(values[entry] != escape) {
            continue;
        }
        if(cursor == escaped.size() || escaped[cursor] != entry) {
            return false;
        }
        cursor += 1;
    }
    return cursor == escaped.size();
}

inline bool escapes_ok(llvm::ArrayRef<std::uint8_t> lengths,
                       llvm::ArrayRef<std::uint32_t> escaped,
                       std::size_t escape_values) {
    return escapes_ok(lengths, length_escape, escaped, escape_values);
}

/// Sentinel lengths inside the packed column, extracted for escapes_ok.
/// The no-range sentinel word shares the escape byte pattern but owns no
/// escape entry — report it as a plain length.
inline llvm::SmallVector<std::uint8_t> packed_lengths(llvm::ArrayRef<std::uint32_t> packed) {
    llvm::SmallVector<std::uint8_t> lengths;
    lengths.reserve(packed.size());
    for(auto value: packed) {
        lengths.push_back(value == packed_sentinel ? 0 : static_cast<std::uint8_t>(value & 0xff));
    }
    return lengths;
}

/// The delta columns' shape: a begin per block, a delta per entry, 0 at a
/// block's start and the escape exactly where the begin moved 255 bytes or
/// more, every escape paired with its begin.
inline bool deltas_ok(const RangeColumnsView& columns) {
    auto count = columns.deltas.size();
    if(columns.lengths.size() != count ||
       columns.block_begins.size() != (count + delta_block - 1) / delta_block ||
       !escapes_ok(columns.deltas, delta_escape, columns.far_entries, columns.far_begins.size())) {
        return false;
    }
    std::uint32_t begin = 0;
    for(std::uint32_t entry = 0; entry < count; entry += 1) {
        auto next = columns.next_begin(entry, begin);
        if(entry % delta_block == 0 ? columns.deltas[entry] != 0 || (entry != 0 && next < begin)
                                    : next < begin || (columns.deltas[entry] == delta_escape) !=
                                                          (next - begin >= delta_escape)) {
            return false;
        }
        begin = next;
    }
    return true;
}

/// Exactly one range encoding — delta for a side sorted by begin, else the
/// one the content size picks — with its escape tables paired with the
/// entries.
inline CheckResult range_encoding_ok(const RangeColumnsView& columns,
                                     std::uint32_t content_size,
                                     bool sorted) {
    bool delta_empty = columns.block_begins.empty() && columns.deltas.empty() &&
                       columns.far_entries.empty() && columns.far_begins.empty();
    bool encoding_ok = sorted
                           ? columns.packed.empty() && columns.begins.empty() && deltas_ok(columns)
                       : content_size <= packed_range_limit
                           ? delta_empty && columns.begins.empty() && columns.lengths.empty()
                           : delta_empty && columns.packed.empty() &&
                                 columns.lengths.size() == columns.begins.size();
    if(!encoding_ok) {
        return std::unexpected("range encoding is not the canonical one");
    }
    bool escaped_ok =
        columns.packed.empty()
            ? escapes_ok(columns.lengths, columns.long_entries, columns.long_ends.size())
            : escapes_ok(packed_lengths(columns.packed),
                         columns.long_entries,
                         columns.long_ends.size());
    if(!escaped_ok) {
        return std::unexpected("range escape table does not pair with its entries");
    }
    return {};
}

/// A sparse entry table: one value per listed entry, entries strictly
/// ascending and inside the entry count.
inline bool sparse_ok(llvm::ArrayRef<std::uint32_t> entries,
                      std::size_t values,
                      std::size_t count) {
    return entries.size() == values && std::ranges::is_sorted(entries, std::less_equal{}) &&
           (entries.empty() || entries.back() < count);
}

/// One id per entry in the width the table's size picks, every id inside
/// the table. The width was picked from a table size read before
/// validation, which this table size may contradict.
inline CheckResult ids_ok(const IdColumnView& ids, std::size_t count, std::size_t table_size) {
    if(ids.width != id_width(table_size) || ids.bytes.size() != count * ids.width) {
        return std::unexpected("id column does not match the entry count");
    }
    for(std::size_t i = 0; i < count; i += 1) {
        if(ids[i] >= table_size) {
            return std::unexpected("id past its table");
        }
    }
    return {};
}

/// An ascending column of `count` ids into a table of `table_size`;
/// `strict` forbids repeats.
inline CheckResult ascending_ids_ok(const IdColumnView& ids,
                                    std::size_t count,
                                    std::size_t table_size,
                                    bool strict) {
    if(auto ok = ids_ok(ids, count, table_size); !ok) {
        return ok;
    }
    for(std::size_t i = 1; i < count; i += 1) {
        if(strict ? ids[i] <= ids[i - 1] : ids[i] < ids[i - 1]) {
            return std::unexpected("id column is not ascending");
        }
    }
    return {};
}

/// The end offsets of `count` entries of a text of `text_size` bytes, back
/// to back.
inline CheckResult ends_ok(const IdColumnView& ends, std::size_t count, std::size_t text_size) {
    if(auto ok = ascending_ids_ok(ends, count, text_size + 1, false); !ok) {
        return ok;
    }
    if(count == 0 ? text_size != 0 : ends[count - 1] != text_size) {
        return std::unexpected("end offsets do not cover their text");
    }
    return {};
}

/// Appends ids in the width chosen for the whole column from its table's
/// size.
struct IdColumnWriter {
    IdColumn& column;
    std::uint8_t width;

    std::size_t size() const {
        return column.size() / width;
    }

    void add(std::uint32_t id) {
        auto at = column.size();
        column.resize(at + width);
        switch(width) {
            case 1: column[at] = static_cast<std::uint8_t>(id); break;
            case 2: {
                auto narrow = static_cast<std::uint16_t>(id);
                std::memcpy(column.data() + at, &narrow, sizeof(narrow));
                break;
            }
            default: std::memcpy(column.data() + at, &id, sizeof(id)); break;
        }
    }
};

/// A writer of `column` for ids into a table of `table_size` entries.
inline IdColumnWriter id_writer(IdColumn& column, std::size_t table_size) {
    return {.column = column, .width = id_width(table_size)};
}

/// Fill `column` with `ids` into a table of `table_size` entries.
inline void write_ids(IdColumn& column, llvm::ArrayRef<std::uint32_t> ids, std::size_t table_size) {
    auto writer = id_writer(column, table_size);
    for(auto id: ids) {
        writer.add(id);
    }
}

/// How a side's ranges are stored (RangeColumns).
enum class RangeEncoding : std::uint8_t {
    Delta,
    Packed,
    Wide,
};

/// The encoding of an unsorted side over content of `content_size` bytes.
inline RangeEncoding unsorted_encoding(std::uint32_t content_size) {
    return content_size <= packed_range_limit ? RangeEncoding::Packed : RangeEncoding::Wide;
}

/// Appends entry ranges in the encoding chosen for the whole side.
struct RangeWriter {
    RangeColumns& side;
    RangeEncoding encoding;
    std::uint32_t count = 0;
    std::uint32_t previous = 0;

    void add(LocalSourceRange range) {
        if(is_no_range(range.begin, range.end)) {
            // The no-range sentinel of pair relations, which no sorted side
            // holds; the wide columns hold it natively, the packed column
            // spells it as the reserved word.
            assert(encoding != RangeEncoding::Delta && "a sorted side holds real ranges");
            if(encoding == RangeEncoding::Packed) {
                side.packed.push_back(packed_sentinel);
            } else {
                side.begins.push_back(range.begin);
                side.lengths.push_back(0);
            }
            count += 1;
            return;
        }
        auto length = range.end - range.begin;
        std::uint8_t stored =
            length >= length_escape ? length_escape : static_cast<std::uint8_t>(length);
        if(stored == length_escape) {
            side.long_entries.push_back(count);
            side.long_ends.push_back(range.end);
        }
        switch(encoding) {
            case RangeEncoding::Delta: {
                assert((count == 0 || range.begin >= previous) && "a sorted side ascends");
                if(count % delta_block == 0) {
                    side.block_begins.push_back(range.begin);
                    side.deltas.push_back(0);
                } else if(range.begin - previous >= delta_escape) {
                    side.deltas.push_back(delta_escape);
                    side.far_entries.push_back(count);
                    side.far_begins.push_back(range.begin);
                } else {
                    side.deltas.push_back(static_cast<std::uint8_t>(range.begin - previous));
                }
                side.lengths.push_back(stored);
                previous = range.begin;
                break;
            }
            case RangeEncoding::Packed: {
                side.packed.push_back(pack_range(range.begin, stored));
                break;
            }
            case RangeEncoding::Wide: {
                side.begins.push_back(range.begin);
                side.lengths.push_back(stored);
                break;
            }
        }
        count += 1;
    }
};

}  // namespace clice::index
