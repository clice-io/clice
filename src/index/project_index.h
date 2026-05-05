#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

#include "index/tu_index.h"

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

    /// Selective merge: only update symbols referenced by files in new_file_ids.
    /// new_file_ids contains TU-local path indices for cache-miss FileIndices.
    llvm::SmallVector<std::uint32_t> merge(this ProjectIndex& self,
                                           TUIndex& index,
                                           const Bitmap& new_file_ids);

    void serialize(this ProjectIndex& self, llvm::raw_ostream& os);

    static ProjectIndex from(const void* data);
};

}  // namespace clice::index
