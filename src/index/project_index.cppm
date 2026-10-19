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
import :support.signal;
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

    /// Serve the blobs straight from the read snapshot, pinned for the
    /// index's lifetime, instead of copying each one fetched out of it. For
    /// a short-lived reader (`clice query`, `clice index stats`) the copy
    /// is wasted; a long read-only session (batch lint) copies so a
    /// concurrent writer can reclaim the pages of the snapshot it opened.
    bool borrow = false;
};

/// What a load (ProjectIndex::load) found in the database.
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
/// - one manifest per indexed TU (a unit), replaced wholesale by its
///   reindex, and what the units' manifests say keyed the way queries ask:
///   per file, which unit contributed which variant — its distinct hashes are
///   the file's live variants, the mask Shard queries filter by, and its
///   emptiness is what retires a shard blob —, the places failed lookups
///   looked, the inputs of imported modules, the shared locals' file sets
///   and the instantiation uses. The project table persists those tables;
///   a session reads them there, under the units it merged or dropped, whose
///   manifests it holds and derives the same tables from. A persisted
///   manifest is read only when a unit's include tree or old state is
///   wanted,
/// - the per-file shard blobs and instantiation blobs, read from the
///   database the first time a query or a merge wants one and checked then
///   against what the units say they hold: one that fails is lost data,
///   whose units a writer drops and rebuilds (take_lost),
/// - the name search index and the entries it does not describe.
///
/// A reader and a writer read alike; a writer merges TU index results in,
/// drops TUs, judges their freshness, and persists: compaction, the save
/// batch and its failures, the move of borrowed bytes onto a newer read
/// snapshot, the recovery from a corrupt database.
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

    /// Whether the TU has a current manifest.
    bool has_manifest(Fid tu) const;

    /// A TU's current manifest, in the file table's version ids: the one
    /// this session merged, else the persisted one. Nullopt when the TU has
    /// none, or its persisted one is lost.
    std::optional<TUManifest> manifest(Fid tu) const;

    /// Every TU with a current manifest.
    void for_each_unit(llvm::function_ref<void(Fid tu)> visit) const;

    std::uint32_t unit_count() const;

    /// The TUs that contributed a variant to `file`.
    void for_each_contributor(Fid file, llvm::function_ref<void(Fid)> visit) const;

    /// The TUs that contributed to `file`, each with the variant it
    /// contributed.
    void for_each_contribution(Fid file,
                               llvm::function_ref<void(Fid tu, std::uint64_t variant)> visit) const;

    /// Every file some TU contributed a variant to.
    void for_each_indexed_file(llvm::function_ref<void(Fid)> visit) const;

    /// The TUs some failed lookup of whose parse looked at `place`
    /// (TUManifest::absent).
    void for_each_absent_lookup(Fid place, llvm::function_ref<void(Fid)> visit) const;

    /// The TUs reading `input` through an imported module or a loaded
    /// precompiled header (TUManifest::imports).
    void for_each_importer(Fid input, llvm::function_ref<void(Fid)> visit) const;

    /// The files holding entries of a symbol below external scope: `anchor`,
    /// a file that holds some, and the files of every TU's shared local
    /// (TUManifest::shared_locals) for it that lists a file reached. A
    /// `static` in a header is thus reached in every unit including the
    /// header or reading it through a precompiled header. Each file once.
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

    /// The file's shard: held, or read from the database and checked
    /// against the variants and the content generation the units say it
    /// holds. Null when the file has no index, or its blob is lost
    /// (take_lost). A query keeps the pointers to the shards it read while
    /// its fan-out reads others.
    const Shard* shard(Fid file) const;

    /// Every shard held or read so far.
    void for_each_shard(llvm::function_ref<void(Fid, const Shard&)> visit) const;

    std::uint32_t shard_count() const;

    /// The file's instantiation shard, like shard(). One lives with its
    /// file's shard of the same content generation, holding every use the
    /// units name there.
    const InstantiationShard* instantiation_shard(Fid file) const;

    /// Every instantiation shard held or read so far.
    void for_each_instantiation_shard(
        llvm::function_ref<void(Fid, const InstantiationShard&)> visit) const;

    /// Read every indexed file's shard and instantiation shard and every
    /// unit's manifest, so that lost_units tells all the lost data: for the
    /// commands that walk the whole index.
    void read_all() const;

    /// The units whose index lost data reads found since take_lost: the
    /// ones contributing to a lost shard, the ones using resolutions a
    /// file's instantiation blob lost, the ones whose manifest is lost.
    llvm::SmallVector<Fid> lost_units() const;

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
    std::optional<Fid> find_unit(std::uint64_t hash) const;

    /// Whether the run that walked a key is its TU's current manifest: a
    /// key outlives it only until the next compaction of its file.
    bool current_run(const Producer& producer) const;

    /// Load the persisted index: the project table and the search index it
    /// pins; manifests, shards and instantiation shards stay in the
    /// database until something reads them. Unless `read_only`, the blobs
    /// no unit names leave at the first save, and the TU of a manifest
    /// that outran its project table write goes into `report`'s reindex
    /// debt.
    LoadOutcome load(BlobDatabase& db, LoadOptions options, IndexReport& report);

    /// The lost data reads found since the last call, as a writer heals it:
    /// the TUs whose index a lost shard held, and the TUs whose manifest is
    /// lost, lose their manifest and are owed a reindex, as are the TUs
    /// whose instantiation uses a file's blob lost.
    IndexReport take_lost();

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
        stale_units.reset();
    }

    /// Check whether a file needs re-indexing: no manifest, or a stale
    /// FileVersion among its dependencies, or a file at a place its failed
    /// lookups looked. Valid only within one round: the results are cleared
    /// by begin_round(), never here. A unit the session did not merge is
    /// judged through the table's per-version units, never its manifest.
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
    /// their owners requeued, the units the session did not merge leave and
    /// reindex, while every merged manifest and the project table re-dirty
    /// to re-persist into the database opened in its place (attach).
    void recover_corrupt_database(IndexReport& report);

    /// Read blobs from `database` from now on: the one opened in place of
    /// a corrupt one, or none.
    void attach(BlobDatabase* database) {
        db = database;
    }

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

    /// Intern a bound blob's file versions into the file table. Rejects a
    /// blob whose version table is inconsistent — or whose units name a
    /// version it lacks —, leaving the file table untouched.
    std::expected<void, llvm::StringRef> adopt_file_versions();

    /// Whether every reference bitmap of the base decodes and stays inside
    /// its path table — the writer's gate: a malformed image normalized to
    /// empty would silently lose the symbol's reference files with nothing
    /// ever rebuilding them, so the whole blob is rejected instead.
    bool verify_bitmaps() const;

    /// bind_table, adopt_file_versions and verify_bitmaps, all or nothing:
    /// the loader's gate for a persisted project table.
    std::expected<void, llvm::StringRef> load_table(std::unique_ptr<llvm::MemoryBuffer> buffer);

    /// load_table over a copy of `data`.
    std::expected<void, llvm::StringRef> load_table(llvm::StringRef data);

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
    std::string serialize_table();

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
    /// carry — the read's staleness gate for manifests.
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
    /// generation expected of every TU's manifest; a read adopts a manifest
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

    /// Fires when a read finds the first lost data since take_lost.
    Signal<> on_data_lost;

private:
    friend struct testing::IndexInternals;

    /// A variant a TU contributed to a file, and the file version it was
    /// built from.
    struct Contributed {
        std::uint64_t variant = 0;

        VersionID version;
    };

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

    /// Drop every resident view that may borrow database memory —
    /// everything not dirty, since dirty shards own their bytes by
    /// construction (merges install memory copies) — returning the files
    /// whose loaded view went.
    llvm::SmallVector<Fid> drop_borrowed_views();

    /// Two-layer staleness test on a FileVersion, cached per round; a hash
    /// match after a stat mismatch repairs the version's stat fast path in
    /// place for every consumer.
    bool file_version_stale(VersionID version);

    /// The file table's id of every base unit's TU, overridden or not.
    llvm::ArrayRef<Fid> base_units() const;

    /// The base's position of a TU's unit, under the units the session
    /// overrode; nullopt for a TU the base has no live unit of.
    std::optional<std::uint32_t> base_unit(Fid tu) const;

    /// The file table's ids of a base unit set, but the units the session
    /// overrode.
    void for_each_base_unit(llvm::ArrayRef<std::uint8_t> units,
                            llvm::function_ref<void(Fid)> visit) const;

    /// Every unit contributing to `file`, with what it contributed.
    void for_each_contributed(Fid file,
                              llvm::function_ref<void(Fid tu, const Contributed&)> visit) const;

    /// Every unit using resolutions of `file`'s instantiation shard, with
    /// its use.
    void for_each_use(Fid file, llvm::function_ref<void(Fid tu, std::uint64_t use)> visit) const;

    /// The TU's persisted manifest in the file table's version ids, when the
    /// base pins it and the database holds it at the pinned generation.
    std::optional<TUManifest> persisted_manifest(Fid tu) const;

    /// The one way a unit leaves the base: what the base says of it stops
    /// counting (overridden, overridden_ids, units).
    void hide_base_unit(Fid tu);

    /// Take a TU out of the base, returning the files its persisted
    /// manifest says it contributed to or used resolutions in; every file
    /// with a held view when that manifest is lost.
    llvm::SmallVector<Fid> override_unit(Fid tu);

    /// The base units judged stale this round (needs_reindex).
    const llvm::DenseSet<Fid>& stale_base_units();

    /// The content generations the units' variants of `file` were built
    /// from; 0 is no generation.
    llvm::SmallVector<std::uint64_t, 1> generations_of(Fid file) const;

    /// The content generation a variant some unit contributes to `file`
    /// was built from; nullopt when no unit contributes it, or its file
    /// version has no content hash.
    std::optional<std::uint64_t> variant_generation(Fid file, std::uint64_t variant) const;

    /// The content generation of `file`'s shard: the held shard's, else the
    /// one every unit's variant was built from, else (units built from
    /// several) the shard's own, read. Nullopt when the file has no shard.
    std::optional<std::uint64_t> shard_generation(Fid file) const;

    /// Read a file's shard from the database and check it holds every live
    /// variant at the units' generation; a failure is recorded as lost.
    Shard fetch_shard(Fid file) const;

    /// Read a file's instantiation shard and check it lives with the
    /// file's shard of content generation `generation` and holds every live
    /// use; a failure is recorded as lost.
    InstantiationShard fetch_instantiations(Fid file, std::uint64_t generation) const;

    /// What lost data a read found.
    enum class Lost : std::uint8_t {
        /// A file's shard: missing, corrupt, or not holding what the units
        /// say.
        Shard,
        /// A use some unit names in a file's instantiation blob.
        Uses,
        /// A unit's manifest: missing, corrupt, or of another generation
        /// than the table pins.
        Manifest,
    };

    /// Record lost data, firing on_data_lost for the first since take_lost.
    void lose(Fid file, Lost kind) const;

    /// The units that lost lost_units' part of their index they cannot
    /// serve without: whose manifest is lost, or contributing to a lost
    /// shard.
    llvm::SetVector<Fid> lost_owners() const;

    /// What a database read hands a reader: the bytes, copied out of the
    /// snapshot unless the session borrows it.
    std::unique_ptr<llvm::MemoryBuffer> read_blob(BlobKind kind, Fid file) const;

    /// Erase a manifest's entries from the tables derived from the merged
    /// manifests; returns the files it contributed to or used resolutions in.
    llvm::SmallVector<Fid> erase_derived(Fid tu, const TUManifest& manifest);

    /// A committed project table holds every TU the session merged or
    /// dropped but the ones a merge or a drop dirtied since it was
    /// serialized: those leave the session's own tables, read from the
    /// table once it is the base.
    void fold_written();

private:
    FileTable& files;

    std::unique_ptr<ProjectTable> base;

    /// Entries merged or changed since the base was bound, in file-table ids.
    llvm::DenseMap<SymbolHash, Symbol> changed;

    /// Entries serialized by the last serialize_table and not yet known to
    /// have been persisted; reads consult them after `changed`.
    llvm::DenseMap<SymbolHash, Symbol> written;

    std::uint64_t generation = 0;

    /// TU -> its manifest, for the TUs this session merged.
    llvm::DenseMap<Fid, TUManifest> manifests;

    /// The TUs whose base unit no longer counts: merged (their manifest is
    /// in `manifests`) or dropped this session.
    llvm::DenseSet<Fid> overridden;

    /// The same TUs as the base's path ids, the units its sets name.
    Bitmap overridden_ids;

    /// The base's units: the file table's id of each one's TU, and back,
    /// built on first use.
    mutable std::vector<Fid> base_unit_files;

    mutable llvm::DenseMap<Fid, std::uint32_t> base_unit_index;

    /// Derived from `manifests`: file -> (TU -> what it contributed there).
    llvm::DenseMap<Fid, llvm::SmallDenseMap<Fid, Contributed, 2>> contributions;

    /// Derived from `manifests`: a place some TU's failed lookup looked ->
    /// those TUs.
    llvm::DenseMap<Fid, llvm::SmallDenseSet<Fid, 2>> absent_lookups;

    /// Derived from `manifests`: an input of an imported module or of a
    /// loaded precompiled header -> the TUs reading it.
    llvm::DenseMap<Fid, llvm::SmallDenseSet<Fid, 2>> importers;

    /// Derived from `manifests`: a symbol below external scope -> the sets
    /// of files the TUs' shared locals list for it, each with the TUs
    /// listing it. Kept apart, as two TUs can name one hash in files they
    /// do not share: a namespace nested in an anonymous namespace has no
    /// file in its hash.
    llvm::DenseMap<SymbolHash, std::map<std::vector<Fid>, llvm::SmallDenseSet<Fid, 2>>>
        shared_local_sets;

    /// Derived from `manifests`: file -> (TU -> the instantiation use it
    /// names there).
    llvm::DenseMap<Fid, llvm::SmallDenseMap<Fid, std::uint64_t, 2>> instantiation_uses;

    /// Unit hash -> TU, for the base's units (built on first use) and the
    /// merged ones.
    mutable llvm::DenseMap<std::uint64_t, Fid> units;

    mutable bool base_units_hashed = false;

    /// The base units judged stale in this round, once needs_reindex asked.
    std::optional<llvm::DenseSet<Fid>> stale_units;

    /// Lost data reads found since take_lost: files whose shard is lost,
    /// files whose instantiation blob lost a use some unit names, and units
    /// whose manifest is lost.
    mutable llvm::DenseSet<Fid> lost_shards;

    mutable llvm::DenseSet<Fid> lost_instantiations;

    mutable llvm::DenseSet<Fid> lost_manifests;

    /// Files whose live variants or uses shrank since the last save: the
    /// save compacts or retires their blobs.
    llvm::DenseSet<Fid> shrunk;

    /// Per-file shard blobs keyed by the file table's ids: symbol
    /// occurrences, relations and stored content for position mapping,
    /// served zero-copy — the ones merged, and the ones read since the load,
    /// a failed read remembered as an empty entry. A node-based map: a query
    /// keeps pointers to the shards it read while its fan-out reads others.
    mutable std::map<Fid, Shard> shards;

    /// Per-file instantiation blobs, held and read like `shards`.
    mutable std::map<Fid, InstantiationShard> instantiations;

    /// The database the blobs are read from; null for an index never loaded.
    BlobDatabase* db = nullptr;

    /// Whether blobs read stay in the database's snapshot (LoadOptions).
    bool borrow = true;

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
    std::string search_bytes;

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
    /// the base and its manifests are read); writing maps back, handing
    /// out ids to the versions the lineage has not persisted yet, which
    /// the written table, once the base, names in turn.
    llvm::DenseMap<std::uint32_t, VersionID> runtime_ids;

    llvm::DenseMap<VersionID, std::uint32_t> persisted_ids;

    std::uint32_t next_persisted_id = 0;
};

}  // namespace clice::index
