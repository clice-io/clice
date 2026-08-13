#pragma once

#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "index/path_pool.h"
#include "index/tu_index.h"
#include "semantic/symbol.h"
#include "support/bitmap.h"

#include "kota/codec/fbs/fbs.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/raw_ostream.h"

namespace kota::meta {

/// Roaring bitmaps travel as their own serialized image (the non-portable
/// format, matching every in-process reader).
template <>
struct repr<clice::Bitmap, codec::fbs::format> {
    using type = std::vector<std::byte>;

    static type to(const clice::Bitmap& bitmap) {
        type buffer(bitmap.getSizeInBytes(false));
        bitmap.write(reinterpret_cast<char*>(buffer.data()), false);
        return buffer;
    }

    static clice::Bitmap from(const type& buffer) {
        return clice::Bitmap::read(reinterpret_cast<const char*>(buffer.data()), false);
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

/// A PathPool persists as its path table; ids are the dense indices, so
/// interning the table back in order reproduces them. Both directions drive
/// the visitor: encoding writes the interned StringRefs straight to the
/// wire, decoding interns one path at a time.
template <>
struct repr<clice::index::PathPool, codec::fbs::format> {
    using type = std::vector<std::string>;

    template <typename Config>
    static bool serialize(auto& vis, const clice::index::PathPool& pool) {
        return codec::encode_value<Config>(vis, pool.paths);
    }

    template <typename Config>
    static bool deserialize(auto& vis, clice::index::PathPool& pool) {
        type shape;
        return vis.visit_seq(shape, [&](auto& sv) -> bool {
            while(sv.has_element()) {
                std::string path;
                if(!sv.visit_element(
                       [&](auto& ev) -> bool { return codec::decode_value<Config>(ev, path); })) {
                    return false;
                }
                // The pool never interns an empty path, so a blob carrying
                // one is corrupt; reject it instead of tripping the intern
                // precondition.
                if(path.empty()) {
                    return false;
                }
                pool.path_id(path);
            }
            return true;
        });
    }
};

/// A StringMap iterates as StringMapEntry, which no codec understands;
/// persist the entries as key/value pairs, sorted by value for
/// deterministic blobs (values are unique canonical ids). Format-agnostic,
/// unlike the reprs above: the pair-list form is not fbs-specific, and the
/// schema layer classifies fields without a format tag — a format-scoped
/// repr would leave it staring at StringMapEntry, which it rejects.
template <>
struct repr<llvm::StringMap<std::uint32_t>> {
    using type = std::vector<std::pair<std::string, std::uint32_t>>;

    template <typename Config>
    static bool serialize(auto& vis, const llvm::StringMap<std::uint32_t>& map) {
        llvm::SmallVector<std::pair<llvm::StringRef, std::uint32_t>> entries;
        entries.reserve(map.size());
        for(const auto& entry: map) {
            entries.emplace_back(entry.getKey(), entry.getValue());
        }
        llvm::sort(entries, llvm::less_second{});
        return codec::encode_value<Config>(vis, entries);
    }

    template <typename Config>
    static bool deserialize(auto& vis, llvm::StringMap<std::uint32_t>& map) {
        type shape;
        return vis.visit_seq(shape, [&](auto& sv) -> bool {
            while(sv.has_element()) {
                std::pair<std::string, std::uint32_t> entry;
                if(!sv.visit_element(
                       [&](auto& ev) -> bool { return codec::decode_value<Config>(ev, entry); })) {
                    return false;
                }
                map.try_emplace(entry.first, entry.second);
            }
            return true;
        });
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
constexpr inline std::uint32_t index_format_version = 3;

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
