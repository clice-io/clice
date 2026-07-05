#pragma once

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <vector>

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Allocator.h"

namespace clice::index {

/// Intern pool mapping paths to dense local ids. Used for the self-contained
/// path tables of serialized index artifacts: every persisted path id is an
/// index into its artifact's own table, never a runtime pool id (those are
/// per-session).
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

    llvm::StringRef path(std::uint32_t id) const {
        return paths[id];
    }

    /// Look up a path in the cache, normalizing backslashes first.
    /// Returns cache.end() if the path is not interned.
    auto find(llvm::StringRef path) const {
        llvm::SmallString<256> normalized;
        if(path.contains('\\')) {
            normalized = path;
            std::replace(normalized.begin(), normalized.end(), '\\', '/');
            path = normalized;
        }
        return cache.find(path);
    }

    /// Tables are equal when they intern the same paths in the same order.
    friend bool operator==(const PathPool& lhs, const PathPool& rhs) {
        return lhs.paths == rhs.paths;
    }
};

}  // namespace clice::index
