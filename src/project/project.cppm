module clice:project.project;

import :command.database;
import :command.table;
import :command.toolchain;
import :compile.compilation;
import :index.database;
import :index.project_index;
import :index.query;
import :index.shard;
import :index.tu_index;
import :index.writer_lock;
import :project.background_index;
import :project.build;
import :project.build_snapshot;
import :project.graph;
import :project.hosting;
import :project.memo;
import :project.pch;
import :project.pcm;
import :project.unit;
import :semantic.symbol;
import :settings.settings;
import :support.failure_record;
import :support.prelude;
import :syntax.dependency_graph;
import :vfs.cache_store;
import :vfs.file_table;
import :worker.pool;

namespace clice {

/// On-disk cache layout version (CacheStore root `cache/v{N}`).
/// Bump to discard the whole cache after incompatible format changes.
constexpr inline std::uint32_t cache_format_version = 15;

/// The user's choice for a file in the editor (clice/switchContext): the
/// host to borrow a command from, or one of the file's own entries.
/// Persisted in the context choices; validated on didOpen.
struct ContextChoice {
    /// Header context host; invalid = none.
    Fid host_path_id;

    /// Chosen include occurrence; no value = automatic.
    std::optional<std::uint32_t> occurrence;

    std::string command_hash;  ///< Chosen CDB entry (rules applied); empty = none.

    /// Base command hash of the chosen entry, resolved when chosen. The
    /// applied hash is the protocol identity; the base disambiguates
    /// candidates whose applied hashes collapse under the current rules.
    std::string base_hash;
};

/// What failed of the PCH and PCM builds, each failure with the inputs it stands on
/// (see Memo): those of a failed build are what it read, those of a crash
/// what was known before it ran. Only a success of the same operation, a
/// change of those inputs, or a consumer's save lets one run again — but for
/// the consumption of a PCH, which only a consumer's success clears.
struct PrecompiledFailures {
    /// The PCH builds by key.
    Memo<std::string, FailureRecord> pch_builds;

    /// The PCH pairs by key that a parse died reading or found unreadable:
    /// rebuilding a pair every consumer fails on would fare no better.
    Memo<std::string, FailureRecord> pch_consumes;

    /// The module builds by unit.
    Memo<Fid, ModuleFailure> pcm_builds;
};

/// The editor's context choices of the project, as its index database
/// carries them beside the index. A save moves bytes only: the server
/// serializes its choices into them whenever they change and parses them
/// after a load (project/contexts), so a process without editor state writes
/// back the bytes it loaded, and only into a database replacing a corrupt
/// one.
struct ContextChoices {
    /// The serialized choices; empty when there are none, or when
    /// serializing failed (the save then writes nothing).
    std::string bytes;

    bool dirty = false;
};

/// All persistent, project-wide state derived from files on disk, and what
/// the project builds from it: its PCHs, its modules and its units' index.
///
/// Design principle: open files are never depended upon by other files.
/// Dependencies always point to disk files.  This enforces a clean two-layer
/// architecture:
///   - Global layer (Project): tracks disk truth, shared by all files
///   - Per-file layer (Session): tracks buffer truth, isolated per TU
///
/// Project is the single source of truth for:
///   - dependency relationships (include graph, module DAG)
///   - the PCHs and PCMs shared across files
///   - symbol index (ProjectIndex + per-file Shard blobs)
///   - compilation database and settings
///
/// Project is NEVER modified by unsaved buffer content.  The only mutation
/// paths are:
///   - Opening         (open, once)
///   - A disk change   (rescan_disk_file: rescan disk, cascade invalidation)
///   - Background index (merge TUIndex results from stateless workers)
///
/// A project builds through `loop` and `pool`, which an owner that builds
/// (the server, `clice index`, `clice lint`) hands it at construction and
/// keeps alive past it; the read-only entry points construct it without
/// them and never reach the build functions (pch.cppm, pcm.cppm,
/// unit.cppm, background_index.cppm). The task graph's attempts and the
/// background index's tasks use the members before them: an owner awaits
/// stop_background_index() and shutdown() before the project dies.
struct Project {
    explicit Project(FileTable& file_table,
                     CanonicalPath root = {},
                     async::event_loop* loop = nullptr,
                     WorkerPool* pool = nullptr);

    /// The event loop and the worker pool the project builds with; null for
    /// a read-only open.
    async::event_loop* loop;

    WorkerPool* pool;

    /// The project's root: the `${workspace}` value, the anchor of rules and
    /// databases no configuration file supplied, the enumeration root of
    /// `**`-led patterns; empty for the project of files no folder holds.
    CanonicalPath root;

    /// A default-constructed Settings is born valid (every option holds its
    /// real default), so a directly-built Project (unit tests, tools)
    /// needs no init step. The server replaces this wholesale with the
    /// loaded user config and finalizes it after the initializationOptions
    /// overlay.
    Settings settings;

    /// The process's fid space, shared with everything else keyed by file
    /// (sessions, the task graph, the pool) — CDB entry file ids and
    /// project fids are the same ids. Persisted state never stores fids:
    /// it names files by path and re-interns them at load.
    FileTable& file_table;

    CommandTable commands;

    Toolchain toolchain{commands, file_table};

    CompilationDatabase cdb{file_table, commands};

    /// Which entries and hand-written commands apply to a file under the
    /// active configuration; the only reader of the rules.
    Build build{settings, root, cdb, file_table};

    /// Unified on-disk blob store for PCH, PCM and index blobs.  Opened by
    /// open() when cache_dir is configured; absent means caching
    /// is disabled.  Owns blob lifecycle (atomic writes, LRU, crash
    /// recovery); validity metadata (deps snapshots) lives in the index
    /// database, written by save_project.
    std::optional<CacheStore> store;

    /// The cache directory's writer lock, taken by a session that persists
    /// its index and held until the project dies — after `database`, so
    /// a reopened database never races another writer for the directory.
    std::optional<index::WriterLock> writer_lock;

    /// Index blob persistence, opened together with the cache store.
    /// Declared right after `store` (both backends borrow it) and before
    /// every index structure that borrows database bytes (`shards`), so
    /// destruction runs shards → database → store.
    std::unique_ptr<index::BlobDatabase> database;

    /// The real paths the build's databases resolved to at load
    /// (resolved_databases): a look finding others tells the build moved to
    /// another index.
    std::string resolved_databases;

    /// Which database `database` is (database_key).
    index::DatabaseKey database_key;

    /// Include relationships between files on disk (#include edges).
    /// Built once at startup from CDB scan; updated incrementally on didSave.
    DependencyGraph dep_graph;

    /// The persisted index, read as it is used: the symbol and file
    /// records, the per-TU manifests, the per-file shard blobs and the name
    /// search index.
    index::ProjectIndex index{file_table};

    /// What the reads of the index withhold: the files whose content moved
    /// on since they were indexed. Whoever opens the project sets its
    /// options.
    index::FreshnessGate freshness{file_table};

    /// The reads over the index, gated by `freshness`; a server attaches the
    /// live sources of the documents it serves.
    index::IndexQuery index_query{index, file_table, &freshness, nullptr};

    /// What the index database records of the build.
    BuildSnapshot build_snapshot;

    /// The editor's context choices, as the index database carries them.
    ContextChoices choices;

    /// Serializes concurrent saves (save_project): the background index's round-end
    /// save, the master's metadata flush and the shutdown save may overlap
    /// on the event loop, and a save moves the dirty state into its batch
    /// assuming one save at a time.
    async::semaphore save_gate{1};

    /// A compact copy of the database failed this session, or could not
    /// replace the file: the file shrinks no more until the next session.
    bool shrink_failed = false;

    /// Monotonic generation of context-affecting project state: the include
    /// graph, the include trees and self-containment results hosting
    /// learns. clice/queryContext tags its results with it and
    /// clice/switchContext rejects requests made against an older epoch,
    /// so a client can never apply a context picked from a stale listing
    /// without noticing.
    std::uint64_t context_epoch = 1;

    /// Generation of the build's commands: bumped when a database reloads
    /// or a member appears, not on saves like context_epoch.
    std::uint64_t commands_epoch = 1;

    /// What the project knows of the headers borrowing a unit's command.
    Hosting hosting;

    /// Rescan a file whose disk content changed (rescan_dependency_graph),
    /// so host lookups and context queries see includes the change added
    /// or removed, and its module declaration. The module-graph cascade is
    /// the served project's change handling (invalidate_module).
    void rescan_disk_file(Fid path_id);

    /// A file vanished from disk: it stops providing its module name (a
    /// replacement provider would otherwise sit behind it and never be
    /// selected) and its import syntax (the last import-bearing file must
    /// release the project-wide scan gate), and its outgoing edges go, so
    /// it stops being a host candidate. Incoming edges stay — includers'
    /// text still names it, and their own rescans own those edges.
    void forget_file(Fid path_id);

    /// What rebuilding the dependency graph changed.
    struct Rebuilt {
        /// Module names that gained their first provider.
        llvm::SmallVector<std::string> appeared;

        /// The previous providers of names whose providers changed: an
        /// importer picks the nearest of them, which may now be another.
        llvm::SmallVector<Fid> replaced;

        /// How the provisional members followed (see refresh_provisional).
        CDBDiff provisional;
    };

    /// Rebuild the dependency graph from scratch against the current
    /// database: entry additions, removals and flag changes all funnel into
    /// one uniform rescan instead of per-entry graph surgery. Still cheap —
    /// per-file scan results are content-keyed in the file table, so
    /// unchanged files re-resolve without a read or lex. The provisional
    /// members then follow the rebuilt graph, which alone tells whether a
    /// unit includes one, and the graph takes them in.
    Rebuilt rebuild_dependency_graph();

    /// Bring the provisional members (Build::record) in line with the build:
    /// drop the record of a file the build declares now, and give every
    /// other recorded file the command it borrows — its lender's, while it
    /// has one and no declared unit includes it, by the dependency graph —
    /// reporting the files that joined (`added`), borrow another command
    /// (`changed`) or left (`removed`), as a database reload reports its
    /// entries. The only writer of the borrowed commands.
    CDBDiff refresh_provisional();

    /// Persistence signal for the precompiled records (the PCH and PCM
    /// records, self-containment results) the index database carries
    /// beyond the index itself: producers mark, the single write pipeline
    /// (save_project) flushes it on its next run.
    bool precompiled_dirty = false;

    /// Connected by the master to schedule a flush soon after a mark; unset
    /// (tests, batch tools) means the owner saves on its own cadence.
    std::function<void()> request_flush;

    void mark_precompiled_dirty() {
        precompiled_dirty = true;
        if(request_flush) {
            request_flush();
        }
    }

    PrecompiledFailures failures;

    /// The PCHs the cache holds, keyed by content key (preamble text +
    /// canonical flags), so files with identical preambles share one PCH:
    /// a mirror of the cache store's state, which names their paths.
    llvm::StringMap<PCHState> pch_cache;

    /// Keys of pch_cache entries whose pch.idx index is currently loaded,
    /// most recently used first (see enforce_loaded_budget).
    llvm::SmallVector<std::string, 8> loaded_pch_states;

    /// Open-document count provider, connected by the server. Sizes the
    /// loaded-state budget; unset (tests, tools) falls back to a small
    /// editor-like working set.
    std::function<std::size_t()> open_documents;

    /// The last PCM build of each module unit that built.
    llvm::DenseMap<Fid, PCMBuild> pcm_builds;

    /// The import scans of open buffers, and of units' disk text: kept while
    /// the arguments and the project's disk state (context_epoch) stay the
    /// same — and for a buffer, its directives. A failed module's every
    /// importer waits on it again, and each wait would preprocess it again.
    llvm::DenseMap<Fid, ImportScan> buffer_scans;

    llvm::DenseMap<Fid, ImportScan> disk_scans;

    /// Preprocessor passes direct_deps() ran.
    std::uint64_t import_scans = 0;

    /// TUs whose last result named a variant by hash that the project index
    /// did not hold for its file: the next compile sends every section's
    /// bytes, so the retry cannot fail the same way.
    llvm::DenseSet<Fid> send_in_full;

    /// TUs whose last compile crashed its worker over their target's
    /// precompiled header, with its key: the next compile leaves it out.
    llvm::DenseMap<Fid, std::string> crashed_over_pch;

    /// What each TU's compiles in this session measured: the peak of the
    /// last finished one, at least what a compile killed for memory before
    /// it reached, and the memory one killed since reached. Ahead of the
    /// manifest's peak, which only an index that merged records.
    llvm::DenseMap<Fid, RequestCost> unit_costs;

    /// The task graph the PCH, PCM and unit nodes build on, the server's
    /// AST nodes beside them. Its attempts use everything above.
    TaskGraph graph;

    BackgroundIndex background_index;

    /// Persist the index and the cache store's manifest as they stand, a
    /// round running or not: a cold round covers the whole project, and a
    /// process killed mid-round loses only what merged since the last
    /// checkpoint.
    async::task<> checkpoint();

    /// The shutdown tail once compile and index work is quiesced (contract
    /// 11; stop_background_index() ran): wind down the graph's attempts, then
    /// the final save. The owner closes the cache store next (see close).
    async::task<> shutdown();

    /// Close the cache store once no build of this project is awaited:
    /// after the pool stopped, or right after shutdown() while the pool
    /// serves other projects — a worker still finishing a cancelled build
    /// then only fails its write.
    void close();

    /// How much of the project a run opens (open()).
    struct OpenOptions {
        /// What of the build loads.
        enum class Build : std::uint8_t {
            /// No database: the rules alone, for commands converted under
            /// them.
            None,
            /// The databases alone, for the commands of one file.
            Databases,
            /// The databases, the provisional members the index remembers,
            /// the members and the dependency graph.
            Members,
        };

        /// How the cache store opens.
        enum class Store : std::uint8_t {
            None,
            /// For a run reading what others built.
            ReadOnly,
            /// For a run building PCHs and PCMs itself.
            Writable,
        };

        /// How the index database opens.
        enum class Index : std::uint8_t {
            None,
            /// Read-only, without the writer lock: a concurrent writer keeps
            /// owning it, and what the run changes stays in memory.
            Read,
            /// As the writer, under the cache directory's writer lock.
            Write,
        };

        /// The build configuration to activate (LoadedSettings::configuration).
        llvm::StringRef configuration;

        Build build = Build::Members;

        Store store = Store::None;

        Index index = Index::None;

        /// The file a run is about: the databases in its directory and
        /// above it up to the root join the ones discovered.
        CanonicalPath file;
    };

    /// What open() found.
    struct Opened {
        /// The build's translation units: the indexing pass's work list,
        /// so no driver walks the tree a second time.
        std::vector<Fid> members;

        /// The dependency scan from them.
        ScanReport scan;

        /// The units owed a reindex the load found: the ones a rule keeps
        /// out or whose command changed while no server ran.
        index::IndexReport report;

        /// Why the index could not be read, for a run reading it: it opens
        /// with none then. Empty when it was read.
        std::string unreadable;
    };

    /// Open the project over its final settings (load_settings), once,
    /// in a fixed order: the cache store and the index database; what the
    /// index remembers (the databases of earlier sessions, the provisional
    /// members); the build; the saved state — the index, the precompiled
    /// records, the context choices — reconciled with the build when that
    /// is loaded: the units a rule keeps out and those whose command
    /// changed while no server ran leave the index, in memory only when the
    /// database is read-only. A run reading the index without the build
    /// binds its tables alone, fetched on first use.
    Opened open(const OpenOptions& options);
};

/// What a reader says of an index database the index reports damaged: a
/// writer discards one only when it meets the damage itself, which may sit
/// in records no `clice index` run reads, so the reader names the directory
/// to delete.
std::string damaged_index_error(const Project& project);

/// The real paths that the databases of the rules active under
/// `configuration` and resolving symlinks (Rule::resolve_symlinks)
/// resolve to now, one per line; empty when no active rule resolves them.
std::string resolved_databases(const Settings& settings, llvm::StringRef configuration);

/// The index database of the build `configuration` reads, its databases
/// `resolved` (resolved_databases): the configuration's own when nothing
/// resolved; else the one of those real paths, or the own one of another
/// configuration whose rules differ from its own only in naming exactly
/// those databases directly.
index::DatabaseKey database_key(const Settings& settings,
                                llvm::StringRef configuration,
                                llvm::StringRef resolved);

/// Capture a staleness snapshot from a build's reported inputs, interning
/// the consumed versions into the shared table.
///
/// `deps` carries the consumed-content hashes the worker computed at build
/// time; `build_at` is milliseconds since epoch, sampled before the build
/// started. A dependency the worker could not hash takes its hash from
/// the disk only when the file is untouched since `build_at`.
DepsSnapshot capture_deps_snapshot(FileTable& files,
                                   llvm::ArrayRef<DepFile> deps,
                                   std::int64_t build_at);

/// The file as the disk holds it now, as a dependency of a build that
/// reported nothing it read: a crash. None when the file cannot be read.
std::optional<DepState> current_dep(FileTable& files, Fid file);

/// Append the entries of `parts` that `into` lacks. A module's snapshot
/// already carries its imports', so a plain concatenation of several
/// repeats a module's inputs once per import path to it — exponentially
/// many over a deep partition graph.
void merge_deps(DepsSnapshot& into, llvm::ArrayRef<const DepsSnapshot*> parts);

}  // namespace clice
