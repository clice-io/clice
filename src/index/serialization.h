#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "index/path_pool.h"
#include "index/shared.h"
#include "semantic/relation_kind.h"
#include "semantic/symbol_kind.h"
#include "support/bitmap.h"

#include "kota/codec/fbs/fbs.h"

/// Type-level codec adapters bridging clice's non-reflectable leaf types onto
/// kotatsu's visitor framework. They are keyed on the value type only, so they
/// apply wherever the type appears — as a struct field, a map key/value or a
/// sequence element — and the declared wire_type drives both the FlatBuffers
/// vector layout and verify_flatbuffer's schema walk.
namespace kota::codec {

template <typename Vis, typename Config>
struct serialize_visit<Vis, clice::RelationKind, Config> {
    using wire_type = std::uint32_t;

    static bool visit(Vis& vis, const clice::RelationKind& kind) {
        return encode_value<Config>(vis, kind.value());
    }
};

template <typename Vis, typename Config>
struct deserialize_visit<Vis, clice::RelationKind, Config> {
    static bool visit(Vis& vis, clice::RelationKind& out) {
        std::uint32_t value = 0;
        if(!decode_value<Config>(vis, value)) {
            return false;
        }
        out = clice::RelationKind(static_cast<clice::RelationKind::Kind>(value));
        return true;
    }
};

template <typename Vis, typename Config>
struct serialize_visit<Vis, clice::SymbolKind, Config> {
    using wire_type = std::uint8_t;

    static bool visit(Vis& vis, const clice::SymbolKind& kind) {
        return encode_value<Config>(vis, kind.value());
    }
};

template <typename Vis, typename Config>
struct deserialize_visit<Vis, clice::SymbolKind, Config> {
    static bool visit(Vis& vis, clice::SymbolKind& out) {
        std::uint8_t value = 0;
        if(!decode_value<Config>(vis, value)) {
            return false;
        }
        out = clice::SymbolKind(static_cast<clice::SymbolKind::Kind>(value));
        return true;
    }
};

/// Roaring bitmaps travel as their non-portable serialized bytes, matching the
/// `write(..., false)` format the flatc-era schema used. An empty bitmap is
/// written as an empty byte vector.
template <typename Vis, typename Config>
struct serialize_visit<Vis, roaring::Roaring, Config> {
    using wire_type = std::vector<std::byte>;

    static bool visit(Vis& vis, const roaring::Roaring& bitmap) {
        std::vector<std::byte> bytes;
        if(!bitmap.isEmpty()) {
            bytes.resize(bitmap.getSizeInBytes(false));
            bitmap.write(reinterpret_cast<char*>(bytes.data()), false);
        }
        return encode_value<Config>(vis, bytes);
    }
};

template <typename Vis, typename Config>
struct deserialize_visit<Vis, roaring::Roaring, Config> {
    static bool visit(Vis& vis, roaring::Roaring& out) {
        std::vector<std::byte> bytes;
        if(!decode_value<Config>(vis, bytes)) {
            return false;
        }
        if(bytes.empty()) {
            out = roaring::Roaring();
        } else {
            out = roaring::Roaring::read(reinterpret_cast<const char*>(bytes.data()), false);
        }
        return true;
    }
};

template <typename Vis, typename Config>
struct serialize_visit<Vis, std::chrono::milliseconds, Config> {
    using wire_type = std::int64_t;

    static bool visit(Vis& vis, const std::chrono::milliseconds& value) {
        return encode_value<Config>(vis, static_cast<std::int64_t>(value.count()));
    }
};

template <typename Vis, typename Config>
struct deserialize_visit<Vis, std::chrono::milliseconds, Config> {
    static bool visit(Vis& vis, std::chrono::milliseconds& out) {
        std::int64_t ticks = 0;
        if(!decode_value<Config>(vis, ticks)) {
            return false;
        }
        out = std::chrono::milliseconds(ticks);
        return true;
    }
};

/// A PathPool travels as its path table (a vector of strings, in id order);
/// decoding re-interns every entry so the cache and ids are rebuilt.
template <typename Vis, typename Config>
struct serialize_visit<Vis, clice::index::PathPool, Config> {
    using wire_type = std::vector<std::string>;

    static bool visit(Vis& vis, const clice::index::PathPool& pool) {
        std::vector<std::string_view> paths;
        paths.reserve(pool.paths.size());
        for(auto path: pool.paths) {
            paths.emplace_back(path.data(), path.size());
        }
        return encode_value<Config>(vis, paths);
    }
};

template <typename Vis, typename Config>
struct deserialize_visit<Vis, clice::index::PathPool, Config> {
    static bool visit(Vis& vis, clice::index::PathPool& out) {
        std::vector<std::string> paths;
        if(!decode_value<Config>(vis, paths)) {
            return false;
        }
        out.paths.clear();
        out.cache.clear();
        for(const auto& path: paths) {
            if(!path.empty()) {
                out.path_id(path);
            }
        }
        return true;
    }
};

}  // namespace kota::codec
