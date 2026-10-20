module;

#include "modules/prelude.h"

module clice:index.project_index;

import :index.database;
import :index.format;
import :index.instantiation_shard;
import :index.project_table;
import :index.search_index;
import :index.shard;
import :index.tu_index;
import :index.tu_manifest;
import :vfs.file_table;

namespace clice::testing {

struct IndexInternals;

}

namespace clice::index {

/// What one index-changing call did to state others own. Collections are
/// deduplicated.
class IndexReport {
public:
    /// TUs owed a ContentChanged reindex: their index is missing,
    /// stale or discarded and no in-process event would rebuild it.
    /// The pump takes these before the current attempt finishes.
    llvm::ArrayRef<Fid> reindex() const {
        return reindex_ids;
    }

    /// Files whose stored index was replaced, re-masked or dropped
    /// while possibly index-served: the serving adapter checks which
    /// of them an open session actually serves and refreshes those.
    llvm::ArrayRef<Fid> index_changed() const {
        return index_ids;
    }

    void add_reindex(Fid id) {
        if(reindex_seen.insert(id).second) {
            reindex_ids.push_back(id);
        }
    }

    void add_index_changed(Fid id) {
        if(index_seen.insert(id).second) {
            index_ids.push_back(id);
        }
    }

    /// This save left state that an immediate retry commits: debt
    /// that showed up after its command snapshot serialized (write-time
    /// corruption recovery), or a batch a full map refused before the
    /// map grew.
    bool owes_retry = false;

private:
    llvm::SmallVector<Fid> reindex_ids;

    llvm::SmallVector<Fid> index_ids;

    llvm::DenseSet<Fid> reindex_seen;

    llvm::DenseSet<Fid> index_seen;
};

/// When a save rebuilds the derived search index before it commits.
enum class SearchRebuild : std::uint8_t {
    /// Only when the merges since its last build left it far behind,
    /// or it is damaged.
    Behind,
    /// Also when it is missing or merely behind: no more indexing is
    /// queued, so the persisted one is fresh for the next cold start.
    Idle,
    /// Never: the index commits without waiting for it, and the next
    /// session rebuilds it.
    Never,
};

/// Why a merge committed nothing.
enum class MergeError : std::uint8_t {
    /// The result failed verification: the file counts as failed, not
    /// indexed.
    Invalid,
    /// A variant the result names by hash alone is not stored for its
    /// file — it left after known_variants was taken, or a file with the
    /// same bytes holds it: the file must run again.
    Outdated,
};

struct LoadOptions {
    /// Keep the removals in memory only: an out-of-process reader must not
    /// delete blobs a concurrently running server may be about to
    /// reference.
    bool read_only = false;

    /// Serve the shards straight from the opening read snapshot, pinned
    /// for the index's lifetime, instead of copying them out and retiring
    /// it. For a short-lived reader (`clice query`, `clice index stats`)
    /// the copy is the whole cost of the load; a long read-only session
    /// (batch lint) copies so a concurrent writer can reclaim its pages
    /// meanwhile.
    bool borrow = false;
};

/// What a writer's load (ProjectIndex::load) found in the database.
enum class LoadOutcome : std::uint8_t {
    /// The persisted index loaded.
    Loaded,
    /// There was no project table: a first run. What the database held
    /// beside it is unreachable and leaves.
    Empty,
    /// A project table blob existed but did not decode (an old format, or
    /// corrupt): the index starts empty and the database's blobs leave.
    /// A read-only reader must report an unusable cache instead of an
    /// empty index.
    Undecodable,
    /// A project table blob exists but could not be read: a transient IO
    /// error, not absence — the database is left as it is.
    Unreadable,
};

/// One save's blobs: the index's (ProjectIndex::begin_save), in commit
/// order, then whatever the caller appends, and the keys it removes.
struct SaveBatch {
    std::vector<BlobDatabase::Blob> blobs;

    llvm::SmallVector<BlobKey> removals;

    /// The files and TUs of the index's slots, in slot order.
    llvm::SmallVector<Fid> shard_ids;

    llvm::SmallVector<Fid> instantiation_ids;

    llvm::SmallVector<Fid> manifest_ids;

    /// Instantiation shards the batch removes.
    llvm::SmallVector<Fid> retired_ids;

    std::optional<std::uint32_t> table_slot;

    std::optional<std::uint32_t> search_slot;

    /// How many slots the index filled.
    std::uint32_t index_slots = 0;

    bool had_table = false;
};

/// The project's index, in memory and on disk:
///
/// - the project-wide external symbol table with per-symbol reference-file
///   bitmaps (the cross-file query fan-out): the persisted project table read
///   in place, plus the entries merged or changed since it was bound. A
///   reader never copies an entry; a writer copies only the entries it changes
///   and folds them into the next blob it writes,
/// - one manifest per indexed TU (replaced wholesale by its reindex),
/// - the tables derived from the manifests: per file, which TU contributed
///   which variant — its distinct hashes are the file's live variants, the
///   mask Shard queries filter by, and its emptiness is what retires a
///   shard blob —, the places failed lookups looked, the inputs of imported
///   modules, the shared locals' file sets and the instantiation uses,
/// - the per-file shard blobs, fetched from the database on first use by a
///   reader and kept whole by the writer, and beside them the per-file
///   instantiation blobs, whose resolutions live while some manifest uses
///   them,
/// - the name search index and the entries it does not describe.
///
/// The writer merges TU index results in, drops TUs, judges their
/// freshness, and persists: compaction, the save batch and its failures,
/// the move of borrowed bytes onto a newer read snapshot, the recovery from
/// a corrupt database.
///
/// The FileVersion table manifests reference lives in clice::FileTable,
/// shared with every other freshness consumer — and, in a server running
/// several projects, with every other project's index. The project table
/// persists the versions some manifest references under ids of its own
/// (see persisted_ids), which adopt_file_versions maps into the table.
///
/// There is a single path-id space at runtime (clice::FileTable); the blob
/// carries its own path table, appended to across writes and mapped to
/// the table's ids when bound.
class ProjectIndex {
public:
    explicit ProjectIndex(FileTable& files);
    ~ProjectIndex();
    ProjectIndex(ProjectIndex&&) noexcept;

    /// The name the database keeps for `path`.
    std::string portable(llvm::StringRef path) const;

    /// The path a name the database keeps names in this checkout.
    Spelling local(llvm::StringRef name) const;

    /// The key of a file's blobs (its shard, a TU's manifest) in the
    /// database.
    std::string key_of(Fid file) const;

    /// The symbol's stored facts; the strings borrow the base or the entry.
    std::optional<SymbolView> find_symbol(SymbolHash hash) const;

    /// How many files reference the symbol; 0 for an unknown hash.
    std::uint32_t reference_count(SymbolHash hash) const;

    /// The files referencing the symbol, as the file table's ids.
    void for_each_reference_file(SymbolHash hash, llvm::function_ref<void(Fid)> visit) const;

    /// The files whose instantiation shards name the symbol.
    void for_each_instantiation_file(SymbolHash hash, llvm::function_ref<void(Fid)> visit) const;

    /// Every symbol, base entries and changed entries alike, in no particular
    /// order; `visit` returns false to stop.
    void for_each_symbol(
        llvm::function_ref<bool(SymbolHash, const SymbolView&, std::uint32_t references)> visit)
        const;

    std::uint32_t symbol_count() const;

    /// A TU's manifest: held by a writer, fetched on first use by a reader
    /// — whose copy names its versions by persisted ids. Null when the TU
    /// has none current.
    const TUManifest* manifest(Fid tu) const;

    /// A TU's manifest a writer holds, in the file table's version ids;
    /// null in a reader, which fetches them (manifest()).
    const TUManifest* held_manifest(Fid tu) const;

    /// Every manifest a writer holds.
    void for_each_manifest(llvm::function_ref<void(Fid tu, const TUManifest&)> visit) const;

    /// How many TUs a writer holds a manifest of.
    std::uint32_t manifest_count() const;

    /// The TUs that contributed a variant to `file`: from the manifests in a
    /// writer, from the project table's persisted copy in a reader, which
    /// holds no manifests to derive it from.
    void for_each_contributor(Fid file, llvm::function_ref<void(Fid)> visit) const;

    /// The TUs that contributed to `file` in a writer, each with the variant
    /// it contributed.
    void for_each_contribution(Fid file,
                               llvm::function_ref<void(Fid tu, std::uint64_t variant)> visit) const;

    /// The TUs some failed lookup of whose parse looked at `place`
    /// (TUManifest::absent).
    void for_each_absent_lookup(Fid place, llvm::function_ref<void(Fid)> visit) const;

    /// The TUs reading `input` through an imported module or a loaded
    /// precompiled header (TUManifest::imports).
    void for_each_importer(Fid input, llvm::function_ref<void(Fid)> visit) const;

    /// The files holding entries of a symbol below external scope: `anchor`,
    /// a file that holds some, and the files of every TU's shared local
    /// (TUManifest::shared_locals) for it that lists a file reached — from
    /// the manifests in a writer, from the project table's persisted copy in a
    /// reader. A `static` in a header is thus reached in every unit
    /// including the header or reading it through a precompiled header.
    /// Each file once.
    void for_each_file_of_local(SymbolHash hash,
                                Fid anchor,
                                llvm::function_ref<void(Fid)> visit) const;

    /// The include edges of a document, the input of the document-link
    /// projection: from its own TU manifest when it has one, else from the
    /// contributing TUs' manifests (a header reached only through source
    /// TUs — its directives are nodes hanging off the header's own node
    /// there). Only manifests that entered the document at its shard's
    /// content generation contribute; empty when the file was never
    /// indexed.
    std::vector<IncludeEdge> include_edges(Fid file) const;

    /// The file's shard: held, or fetched from the database the index was
    /// opened over. Null when the file has no index. A query keeps the
    /// pointers to the shards it read while its fan-out fetches others.
    const Shard* shard(Fid file) const;

    /// Every shard held or fetched so far: all of them in a writer.
    void for_each_shard(llvm::function_ref<void(Fid, const Shard&)> visit) const;

    std::uint32_t shard_count() const;

    /// The file's instantiation shard, like shard(). One lives with its
    /// file's shard of the same content generation.
    const InstantiationShard* instantiation_shard(Fid file) const;

    /// Every instantiation shard held or fetched so far.
    void for_each_instantiation_shard(
        llvm::function_ref<void(Fid, const InstantiationShard&)> visit) const;

    /// The distinct variant hashes contributed to `path_id` — the file's live
    /// variant set.
    llvm::SmallVector<std::uint64_t> live_variants(Fid path_id) const;

    /// The distinct uses named in `path_id`, ascending — the file's live
    /// use set.
    llvm::SmallVector<std::uint64_t> live_uses(Fid path_id) const;

    /// The name search index over the symbol table as last built, and the
    /// entries it does not describe — merged or changed since — which a
    /// search reads from the table instead until the writer folds them
    /// into a rebuilt index. Both persist next to the table: the table
    /// pins the search blob's generation and carries the pending entries,
    /// so a reader adopts exactly the search index the table was saved
    /// with.
    const SearchIndex& search_index() const {
        return search;
    }

    const llvm::DenseSet<SymbolHash>& search_pending() const {
        return pending_symbols;
    }

    /// The files whose names of internal linkage, in no table, changed
    /// since the build: a search reads them from the files' shards.
    const llvm::DenseSet<Fid>& search_pending_files() const {
        return pending_files;
    }

    /// What an instantiation shard names a TU by: the hash of its database
    /// key.
    std::uint64_t unit_hash(Fid tu) const {
        return llvm::xxh3_64bits(key_of(tu));
    }

    /// The TU with a manifest a unit hash names.
    std::optional<Fid> find_unit(std::uint64_t hash) const {
        auto it = units.find(hash);
        return it != units.end() ? std::optional(it->second) : std::nullopt;
    }

    /// Whether the run that walked a key is its TU's current manifest: a
    /// key outlives it only until the next compaction of its file.
    bool current_run(const Producer& producer) const {
        auto tu = find_unit(producer.unit);
        return tu && manifests.find(*tu)->second.built_at == producer.run;
    }

    /// Open the persisted index for reading: the project table and search blobs
    /// bound in place from the database's read snapshot — which the
    /// caller keeps pinned for the index's lifetime — and shards fetched
    /// from it on first use. False when there is no readable project table.
    bool open(BlobDatabase& db);

    /// Load the persisted index as the writer serves it: the project table,
    /// the search index it pins, every manifest the table pins at its
    /// generation, the shards and instantiation shards those manifests
    /// expect. What does not hold together — stale or lost manifests,
    /// shards that miss a variant or carry another content generation,
    /// orphans — is left out, its TUs in `report`'s reindex debt and its
    /// blobs removed at the first save unless `read_only`.
    LoadOutcome load(BlobDatabase& db, LoadOptions options, IndexReport& report);

    /// Forget everything loaded and merged, as after a corrupt database:
    /// the workspace stays, and the next save writes an empty project table
    /// in place of the lost one, so the next load reads the command
    /// snapshot's debt back.
    void clear();

    /// Merge a TUIndex result: intern FileVersions, replace the TU's
    /// manifest, and write shard blobs only for variants no shard stores yet
    /// — a re-index whose entries are unchanged records its contributions and
    /// touches nothing else. `imports` is what the PCMs and the
    /// precompiled header the parse read were built from.
    std::expected<IndexReport, MergeError> merge(llvm::StringRef tu_index,
                                                 llvm::ArrayRef<DepState> imports);

    /// Drop a TU's index wholesale: manifest and contributions now (the
    /// affected shards' live masks follow), persisted blobs at the next
    /// save. For invalidation content-based freshness cannot see — a
    /// compile-command change — where a surviving manifest would keep
    /// judging the old-command index fresh, in this session and after a
    /// restart.
    void drop(Fid tu, IndexReport& report);

    /// The variant identities stored for the files `tu` is expected to
    /// include — `include_closure`, its scanned include closure, and its
    /// last manifest's files — sorted: what the worker indexing it may send
    /// as bare hashes (TUIndexOptions::known_variants).
    std::vector<std::uint64_t> known_variants(Fid tu, llvm::DenseSet<Fid> include_closure) const;

    /// The instantiation keys stored for the same files but `tu` itself
    /// whose walk still holds, sorted: what the worker indexing `tu` may
    /// skip (TUIndexOptions::known_instantiations). A key holds while the
    /// run that walked it is its unit's current one and that unit is fresh
    /// — an edit of a file the instantiation read makes every unit
    /// including it stale, and the first to rebuild walks the key again —
    /// and never for `tu` itself, whose rebuild is what would confirm it.
    /// The file `tu` names walks its own instantiations: an open buffer's
    /// index stands in for the file's stored one. Valid within one
    /// freshness round, like needs_reindex.
    std::vector<std::uint64_t> known_instantiations(Fid tu, llvm::DenseSet<Fid> include_closure);

    /// Start a freshness round: FileVersion results hold for one round —
    /// the disk can change under a running round, but staleness is
    /// re-judged per round anyway.
    void begin_round() {
        version_stale.clear();
    }

    /// Check whether a file needs re-indexing: no manifest, or a stale
    /// FileVersion among its dependencies. Valid only within one round:
    /// the results are cleared by begin_round(), never here.
    bool needs_reindex(Fid file);

    /// Rebuild the search index from the symbol table when `search` says
    /// it is due — the build runs on the thread pool over a snapshot —,
    /// adopt it, and hold its blob for the next batch.
    kota::task<> rebuild_search(SearchRebuild search);

    /// Compact the shards whose variant set shrank and retire the ones with
    /// no live variant left, then move the dirty state into `batch`: the
    /// shards, the instantiation shards, the manifests, the project table
    /// and the search blob, in this order, and the removals. Merges
    /// committing while the batch is written re-dirty for the next save.
    void begin_save(SaveBatch& batch, IndexReport& report);

    /// The batch never reached the database: what it carried is dirty
    /// again.
    void abandon_save(SaveBatch& batch);

    /// The batch was written; `failed` are the slots the database did not
    /// commit, which stay dirty for a later save. The project table
    /// written becomes the base.
    void finish_save(SaveBatch& batch, llvm::ArrayRef<std::uint32_t> failed);

    /// After a save committed, move the shards that borrow database
    /// memory onto a fresh read snapshot (growing the map first when the
    /// write hit a full one), then retire the previous snapshot. False when
    /// the database turned out corrupt: the caller recovers
    /// (recover_corrupt_database).
    kota::task<bool> migrate_views(BlobDatabase& db, kota::event_loop& loop, IndexReport& report);

    /// The index's part of the recovery from a corrupt database, whose
    /// contents nothing survives: the shards borrowing it are shed with
    /// their owners requeued, while every manifest and the project table
    /// re-dirty to re-persist into the database opened in its place.
    void recover_corrupt_database(IndexReport& report);

    /// Shard blobs whose write has not durably completed: dirty since the
    /// last save plus the batch a running save is committing. The gauge
    /// reaches zero only once every shard write finished — never in the
    /// window where a save has moved the dirty set into its batch but its
    /// commit (and the last_save_shards update) is still in flight.
    std::uint32_t pending_shard_writes() const {
        return static_cast<std::uint32_t>(dirty_shards.size() + dirty_instantiations.size()) +
               saving_shards;
    }

    /// How many shard and instantiation blobs the last save durably
    /// committed. A steady-state save commits 0 — only variant-set and
    /// resolution changes rewrite a blob — so the stats endpoint can pin
    /// full-rewrite regressions.
    std::uint32_t last_save_shards() const {
        return saved_shards;
    }

    /// Whether index state remains that no save committed.
    bool has_unsaved_state() const {
        return !dirty_shards.empty() || !dirty_instantiations.empty() || !dirty_manifests.empty() ||
               table_dirty || !search_bytes.empty();
    }

    /// Bind a project table blob as the table's base (copied when it is not
    /// readable_in_place), mapping its path table into the file table. False — and the index
    /// untouched — when the bytes are not a project table blob of this format or are inconsistent.
    /// The base's bitmaps are not decoded here: a reader decodes one when a query reaches it, a
    /// writer proves them all with verify_bitmaps.
    bool bind_table(std::unique_ptr<llvm::MemoryBuffer> blob);

    /// The writer's half of a bound blob: intern its file versions into
    /// the file table and read the per-TU manifest
    /// pins — `manifest_pins` maps each pinned TU's tu_version (as the table's
    /// id) to the generation its manifest must carry to be adopted.
    /// Rejects a blob whose version table is inconsistent, leaving the file
    /// table untouched.
    std::expected<void, llvm::StringRef>
        adopt_file_versions(llvm::DenseMap<VersionID, std::uint64_t>& manifest_pins);

    /// Whether every reference bitmap of the base decodes and stays inside
    /// its path table — the writer's gate: a malformed image normalized to
    /// empty would silently lose the symbol's reference files with nothing
    /// ever rebuilding them, so the whole blob is rejected instead.
    bool verify_bitmaps() const;

    /// bind_table, adopt_file_versions and verify_bitmaps over a copy of
    /// `data`, all or nothing: the loader's gate for a persisted project table.
    std::expected<void, llvm::StringRef>
        load_table(llvm::StringRef data, llvm::DenseMap<VersionID, std::uint64_t>& manifest_pins);

    /// The symbol's entry to change: the changed entry when there is one,
    /// else a copy of the base entry, else a fresh one.
    Symbol& edit(SymbolHash hash);

    /// Merge a TU's external symbols straight off the serialized TU index; `file_ids_map`
    /// maps the TU-local ids of `index`'s path table to pool ids. Symbol
    /// names are copied only for symbols new to the table. `added`
    /// receives the hashes of the symbols new to the table or whose name,
    /// arguments, parent, file or flags the merge changed — what a name
    /// search keyed on the table's entries must re-read.
    void merge_symbols(const TUIndex& index,
                       llvm::ArrayRef<Fid> file_ids_map,
                       llvm::SmallVectorImpl<SymbolHash>* added = nullptr);

    /// Serialize the project table: the versions some manifest still
    /// references (the shared table is not touched — a version the index
    /// stops referencing can still anchor another consumer's check; ids
    /// are never reused either way), the symbol table — base entries copied,
    /// changed entries encoded, in hash order — with its path table, a per-TU
    /// pin of every manifest's generation, and the search index's
    /// generation and pending entries. The changed entries are held aside until
    /// rebase or restore_unwritten says whether the bytes were persisted.
    std::vector<std::byte> serialize_table();

    /// The bytes serialize_table produced were persisted: they become the
    /// base, and the entries they hold leave the changed set. Entries changed
    /// since the serialization stay changed.
    void rebase(std::unique_ptr<llvm::MemoryBuffer> blob);

    /// The bytes serialize_table produced were not persisted: the entries
    /// held aside return to the changed set, under the entries changed since.
    void restore_unwritten();

    /// The file table's id for a version id the loaded project table
    /// carries; nullopt for any other.
    std::optional<VersionID> runtime_version(std::uint32_t persisted) const;

    /// Rewrite a manifest read from disk into the file table's version
    /// ids. False when it names a version the loaded project table does not
    /// carry — the loader's staleness gate for manifests.
    bool import_manifest(TUManifest& manifest) const;

    /// The manifest as persisted: its versions under this index's
    /// persisted ids, handing ids out to versions that have none yet.
    TUManifest export_manifest(const TUManifest& manifest);

    /// Install (or replace) a TU's manifest, rederive the tables derived
    /// from it and refresh the live masks of the files whose contribution
    /// or use set changed. Returns those files.
    llvm::SmallVector<Fid> apply_manifest(Fid tu, TUManifest manifest);

    /// Drop a TU's manifest and its entries in the tables derived from it,
    /// refreshing the live masks like apply_manifest. Returns the affected
    /// files.
    llvm::SmallVector<Fid> remove_manifest(Fid tu);

    /// Adopt a persisted search blob when the bound project table pins its
    /// generation; otherwise the index stays unloaded and searches scan
    /// the table.
    bool bind_search(std::unique_ptr<llvm::MemoryBuffer> blob);

    /// The blobs of a batch the database took stay dirty: written into a
    /// database that turned out corrupt, they re-persist into the one
    /// opened in its place.
    void redirty(const SaveBatch& batch);

    /// Generation of the persisted project table, bumped once per save that
    /// writes it. Manifests record the generation they were
    /// saved under (TUManifest::table_generation), and the project table pins the
    /// generation expected of every TU's manifest; the loader adopts a manifest
    /// only on an exact match, so a lost or failed manifest write cannot
    /// leave an older on-disk manifest serving as current.
    std::uint64_t table_generation() const {
        return generation;
    }

    /// The root the database names the files under relative to, by their
    /// portable names (path::portable), so a moved checkout keeps its
    /// index; empty names every file by its identity. Set before a blob
    /// is bound or written.
    CanonicalPath workspace;

private:
    friend struct testing::IndexInternals;

    /// Take the bound base's generation and search state.
    void adopt_base();

    /// A symbol's entry newer than the bound base's: the changed one, else
    /// the one held aside for a write.
    const Symbol* newer_entry(SymbolHash hash) const;

    std::uint32_t persisted_id(VersionID version);

    /// Recompute a file's live variants and live uses from the manifests.
    void refresh_live(Fid file);

    /// The file's internal names may have changed — its shard replaced, its
    /// live variants changed: searches scan its shard until the next build.
    void search_file_changed(Fid file);

    /// Whether the search index is worth rebuilding now: the symbols
    /// merged since its build outgrew what a direct scan should carry,
    /// or indexing became idle after enough merges (or with no index at all).
    bool search_rebuild_due(SearchRebuild search) const;

    /// Retire a file's instantiation shard, now or at the next save.
    void retire_instantiations(Fid file);

    /// Add every TU contributing to `file`'s shard to the report's
    /// reindex debt. Used when the file's resident index is lost while its
    /// manifests still read fresh: no in-process event would ever rebuild
    /// it, and for standalone headers no restart pass would either.
    void requeue_owners(Fid file, IndexReport& report);

    /// Drop every resident shard that may borrow database memory —
    /// everything not dirty, since dirty shards own their bytes by
    /// construction (merges install memory copies) — and requeue the
    /// owners of the dropped shards.
    void shed_borrowed_shards(IndexReport& report);

    /// Two-layer staleness test on a FileVersion, cached per round; a hash
    /// match after a stat mismatch repairs the version's stat fast path in
    /// place for every consumer.
    bool file_version_stale(VersionID version);

private:
    FileTable& files;

    std::unique_ptr<ProjectTable> base;

    /// Entries merged or changed since the base was bound, in file-table ids.
    llvm::DenseMap<SymbolHash, Symbol> changed;

    /// Entries serialized by the last serialize_table and not yet known to
    /// have been persisted; reads consult them after `changed`.
    llvm::DenseMap<SymbolHash, Symbol> written;

    std::uint64_t generation = 0;

    /// TU -> its manifest.
    llvm::DenseMap<Fid, TUManifest> manifests;

    /// A reader's manifests, fetched on first use; nullopt remembers a
    /// TU without a current one.
    mutable llvm::DenseMap<Fid, std::optional<TUManifest>> fetched_manifests;

    /// Derived from `manifests`: file -> (TU -> variant hash).
    llvm::DenseMap<Fid, llvm::SmallDenseMap<Fid, std::uint64_t, 2>> contributions;

    /// Derived from `manifests`: a place some TU's failed lookup looked ->
    /// those TUs.
    llvm::DenseMap<Fid, llvm::SmallDenseSet<Fid, 2>> absent_lookups;

    /// Derived from `manifests`: an input of an imported module or of a
    /// loaded precompiled header -> the TUs reading it.
    llvm::DenseMap<Fid, llvm::SmallDenseSet<Fid, 2>> importers;

    /// Derived from `manifests`: a symbol below external scope -> the sets
    /// of files the TUs' shared locals list for it, each with how many list
    /// it. Kept apart, as two TUs can name one hash in files they do not
    /// share: a namespace nested in an anonymous namespace has no file in
    /// its hash.
    llvm::DenseMap<SymbolHash, std::map<std::vector<Fid>, std::uint32_t>> shared_local_sets;

    /// Derived from `manifests`: file -> (TU -> the instantiation use it
    /// names there).
    llvm::DenseMap<Fid, llvm::SmallDenseMap<Fid, std::uint64_t, 2>> instantiation_uses;

    /// Derived from `manifests`: unit hash -> TU.
    llvm::DenseMap<std::uint64_t, Fid> units;

    /// Per-file shard blobs keyed by the file table's ids: symbol
    /// occurrences, relations and stored content for position mapping,
    /// served zero-copy. The writer holds every persisted shard here; a
    /// reader opened over a database fills it on first use. A node-based
    /// map: a query keeps pointers to the shards it read while its fan-out
    /// fetches others.
    mutable std::map<Fid, Shard> shards;

    /// Per-file instantiation blobs, held and fetched like `shards`.
    mutable std::map<Fid, InstantiationShard> instantiations;

    /// The database a reader fetches from, when opened over one.
    BlobDatabase* db = nullptr;

    SearchIndex search;

    llvm::DenseSet<SymbolHash> pending_symbols;

    llvm::DenseSet<Fid> pending_files;

    std::uint64_t search_generation = 0;

    /// Merges committed since the search index was built.
    std::uint32_t merges_since_search_build = 0;

    /// While a search index is built, the symbols and the files merges
    /// change: they stay pending after the build, which read them before
    /// the change.
    struct SearchBuildChanges {
        llvm::DenseSet<SymbolHash> symbols;

        llvm::DenseSet<Fid> files;
    };

    std::optional<SearchBuildChanges> changed_during_search_build;

    /// A rebuilt search blob no batch has committed yet.
    std::vector<std::byte> search_bytes;

    /// Blobs mutated since the last save, plus whether the project table
    /// (symbols, FileVersion table) changed.
    llvm::DenseSet<Fid> dirty_shards;

    llvm::DenseSet<Fid> dirty_instantiations;

    llvm::DenseSet<Fid> dirty_manifests;

    bool table_dirty = false;

    /// Instantiation shards retired since the last save, whose persisted
    /// blobs it removes.
    llvm::DenseSet<Fid> retired_instantiations;

    /// Blob removals discovered during load (stale manifests, orphan
    /// shards, removed layouts), deferred into the first save so startup
    /// never runs synchronous database commits on the event loop.
    llvm::SmallVector<BlobKey> pending_removals;

    std::uint32_t saved_shards = 0;

    /// Shards in the batch a running save is committing (see
    /// pending_shard_writes).
    std::uint32_t saving_shards = 0;

    /// Per-round FileVersion staleness results: many TUs share the same
    /// versions, and one stat (or repair) per version per round is enough.
    llvm::DenseMap<VersionID, bool> version_stale;

    /// The persisted FileVersion id space: the project table and the
    /// manifests name versions by ids private to this index's lineage,
    /// handed out from `next_persisted_id` and never reused, so one file
    /// table can hold the versions of several projects' indexes. Loading
    /// maps the blob's ids to the table's (`runtime_ids`, consulted while
    /// the manifests load); writing maps back, handing out ids to the
    /// versions the lineage has not persisted yet.
    llvm::DenseMap<std::uint32_t, VersionID> runtime_ids;

    llvm::DenseMap<VersionID, std::uint32_t> persisted_ids;

    std::uint32_t next_persisted_id = 0;
};

}  // namespace clice::index
