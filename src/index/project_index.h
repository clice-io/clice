#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "index/shared.h"
#include "index/tu_index.h"
#include "support/path_pool.h"

#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/raw_ostream.h"

namespace clice::index {

/// Project-wide symbol table accumulated from background indexing.
///
/// There is a single path-id space at runtime: the server-wide
/// clice::PathPool. Symbol reference bitmaps carry those ids directly, so
/// queries never translate between pools. Runtime ids are per-session and
/// never persist — serialization remaps every referenced id into a compact
/// self-contained path table (which is also the garbage collection: paths no
/// longer referenced by any symbol or shard are simply not written), and
/// loading interns the table back into the running pool.
///
/// The struct is its own serialization root: format_version / paths / shards
/// belong to the on-disk form, whose bitmaps index the embedded path table.
/// On the live instance they stay empty and bitmaps hold pool ids.
struct ProjectIndex {
    /// On-disk schema version, serialized first so loaders can reject
    /// incompatible blobs before touching anything else.
    std::uint32_t format_version = index_format_version;

    /// Compact self-contained path table the persisted bitmaps index into.
    /// Empty at runtime.
    std::vector<std::string> paths;

    SymbolTable symbols;

    /// Blob-local ids of the files owning a MergedIndex shard, persisted so
    /// the loader knows what to fetch. Empty at runtime — the live manifest
    /// lives in workspace.merged_indices.
    std::vector<std::uint32_t> shards;

    /// Merge a TU's external symbols, interning the TU's paths into `pool`.
    /// Returns the TU-local id → pool id mapping for the TU's path graph.
    llvm::SmallVector<std::uint32_t> merge(this ProjectIndex& self,
                                           TUIndex& index,
                                           clice::PathPool& pool);

    /// Serialize with a compact path table covering exactly the ids used by
    /// the symbol bitmaps plus `shards`, the pool ids of the files owning a
    /// MergedIndex shard blob (persisted so the loader knows what to fetch).
    void serialize(this const ProjectIndex& self,
                   llvm::raw_ostream& os,
                   const clice::PathPool& pool,
                   llvm::ArrayRef<std::uint32_t> shards);

    /// Restore from a serialized blob, interning its path table into `pool`
    /// and filling `shards` with the pool ids of the files whose shard blobs
    /// the loader should fetch. Returns nullopt for an unreadable or
    /// old-format blob — the caller treats that as "no index on disk" and
    /// rebuilds in the background.
    static std::optional<ProjectIndex> from(const void* data,
                                            std::size_t size,
                                            clice::PathPool& pool,
                                            llvm::SmallVectorImpl<std::uint32_t>& shards);
};

}  // namespace clice::index
