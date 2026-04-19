#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

#include "semantic/relation_kind.h"
#include "semantic/symbol_kind.h"
#include "support/bitmap.h"

#include "kota/codec/arena/traits.h"
#include "kota/codec/detail/fwd.h"

/// Type-level wire traits for clice index types.
///
/// These partially specialize the primary
/// `kota::codec::serialize_traits<S, T>` / `deserialize_traits<D, T>`
/// templates, constrained so only arena backends pick them up. They
/// declare the wire representation for `T` and propagate through map
/// values, sequence elements, and nested containers — no per-field
/// `annotation<T, with<...>>` required.

namespace kota::codec {

/// `std::chrono::milliseconds` ⇄ `int64` tick count.
template <typename S>
    requires arena::arena_serializer_like<S>
struct serialize_traits<S, std::chrono::milliseconds> {
    using wire_type = std::int64_t;

    static std::int64_t serialize(S&, std::chrono::milliseconds value) noexcept {
        return value.count();
    }
};

template <typename D>
    requires arena::arena_deserializer_like<D>
struct deserialize_traits<D, std::chrono::milliseconds> {
    using wire_type = std::int64_t;

    static std::chrono::milliseconds deserialize(const D&, std::int64_t value) noexcept {
        return std::chrono::milliseconds(value);
    }
};

/// `RelationKind` ⇄ underlying `uint32` bitflags.
template <typename S>
    requires arena::arena_serializer_like<S>
struct serialize_traits<S, clice::RelationKind> {
    using wire_type = std::uint32_t;

    static std::uint32_t serialize(S&, const clice::RelationKind& k) noexcept {
        return k.value();
    }
};

template <typename D>
    requires arena::arena_deserializer_like<D>
struct deserialize_traits<D, clice::RelationKind> {
    using wire_type = std::uint32_t;

    static clice::RelationKind deserialize(const D&, std::uint32_t v) noexcept {
        return clice::RelationKind(static_cast<clice::RelationKind::Kind>(v));
    }
};

/// `SymbolKind` ⇄ underlying `uint8`.
template <typename S>
    requires arena::arena_serializer_like<S>
struct serialize_traits<S, clice::SymbolKind> {
    using wire_type = std::uint8_t;

    static std::uint8_t serialize(S&, const clice::SymbolKind& k) noexcept {
        return k.value();
    }
};

template <typename D>
    requires arena::arena_deserializer_like<D>
struct deserialize_traits<D, clice::SymbolKind> {
    using wire_type = std::uint8_t;

    static clice::SymbolKind deserialize(const D&, std::uint8_t v) noexcept {
        return clice::SymbolKind(v);
    }
};

/// `clice::Bitmap` (= `roaring::Roaring`) ⇄ opaque byte blob produced by
/// Roaring's non-portable serialization (matches the legacy wire format).
template <typename S>
    requires arena::arena_serializer_like<S>
struct serialize_traits<S, clice::Bitmap> {
    using wire_type = std::vector<std::byte>;

    static std::vector<std::byte> serialize(S&, const clice::Bitmap& bitmap) {
        std::vector<std::byte> buffer;
        if(bitmap.isEmpty()) {
            return buffer;
        }
        buffer.resize(bitmap.getSizeInBytes(false));
        bitmap.write(reinterpret_cast<char*>(buffer.data()), false);
        return buffer;
    }
};

template <typename D>
    requires arena::arena_deserializer_like<D>
struct deserialize_traits<D, clice::Bitmap> {
    using wire_type = std::vector<std::byte>;

    static clice::Bitmap deserialize(const D&, std::vector<std::byte> bytes) {
        if(bytes.empty()) {
            return clice::Bitmap();
        }
        return clice::Bitmap::read(reinterpret_cast<const char*>(bytes.data()), false);
    }
};

}  // namespace kota::codec
