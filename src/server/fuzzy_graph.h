#pragma once

#include <cstdint>

#include "server/path_pool.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallVector.h"

namespace clice {

/// Approximate include graph built from fast scanning (no full compilation).
/// Maintains forward (file → included files) and backward (file → including files)
/// edges using ServerPathPool IDs. "Fuzzy" because conditional includes are
/// unconditionally followed, giving an over-approximation.
class FuzzyGraph {
public:
    explicit FuzzyGraph(ServerPathPool& pool) : path_pool(pool) {}

    void update_file(std::uint32_t path_id,
                     llvm::ArrayRef<std::uint32_t> new_includes);

    void remove_file(std::uint32_t path_id);

    llvm::SmallVector<std::uint32_t> get_includes(std::uint32_t path_id) const;

    llvm::SmallVector<std::uint32_t> get_reverse_includes(std::uint32_t path_id) const;

    /// Get all files transitively affected by a change to `path_id`.
    llvm::SmallVector<std::uint32_t> get_affected_files(std::uint32_t path_id) const;

    bool has_file(std::uint32_t path_id) const;

    std::size_t file_count() const { return forward.size(); }

    /// In-degree: how many files include this file (used for indexing priority).
    std::size_t in_degree(std::uint32_t path_id) const;

private:
    ServerPathPool& path_pool;
    llvm::DenseMap<std::uint32_t, llvm::DenseSet<std::uint32_t>> forward;
    llvm::DenseMap<std::uint32_t, llvm::DenseSet<std::uint32_t>> backward;
};

}  // namespace clice
