module;

#include "modules/prelude.h"

/// The column encodings the per-file blobs share — shard blobs and
/// instantiation blobs: entry ranges in whichever encoding the content size
/// picks, symbol-id columns in whichever width the symbol table picks, and
/// the load-time checks that let readers trust them.

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

/// An id column in whichever width the blob stores; validation proves
/// exactly one width is present.
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

    /// The first position whose id is not below `id`, in an ascending
    /// column.
    std::size_t lower_bound(std::uint32_t id) const {
        auto positions = std::views::iota(std::size_t(0), size());
        auto it =
            std::ranges::lower_bound(positions, id, {}, [&](std::size_t i) { return (*this)[i]; });
        return static_cast<std::size_t>(it - positions.begin());
    }

    /// Where an ascending column holds `id`.
    std::optional<std::size_t> find(std::uint32_t id) const {
        auto i = lower_bound(id);
        if(i == size() || (*this)[i] != id) {
            return std::nullopt;
        }
        return i;
    }

    /// Entry `i` of a text whose entries end at the offsets of this column,
    /// back to back.
    llvm::StringRef slice(llvm::StringRef text, std::size_t i) const {
        return text.slice(i == 0 ? 0 : (*this)[i - 1], (*this)[i]);
    }
};

/// One side of a blob's entry storage as contiguous column refs,
/// encoding-agnostic: the range accessors read whichever range encoding the
/// blob stores, the mask columns whichever mask encoding.
struct RangeColumnsView {
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
        return packed.empty() ? begins.size() : packed.size();
    }

    std::uint32_t begin_of(std::uint32_t entry) const {
        if(packed.empty()) {
            return begins[entry];
        }
        return packed[entry] == packed_sentinel ? ~std::uint32_t(0) : packed[entry] >> 8;
    }

    std::uint32_t end_of(std::uint32_t entry) const {
        if(!packed.empty() && packed[entry] == packed_sentinel) {
            return ~std::uint32_t(0);
        }
        auto length =
            packed.empty() ? lengths[entry] : static_cast<std::uint8_t>(packed[entry] & 0xff);
        if(length == length_escape) {
            // range_encoding_ok() proves every sentinel owns exactly one
            // escape entry, so the search always finds it.
            auto it = std::ranges::lower_bound(long_entries, entry);
            return long_ends[it - long_entries.begin()];
        }
        return begin_of(entry) + length;
    }

    LocalSourceRange range_of(std::uint32_t entry) const {
        return {begin_of(entry), end_of(entry)};
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

inline SymIds ids_of(kota::codec::fbs::table_view<IdColumn> view) {
    if(!view.valid()) {
        return {};
    }
    return {
        to_array_ref(view[&IdColumn::ids8]),
        to_array_ref(view[&IdColumn::ids16]),
        to_array_ref(view[&IdColumn::ids32]),
    };
}

/// The escape table must pair one-to-one, in column order, with the sentinel
/// lengths: readers trust the pairing, and a sentinel missing its entry
/// (or a stray entry masking one elsewhere) can pass every range bound
/// while serving a wrong value forever.
inline bool escapes_ok(llvm::ArrayRef<std::uint8_t> lengths,
                       llvm::ArrayRef<std::uint32_t> escaped,
                       std::size_t escape_values) {
    if(escape_values != escaped.size()) {
        return false;
    }
    std::size_t cursor = 0;
    for(std::uint32_t entry = 0; entry < lengths.size(); entry += 1) {
        if(lengths[entry] != length_escape) {
            continue;
        }
        if(cursor == escaped.size() || escaped[cursor] != entry) {
            return false;
        }
        cursor += 1;
    }
    return cursor == escaped.size();
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

/// Exactly one range encoding, chosen by the content size, with its escape
/// table paired with the entries.
inline CheckResult range_encoding_ok(const RangeColumnsView& columns, std::uint32_t content_size) {
    bool encoding_ok =
        content_size <= packed_range_limit
            ? columns.begins.empty() && columns.lengths.empty()
            : columns.packed.empty() && columns.lengths.size() == columns.begins.size();
    if(!encoding_ok) {
        return std::unexpected("range tier is not the canonical one");
    }
    bool escaped_ok =
        columns.packed.empty()
            ? escapes_ok(columns.lengths, columns.long_entries, columns.long_ends.size())
            : escapes_ok(packed_lengths(columns.packed),
                         columns.long_entries,
                         columns.long_ends.size());
    if(!escaped_ok) {
        return std::unexpected("range escape table does not pair with its rows");
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

/// Exactly one id width, chosen by the table's size, one id per entry and
/// every id inside the table.
inline CheckResult sym_ids_ok(const SymIds& ids, std::size_t count, std::size_t table_size) {
    if(ids.size() != count) {
        return std::unexpected("id column does not match the entry count");
    }
    bool width_ok = table_size <= 0x100     ? ids.ids16.empty() && ids.ids32.empty()
                    : table_size <= 0x10000 ? ids.ids8.empty() && ids.ids32.empty()
                                            : ids.ids8.empty() && ids.ids16.empty();
    if(!width_ok) {
        return std::unexpected("id width is not the canonical one");
    }
    auto in_table = [&](auto column) {
        return llvm::all_of(column, [&](std::uint32_t id) { return id < table_size; });
    };
    if(!in_table(ids.ids8) || !in_table(ids.ids16) || !in_table(ids.ids32)) {
        return std::unexpected("id past its table");
    }
    return {};
}

/// Appends entry ranges in the encoding the content size picks for the
/// whole blob.
struct RangeWriter {
    RangeColumns& side;
    bool narrow;
    std::uint32_t count = 0;

    void add(LocalSourceRange range) {
        if(is_no_range(range.begin, range.end)) {
            // The no-range sentinel of pair relations; the wide columns hold
            // it natively, the packed column spells it as the reserved word.
            if(narrow) {
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
        if(narrow) {
            side.packed.push_back(pack_range(range.begin, stored));
        } else {
            side.begins.push_back(range.begin);
            side.lengths.push_back(stored);
        }
        count += 1;
    }
};

/// An ascending column of `count` ids into a table of `table_size`;
/// `strict` forbids repeats.
inline CheckResult
    ascending_ids_ok(const SymIds& ids, std::size_t count, std::size_t table_size, bool strict) {
    if(auto ok = sym_ids_ok(ids, count, table_size); !ok) {
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
inline CheckResult ends_ok(const SymIds& ends, std::size_t count, std::size_t text_size) {
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

/// A writer of `column` for ids into a table of `table_size` entries.
inline SymIdWriter id_writer(IdColumn& column, std::size_t table_size) {
    return {.width = SymIdWriter::width_for(table_size),
            .ids8 = column.ids8,
            .ids16 = column.ids16,
            .ids32 = column.ids32};
}

/// Fill `column` with `ids` into a table of `table_size` entries.
inline void write_ids(IdColumn& column, llvm::ArrayRef<std::uint32_t> ids, std::size_t table_size) {
    auto writer = id_writer(column, table_size);
    for(auto id: ids) {
        writer.add(id);
    }
}

}  // namespace clice::index
