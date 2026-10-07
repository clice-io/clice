module;

#include "modules/prelude.h"

module clice:index.project_index;

import :index.database;
import :index.manifest;
import :index.search_index;
import :index.shard;
import :index.tu_index;
import :vfs.file_table;

namespace clice::index {

/// The persisted index as loaded, in every part:
///
/// - the project-wide external symbol table with per-symbol reference-file
///   bitmaps (the cross-file query fan-out): the persisted global blob read
///   in place, plus the rows merged or changed since it was bound. A
///   reader never copies a row; a writer copies only the rows it changes
///   and folds them into the next blob it writes,
/// - one manifest per indexed TU (replaced wholesale by its reindex),
/// - `contributions`, derived from the manifests at load: per file, which
///   TU contributed which rows variant. Its distinct hashes per file are
///   the file's live variants — the mask Shard queries filter by — and its
///   emptiness is what retires a shard blob,
/// - the per-file row blobs (`shards`), fetched from the database on first
///   use by a reader and kept whole by the writer,
/// - the name search index and the rows it does not describe.
///
/// The FileVersion table manifests reference lives in clice::FileTable,
/// shared with every other freshness consumer — and, in a server running
/// several projects, with every other project's index. The global blob
/// persists the versions some manifest references under ids of its own
/// (see persisted_ids), which adopt_file_versions maps into the table.
///
/// There is a single path-id space at runtime (clice::FileTable); the blob
/// carries its own path table, appended to across writes and mapped to
/// the table's ids when bound.
struct ProjectIndex {
    ProjectIndex();
    ~ProjectIndex();
    ProjectIndex(ProjectIndex&&) noexcept;
    ProjectIndex& operator=(ProjectIndex&&) noexcept;

    /// The root the database names the files under relative to, by their
    /// portable names (path::portable), so a moved checkout keeps its
    /// index; empty names every file by its identity. Set before a blob
    /// is bound or written.
    CanonicalPath workspace;

    /// The name the database keeps for `path`.
    std::string portable(llvm::StringRef path) const;

    /// The path a name the database keeps names in this checkout.
    Spelling local(llvm::StringRef name) const;

    /// The key of a file's blobs (its shard, a TU's manifest) in the
    /// database.
    std::string key_of(const FileTable& files, Fid file) const;

    /// Bind a global blob as the table's base, mapping its path table into
    /// `files`. False — and the index untouched — when the bytes are not
    /// a global blob of this format or are inconsistent. The base's
    /// bitmaps are not decoded here: a reader decodes one when a query
    /// reaches it, a writer proves them all with verify_bitmaps.
    bool bind_global(std::unique_ptr<llvm::MemoryBuffer> blob, FileTable& files);

    /// The writer's half of a bound blob: intern its file versions into
    /// `files` and read the per-TU manifest
    /// pins — `manifest_pins` maps each pinned TU's tu_fv (as the table's
    /// id) to the generation stamp its manifest must carry to be adopted.
    /// Rejects a blob whose version table is inconsistent, leaving `files`
    /// untouched.
    std::expected<void, llvm::StringRef>
        adopt_file_versions(FileTable& files,
                            llvm::DenseMap<VersionID, std::uint64_t>& manifest_pins);

    /// Whether every reference bitmap of the base decodes and stays inside
    /// its path table — the writer's gate: a malformed image normalized to
    /// empty would silently lose the symbol's reference files with nothing
    /// ever rebuilding them, so the whole blob is rejected instead.
    bool verify_bitmaps() const;

    /// bind_global, adopt_file_versions and verify_bitmaps over a copy of
    /// `data`, all or nothing: the loader's gate for a persisted global.
    std::expected<void, llvm::StringRef>
        load_global(llvm::StringRef data,
                    FileTable& files,
                    llvm::DenseMap<VersionID, std::uint64_t>& manifest_pins);

    /// The symbol's stored facts; the strings borrow the base or the row.
    std::optional<SymbolIdentity> identity_of(SymbolHash hash) const;

    /// How many files reference the symbol; 0 for an unknown hash.
    std::uint32_t reference_count(SymbolHash hash) const;

    /// The files referencing the symbol, as the file table's ids.
    void each_reference_file(SymbolHash hash, llvm::function_ref<void(Fid)> visit) const;

    /// Every symbol, base rows and changed rows alike, in no particular
    /// order; `visit` returns false to stop.
    void for_each_symbol(
        llvm::function_ref<bool(SymbolHash, const SymbolIdentity&, std::uint32_t references)> visit)
        const;

    std::size_t symbol_count() const;

    /// The symbol's row to change: the changed row when there is one,
    /// else a copy of the base row, else a fresh one.
    Symbol& touch(SymbolHash hash);

    /// Merge a TU's external symbols straight off the wire; `file_ids_map`
    /// maps the TU-local ids of `index`'s path table to pool ids. Symbol
    /// names are copied only for symbols new to the table. `added`
    /// receives the hashes of the symbols new to the table or whose name,
    /// arguments, parent, file or flags the merge changed — what a name
    /// search keyed on the table's rows must re-read.
    void merge(const TUIndex& index,
               llvm::ArrayRef<Fid> file_ids_map,
               llvm::SmallVectorImpl<SymbolHash>* added = nullptr);

    /// Serialize the global blob: the versions some manifest still
    /// references (the shared table is not touched — a version the index
    /// stops referencing can still anchor another consumer's check; ids
    /// are never reused either way), the symbol table — base rows copied,
    /// changed rows encoded, in hash order — with its path table, a per-TU
    /// pin of every manifest's generation stamp, and the search index's
    /// generation and pending rows. The changed rows are held aside until
    /// rebase or restore_unwritten says whether the bytes landed.
    void serialize_global(llvm::raw_ostream& os, const FileTable& files);

    /// The bytes serialize_global produced were persisted: they become the
    /// base, and the rows they hold leave the changed set. Rows changed
    /// since the serialization stay changed.
    void rebase(std::unique_ptr<llvm::MemoryBuffer> blob, FileTable& files);

    /// The bytes serialize_global produced were not persisted: the rows
    /// held aside return to the changed set, under the rows changed since.
    void restore_unwritten();

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

    /// Derived from `manifests`: a place some TU's failed lookup looked ->
    /// those TUs (TUManifest::absent).
    llvm::DenseMap<Fid, llvm::SmallDenseSet<Fid, 2>> probed;

    /// The file table's id for a version id the loaded global blob
    /// carries; nullopt for any other.
    std::optional<VersionID> runtime_version(std::uint32_t persisted) const;

    /// Rewrite a manifest read from disk into the file table's version
    /// ids. False when it names a version the loaded global blob does not
    /// carry — the loader's staleness gate for manifests.
    bool import_manifest(TUManifest& manifest) const;

    /// The manifest as persisted: its versions under this index's
    /// persisted ids, handing ids out to versions that have none yet.
    TUManifest export_manifest(const TUManifest& manifest);

    /// Install (or replace) a TU's manifest and rederive the affected
    /// contribution entries. Returns the file path_ids whose contribution
    /// set changed — the caller refreshes those shards' live-variant masks.
    llvm::SmallVector<Fid> apply_manifest(const FileTable& files,
                                          Fid tu_path_id,
                                          TUManifest manifest);

    /// Drop a TU's manifest and its contribution entries. Returns the
    /// affected file path_ids, like apply_manifest.
    llvm::SmallVector<Fid> remove_manifest(const FileTable& files, Fid tu_path_id);

    /// The distinct rows hashes contributed to `path_id` — the file's live
    /// variant set.
    llvm::SmallVector<std::uint64_t> live_variants(Fid path_id) const;

    /// The TUs that contributed rows to `file`: from `contributions` in a
    /// writer, from the global blob's persisted copy in a reader, which
    /// holds no manifests to derive it from.
    void each_contributor(Fid file, llvm::function_ref<void(Fid)> visit) const;

    /// The files holding rows of an internal-linkage symbol, starting from
    /// `anchor`, a file that holds some: the anchor, and every file the
    /// local fanout (TUManifest::local_fanout) of a TU contributing to a
    /// file reached so far lists for the symbol. A `static` in a header is
    /// thus reached in every unit including the header, whichever of its
    /// files the walk starts from. Each file once.
    void each_fanout_file(SymbolHash hash,
                          Fid anchor,
                          const FileTable& files,
                          llvm::function_ref<void(Fid)> visit) const;

    /// A TU's manifest: held by a writer, fetched on first use by a reader
    /// — whose copy names its versions by persisted ids. Null when the TU
    /// has none current.
    const TUManifest* tu_manifest(Fid tu) const;

    /// Per-file row blobs keyed by project-level path_id: symbol
    /// occurrences, relations and stored content for position mapping,
    /// served zero-copy. The writer holds every persisted shard here; a
    /// reader opened over a database fills it on first use (shard()). A
    /// node-based map: a query keeps pointers to the shards it read while
    /// its fan-out fetches others.
    mutable std::map<Fid, Shard> shards;

    /// The file's shard: held, or fetched from the database the index was
    /// opened over. Null when the file has no rows.
    const Shard* shard(Fid file) const;

    /// The name search index over the symbol table as last built, and the
    /// rows it does not describe — merged or changed since — which a
    /// search reads from the table instead until the writer folds them
    /// into a rebuilt index. Both persist next to the table: the global
    /// blob pins the search blob's generation and carries the pending rows,
    /// so a reader adopts exactly the search index the table was saved
    /// with.
    SearchIndex search_index;
    llvm::DenseSet<SymbolHash> search_pending;
    std::uint64_t search_generation = 0;

    /// Adopt a persisted search blob when the bound global pins its
    /// generation; otherwise the index stays unloaded and searches scan
    /// the table.
    bool bind_search(std::unique_ptr<llvm::MemoryBuffer> blob);

    /// Open the persisted index for reading: the global and search blobs
    /// bound in place from the database's read snapshot — which the
    /// caller keeps pinned for the index's lifetime — and shards fetched
    /// from it on first use. False when there is no readable global blob.
    bool open(BlobDatabase& db, FileTable& files);

    /// The byte split of the base blob's symbol table and reverse include
    /// graph; everything else in the blob (file versions, the path table,
    /// framing) is the remainder of its serialized size.
    struct GlobalColumns {
        std::size_t names = 0;
        std::size_t args = 0;
        std::size_t bitmaps = 0;
        /// Hash, parent, kind, flags and file per symbol.
        std::size_t fixed = 0;
        std::size_t contributors = 0;
    };

    /// For `clice index --stats`.
    GlobalColumns global_columns() const;

private:
    struct Base;
    std::unique_ptr<Base> base;

    /// Rows merged or changed since the base was bound, in file-table ids.
    llvm::DenseMap<SymbolHash, Symbol> changed;

    /// Rows serialized by the last serialize_global and not yet known to
    /// have landed; reads consult them after `changed`.
    llvm::DenseMap<SymbolHash, Symbol> written;

    /// A reader's manifests, fetched on first use; nullopt remembers a
    /// TU without a current one.
    mutable llvm::DenseMap<Fid, std::optional<TUManifest>> fetched_manifests;

    /// The database shard() fetches from, when opened over one.
    BlobDatabase* db = nullptr;
    const FileTable* files = nullptr;

    /// The persisted FileVersion id space: the global blob and the
    /// manifests name versions by ids private to this index's lineage,
    /// handed out from `next_persisted_id` and never reused, so one file
    /// table can hold the versions of several projects' indexes. Loading
    /// maps the blob's ids to the table's (`runtime_ids`, consulted while
    /// the manifests load); writing maps back, handing out ids to the
    /// versions the lineage has not persisted yet.
    llvm::DenseMap<std::uint32_t, VersionID> runtime_ids;
    llvm::DenseMap<VersionID, std::uint32_t> persisted_ids;
    std::uint32_t next_persisted_id = 0;

    std::uint32_t persisted_id(VersionID version);
};

}  // namespace clice::index
