/// The persisted layouts of the project index's records: the header, a
/// file's records, a shared local's and a unit's manifest.

module clice:index.project_index.layout;

import :support.prelude;

namespace clice::index {

/// The index database's header, written by every save. Its format version
/// covers every record: a database of another one is discarded whole.
template <template <typename> typename Seq>
struct HeaderBlobOf {
    std::uint32_t format_version = 0;

    /// The path id the next file the database names receives.
    std::uint32_t next_path = 0;

    /// The id the next file version the manifests name receives.
    std::uint32_t next_version = 0;

    /// The units the database holds a manifest of.
    std::uint32_t unit_count = 0;

    /// The search index's segments, ascending, oldest run first, and where
    /// each run ends among them; the id the next segment receives.
    Seq<std::uint32_t> search_segments;
    Seq<std::uint32_t> search_run_ends;
    std::uint32_t next_segment = 0;

    /// The symbols and the files (path ids) whose names of internal linkage
    /// changed since the newest run's snapshot, ascending.
    Seq<std::uint64_t> search_pending;
    Seq<std::uint32_t> search_pending_files;

    /// The units (path ids) owed a reindex because a rebuild of a file they
    /// contribute to discarded their variants, ascending: their files' content
    /// reads fresh, so no start-up scan would find them.
    Seq<std::uint32_t> owed_units;
};

using HeaderBlob = HeaderBlobOf<codec::blob::Owned>;

/// What the database knows of one file, small enough for the start-up
/// scan to read for every file: its path id, the contents the units read,
/// what its shard stores and, for a unit, its current run.
template <template <typename> typename Seq>
struct FileBlobOf {
    std::uint32_t path = 0;

    /// Some unit's failed lookup looked here and found no file.
    bool absent = false;

    /// The content hashes the units entering the file, or reading it
    /// through a module or a precompiled header, consumed, ascending.
    Seq<std::uint64_t> consumed;

    /// The content generation of the file's shard, 0 for none, and the
    /// variants it stores in mask order — what a merge or a worker may
    /// take as stored without reading the shard.
    std::uint64_t generation = 0;
    Seq<std::uint64_t> stored;

    /// For a unit, its manifest's built_at; 0 when the file is no unit.
    std::uint64_t run = 0;

    /// For a unit, its manifest's command.
    std::uint64_t command = 0;
};

using FileBlob = FileBlobOf<codec::blob::Owned>;

/// A set of units (path ids, as a file set: append_file_set) under a key.
template <template <typename> typename Seq>
struct UnitSetOf {
    std::uint64_t key = 0;
    Seq<std::uint8_t> units;
};

/// How a set of units resolved a file's own `#include` lines at one
/// content: the directive lines and their targets (path ids), in line
/// order.
template <template <typename> typename Seq>
struct IncludeResolutionOf {
    std::uint64_t content = 0;
    Seq<std::uint32_t> lines;
    Seq<std::uint32_t> targets;
    Seq<std::uint8_t> units;
};

/// The units depending on one file, each list ascending by key.
template <template <typename> typename Seq>
struct DependentsBlobOf {
    /// Per consumed content hash, the units that entered the file at it.
    Seq<UnitSetOf<Seq>> readers;
    /// Per variant, the units that contributed it.
    Seq<UnitSetOf<Seq>> contributors;
    /// The units whose failed lookups looked here.
    Seq<std::uint8_t> absent;
    /// Per content hash, the units reading the file through a module they
    /// import or the precompiled header they load.
    Seq<UnitSetOf<Seq>> importers;
    /// Per instantiation use, the units using it.
    Seq<UnitSetOf<Seq>> uses;
    /// The distinct resolutions of the file's own `#include` lines.
    Seq<IncludeResolutionOf<Seq>> resolutions;
    /// The versions of the file the manifests name, ascending by content
    /// hash: each one's version record id and how many manifests name it.
    Seq<std::uint64_t> version_hashes;
    Seq<std::uint32_t> version_ids;
    Seq<std::uint32_t> version_manifests;
};

using DependentsBlob = DependentsBlobOf<codec::blob::Owned>;

/// The names of internal linkage a file's shard declares, for the search
/// index, ascending by hash.
template <template <typename> typename Seq>
struct InternalsBlobOf {
    Seq<std::uint64_t> hashes;
    Seq<char> names;
    Seq<std::uint32_t> name_ends;
    Seq<char> args;
    Seq<std::uint32_t> args_ends;
    Seq<std::uint64_t> parents;
    Seq<std::uint8_t> kinds;
    Seq<std::uint16_t> flags;
};

using InternalsBlob = InternalsBlobOf<codec::blob::Owned>;

/// A file set (append_file_set) and how many units list it.
template <template <typename> typename Seq>
struct ListedFilesOf {
    std::uint32_t units = 0;
    Seq<std::uint8_t> files;
};

/// A symbol below external scope several files hold: the file sets the
/// units list for it (TUManifest::shared_locals), each once.
template <template <typename> typename Seq>
struct SharedLocalBlobOf {
    Seq<ListedFilesOf<Seq>> sets;
};

using SharedLocalBlob = SharedLocalBlobOf<codec::blob::Owned>;

/// The manifest's persisted form. Its file versions are the ids of the
/// database's version records, which every other field indexes by their
/// position; a path appears twice when the modules a unit imports were
/// built from two versions of it. Node and contribution payloads are
/// varint-packed by hand: a big TU enters thousands of files, and LEB128 on
/// the small ids and lines roughly halves the blob against fixed-width
/// columns.
template <template <typename> typename Seq>
struct TUManifestBlobOf {
    std::uint32_t format_version = 0;

    std::uint64_t built_at = 0;

    std::uint64_t peak = 0;

    std::uint64_t command = 0;

    /// The version ids, ascending, as postings below `version_bound`
    /// (write_postings).
    std::uint32_t version_bound = 0;
    Seq<std::uint8_t> versions;

    std::uint32_t tu_version = 0;

    std::uint32_t node_count = 0;

    std::uint32_t contribution_count = 0;

    /// node_count × (varint version, varint parent, varint line): the parent 0
    /// for the previous node's, 1 for the root, else its index plus 2; the
    /// line shifted left by two over a bit for a skipped node and a bit for
    /// a line stored as its distance past the previous node's — only
    /// beside the previous node's parent, at or past its line.
    Seq<std::uint8_t> nodes;

    /// contribution_count × (varint version, 8-byte little-endian variant hash —
    /// hashes are random bits, varint would only inflate them)
    Seq<std::uint8_t> contributions;

    /// Sets of versions, as postings below their count.
    Seq<std::uint8_t> absent;
    Seq<std::uint8_t> imports;

    /// TUManifest::shared_locals as columns: the symbols ascending, each
    /// one's file versions back to back.
    Seq<std::uint64_t> local_symbols;
    Seq<std::uint32_t> local_file_ends;
    Seq<std::uint32_t> local_files;

    /// TUManifest::instantiations as columns: per file its version and its
    /// use.
    Seq<std::uint32_t> instantiation_files;
    Seq<std::uint64_t> instantiation_uses;
};

using TUManifestBlob = TUManifestBlobOf<codec::blob::Owned>;

}  // namespace clice::index
