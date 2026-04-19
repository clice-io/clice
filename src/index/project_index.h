#pragma once

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "index/tu_index.h"

#include "kota/codec/arena/traits.h"
#include "kota/codec/detail/fwd.h"
#include "kota/support/expected_try.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Allocator.h"

namespace clice::index {

struct PathPool {
    llvm::BumpPtrAllocator allocator;

    std::vector<llvm::StringRef> paths;

    llvm::DenseMap<llvm::StringRef, std::uint32_t> cache;

    llvm::StringRef save(llvm::StringRef s) {
        auto data = allocator.Allocate<char>(s.size() + 1);
        std::ranges::copy(s, data);
        data[s.size()] = '\0';
        return llvm::StringRef(data, s.size());
    }

    auto path_id(llvm::StringRef path) {
        assert(!path.empty());

        // Normalize backslashes to forward slashes so that paths from different
        // sources (URI decoding, CDB, clang FileManager) compare equal on
        // Windows where native separators are backslashes.
        llvm::SmallString<256> normalized;
        if(path.contains('\\')) {
            normalized = path;
            std::replace(normalized.begin(), normalized.end(), '\\', '/');
            path = normalized;
        }

        auto [it, success] = cache.try_emplace(path, paths.size());
        if(!success) {
            return it->second;
        }

        auto& [k, v] = *it;
        k = save(path);
        paths.emplace_back(k);
        return it->second;
    }

    llvm::StringRef path(std::uint32_t id) {
        return paths[id];
    }

    /// Look up a path in the cache, normalizing backslashes first.
    /// Returns cache.end() if the path is not interned.
    auto find(llvm::StringRef path) {
        llvm::SmallString<256> normalized;
        if(path.contains('\\')) {
            normalized = path;
            std::replace(normalized.begin(), normalized.end(), '\\', '/');
            path = normalized;
        }
        return cache.find(path);
    }
};

struct FileInfo {
    std::int64_t mtime;
};

struct ProjectIndex {
    PathPool path_pool;

    llvm::DenseMap<std::uint32_t, std::uint32_t> indices;

    SymbolTable symbols;

    llvm::SmallVector<std::uint32_t> merge(this ProjectIndex& self, TUIndex& index);

    void serialize(this ProjectIndex& self, llvm::raw_ostream& os);

    static ProjectIndex from(const void* data, std::size_t size);
};

}  // namespace clice::index

namespace kota::codec {

/// `PathPool` on the wire is a flat list of absolute paths; `id` is the
/// position in the vector. The allocator and reverse cache are runtime-only.
///
/// Streaming serialize: iterate `pool.paths` and allocate strings directly
/// into the builder, avoiding the double-copy that a value-mode
/// `wire_type = std::vector<std::string>` conversion would introduce.
template <typename S>
    requires arena::arena_serializer_like<S>
struct serialize_traits<S, clice::index::PathPool> {
    // Structural wire shape — declared so the flatbuffers proxy views
    // a `PathPool` field as an `array_view<std::string>`.
    using wire_type = std::vector<std::string>;

    static auto serialize(S& s, const clice::index::PathPool& pool)
        -> std::expected<typename S::vector_ref, typename S::error_type> {
        std::vector<typename S::string_ref> offsets;
        offsets.reserve(pool.paths.size());
        for(const auto& path: pool.paths) {
            auto r = s.alloc_string(std::string_view(path.data(), path.size()));
            if(!r) {
                return std::unexpected(r.error());
            }
            offsets.push_back(*r);
        }
        return s.alloc_string_vector(
            std::span<const typename S::string_ref>(offsets.data(), offsets.size()));
    }
};

/// Streaming deserialize: read each path out of the flatbuffer's
/// string-vector view directly, interning it into the pool's allocator
/// in-place. Avoids the transient `std::vector<std::string>` the
/// value-mode form would materialize.
template <typename D>
    requires arena::arena_deserializer_like<D>
struct deserialize_traits<D, clice::index::PathPool> {
    using wire_type = std::vector<std::string>;

    static auto deserialize(const D& d,
                            typename D::TableView view,
                            typename D::slot_id sid,
                            clice::index::PathPool& out)
        -> std::expected<void, typename D::error_type> {
        if(!view.has(sid)) {
            return {};
        }
        KOTA_EXPECTED_TRY_V(auto vec, d.get_string_vector(view, sid));
        out.paths.resize(vec.size());
        for(std::size_t i = 0; i < vec.size(); ++i) {
            auto sv = vec[i];
            llvm::SmallString<256> normalized(llvm::StringRef(sv.data(), sv.size()));
            std::replace(normalized.begin(), normalized.end(), '\\', '/');
            auto interned = out.save(normalized.str());
            out.paths[i] = interned;
            out.cache.try_emplace(interned, static_cast<std::uint32_t>(i));
        }
        return {};
    }
};

}  // namespace kota::codec
