#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

#include "index/tu_index.h"
#include "support/path_pool.h"

#include "llvm/ADT/ArrayRef.h"
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

    /// Bidirectional cache between this index's path ids and the server-wide
    /// clice::PathPool's path ids for the same file.  Purely a query
    /// accelerator for translating ids across the two pools without a string
    /// round-trip.  Never serialized: server ids are per-session, so a fresh
    /// ProjectIndex (including one restored by from()) starts with an empty
    /// mapping and it is rebuilt as paths are linked again.  Kept in sync by
    /// link_server_paths() after each merge and lazily backfilled by
    /// IndexQuery on a miss (a path may enter one pool before the other).
    llvm::DenseMap<std::uint32_t, std::uint32_t> proj_to_server;
    llvm::DenseMap<std::uint32_t, std::uint32_t> server_to_proj;

    /// Record a proj_id <-> server_id correspondence in both directions.
    void link_server_path(this ProjectIndex& self, std::uint32_t proj_id, std::uint32_t server_id) {
        self.proj_to_server[proj_id] = server_id;
        self.server_to_proj[server_id] = proj_id;
    }

    /// Extend the path-id mapping for the given project path ids, linking each
    /// to the server pool's id for the same file when the server already knows
    /// it.  Paths the server has not interned yet are skipped and left for
    /// IndexQuery to backfill lazily on first use.  Called after a merge so
    /// newly interned paths get mapped as soon as both pools know them.
    void link_server_paths(this ProjectIndex& self,
                           const clice::PathPool& server_pool,
                           llvm::ArrayRef<std::uint32_t> proj_ids) {
        for(auto proj_id: proj_ids) {
            if(self.proj_to_server.contains(proj_id))
                continue;
            auto it = server_pool.cache.find(self.path_pool.path(proj_id));
            if(it != server_pool.cache.end())
                self.link_server_path(proj_id, it->second);
        }
    }

    llvm::SmallVector<std::uint32_t> merge(this ProjectIndex& self, TUIndex& index);

    void serialize(this ProjectIndex& self, llvm::raw_ostream& os);

    static ProjectIndex from(const void* data);
};

}  // namespace clice::index
