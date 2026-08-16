#pragma once

#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "index/tu_index.h"
#include "semantic/symbol.h"
#include "support/bitmap.h"

#include "kota/codec/fbs/fbs.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/raw_ostream.h"

namespace clice::index {

/// Decode a serialized bitmap without trusting its bytes: bounded by the
/// buffer, nullopt on a failed parse — croaring's C++ read wrappers abort
/// on one, and blob bitmaps are untrusted disk/wire input. The caller
/// chooses what a failure means: anything feeding persisted state (the
/// project merge, blob loaders) rejects the whole input — normalized to
/// empty, reference bits would read as fresh and stay lost forever —
/// while the session-scoped full decode degrades to an empty bitmap,
/// rebuilt by the next parse.
inline std::optional<Bitmap> read_bitmap(const void* data, std::size_t size) {
    auto* decoded =
        roaring::api::roaring_bitmap_portable_deserialize_safe(static_cast<const char*>(data),
                                                               size);
    if(!decoded) {
        return std::nullopt;
    }
    // deserialize_safe only bounds the reads; the bitmap it hands back can
    // still violate internal invariants (unsorted containers), on which
    // croaring's operations are undefined.
    if(!roaring::api::roaring_bitmap_internal_validate(decoded, nullptr)) {
        roaring::api::roaring_bitmap_free(decoded);
        return std::nullopt;
    }
    return Bitmap(decoded);
}

/// Encode a bitmap as its portable image — the only format with a bounded
/// deserializer.
inline std::vector<std::byte> write_bitmap(const Bitmap& bitmap) {
    std::vector<std::byte> buffer(bitmap.getSizeInBytes(true));
    bitmap.write(reinterpret_cast<char*>(buffer.data()), true);
    return buffer;
}

}  // namespace clice::index

namespace kota::meta {

/// Roaring bitmaps travel in the portable format — the only one with a
/// bounded deserializer. Only the session-scoped full decode goes through
/// this repr (it has no failure channel, so a malformed image degrades to
/// empty); the merge path and persisted blobs read raw images and reject
/// unparseable ones.
template <>
struct repr<clice::Bitmap, codec::fbs::format> {
    using type = std::vector<std::byte>;

    static type to(const clice::Bitmap& bitmap) {
        return clice::index::write_bitmap(bitmap);
    }

    static clice::Bitmap from(const type& buffer) {
        return clice::index::read_bitmap(buffer.data(), buffer.size()).value_or(clice::Bitmap{});
    }
};

/// SymbolKind hides its enum behind constructors, which keeps it out of
/// reflection; persist the underlying value.
template <>
struct repr<clice::SymbolKind, codec::fbs::format> {
    using type = std::uint8_t;

    static type to(clice::SymbolKind kind) {
        return kind.value();
    }

    static clice::SymbolKind from(type value) {
        return clice::SymbolKind(value);
    }
};

template <>
struct repr<std::chrono::milliseconds, codec::fbs::format> {
    using type = std::int64_t;

    static type to(std::chrono::milliseconds ms) {
        return ms.count();
    }

    static std::chrono::milliseconds from(type count) {
        return std::chrono::milliseconds(count);
    }
};

}  // namespace kota::meta

namespace clice::index {

/// On-disk index blob schema version. Every persisted blob carries it as a
/// regular field and every loader discards blobs with a different value —
/// including version-less blobs from older builds, which read back as 0.
/// Bump it whenever a persisted type's reflected layout changes.
constexpr inline std::uint32_t index_format_version = 5;

/// Serialize a reflected index blob to `os` as a verified-readable
/// flatbuffer. Encoding only fails on structural impossibilities (e.g. more
/// fields than slots), which the persisted index types cannot hit.
template <typename T>
void serialize_blob(const T& value, llvm::raw_ostream& os) {
    auto encoded = kota::codec::fbs::to_bytes(value);
    assert(encoded.has_value());
    os.write(reinterpret_cast<const char*>(encoded->data()), encoded->size());
}

/// The bytes of `data` as the span every kota fbs entry point takes.
inline std::span<const std::uint8_t> blob_bytes(llvm::StringRef data) {
    return {reinterpret_cast<const std::uint8_t*>(data.data()), data.size()};
}

/// Verify and deserialize a blob into `out`. The out-parameter overload of
/// from_bytes is deliberate: index types hold llvm::DenseMap members whose
/// explicit default constructors fail std::default_initializable, which the
/// value-returning overload requires.
template <typename T>
bool deserialize_blob(llvm::StringRef data, T& out) {
    return kota::codec::fbs::from_bytes(blob_bytes(data), out).has_value();
}

/// Scan the occurrences containing `offset` in a sequence sorted by
/// (range.begin, range.end, target): binary-search the first entry whose
/// range ends at or past the offset, then walk while ranges contain it.
/// Binary-searching on range.end is sound because occurrence ranges are
/// name-token spans, pairwise disjoint or identical — never partially
/// overlapping or nested — so under this order range.end is monotonic too.
/// `get(i)` yields the i-th Occurrence.
template <typename GetOccurrence>
void scan_occurrences_at(std::size_t size,
                         std::uint32_t offset,
                         GetOccurrence&& get,
                         llvm::function_ref<bool(const Occurrence&)> callback) {
    std::size_t lo = 0;
    std::size_t hi = size;
    while(lo < hi) {
        auto mid = lo + (hi - lo) / 2;
        if(get(mid).range.end < offset) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }

    for(; lo < size; ++lo) {
        Occurrence occurrence = get(lo);
        if(!occurrence.range.contains(offset)) {
            break;
        }
        if(!callback(occurrence)) {
            break;
        }
    }
}

inline llvm::StringRef to_ref(std::string_view text) {
    return {text.data(), text.size()};
}

/// A scalar array view as an ArrayRef borrowing the mapped blob.
template <typename T>
llvm::ArrayRef<T> to_array_ref(kota::codec::fbs::array_view<T> view) {
    if(!view.valid()) {
        return {};
    }
    return {view.raw()->data(), view.raw()->size()};
}

}  // namespace clice::index
