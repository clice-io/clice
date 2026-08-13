#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "index/tu_index.h"
#include "support/path_pool.h"

#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/raw_ostream.h"

namespace clice::index {

/// Project-wide symbol table accumulated from background indexing.
///
/// There is a single path-id space at runtime: the server-wide
/// clice::PathPool. Symbol reference bitmaps carry those ids directly, so
/// queries never translate between pools, and they persist as-is — the blob
/// stays self-contained through a path table mapping every referenced id to
/// its path (which is also the garbage collection: paths no longer
/// referenced by any symbol or shard are simply not written). Loading
/// interns the table into the running pool and remaps every id.
///
/// Serialization reflects this object directly; `format_version`, `paths`
/// and `shards` are serialize-time state populated by serialize() and
/// consumed by from().
struct ProjectIndex {
    /// Persisted-blob schema version (index_format_version), stamped by
    /// serialize() and gated by from().
    std::uint32_t format_version = 0;

    /// The blob's self-contained path table: pool id → path for every id
    /// the symbol bitmaps and the shard manifest reference.
    std::vector<std::pair<std::uint32_t, std::string>> paths;

    SymbolTable symbols;

    /// Pool ids of the files owning a MergedIndex shard blob, persisted so
    /// the loader knows which blobs to fetch.
    std::vector<std::uint32_t> shards;

    /// Merge a TU's external symbols, interning the TU's paths into `pool`.
    /// Returns the TU-local id → pool id mapping for the TU's path graph.
    llvm::SmallVector<std::uint32_t> merge(this ProjectIndex& self,
                                           TUIndex& index,
                                           clice::PathPool& pool);

    /// Serialize with a path table covering exactly the ids used by the
    /// symbol bitmaps plus `shards`, the pool ids of the files owning a
    /// MergedIndex shard blob.
    void serialize(this ProjectIndex& self,
                   llvm::raw_ostream& os,
                   const clice::PathPool& pool,
                   llvm::ArrayRef<std::uint32_t> shards);

    /// Restore from a serialized blob, interning its path table into `pool`
    /// and filling `shards` with the pool ids of the files whose shard blobs
    /// the loader should fetch. Returns nullopt for an unreadable or
    /// old-format blob — the caller treats that as "no index on disk" and
    /// rebuilds in the background.
    static std::optional<ProjectIndex> from(llvm::StringRef data,
                                            clice::PathPool& pool,
                                            llvm::SmallVectorImpl<std::uint32_t>& shards);
};

}  // namespace clice::index
