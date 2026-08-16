#pragma once

#include <cstdint>
#include <utility>

#include "index/manifest.h"
#include "index/tu_index.h"
#include "support/path_pool.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/raw_ostream.h"

namespace clice::index {

/// One observed version of a file on disk: the identity every manifest
/// dependency points at, and the single place its freshness baseline
/// lives — the consumed-content hash plus a stat fast path, shared by
/// every TU that consumed this version instead of being copied per TU.
///
/// `size`/`mtime_ns` are recorded only when the file provably did not
/// change since before the indexed build started; mtime_ns == 0 means "no
/// fast path" and the check falls through to the hash comparison, which
/// repairs the fast path in place on a match — once, for all consumers.
struct FileVersionRecord {
    /// Runtime path pool id; persisted as the path string.
    std::uint32_t path_id = 0;

    /// xxh3 of the bytes the indexing compile consumed (0 = the worker had
    /// no buffer to hash; freshness stays conservative for this version).
    std::uint64_t content_hash = 0;

    std::uint64_t size = 0;
    std::int64_t mtime_ns = 0;
};

/// The index's global layer: everything mutable, everything shared across
/// files.
///
/// - the project-wide external symbol table with per-symbol reference-file
///   bitmaps (the cross-file query fan-out),
/// - the FileVersion table anchoring freshness and content identity,
/// - one manifest per indexed TU (replaced wholesale by its reindex),
/// - `contributions`, derived from the manifests at load: per file, which
///   TU contributed which rows variant. Its distinct hashes per file are
///   the file's live variants — the mask Shard queries filter by — and its
///   emptiness is what retires a shard blob.
///
/// There is a single path-id space at runtime (clice::PathPool); persisted
/// blobs are self-contained through path tables and remap on load.
struct ProjectIndex {
    SymbolTable symbols;

    llvm::DenseMap<std::uint32_t, FileVersionRecord> file_versions;

    /// (path_id, content_hash) -> FileVersion id.
    llvm::DenseMap<std::pair<std::uint32_t, std::uint64_t>, std::uint32_t> fv_ids;

    /// Ids are monotonic and never reused, so a manifest on disk stays
    /// resolvable against any later global blob (or is detected as stale).
    std::uint32_t next_fv_id = 0;

    /// Generation of the persisted global blob, bumped once per save that
    /// writes it. Manifests are stamped with the generation they were
    /// saved under (TUManifest::global_gen), and the global blob pins the
    /// stamp expected of every TU's manifest; the loader adopts a manifest
    /// only on an exact match, so a lost or failed manifest write cannot
    /// leave an older on-disk manifest serving as current.
    std::uint64_t global_generation = 0;

    /// TU path_id -> its manifest.
    llvm::DenseMap<std::uint32_t, TUManifest> manifests;

    /// Derived from `manifests`: file path_id -> (TU path_id -> rows hash).
    llvm::DenseMap<std::uint32_t, llvm::SmallDenseMap<std::uint32_t, std::uint64_t, 2>>
        contributions;

    /// Merge a TU's external symbols straight off the wire; `file_ids_map`
    /// maps the TU-local ids of `view`'s path table to pool ids. Symbol
    /// names are copied only for symbols new to the table. Returns false —
    /// with the table untouched — when a reference bitmap fails to decode
    /// or carries an id past the path table (the bound TUIndex::from
    /// enforces; the zero-copy view leaves it to this consumer): the
    /// caller rejects the whole result, because merged bits persist while
    /// the result's recorded versions match the disk, so lost bits would
    /// never be rebuilt.
    bool merge(this ProjectIndex& self,
               const TUIndexView& view,
               llvm::ArrayRef<std::uint32_t> file_ids_map);

    /// The FileVersion id for (path, content hash), interning a new record
    /// on first sight.
    std::uint32_t intern_file_version(this ProjectIndex& self,
                                      std::uint32_t path_id,
                                      std::uint64_t content_hash);

    /// Whether every FileVersion id the manifest references is known —
    /// the loader's staleness gate for manifests read from disk.
    bool knows_file_versions(this const ProjectIndex& self, const TUManifest& manifest);

    /// Install (or replace) a TU's manifest and rederive the affected
    /// contribution entries. Returns the file path_ids whose contribution
    /// set changed — the caller refreshes those shards' live-variant masks.
    llvm::SmallVector<std::uint32_t> apply_manifest(this ProjectIndex& self,
                                                    std::uint32_t tu_path_id,
                                                    TUManifest manifest);

    /// Drop a TU's manifest and its contribution entries. Returns the
    /// affected file path_ids, like apply_manifest.
    llvm::SmallVector<std::uint32_t> remove_manifest(this ProjectIndex& self,
                                                     std::uint32_t tu_path_id);

    /// The distinct rows hashes contributed to `path_id` — the file's live
    /// variant set.
    llvm::SmallVector<std::uint64_t> live_variants(this const ProjectIndex& self,
                                                   std::uint32_t path_id);

    /// Serialize the global blob: the FileVersion table (garbage-collected
    /// down to the versions some manifest still references, in memory too),
    /// the symbol table with a self-contained path table, and a per-TU pin
    /// of every manifest's generation stamp.
    void serialize_global(this ProjectIndex& self,
                          llvm::raw_ostream& os,
                          const clice::PathPool& pool);

    /// Restore the global blob, interning its paths into `pool`. Returns
    /// false for an unreadable or old-format blob, leaving the index (and
    /// `pool`) untouched — the caller treats that as "no index on disk"
    /// and rebuilds in the background. Manifests are loaded separately
    /// (apply_manifest per blob); `manifest_pins` maps each pinned TU's
    /// tu_fv to the generation stamp its manifest must carry to be
    /// adopted.
    bool load_global(this ProjectIndex& self,
                     llvm::StringRef data,
                     clice::PathPool& pool,
                     llvm::DenseMap<std::uint32_t, std::uint64_t>& manifest_pins);
};

}  // namespace clice::index
