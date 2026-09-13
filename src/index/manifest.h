#pragma once

#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

#include "index/include_tree.h"
#include "vfs/file_table.h"

#include "llvm/ADT/StringRef.h"
#include "llvm/Support/raw_ostream.h"

namespace clice::index {

/// What one TU's indexing produced, replaced wholesale by its next reindex:
/// the include tree over file versions (the envelope's nodes with their
/// path ids remapped, which doubles as the TU's dependency set for
/// staleness) and the rows each file received, keyed by content-identity
/// so a re-merge can tell "already stored" from "new variant" without
/// touching any shard.
struct TUManifest {
    /// ProjectIndex::global_generation stamped by the save that persisted
    /// this manifest. The global blob pins the stamp it expects per TU and
    /// the loader adopts a manifest only on an exact match: a manifest
    /// that outran a lost global write would otherwise serve against a
    /// symbol table that never learned its symbols, and a manifest whose
    /// own write failed would pass off the previous reindex's dependency
    /// set and rows as current.
    std::uint64_t global_gen = 0;

    /// Milliseconds since epoch, sampled before the indexed build started.
    std::uint64_t built_at = 0;

    /// The TU's own file version.
    VersionID tu_fv;

    std::vector<IncludeNode> nodes;

    /// FileVersion -> rows hash for every file this TU contributed rows to,
    /// the TU's own file included. Deduplicated: one entry per file version
    /// even when the file was entered several times.
    std::vector<std::pair<VersionID, std::uint64_t>> contributions;

    friend bool operator==(const TUManifest&, const TUManifest&) = default;
};

/// Serialize a manifest (varint-packed nodes inside a small reflected
/// wrapper).
void serialize_manifest(const TUManifest& manifest, llvm::raw_ostream& os);

/// Verify and decode a manifest blob; nullopt for corrupt, truncated or
/// old-format data. FileVersion ids are not resolved here — the loader
/// drops manifests referencing ids the global table does not know.
std::optional<TUManifest> deserialize_manifest(llvm::StringRef data);

}  // namespace clice::index
