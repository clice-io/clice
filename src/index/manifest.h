#pragma once

#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

#include "llvm/ADT/StringRef.h"
#include "llvm/Support/raw_ostream.h"

namespace clice::index {

/// One entered file of a TU's include tree: which file version was entered,
/// through which include directive (the parent node's file at `line`), and
/// where. Multiple entries of one file (headers without guards) are
/// distinct nodes.
struct ManifestNode {
    /// FileVersion id (ProjectIndex::file_versions).
    std::uint32_t fv = 0;

    /// Index of the including node, ~0 when the directive sits in the TU's
    /// own file (the TU root is not itself a node).
    std::uint32_t parent = ~0u;

    /// 1-based line of the include directive in the parent.
    std::uint32_t line = 0;

    friend bool operator==(const ManifestNode&, const ManifestNode&) = default;
};

/// What one TU's indexing produced, replaced wholesale by its next reindex:
/// the include tree over file versions (which doubles as the TU's
/// dependency set for staleness) and the rows each file received, keyed by
/// content-identity so a re-merge can tell "already stored" from "new
/// variant" without touching any shard.
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
    std::uint32_t tu_fv = 0;

    std::vector<ManifestNode> nodes;

    /// FileVersion -> rows hash for every file this TU contributed rows to,
    /// the TU's own file included. Deduplicated: one entry per file version
    /// even when the file was entered several times.
    std::vector<std::pair<std::uint32_t, std::uint64_t>> contributions;

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
