#pragma once

#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "index/path_pool.h"
#include "index/shared.h"
#include "semantic/relation_kind.h"
#include "semantic/symbol_kind.h"
#include "support/bitmap.h"

#include "kota/codec/fbs/fbs.h"
#include "llvm/Support/raw_ostream.h"

namespace clice::index {

/// Encode a reflected object and write the blob to `os`.
template <typename T>
void write_flatbuffer(llvm::raw_ostream& os, const T& value) {
    auto encoded = kota::codec::fbs::to_flatbuffer(value);
    assert(encoded && "flatbuffer serialization failed");
    if(encoded) {
        os.write(reinterpret_cast<const char*>(encoded->data()), encoded->size());
    }
}

/// Deep-verify and decode a blob into `out`; false on any malformed input.
template <typename T>
bool read_flatbuffer(const void* data, std::size_t size, T& out) {
    if(data == nullptr || size == 0) {
        return false;
    }
    std::span<const std::uint8_t> bytes(static_cast<const std::uint8_t*>(data), size);
    return static_cast<bool>(kota::codec::fbs::from_flatbuffer(bytes, out));
}

}  // namespace clice::index

/// Value-mode codec adapters bridging clice's non-reflectable leaf types onto
/// kotatsu's visitor framework. Each specialization declares the on-wire
/// layout (wire_type) plus the two conversions, and applies wherever the type
/// appears — struct field, map key/value or sequence element. The wire_type
/// also drives the FlatBuffers vector layout and verify_flatbuffer's schema
/// walk.
namespace kota::codec {

template <typename Vis, typename Config>
struct serialize_visit<Vis, clice::RelationKind, Config> {
    using wire_type = std::uint32_t;

    static wire_type to_wire(clice::RelationKind kind) {
        return kind.value();
    }

    static clice::RelationKind from_wire(wire_type value) {
        return clice::RelationKind(static_cast<clice::RelationKind::Kind>(value));
    }
};

template <typename Vis, typename Config>
struct serialize_visit<Vis, clice::SymbolKind, Config> {
    using wire_type = std::uint8_t;

    static wire_type to_wire(clice::SymbolKind kind) {
        return kind.value();
    }

    static clice::SymbolKind from_wire(wire_type value) {
        return clice::SymbolKind(static_cast<clice::SymbolKind::Kind>(value));
    }
};

template <typename Vis, typename Config>
struct serialize_visit<Vis, std::chrono::milliseconds, Config> {
    using wire_type = std::int64_t;

    static wire_type to_wire(std::chrono::milliseconds value) {
        return value.count();
    }

    static std::chrono::milliseconds from_wire(wire_type ticks) {
        return std::chrono::milliseconds(ticks);
    }
};

/// Roaring bitmaps travel as their non-portable serialized bytes, matching the
/// `write(..., false)` format the flatc-era schema used. An empty bitmap is
/// written as an empty byte vector.
template <typename Vis, typename Config>
struct serialize_visit<Vis, roaring::Roaring, Config> {
    using wire_type = std::vector<std::byte>;

    static wire_type to_wire(const roaring::Roaring& bitmap) {
        std::vector<std::byte> bytes;
        if(!bitmap.isEmpty()) {
            bytes.resize(bitmap.getSizeInBytes(false));
            bitmap.write(reinterpret_cast<char*>(bytes.data()), false);
        }
        return bytes;
    }

    static roaring::Roaring from_wire(const wire_type& bytes) {
        if(bytes.empty()) {
            return roaring::Roaring();
        }
        return roaring::Roaring::read(reinterpret_cast<const char*>(bytes.data()), false);
    }
};

/// A PathPool travels as its path table (a vector of strings, in id order);
/// decoding re-interns every entry so the cache and ids are rebuilt.
template <typename Vis, typename Config>
struct serialize_visit<Vis, clice::index::PathPool, Config> {
    using wire_type = std::vector<std::string>;

    static std::vector<std::string_view> to_wire(const clice::index::PathPool& pool) {
        std::vector<std::string_view> paths;
        paths.reserve(pool.paths.size());
        for(auto path: pool.paths) {
            paths.emplace_back(path.data(), path.size());
        }
        return paths;
    }

    static clice::index::PathPool from_wire(const wire_type& paths) {
        clice::index::PathPool pool;
        for(const auto& path: paths) {
            if(!path.empty()) {
                pool.path_id(path);
            }
        }
        return pool;
    }
};

}  // namespace kota::codec
