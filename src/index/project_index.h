#pragma once

#include <cstdint>
#include <utility>

#include "index/manifest.h"
#include "index/tu_index.h"
#include "vfs/file_table.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/raw_ostream.h"

namespace clice::index {

/// The index's global layer: everything mutable, everything shared across
/// files.
///
/// - the project-wide external symbol table with per-symbol reference-file
///   bitmaps (the cross-file query fan-out),
/// - one manifest per indexed TU (replaced wholesale by its reindex),
/// - `contributions`, derived from the manifests at load: per file, which
///   TU contributed which rows variant. Its distinct hashes per file are
///   the file's live variants — the mask Shard queries filter by — and its
///   emptiness is what retires a shard blob.
///
/// The FileVersion table manifests reference lives in clice::FileTable,
/// shared with every other freshness consumer; the global blob persists
/// the versions some manifest references, and load_global restores them
/// id-for-id — so it must run before anything interns a version.
///
/// There is a single path-id space at runtime (clice::FileTable); persisted
/// blobs are self-contained through path tables and remap on load.
struct ProjectIndex {
    SymbolTable symbols;

    /// Generation of the persisted global blob, bumped once per save that
    /// writes it. Manifests are stamped with the generation they were
    /// saved under (TUManifest::global_gen), and the global blob pins the
    /// stamp expected of every TU's manifest; the loader adopts a manifest
    /// only on an exact match, so a lost or failed manifest write cannot
    /// leave an older on-disk manifest serving as current.
    std::uint64_t global_generation = 0;

    /// TU fid -> its manifest.
    llvm::DenseMap<Fid, TUManifest> manifests;

    /// Derived from `manifests`: file fid -> (TU fid -> rows hash).
    llvm::DenseMap<Fid, llvm::SmallDenseMap<Fid, std::uint64_t, 2>> contributions;

    /// Merge a TU's external symbols straight off the wire; `file_ids_map`
    /// maps the TU-local ids of `index`'s path table to pool ids. Symbol
    /// names are copied only for symbols new to the table. Returns false —
    /// with the table untouched — when a reference bitmap fails to decode
    /// or carries an id past the path table (the bound TUIndex::from_bytes
    /// enforces; the zero-copy reader leaves it to this consumer): the
    /// caller rejects the whole result, because merged bits persist while
    /// the result's recorded versions match the disk, so lost bits would
    /// never be rebuilt.
    bool merge(this ProjectIndex& self, const TUIndex& index, llvm::ArrayRef<Fid> file_ids_map);

    /// Whether every FileVersion id the manifest references is known —
    /// the loader's staleness gate for manifests read from disk.
    bool knows_file_versions(this const ProjectIndex& self,
                             const clice::FileTable& files,
                             const TUManifest& manifest);

    /// Install (or replace) a TU's manifest and rederive the affected
    /// contribution entries. Returns the file path_ids whose contribution
    /// set changed — the caller refreshes those shards' live-variant masks.
    llvm::SmallVector<Fid> apply_manifest(this ProjectIndex& self,
                                          const clice::FileTable& files,
                                          Fid tu_path_id,
                                          TUManifest manifest);

    /// Drop a TU's manifest and its contribution entries. Returns the
    /// affected file path_ids, like apply_manifest.
    llvm::SmallVector<Fid> remove_manifest(this ProjectIndex& self,
                                           const clice::FileTable& files,
                                           Fid tu_path_id);

    /// The distinct rows hashes contributed to `path_id` — the file's live
    /// variant set.
    llvm::SmallVector<std::uint64_t> live_variants(this const ProjectIndex& self, Fid path_id);

    /// Serialize the global blob: the versions some manifest still
    /// references (the shared table is not touched — a version the index
    /// stops referencing can still anchor another consumer's check; ids
    /// are never reused either way), the symbol table with a
    /// self-contained path table, and a per-TU pin of every manifest's
    /// generation stamp.
    void serialize_global(this ProjectIndex& self,
                          llvm::raw_ostream& os,
                          const clice::FileTable& files);

    /// Restore the global blob, interning its paths and file versions
    /// (id-for-id, which is why it must run before anything else interns a
    /// version) into `files`. Returns false for an unreadable or
    /// old-format blob, leaving the index (and `files`) untouched — the
    /// caller treats that as "no index on disk" and rebuilds in the
    /// background. Manifests are loaded separately (apply_manifest per
    /// blob); `manifest_pins` maps each pinned TU's tu_fv to the
    /// generation stamp its manifest must carry to be adopted.
    bool load_global(this ProjectIndex& self,
                     llvm::StringRef data,
                     clice::FileTable& files,
                     llvm::DenseMap<VersionID, std::uint64_t>& manifest_pins);
};

}  // namespace clice::index
