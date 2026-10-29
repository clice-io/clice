module clice:index.project_index;

import :index.database;
import :index.format;
import :index.instantiation_shard;
import :index.project_records;
import :index.search_index;
import :index.shard;
import :index.tu_index;
import :index.tu_manifest;
import :support.prelude;
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

/// How much of the derived search index a save may rebuild before it
/// commits. Whatever it is, a save adds the symbols and files changed since
/// the newest run's snapshot as a run of their own.
enum class SearchRebuild : std::uint8_t {
    /// Also fold the newest runs into it, and build the whole index from the
    /// records when it is damaged or missing and far behind.
    Behind,
    /// Also build the whole index when it is missing or its newer runs
    /// outgrew half the first: no more indexing is queued, so the persisted
    /// one is compact for the next cold start.
    Idle,
    /// Nothing more: the index commits without waiting for a fold or a
    /// build, which a later save does.
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

/// What opening the database (ProjectIndex::open) found.
enum class LoadOutcome : std::uint8_t {
    /// The persisted index opened.
    Loaded,
    /// There was no header: a first run.
    Empty,
    /// The header is of another format version, or is not one: the
    /// database is discarded whole. A read-only reader reports an unusable
    /// cache instead.
    Undecodable,
    /// A header exists but could not be read: a transient IO error, not
    /// absence — the database is left as it is.
    Unreadable,
};

/// One save's blobs: the index's (ProjectIndex::begin_save), then whatever
/// the caller appends, and the keys it removes.
struct SaveBatch {
    std::vector<BlobDatabase::Blob> blobs;

    llvm::SmallVector<BlobKey> removals;

    /// How many shard and instantiation blobs the index put in.
    std::uint32_t shards = 0;
};

/// The project's index: a view of one read snapshot of the database plus,
/// in a writer, the changes no save has committed yet. Every session reads
/// the same way — a record is read when something asks for it, checked
/// then, and kept — and opening reads only the header. The records:
///
/// - per external symbol, its facts and the files referencing it (the
///   cross-file query fan-out),
/// - per file, what the units read of it, what its shard stores, the units
///   depending on it and how they resolved its `#include` lines, and the
///   names of internal linkage it declares,
/// - per symbol below external scope several files hold, the file sets the
///   units list for it,
/// - one manifest per indexed TU (replaced wholesale by its reindex), the
///   source the per-file records derive from,
/// - per file its shard and its instantiation shard,
/// - the name search index and the entries it does not describe.
///
/// A record failing its checks means a bug or damaged storage, never a
/// state a workflow reaches: it reads as missing, the error is logged, and
/// damaged() asks the owner to discard the database.
///
/// Files are named by path ids of the database's own, mapped to the file
/// table's ids as they are met; versions of files by (path id, content
/// hash), interned into the file table as manifests are read.
class ProjectIndex {
public:
    explicit ProjectIndex(FileTable& files);
    ~ProjectIndex();
    ProjectIndex(ProjectIndex&&) noexcept;

    /// The name the database keeps for `path`.
    std::string portable(llvm::StringRef path) const;

    /// The path a name the database keeps names in this checkout.
    Spelling local(llvm::StringRef name) const;

    /// The file table's id for a path id.
    Fid fid_of(std::uint32_t path) const;

    /// The symbol's stored facts; the strings borrow the index and live
    /// until the next save or clear.
    std::optional<SymbolView> find_symbol(SymbolHash hash) const;

    /// How many files reference the symbol; 0 for an unknown hash.
    std::uint32_t reference_count(SymbolHash hash) const;

    /// The files referencing the symbol, as the file table's ids.
    void for_each_reference_file(SymbolHash hash, llvm::function_ref<void(Fid)> visit) const;

    /// The files whose instantiation shards name the symbol.
    void for_each_instantiation_file(SymbolHash hash, llvm::function_ref<void(Fid)> visit) const;

    /// Every symbol, in no particular order — a scan of every symbol
    /// record; `visit` returns false to stop.
    void for_each_symbol(
        llvm::function_ref<bool(SymbolHash, const SymbolView&, std::uint32_t references)> visit)
        const;

    /// How many symbol records the database holds, the ones no save has
    /// committed yet left out.
    std::uint32_t symbol_count() const;

    /// Whether `tu` has a current manifest.
    bool indexed(Fid tu) const;

    /// A TU's current manifest, in the file table's version ids.
    std::optional<TUManifest> manifest(Fid tu) const;

    /// A count that moves whenever a unit's manifest changes, and with it
    /// what the index says the unit read and contributed.
    std::uint64_t manifest_generation() const {
        return manifests_changed;
    }

    /// Every TU with a current manifest — a scan of every file record.
    void for_each_unit(llvm::function_ref<void(Fid tu)> visit) const;

    /// The TUs that contributed a variant to `file`.
    void for_each_contributor(Fid file, llvm::function_ref<void(Fid)> visit) const;

    /// The TUs that contributed to `file`, each with the variant it
    /// contributed.
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
    /// (TUManifest::shared_locals) for it that lists a file reached. A
    /// `static` in a header is thus reached in every unit including the
    /// header or reading it through a precompiled header. Each file once.
    void for_each_file_of_local(SymbolHash hash,
                                Fid anchor,
                                llvm::function_ref<void(Fid)> visit) const;

    /// The include edges of a document, the input of the document links
    /// computed from the index: every way a unit resolved its `#include` lines at its
    /// shard's content generation. Empty when the file was never indexed.
    std::vector<IncludeEdge> include_edges(Fid file) const;

    /// The file's shard, read from the database on first use. Null when the
    /// file has no index. Pointers stay valid until the next save or clear,
    /// so a query keeps the shards it read while its fan-out reads others.
    const Shard* shard(Fid file) const;

    /// Every file with a shard — a scan of every file record.
    void for_each_indexed_file(llvm::function_ref<void(Fid)> visit) const;

    /// How many files have a shard, and the shards' size.
    std::uint32_t shard_count() const;

    std::uint64_t shard_bytes() const;

    /// The file's instantiation shard, like shard(). One lives with its
    /// file's shard of the same content generation.
    const InstantiationShard* instantiation_shard(Fid file) const;

    /// The distinct variant hashes contributed to `file` — its live variant
    /// set.
    llvm::SmallVector<std::uint64_t> live_variants(Fid file) const;

    /// The distinct uses named in `file`, ascending — its live use set.
    llvm::SmallVector<std::uint64_t> live_uses(Fid file) const;

    /// The name search index over the symbols as of its newest run's
    /// snapshot, and the entries it does not describe — merged or changed
    /// since — which a search reads from the records instead until a save
    /// adds them as a run. Both persist with the header: it lists the
    /// runs' segments and carries the pending entries.
    const SearchIndex& search_index() const {
        return search;
    }

    const llvm::DenseSet<SymbolHash>& search_pending() const {
        return pending_symbols;
    }

    /// The files whose names of internal linkage, in no symbol record,
    /// changed since the newest run's snapshot: a search reads them from the
    /// files' shards.
    const llvm::DenseSet<Fid>& search_pending_files() const {
        return pending_files;
    }

    /// The units a rebuild of a file they contribute to discarded the
    /// variants of, until they merge again: their content reads fresh, so
    /// the start-up scan would not find them. Persisted with the header.
    const llvm::DenseSet<Fid>& owed_units() const {
        return owed;
    }

    /// What an instantiation shard names a TU by.
    std::uint64_t unit_hash(Fid tu) const {
        return path_hash(portable(files.resolve(tu)));
    }

    /// The TU with a current manifest a unit hash names.
    std::optional<Fid> find_unit(std::uint64_t hash) const;

    /// Whether the run that walked a key is its TU's current manifest: a
    /// key outlives it only until the next compaction of its file.
    bool current_run(const Producer& producer) const;

    /// Read the persisted index from `db`'s read snapshot — the header and
    /// the search index — and fetch everything else from it on first use.
    /// The snapshot stays pinned until a save of this index moves it on.
    LoadOutcome open(BlobDatabase& db);

    /// The writer's start-up scan: every file record checked against the
    /// disk, and the TUs depending on a file whose content changed, on a
    /// place a failed lookup looked at that now holds a file, or on a
    /// changed input of a module or precompiled header. They are marked
    /// behind the disk (mark_behind) too.
    llvm::SmallVector<Fid> stale_units();

    /// Forget everything read and merged, as after the database was
    /// discarded: the workspace stays.
    void clear();

    /// Merge a TUIndex result: intern FileVersions, replace the TU's
    /// manifest, and write shard blobs only for variants no shard stores yet
    /// — a re-index whose entries are unchanged records its contributions and
    /// touches nothing else. `imports` is what the PCMs and the
    /// precompiled header the parse read were built from; `peak` the memory
    /// its worker reached, which the manifest keeps.
    std::expected<IndexReport, MergeError> merge(llvm::StringRef tu_index,
                                                 llvm::ArrayRef<DepState> imports,
                                                 std::uint64_t peak = 0);

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
    /// run that walked it is its unit's current one and that unit is not
    /// behind the disk (mark_behind) — the first unit to rebuild after an
    /// edit of a file the instantiation read walks the key again — and
    /// never for `tu` itself, whose rebuild is what would confirm it.
    std::vector<std::uint64_t> known_instantiations(Fid tu,
                                                    llvm::DenseSet<Fid> include_closure) const;

    /// A file `tu` depends on changed now: until a run of `tu` that started
    /// later merges, the instantiation keys its index walked do not hold.
    void mark_behind(Fid tu);

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

    /// Whether a save that may rebuild as much as `search` says changes the
    /// search index.
    bool search_update_due(SearchRebuild search) const;

    /// Add the changes since the newest run's snapshot to the search index
    /// as a run, folding into it as many of the newest runs as `search` and
    /// their sizes call for, or build the whole index from the symbol and
    /// internals records — the build runs on the thread pool over a
    /// snapshot —, adopt the run, and hold its segments for the next batch.
    kota::task<> update_search(SearchRebuild search);

    /// Compact the shards whose variant or use set shrank and retire the
    /// ones with no live variant left, then move the changes into `batch`.
    /// Merges committing while the batch is written stay for the next save.
    void begin_save(SaveBatch& batch, IndexReport& report);

    /// The batch did not commit: what it carried is changed again, under
    /// whatever changed since.
    void abandon_save(SaveBatch& batch);

    /// After a batch was written, committed or not, move onto a fresh read
    /// snapshot (growing the map first when the write hit a full one): the
    /// committed changes leave memory only once it opened, and the shards
    /// borrowing the previous snapshot move onto it. False when the
    /// database turned out corrupt or a shard diverged from its blob: the
    /// caller discards it.
    kota::task<bool> migrate_views(BlobDatabase& db, kota::event_loop& loop, IndexReport& report);

    /// Move the views borrowing an earlier read snapshot — resident shards
    /// and search segments — onto the database's current one, then retire
    /// the earlier ones: in batches with yields between them, or `at_once`
    /// when the earlier snapshots died already (a grown map, a file
    /// replaced by its compact copy). False when the database turned out corrupt or a view diverged
    /// from its blob: the caller discards it.
    kota::task<bool> rebind_views(BlobDatabase& db, kota::event_loop& loop, bool at_once);

    /// Whether a record read since open failed its checks, or the database
    /// met a damaged page: the database is to be discarded.
    bool damaged() const {
        return damage || (db && db->corrupted());
    }

    /// Shard blobs whose write has not durably completed: changed since the
    /// last save plus the batch a running save is committing.
    std::uint32_t pending_shard_writes() const;

    /// How many shard and instantiation blobs the last save durably
    /// committed. A steady-state save commits 0 — only variant-set and
    /// resolution changes rewrite a blob — so the stats endpoint can pin
    /// full-rewrite regressions.
    std::uint32_t last_save_shards() const {
        return saved_shards;
    }

    /// Whether index state remains that no save committed.
    bool has_unsaved_state() const;

    /// The symbol's entry to change: the changed entry when there is one,
    /// else a copy of the stored one, else a fresh one. Its file and file
    /// sets are path ids.
    Symbol& edit(SymbolHash hash);

    /// Merge a TU's external symbols straight off the serialized TU index;
    /// `file_ids_map` maps the TU-local ids of `index`'s path table to pool
    /// ids. A symbol whose stored facts the TU changes nothing of is not
    /// touched. `added` receives the hashes of the symbols without a record yet
    /// or whose name, arguments, parent, file or flags the merge changed, or
    /// whose reference count crossed a power of two — what a name search
    /// keyed on the records must re-read.
    void merge_symbols(const TUIndex& index,
                       llvm::ArrayRef<Fid> file_ids_map,
                       llvm::SmallVectorImpl<SymbolHash>* added = nullptr);

    /// Install (or replace) a TU's manifest, update the file records it
    /// changes and refresh the live masks of the files whose contribution
    /// or use set changed. Returns those files.
    llvm::SmallVector<Fid> apply_manifest(Fid tu, TUManifest manifest);

    /// Drop a TU's manifest and its entries in the file records, refreshing
    /// the live masks like apply_manifest. Returns the affected files.
    llvm::SmallVector<Fid> remove_manifest(Fid tu);

    /// The root the database names the files under relative to, by their
    /// portable names (path::portable), so a moved checkout keeps its
    /// index; empty names every file by its identity. Set before the index
    /// opens a database or saves.
    CanonicalPath workspace;

private:
    friend struct testing::IndexInternals;

    /// The records of one kind as the index sees them: the ones changed
    /// since the last save, the ones a running save writes, and copies of
    /// the ones read from the database. A removed record is an empty
    /// optional. StringMap boxes its entries, so a view of one survives
    /// later reads.
    template <typename Value>
    struct Records {
        BlobKind kind;

        llvm::StringMap<std::optional<Value>> changed;

        llvm::StringMap<std::optional<Value>> written;

        llvm::StringMap<std::optional<Value>> cached;
    };

    /// The newest `key` record of `records`: changed, written, cached or
    /// read from the database, `decode` turning its bytes into the value.
    template <typename Value, typename Decode>
    const Value* read(Records<Value>& records, llvm::StringRef key, Decode&& decode) const {
        for(auto* held: {&records.changed, &records.written, &records.cached}) {
            if(auto it = held->find(key); it != held->end()) {
                return it->second ? &*it->second : nullptr;
            }
        }
        std::optional<Value> value;
        if(db) {
            if(auto blob = db->read(records.kind, key)) {
                value = decode(blob.buffer->getBuffer());
                if(!value) {
                    damaged_record(records.kind, key);
                }
            }
        }
        auto& entry = records.cached.try_emplace(key, std::move(value)).first->second;
        return entry ? &*entry : nullptr;
    }

    /// The `key` record of `records` to change, starting from read's.
    template <typename Value, typename Decode>
    Value& change(Records<Value>& records, llvm::StringRef key, Decode&& decode) {
        auto it = records.changed.find(key);
        if(it == records.changed.end()) {
            auto* current = read(records, key, decode);
            it = records.changed.try_emplace(key, current ? *current : Value{}).first;
        } else if(!it->second) {
            it->second.emplace();
        }
        return *it->second;
    }

    /// Every record of `records`, the changed ones over the stored ones, in
    /// no particular order; `visit` returns false to stop.
    template <typename Value, typename Decode, typename Visit>
    void scan(Records<Value>& records, Decode&& decode, Visit&& visit) const {
        llvm::StringSet<> held;
        for(auto* map: {&records.changed, &records.written}) {
            for(auto& entry: *map) {
                if(!held.insert(entry.getKey()).second || !entry.second) {
                    continue;
                }
                if(!visit(entry.getKey(), *entry.second)) {
                    return;
                }
            }
        }
        if(!db) {
            return;
        }
        db->for_each(records.kind, [&](llvm::StringRef key, ReadBlob blob) {
            if(held.contains(key)) {
                return true;
            }
            auto value = decode(blob.buffer->getBuffer());
            if(!value) {
                damaged_record(records.kind, key);
                return true;
            }
            return visit(key, *value);
        });
    }

    /// Install `manifest` as `tu`'s, none for dropping it: the file records
    /// of every file whose facts the unit's manifest changes follow.
    /// Returns the files whose variants, uses or include resolutions
    /// changed.
    llvm::SmallVector<Fid> replace_manifest(Fid tu, std::optional<TUManifest> manifest);

    /// A record of `kind` under `key` failed its checks.
    void damaged_record(BlobKind kind, llvm::StringRef key) const;

    /// The key of a file's records.
    std::string file_key(Fid file) const;

    const FileRecord* file_record(Fid file) const;

    /// The file's record to change; a new one gets the next path id.
    FileRecord& change_file(Fid file);

    const Dependents* dependents(Fid file) const;

    Dependents& change_dependents(Fid file);

    const Symbol* symbol(SymbolHash hash) const;

    /// The path id the database names `file` by, none for a file it never
    /// named.
    std::optional<std::uint32_t> known_path(Fid file) const;

    /// The path id of `file`, handed out when the database never named it.
    std::uint32_t path_id(Fid file);

    /// The file table's version for a version id; none when the database
    /// holds no such version.
    VersionID version_of(std::uint32_t id) const;

    /// A manifest being installed names `version`: count it, handing out a
    /// version id when no other manifest names it.
    void retain_version(VersionID version);

    /// A manifest being replaced no longer names `version`: when no other
    /// one does, its version record goes.
    void release_version(VersionID version);

    /// The manifest the bytes hold, its versions interned into the file
    /// table; nullopt when they are not one.
    std::optional<TUManifest> decode_manifest(llvm::StringRef data) const;

    std::vector<std::byte> encode_manifest(const TUManifest& manifest) const;

    /// Recompute a file's live variants and live uses into its resident
    /// shards' masks, and mark it for the next save's compaction.
    void refresh_live(Fid file);

    /// The file's internal names may have changed — its shard replaced, its
    /// live variants changed: searches scan its shard until the next build,
    /// and the next save rewrites its internals record.
    void search_file_changed(Fid file);

    /// The names of internal linkage the file's shard declares, ascending
    /// by hash.
    Internals internal_names(Fid file) const;

    /// What a full build of the search index reads: every symbol record and
    /// the names of internal linkage of every file.
    SearchSnapshot full_search_snapshot();

    /// What a run over `symbols` and the names of internal linkage of
    /// `files` reads, with the scopes above them.
    SearchSnapshot search_snapshot(llvm::ArrayRef<SymbolHash> symbols, llvm::ArrayRef<Fid> files);

    /// Stop listing the segments of the newest `runs` of the header's runs:
    /// the next save removes them.
    void drop_search_runs(std::uint32_t runs);

    /// Retire a file's instantiation shard, now or at the next save.
    void retire_instantiations(Fid file);

    /// Add every TU contributing to `file`'s shard to the report's
    /// reindex debt. Used when the file's resident index is lost while its
    /// manifests still read fresh: no in-process event would ever rebuild
    /// it, and no start-up scan would either.
    void requeue_owners(Fid file, IndexReport& report);

    /// `tu`'s contribution to a file is no longer stored: it is owed a
    /// reindex (owed_units) and the report asks for one.
    void owe(Fid tu, IndexReport& report);

    /// Two-layer staleness test on a FileVersion, cached per round; a hash
    /// match after a stat mismatch repairs the version's stat fast path in
    /// place for every consumer.
    bool file_version_stale(VersionID version);

private:
    FileTable& files;

    /// The database the index reads from; null for an index in memory only.
    BlobDatabase* db = nullptr;

    Header header;

    bool header_changed = false;

    /// See manifest_generation.
    std::uint64_t manifests_changed = 0;

    mutable Records<FileRecord> file_records{.kind = BlobKind::File};

    mutable Records<Dependents> dependent_records{.kind = BlobKind::Dependents};

    mutable Records<Internals> internal_records{.kind = BlobKind::Internals};

    mutable Records<Symbol> symbol_records{.kind = BlobKind::Symbol};

    mutable Records<SharedLocalSets> shared_local_records{.kind = BlobKind::SharedLocal};

    /// Path id -> portable name.
    mutable Records<std::string> path_records{.kind = BlobKind::Path};

    /// Version id -> the file version.
    mutable Records<PersistedVersion> version_records{.kind = BlobKind::Version};

    /// Manifests no save has committed (an empty optional for a dropped
    /// one), and the ones a running save writes; the others are read from
    /// the database each time, never kept.
    llvm::DenseMap<Fid, std::optional<TUManifest>> changed_manifests;

    llvm::DenseMap<Fid, std::optional<TUManifest>> written_manifests;

    /// Path id <-> file table id, as met.
    mutable llvm::DenseMap<std::uint32_t, Fid> path_fids;

    mutable llvm::DenseMap<Fid, std::uint32_t> fid_paths;

    /// Version id <-> the file table's version, as met, for the versions
    /// some manifest names.
    mutable llvm::DenseMap<std::uint32_t, VersionID> id_versions;

    mutable llvm::DenseMap<VersionID, std::uint32_t> version_ids;

    /// Shards read or merged so far, keyed by the file table's ids; a miss
    /// is remembered as an empty shard. A node-based map: a query keeps
    /// pointers to the shards it read while its fan-out reads others.
    mutable std::map<Fid, Shard> shards;

    /// Per-file instantiation blobs, held like `shards`.
    mutable std::map<Fid, InstantiationShard> instantiations;

    /// The shards and instantiation shards merges or compactions replaced
    /// since the last save (their bytes are the index's own), and the ones
    /// the running save writes.
    llvm::DenseSet<Fid> changed_shards;

    llvm::DenseSet<Fid> changed_instantiations;

    /// What the running save writes of them, and retires.
    llvm::DenseSet<Fid> written_shards;

    llvm::DenseSet<Fid> written_instantiations;

    llvm::DenseSet<Fid> written_retired_shards;

    llvm::DenseSet<Fid> written_retired_instantiations;

    std::uint32_t writing_shards = 0;

    /// Shards and instantiation shards retired since the last save, whose
    /// blobs it removes.
    llvm::DenseSet<Fid> retired_shards;

    llvm::DenseSet<Fid> retired_instantiations;

    /// Files the next save looks at for compaction: their live variant or
    /// use set changed, or a unit that walked instantiation keys there ran
    /// again or left.
    llvm::DenseSet<Fid> compact;

    /// Files whose internals record the next save rewrites.
    llvm::DenseSet<Fid> internals_changed;

    SearchIndex search;

    llvm::DenseSet<SymbolHash> pending_symbols;

    llvm::DenseSet<Fid> pending_files;

    llvm::DenseSet<Fid> owed;

    /// While a search index is built, the symbols and the files merges
    /// change: they stay pending after the build, which read them before
    /// the change.
    struct SearchBuildChanges {
        llvm::DenseSet<SymbolHash> symbols;

        llvm::DenseSet<Fid> files;
    };

    std::optional<SearchBuildChanges> changed_during_search_build;

    /// The search segments built since the last save, which the next one
    /// writes, and the ones the header stopped listing, which it removes.
    std::vector<BlobDatabase::Blob> search_writes;

    llvm::SmallVector<std::uint32_t> search_drops;

    /// Units behind the disk (mark_behind), with when they fell behind in
    /// milliseconds since the epoch, the clock built_at uses.
    llvm::DenseMap<Fid, std::int64_t> behind;

    /// A record failed its checks since open.
    mutable bool damage = false;

    std::uint32_t saved_shards = 0;

    /// Per-round FileVersion staleness results: many TUs share the same
    /// versions, and one stat (or repair) per version per round is enough.
    llvm::DenseMap<VersionID, bool> version_stale;
};

}  // namespace clice::index
