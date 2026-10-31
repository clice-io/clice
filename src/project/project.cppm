module clice:project.project;

import :command.database;
import :command.table;
import :command.toolchain;
import :compile.compilation;
import :settings.settings;
import :index.database;
import :index.project_index;
import :index.query;
import :index.shard;
import :index.tu_index;
import :index.writer_lock;
import :project.build;
import :project.command_snapshot;
import :project.hosting;
import :project.memo;
import :semantic.symbol;
import :support.failure_record;
import :support.prelude;
import :syntax.dependency_graph;
import :vfs.cache_store;
import :vfs.file_table;

namespace clice {

/// On-disk cache layout version (CacheStore root `cache/v{N}`).
/// Bump to discard all cached artifacts after incompatible format changes.
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

/// Cached PCH state.  Stored in Project.pch_cache keyed by the content
/// key (hex of xxh3_128bits over preamble text + directories + canonical
/// flags), so files with identical preambles share one PCH.
///
/// Everything derived from the PCH build beyond validity metadata — the
/// preamble's symbol index, document links, inactive regions, the open
/// conditional stack — lives in the paired pch.idx index (the store's
/// `.pch.idx` aux file), committed and evicted together with the PCH.
/// Open a PCH's `.pch.idx` index (memory-mapped). Returns nullptr when
/// the file is unreadable, structurally invalid, of a different format
/// version, or any embedded shard blob fails verification — callers treat
/// all of these as a PCH cache miss.
std::shared_ptr<index::TUIndex> load_pch_index(llvm::StringRef path);

struct PCHState {
    /// The pair's name in the store: the key and a nonce of the build that
    /// produced it. A rebuild publishes under a new name, never over the
    /// old pair, which workers may still have mapped — Windows refuses to
    /// replace a mapped file.
    std::string blob;

    /// The pair the latest build replaced, kept for one more build: a
    /// consumer that took its path just before the replacement may not
    /// have opened it yet.
    std::string previous;

    std::string path;
    std::uint32_t bound = 0;
    DepsSnapshot deps;

    /// The key of the precompiled header the build declares that this
    /// preamble's PCH is built over and chains onto; empty for none.
    std::string base;

    /// Path of the paired pch.idx index.
    std::string index_path;

    /// Lazily opened blob; shared so a consumer holding it across an await
    /// survives concurrent entry replacement or eviction.
    std::shared_ptr<index::TUIndex> state;

    /// Open the blob on first use (memory-mapped, no deserialization).
    /// Returns nullptr when the blob is missing or unreadable — consumers
    /// degrade (no overlay, no preamble links) and the next PCH round
    /// treats the incomplete pair as a cache miss.
    const std::shared_ptr<index::TUIndex>& load_state();
};

/// The last build of a C++20 module unit that built: what it read and the
/// PCM it wrote. Shared across all files that import the module; a build
/// that failed is a failure record (ArtifactFailures::pcm_builds).
struct PCMBuild {
    /// The build's identity: "{module}-{hash}" over the unit's path and
    /// canonical flags, the build configuration, and the key of every module
    /// it imports (an unresolved import by its name) — so a command change
    /// anywhere below the module changes its key too.
    std::string key;

    /// The PCM's name in the store: the key and a nonce of the build that
    /// produced it. A rebuild publishes under a new name, never over the
    /// old PCM, which a running compile may still read — and the record
    /// names the very bytes it describes, whatever a crash before the
    /// metadata flush or another writer of the store left behind.
    std::string blob;

    /// The PCM the latest build replaced, kept for one more build.
    std::string previous;

    /// The built PCM; empty while a rebuild runs, after a failure, and once
    /// it or the PCH it was built over left the store.
    std::string path;

    /// The PCH the PCM was built over, which its importers read with it;
    /// empty for none.
    std::string pch;

    /// What the build read and looked for, the inputs of the modules it
    /// imports included; none while a rebuild runs.
    DepsSnapshot deps;
};

/// A module unit's build that failed: on the user's code, on an import that
/// failed in turn, or by crashing a worker.
struct ModuleFailure {
    /// The key of the build that failed (PCMBuild::key); empty when an
    /// import failed it and it never ran.
    std::string key;

    /// The build's first error, or how the worker it crashed died; empty
    /// when an import failed it.
    FailureRecord record;

    /// The import whose build failed, or that closed a cycle back to the
    /// unit; invalid when the unit's own build failed.
    Fid import;
};

/// What failed of the artifacts, each failure with the inputs it stands on
/// (see Memo): those of a failed build are what it read, those of a crash
/// what was known before it ran. Only a success of the same operation, a
/// change of those inputs, or a consumer's save lets one run again — but for
/// the consumption of a PCH, which only a consumer's success clears.
struct ArtifactFailures {
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

/// All persistent, project-wide state derived from files on disk.
///
/// Design principle: open files are never depended upon by other files.
/// Dependencies always point to disk files.  This enforces a clean two-layer
/// architecture:
///   - Global layer (Project): tracks disk truth, shared by all files
///   - Per-file layer (Session): tracks buffer truth, isolated per TU
///
/// Project is the single source of truth for:
///   - dependency relationships (include graph, module DAG)
///   - compilation artifacts shared across files (PCH/PCM caches)
///   - symbol index (ProjectIndex + per-file Shard blobs)
///   - compilation database and settings
///
/// Project is NEVER modified by unsaved buffer content.  The only mutation
/// paths are:
///   - Opening         (open, once)
///   - A disk change   (rescan_disk_file: rescan disk, cascade invalidation)
///   - Background index (merge TUIndex results from stateless workers)
struct Project {
    explicit Project(FileTable& file_table, CanonicalPath root = {}) :
        root(std::move(root)), file_table(file_table) {}

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

    /// Unified on-disk blob store for PCH/PCM/index artifacts.  Opened by
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

    /// PCH cache, keyed by content key (preamble text + canonical flags),
    /// so files with identical preambles share one PCH.  Hot-path mirror
    /// of CacheStore state; blob paths come from the store.
    llvm::StringMap<PCHState> pch_cache;

    /// The last PCM build of each module unit that built.
    llvm::DenseMap<Fid, PCMBuild> pcm_builds;

    ArtifactFailures failures;

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

    /// The commands the index was built with, as the index database
    /// records them.
    CommandSnapshot command_snapshot;

    /// The editor's context choices, as the index database carries them.
    ContextChoices choices;

    /// Serializes concurrent saves (save_project): the pump's round-end
    /// save, the master's metadata flush and the shutdown save may overlap
    /// on the event loop, and a save moves the dirty state into its batch
    /// assuming one save at a time.
    kota::semaphore save_gate{1};

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
    /// the invalidator's job (PCMBuilder::invalidate).
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

    /// Persistence signal for the artifact validity metadata (PCH/PCM
    /// records, self-containment results) the index database carries
    /// beyond the index itself: producers mark, the single write pipeline
    /// (save_project) flushes it on its next run.
    bool artifacts_dirty = false;

    /// Connected by the master to schedule a flush soon after a mark; unset
    /// (tests, batch tools) means the owner saves on its own cadence.
    std::function<void()> request_flush;

    void mark_artifacts_dirty() {
        artifacts_dirty = true;
        if(request_flush) {
            request_flush();
        }
    }

    /// The PCM of each built module of `modules`, under its name: what a
    /// compile importing them hands clang.
    void fill_pcm_deps(std::unordered_map<std::string, std::string>& pcms,
                       llvm::ArrayRef<Fid> modules) const;

    /// What decides whether the module's last PCM build still holds: what
    /// it read, or — for a build an import failed — what the import's
    /// failure rests on, down to the module whose own build failed. Null
    /// for a module never built and for a cycle of imports failing each
    /// other.
    const DepsSnapshot* build_inputs(Fid module) const;

    /// The build_inputs of `modules`, each once: what their PCMs were built
    /// from, their imports' inputs included, or what keeps them failing.
    DepsSnapshot module_inputs(llvm::ArrayRef<Fid> modules) const;

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
            /// For a run building artifacts (PCHs, PCMs) itself.
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
    /// members); the build; the saved state — the index, the artifact
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

/// Where a `compile_commands.json` is looked for when no rule declares
/// one: the project root, then its direct subdirectories in name order.
llvm::SmallVector<Spelling> database_places(CanonicalRef project_root);

/// The `compile_commands.json` files to load when no rule declares one:
/// those of database_places that exist. Empty when none exists yet — the
/// server keeps watching the places (ServedProject::discover_databases).
llvm::SmallVector<Spelling> discover_compile_commands(CanonicalRef project_root);

/// The `compile_commands.json` files discovery passes over, deeper than
/// the root's direct subdirectories down to three directories below it
/// (hidden directories, `node_modules` and the cache directory skipped),
/// by path: what the guidance of a project without a database names.
llvm::SmallVector<Spelling> deeper_compile_commands(CanonicalRef project_root,
                                                    CanonicalRef cache_dir);

/// Whether `dir` is a project of its own: it holds a configuration file,
/// or a compile_commands.json where startup discovery looks
/// (discover_compile_commands).
bool defines_project(CanonicalRef dir);

/// The nearest directory at or above `start` holding a configuration file
/// or a compile_commands.json, directly or in its build/ directory — its
/// parent instead when it holds a database alone and the parent is such a
/// directory — spelled as `start` is when `start` lies under it as spelled:
/// the root of the project a directory belongs to, as the server finds it
/// for a file outside every served folder and the command line from the
/// current directory. Empty when no ancestor has either.
Spelling find_project_root(const Spelling& start);

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

/// The sentence the guidance of a project without a database ends with:
/// the rule that loads the compile_commands.json files discovery passes
/// over (deeper_compile_commands). Empty when there is none, or the rules
/// declare the build's sources.
std::string deeper_databases_hint(const Project& project);

/// Whether a database the load found on disk could not be read, which its
/// load logged the reason for.
bool found_unreadable_database(const Project& project);

/// Why the build has no member, completing a batch command's "Nothing to
/// <verb>: ": a database it found that cannot be read, does not exist yet
/// or lists no file; the rules' default commands claiming no file; else
/// that no database was found in `root` or its direct subdirectories, and
/// the deeper ones there are.
std::string why_no_members(const Project& project, llvm::StringRef root);

/// Where a `compile_commands.json` is looked for in `start` and its
/// ancestors up to `project_root`, nearest first.
llvm::SmallVector<Spelling> database_places_above(CanonicalRef start, CanonicalRef project_root);

/// The `compile_commands.json` files of database_places_above that exist:
/// the databases a file deeper in the tree than startup discovery looks
/// may compile from.
llvm::SmallVector<Spelling> compile_commands_above(CanonicalRef start, CanonicalRef project_root);

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
