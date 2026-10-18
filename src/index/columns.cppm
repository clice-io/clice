module;

#include "modules/prelude.h"

/// The column encodings the blobs share: entry ranges in whichever
/// encoding the side and the content size pick (shard and instantiation
/// blobs), id columns in whichever width their table picks (those and the
/// project table), and the load-time checks that let readers trust them.
///
/// The per-entry accessors are declared inline: in a named module a member
/// defined in its class is not, and the readers in other partitions would
/// call them once per entry.

module clice:index.columns;

import :index.format;
import :support.bitmap;
import :syntax.token;

namespace clice::index {

/// A bitmap over a portable image in place: the container payloads stay
/// in `data`, which must outlive the bitmap, and the bitmap is read-only
/// (croaring aborts on a write to a frozen one). Validated without
/// trusting the bytes — bounded by the buffer, croaring's internal
/// invariants checked — plus the container offsets the image's header carries:
/// the in-place reader follows them instead of walking the payload, so a
/// corrupt one would send it outside the image.
std::optional<Bitmap> read_bitmap(const void* data, std::size_t size);

/// Encode a bitmap as its portable image — the only format with a bounded
/// deserializer.
inline std::vector<std::byte> write_bitmap(const Bitmap& bitmap) {
    std::vector<std::byte> buffer(bitmap.getSizeInBytes(true));
    bitmap.write(reinterpret_cast<char*>(buffer.data()), true);
    return buffer;
}

/// Append `value` as a varint: seven bits a byte, low bits first, the high
/// bit set on every byte but the last.
void write_varint(std::vector<std::uint8_t>& out, std::uint64_t value);

/// The varint at `at` in `bytes`, moving `at` past it; nullopt for bytes
/// write_varint would not have written — cut short, a needless trailing
/// zero byte, more than 64 bits.
std::optional<std::uint64_t> read_varint(llvm::ArrayRef<std::uint8_t> bytes, std::size_t& at);

/// An ascending set of ids below `universe` as an Elias–Fano list, about
/// 2 + log2(universe / count) bits an id: the count as a varint; then
/// the low log2(universe / count) bits of each id, back to back; then a
/// bit vector holding, for the i-th id, a one at its remaining high bits
/// plus i. Every bit past the last of each part is zero, and the vector
/// ends with the byte holding its last one.
std::vector<std::uint8_t> write_postings(const Bitmap& ids, std::uint32_t universe);

/// The ids of a list write_postings wrote for `universe`; nullopt for
/// bytes it would not have written.
std::optional<Bitmap> read_postings(llvm::ArrayRef<std::uint8_t> bytes, std::uint32_t universe);

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
Arena slice(Arena arena, llvm::ArrayRef<std::uint32_t> ends, std::uint32_t i) {
    auto begin = i == 0 ? 0 : ends[i - 1];
    return arena.drop_front(begin).take_front(ends[i] - begin);
}

/// The position of `value` in an ascending column; nullopt when it is absent.
template <typename T>
std::optional<std::uint32_t> sorted_find(llvm::ArrayRef<std::type_identity_t<T>> column, T value) {
    auto it = std::ranges::lower_bound(column, value);
    if(it == column.end() || *it != value) {
        return std::nullopt;
    }
    return static_cast<std::uint32_t>(it - column.begin());
}

/// Whether a column ascends strictly, which sorted_find relies on.
template <std::ranges::forward_range Column>
bool strictly_ascending(Column&& column) {
    return std::ranges::adjacent_find(column, std::greater_equal{}) == std::ranges::end(column);
}

/// Load-time verification outcome: the violated invariant, spelled for the
/// log, or success.
using CheckResult = std::expected<void, llvm::StringRef>;

/// The no-range sentinel of pair relations: the default LocalSourceRange.
inline bool is_no_range(std::uint32_t begin, std::uint32_t end) {
    return LocalSourceRange{begin, end} == LocalSourceRange{};
}

/// The width in bytes of ids into a table of `table_size` entries.
inline std::uint8_t id_width(std::size_t table_size) {
    return table_size <= 0x100 ? 1 : table_size <= 0x10000 ? 2 : 4;
}

/// An id column read in the width its table picks (IdColumn).
struct IdColumnView {
    llvm::ArrayRef<std::uint8_t> bytes;
    std::uint8_t width = 1;

    inline std::size_t size() const {
        return bytes.size() / width;
    }

    inline std::uint32_t operator[](std::size_t i) const {
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
    inline llvm::StringRef slice(llvm::StringRef text, std::size_t i) const {
        auto begin = i == 0 ? 0 : (*this)[i - 1];
        return text.slice(begin, (*this)[i]);
    }
};

/// The position of escaped `entry` in its sparse table's `entries`: the
/// load-time pairing (escapes_ok) proves every escape owns exactly one entry
/// there, so the search always finds it.
inline std::size_t escape_position(llvm::ArrayRef<std::uint32_t> entries, std::size_t entry) {
    return static_cast<std::size_t>(std::ranges::lower_bound(entries, entry) - entries.begin());
}

/// One side of a blob's entries as contiguous column refs,
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

    inline std::size_t size() const {
        return packed.empty() ? lengths.size() : packed.size();
    }

    /// The begin of `entry` given the begin of the entry before it, in the
    /// delta encoding.
    inline std::uint32_t next_begin(std::uint32_t entry, std::uint32_t previous) const {
        if(entry % delta_block == 0) {
            return block_begins[entry / delta_block];
        }
        return deltas[entry] == delta_escape ? far_begins[escape_position(far_entries, entry)]
                                             : previous + deltas[entry];
    }

    /// The begin of `entry` of an unsorted side; a delta-encoded one is
    /// walked (for_each_range).
    inline std::uint32_t begin_of(std::uint32_t entry) const {
        assert(deltas.empty() && "a delta-encoded side is walked");
        if(!packed.empty()) {
            return packed[entry] == packed_sentinel ? ~std::uint32_t(0) : packed[entry] >> 8;
        }
        return begins[entry];
    }

    /// The length byte of `entry`: its length, or length_escape for one
    /// whose end the escape table holds.
    inline std::uint8_t stored_length(std::uint32_t entry) const {
        return packed.empty() ? lengths[entry] : static_cast<std::uint8_t>(packed[entry] & 0xff);
    }

    /// The end of `entry`, which begins at `begin`.
    inline std::uint32_t end_of(std::uint32_t entry, std::uint32_t begin) const {
        if(!packed.empty() && packed[entry] == packed_sentinel) {
            return ~std::uint32_t(0);
        }
        auto length = stored_length(entry);
        if(length == length_escape) {
            return long_ends[escape_position(long_entries, entry)];
        }
        return begin + length;
    }

    inline LocalSourceRange range_of(std::uint32_t entry) const {
        auto begin = begin_of(entry);
        return {begin, end_of(entry, begin)};
    }

    /// The first entry whose range `below` rejects, in a delta-encoded side
    /// where `below` holds for a prefix of the entries: a binary search over
    /// the block starts, whose begins are stored, then a scan of one block.
    template <typename Below>
    std::uint32_t partition_point(Below&& below) const {
        auto blocks =
            std::views::iota(std::uint32_t(0), static_cast<std::uint32_t>(block_begins.size()));
        auto block = static_cast<std::uint32_t>(
            std::ranges::partition_point(
                blocks,
                [&](std::uint32_t b) {
                    auto begin = block_begins[b];
                    return below(LocalSourceRange{begin, end_of(b * delta_block, begin)});
                }) -
            blocks.begin());
        if(block == 0) {
            return 0;
        }
        auto point = static_cast<std::uint32_t>(size());
        for_each_range((block - 1) * delta_block + 1,
                       [&](std::uint32_t entry, LocalSourceRange range) {
                           if(below(range)) {
                               return true;
                           }
                           point = entry;
                           return false;
                       });
        return point;
    }

    /// Visit the range of every entry from `first` on in entry order, the
    /// delta encoding decoded in one pass from the start of `first`'s block;
    /// `visit` returns false to stop.
    template <typename Visit>
    void for_each_range(std::uint32_t first, Visit&& visit) const {
        std::uint32_t begin = 0;
        for(auto entry = deltas.empty() ? first : first - first % delta_block; entry < size();
            entry += 1) {
            begin = deltas.empty() ? begin_of(entry) : next_begin(entry, begin);
            if(entry >= first && !visit(entry, LocalSourceRange{begin, end_of(entry, begin)})) {
                return;
            }
        }
    }

    template <typename Visit>
    void for_each_range(Visit&& visit) const {
        for_each_range(0, std::forward<Visit>(visit));
    }

    /// The slice bounds were validated monotonic and in-bounds at load, and
    /// every slice proven to decode, so this cannot fail.
    Bitmap bitmap_of(std::uint32_t entry) const {
        auto begin = roaring_offsets[entry];
        return *read_bitmap(roaring.data() + begin, roaring_offsets[entry + 1] - begin);
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
template <typename T, typename Escape, typename Projection = std::identity>
bool escapes_ok(llvm::ArrayRef<T> column,
                Escape escape,
                llvm::ArrayRef<std::uint32_t> escaped,
                std::size_t escape_values,
                Projection projection = {}) {
    if(escape_values != escaped.size()) {
        return false;
    }
    std::size_t cursor = 0;
    for(std::uint32_t entry = 0; entry < column.size(); entry += 1) {
        if(std::invoke(projection, column[entry]) != escape) {
            continue;
        }
        if(cursor == escaped.size() || escaped[cursor] != entry) {
            return false;
        }
        cursor += 1;
    }
    return cursor == escaped.size();
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

/// Exactly the columns of `encoding`, its escape tables paired with the
/// entries: in the delta encoding a begin per block and a delta per entry,
/// 0 at a block's start and the escape exactly where the begin moved 255
/// bytes or more; in every encoding a length escaped exactly where it is
/// 255 or more. Every range is served as a source range into the content,
/// so it lies inside it — but for the no-range sentinel of pair relations,
/// which only an unsorted side holds.
inline CheckResult range_encoding_ok(const RangeColumnsView& columns,
                                     std::uint32_t content_size,
                                     RangeEncoding encoding) {
    bool delta_empty = columns.block_begins.empty() && columns.deltas.empty() &&
                       columns.far_entries.empty() && columns.far_begins.empty();
    bool encoding_ok = false;
    switch(encoding) {
        case RangeEncoding::Delta: {
            auto count = columns.deltas.size();
            encoding_ok = columns.packed.empty() && columns.begins.empty() &&
                          columns.lengths.size() == count &&
                          columns.block_begins.size() == (count + delta_block - 1) / delta_block &&
                          escapes_ok(columns.deltas,
                                     delta_escape,
                                     columns.far_entries,
                                     columns.far_begins.size());
            break;
        }
        case RangeEncoding::Packed: {
            encoding_ok = delta_empty && columns.begins.empty() && columns.lengths.empty();
            break;
        }
        case RangeEncoding::Wide: {
            encoding_ok = delta_empty && columns.packed.empty() &&
                          columns.lengths.size() == columns.begins.size();
            break;
        }
    }
    if(!encoding_ok) {
        return std::unexpected("range encoding is not the canonical one");
    }
    // The no-range sentinel word shares the escape byte but owns no escape
    // entry: it reads as a plain length.
    auto packed_length = [](std::uint32_t word) {
        return word == packed_sentinel ? std::uint8_t(0) : static_cast<std::uint8_t>(word & 0xff);
    };
    bool escaped_ok = columns.packed.empty() ? escapes_ok(columns.lengths,
                                                          length_escape,
                                                          columns.long_entries,
                                                          columns.long_ends.size())
                                             : escapes_ok(columns.packed,
                                                          length_escape,
                                                          columns.long_entries,
                                                          columns.long_ends.size(),
                                                          packed_length);
    if(!escaped_ok) {
        return std::unexpected("range escape table does not pair with its entries");
    }
    std::uint32_t previous = 0;
    CheckResult result;
    columns.for_each_range([&](std::uint32_t entry, LocalSourceRange range) {
        if(encoding == RangeEncoding::Delta) {
            auto delta = columns.deltas[entry];
            if((entry != 0 && range.begin < previous) ||
               (entry % delta_block == 0
                    ? delta != 0
                    : (delta == delta_escape) != (range.begin - previous >= delta_escape))) {
                result = std::unexpected("begin deltas are not the canonical ones");
                return false;
            }
            previous = range.begin;
        } else if(is_no_range(range.begin, range.end)) {
            return true;
        }
        if(range.end < range.begin || range.end > content_size) {
            result = std::unexpected("range past the content");
        } else if(columns.stored_length(entry) == length_escape && range.length() < length_escape) {
            result = std::unexpected("escaped length below the escape threshold");
        }
        return result.has_value();
    });
    return result;
}

/// A sparse entry table: one value per listed entry, entries strictly
/// ascending and inside the entry count.
inline bool sparse_ok(llvm::ArrayRef<std::uint32_t> entries,
                      std::size_t values,
                      std::size_t count) {
    return entries.size() == values && strictly_ascending(entries) &&
           (entries.empty() || entries.back() < count);
}

/// One id per entry, every id inside the table.
inline CheckResult ids_ok(const IdColumnView& ids, std::size_t count, std::size_t table_size) {
    if(ids.bytes.size() != count * ids.width) {
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
