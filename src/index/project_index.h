#pragma once

#include <cstdint>
#include <optional>

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
struct ProjectIndex {
    SymbolTable symbols;

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
