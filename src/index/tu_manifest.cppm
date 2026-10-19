module;

#include "modules/prelude.h"

module clice:index.tu_manifest;

import :index.symbol;
import :vfs.file_table;

namespace clice::index {

/// An include edge of a document, from a TU manifest: the 1-based
/// directive line and the resolved target's absolute path.
struct IncludeEdge {
    std::uint32_t line = 0;
    std::string target;
};

/// A symbol below external scope more than one of a TU's files names, with
/// those files: the TU's own, the ones its instantiations and expansions
/// resolve in, and the ones declaring it behind the precompiled header or a
/// module. No project-wide table lists such a symbol: this is how a query
/// reaches its entries in files other than the one it started from.
struct SharedLocal {
    std::uint64_t symbol = 0;
    std::vector<VersionID> files;

    friend bool operator==(const SharedLocal&, const SharedLocal&) = default;
};

/// The instantiation resolutions a TU uses in one file — the ones it walked
/// and the ones the known keys it skipped there stand for — by the identity
/// of their set (index::use_hash), which the file's instantiation blob
/// stores.
struct InstantiationUse {
    VersionID file;
    std::uint64_t use = 0;

    friend bool operator==(const InstantiationUse&, const InstantiationUse&) = default;
};

/// What one TU's indexing produced, replaced wholesale by its next reindex:
/// the include tree over file versions (the TU index's nodes with their
/// path ids remapped, which with `absent` and `imports` is the TU's
/// dependency set for staleness) and the variant each file received, keyed by content-identity
/// so a re-merge can tell "already stored" from "new variant" without
/// touching any shard. In memory its versions are the file table's ids;
/// persisted, the index's own (ProjectIndex::export_manifest).
struct TUManifest {
    /// ProjectIndex::table_generation recorded by the save that persisted
    /// this manifest. The project table pins the generation it expects per TU and
    /// the loader adopts a manifest only on an exact match: a manifest
    /// that outran a lost project table write would otherwise serve against a
    /// symbol table that never learned its symbols, and a manifest whose
    /// own write failed would pass off the previous reindex's dependency
    /// set and variants as current.
    std::uint64_t table_generation = 0;

    /// Milliseconds since epoch, sampled before the indexed build started.
    std::uint64_t built_at = 0;

    /// The TU's own file version.
    VersionID tu_version;

    std::vector<IncludeNode> nodes;

    /// FileVersion -> variant hash for every file this TU contributed a variant to,
    /// the TU's own file included. Deduplicated: one entry per file version
    /// even when the file was entered several times.
    std::vector<std::pair<VersionID, std::uint64_t>> contributions;

    /// The places the TU's failed lookups looked that held no file, as
    /// versions without bytes (content hash 0): a file appearing at one
    /// changes what the TU compiles to.
    std::vector<VersionID> absent;

    /// The versions the modules the TU imports were built from, their own
    /// imports' included, and the precompiled header it loads: the parse
    /// read those without entering their files, which carry no variant of
    /// the TU.
    std::vector<VersionID> imports;

    /// Sorted by symbol.
    std::vector<SharedLocal> shared_locals;

    /// One entry per file holding instantiation resolutions the TU uses: what
    /// keeps those resolutions alive.
    std::vector<InstantiationUse> instantiations;

    friend bool operator==(const TUManifest&, const TUManifest&) = default;
};

/// Visit the raw id of every version a manifest names — its TU's, its
/// include tree's, its contributions', its failed lookups', its imports',
/// its shared locals' and its instantiation uses' — by reference, const as
/// `manifest` is.
template <typename Manifest, typename Visit>
    requires std::same_as<std::remove_const_t<Manifest>, TUManifest>
void for_each_version(Manifest& manifest, Visit&& visit) {
    visit(manifest.tu_version.raw);
    for(auto& node: manifest.nodes) {
        visit(node.file);
    }
    for(auto& version: llvm::make_first_range(manifest.contributions)) {
        visit(version.raw);
    }
    for(auto& version: manifest.absent) {
        visit(version.raw);
    }
    for(auto& version: manifest.imports) {
        visit(version.raw);
    }
    for(auto& local: manifest.shared_locals) {
        for(auto& version: local.files) {
            visit(version.raw);
        }
    }
    for(auto& use: manifest.instantiations) {
        visit(use.file.raw);
    }
}

/// Serialize a manifest (varint-packed nodes inside a small reflected
/// wrapper).
std::string serialize_manifest(const TUManifest& manifest);

/// Verify and decode a manifest blob; nullopt for corrupt, truncated or
/// old-format data. FileVersion ids are not resolved here — the loader
/// drops manifests referencing ids the project table does not know.
std::optional<TUManifest> deserialize_manifest(llvm::StringRef data);

}  // namespace clice::index
