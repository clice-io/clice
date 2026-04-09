#pragma once

/// @file project_index.h
/// @brief Project-wide symbol index aggregated from per-TU indices.
///
/// ProjectIndex merges symbol tables from multiple TUIndex instances into a
/// single cross-TU view. It maintains a unified PathPool for path interning
/// and tracks which TU index file corresponds to each source file. The merged
/// symbol table stores reference-file bitmaps remapped to project-wide path IDs.
/// Serialized to/from FlatBuffer for persistence across server restarts.

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

/// Intern pool for file paths, mapping each unique path to a compact uint32 ID.
/// Paths are copied into a BumpPtrAllocator for pointer stability. Backslashes
/// are normalized to forward slashes on insertion for cross-platform consistency.
struct PathPool {
    llvm::BumpPtrAllocator allocator;

    /// All interned paths, indexed by path_id.
    std::vector<llvm::StringRef> paths;

    /// Reverse lookup: path string -> path_id.
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

/// Project-wide index that aggregates symbol metadata from all translation units.
struct ProjectIndex {
    /// Shared path intern pool (project-wide path_ids).
    PathPool path_pool;

    /// Maps source file path_id to its corresponding TU index file path_id.
    llvm::DenseMap<std::uint32_t, std::uint32_t> indices;

    /// Merged symbol table with reference-file bitmaps using project-wide path_ids.
    SymbolTable symbols;

    /// Merge a single TUIndex into this project index.
    /// Remaps the TU's local path_ids to project-wide path_ids and merges
    /// symbol metadata (name, kind, reference bitmaps).
    /// @return A mapping from TU-local path_ids to project-wide path_ids.
    llvm::SmallVector<std::uint32_t> merge(this ProjectIndex& self, TUIndex& index);

    /// Serialize the project index to FlatBuffer binary format.
    void serialize(this ProjectIndex& self, llvm::raw_ostream& os);

    /// Deserialize a ProjectIndex from a FlatBuffer binary blob.
    /// Restores the path pool, index mapping, and symbol table with bitmaps.
    static ProjectIndex from(const void* data);
};

}  // namespace clice::index
