module clice:project.project;

import :command.database;
import :command.table;
import :command.toolchain;
import :compile.compilation;
import :feature.diagnostics;
import :index.database;
import :index.project_index;
import :index.query;
import :index.shard;
import :index.tu_index;
import :index.writer_lock;
import :project.build;
import :project.build_snapshot;
import :project.command;
import :project.contexts;
import :project.hosting;
import :project.indexer;
import :project.memo;
import :project.pch;
import :project.pcm;
import :project.queries;
import :project.save;
import :project.task_graph;
import :project.unit;
import :semantic.symbol;
import :settings.settings;
import :support.failure_record;
import :support.prelude;
import :syntax.dependency_graph;
import :vfs.cache_store;
import :vfs.file_table;
import :vfs.path;
import :worker.pool;
import :worker.protocol;

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
/// after a load (project/command/contexts), so a process without editor state writes
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
/// them and never reach the build functions (graph/pch.cpp, graph/pcm.cpp,
/// graph/unit.cpp, indexer/background.cpp). The task graph's attempts and the
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
    /// database, written by write_database.
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

    /// Serializes concurrent saves (write_database): the background index's pass-end
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

    /// Persistence signal for the precompiled records (the PCH and PCM
    /// records, self-containment results) the index database carries
    /// beyond the index itself: producers mark, the single write pipeline
    /// (write_database) flushes it on its next run.
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
    llvm::StringMap<PCHBuild> pch_cache;

    /// Keys of pch_cache entries whose pch.idx index is currently loaded,
    /// most recently used first (see enforce_loaded_budget).
    llvm::SmallVector<std::string, 8> loaded_pch_indexes;

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

    /// Preprocessor passes scan_imports() ran.
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

    // project.cpp

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

    /// Persist the index and the cache store's manifest as they stand, a
    /// pass running or not: a cold pass covers the whole project, and a
    /// process killed mid-pass loses only what merged since the last
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

    /// What a reader says of an index database the index reports damaged: a
    /// writer discards one only when it meets the damage itself, which may sit
    /// in records no `clice index` run reads, so the reader names the directory
    /// to delete.
    std::string damaged_index_error() const;

    // discovery.cpp

    /// The sentence the guidance of a project without a database ends with:
    /// the rule that loads the compile_commands.json files discovery passes
    /// over (deeper_databases). Empty when there is none, or the rules
    /// declare the build's sources.
    std::string deeper_databases_hint() const;

    /// Whether a database the load found on disk could not be read, which its
    /// load logged the reason for.
    bool found_unreadable_database() const;

    /// Why the build has no member, completing a batch command's "Nothing to
    /// <verb>: ": a database it found that cannot be read, does not exist yet
    /// or lists no file; the rules' default commands claiming no file; else
    /// that no database was found in `root` or its direct subdirectories, the
    /// subdirectories left out as projects of their own, and the deeper ones
    /// there are.
    std::string why_no_members(llvm::StringRef root) const;

    // command/resolve.cpp

    /// The candidate a chosen command names (multi-configuration files and
    /// hosts): its base command hash, resolved when chosen, else its hash as
    /// `file` compiles under it now — `host`'s candidates lent by `host`, the
    /// file's own for none; none when no candidate is it.
    std::optional<Candidate> chosen_candidate(Fid file,
                                              llvm::ArrayRef<Candidate> candidates,
                                              Fid host,
                                              llvm::StringRef chosen_hash,
                                              llvm::StringRef chosen_base);

    /// `command`, one of `host`'s, as `host` lends it to `header`: edited by
    /// the rules of the file it is from and the header's, and a run's extras,
    /// compiled as the host's language (Build::apply_rules).
    CommandRef lent_command(Fid header,
                            Fid host,
                            const Candidate& command,
                            llvm::ArrayRef<std::string> extra_prepend = {},
                            llvm::ArrayRef<std::string> extra_append = {});

    /// Of `host`'s commands fit for `header`, the chosen one, else the first;
    /// nullopt when none fits.
    std::optional<Candidate> chosen_host_command(Fid header,
                                                 Fid host,
                                                 llvm::StringRef chosen_hash,
                                                 llvm::StringRef chosen_base);

    /// The command `host` lends `header`: chosen_host_command, as lent_command
    /// lends it.
    std::optional<CommandRef> host_command(Fid header,
                                           Fid host,
                                           llvm::StringRef chosen_hash,
                                           llvm::StringRef chosen_base,
                                           llvm::ArrayRef<std::string> extra_prepend = {},
                                           llvm::ArrayRef<std::string> extra_append = {});

    /// The dialect a file's text is lexed in, `choice` the user's context choice
    /// for it and `host` the source whose context it is read in, either absent
    /// for none: an explicit -x in the file's own database entry decides
    /// outright; else the host's command; else C when the file is a C source
    /// or every TU contributing its indexed entries is one, C++ else (mixed
    /// inclusion favors the superset).
    const clang::LangOptions& dialect(Fid file, const ContextChoice* choice, Fid host);

    /// Compose a file's final compile command from the project on disk, fill
    /// its arguments and report where they came from. The build (`Build`) says
    /// what a file compiles as; hosting (find_chosen_host, find_ranked_host)
    /// says which unit stands in for a header without a command of its own,
    /// whether the header compiles alone, and the context restoring its
    /// includer's preprocessor state when it does not. Tries, in order: the
    /// chosen host, the file's own command, the host ranking first, a default
    /// command, a nearby member's command and finally the builtin command — so it always
    /// succeeds — then the rule edits and a run's extras. Emits a per-file
    /// decision log (tiers tried, tier hit, command hash).
    Resolution resolve_command(Fid path_id,
                               std::string& directory,
                               std::vector<std::string>& arguments,
                               const CommandRequest& request = {});

    /// The identity of the command the build compiles `unit` under, as its
    /// index records it: the default selection (Build::commands' first) with
    /// the rule edits Build::apply_rules makes of it; 0 when the build compiles
    /// it under none. A run's extras are not part of it.
    std::uint64_t command_identity(Fid unit);

    /// Build::pch_unit for the command `resolution` and `request` gave
    /// `path_id`: edited by the rules the command was edited by and the
    /// request's extras.
    std::optional<CommandRef> pch_unit(Fid path_id,
                                       const Resolution& resolution,
                                       const CommandRequest& request = {});

    // command/hosts.cpp

    /// The translation units that can stand in for `header` — the units of the
    /// build whose compiles read it by the lexical scan, also through a file
    /// their commands force in, or by the entries their indexed compiles gave
    /// it, that compile in a language the header can be part of (a `.h` in
    /// any, a `.hpp` in C++ and the languages built on it, a `.cuh` only in
    /// CUDA) and write no precompiled header — best first: units whose entries
    /// come from the databases the header's own rules name, then the unit
    /// sharing the header's stem, its directory, and path proximity.
    llvm::SmallVector<Fid> ranked_hosts(Fid header);

    /// The commands of `host` that compile `header` in a language it can be
    /// part of, in the host's order: what the header may compile under, its
    /// first the default. Empty when the host cannot stand in for it.
    llvm::SmallVector<Candidate, 2> host_commands(Fid header, Fid host);

    /// The host the user chose for a header, while its compile enters the
    /// header at the chosen place; none else.
    HostSearch find_chosen_host(Fid header, const ContextChoice& choice);

    /// The first ranked host whose compile enters the header, where it first
    /// does.
    HostSearch find_ranked_host(Fid header);

    /// The host `header` compiles under with nothing chosen, or waits for the
    /// include tree of; invalid for a file no unit includes. A header compiled
    /// under its own entry has one too: the source its definitions belong in.
    Fid host_of(Fid header);

    /// The places the compile of `host` under its first command fitting
    /// `header` enters it; nullopt when no tree tells.
    std::optional<std::uint32_t> count_occurrences(Fid host, Fid header);

    // command/include_trees.cpp

    /// Every place the compile of the unit `unit` names, under the command it
    /// names, enters `header`, in the order it does, by the unit's include
    /// tree: its index manifest's for the command the index runs under, its
    /// first, while the index was built under it (command_identity), else the
    /// one a preprocess run took. Empty when the compile never enters the header.
    /// Nullopt when no tree can tell: the manifest cannot — the unit or a file on its way changed
    /// since, a file appeared where the compile looked and found none, or a forced file a
    /// precompiled header held hides the way in — and no run took one since the inputs it read last
    /// changed.
    std::optional<llvm::SmallVector<Host>> include_chains(const UnitCommand& unit, Fid header);

    /// The unit's own command `candidate`, rules applied, and its key.
    TreeRequest tree_request(Fid unit, const Candidate& candidate);

    /// Remember the tree a preprocess run under `request` took: the files it
    /// read (`paths` with the hashes of the bytes read, the unit's own last),
    /// its include nodes over them, and every file it read or looked for
    /// (`deps`), the run started at `started_at`.
    void commit_tree(const TreeRequest& request,
                     llvm::ArrayRef<std::string> paths,
                     llvm::ArrayRef<std::uint64_t> hashes,
                     std::vector<index::IncludeNode> nodes,
                     llvm::ArrayRef<ReportedInput> deps,
                     std::int64_t started_at);

    /// Remember that the run under `request` failed: the unit's command cannot
    /// preprocess it as the unit stands now.
    void commit_failed_tree(const TreeRequest& request);

    /// Take the tree `request` names with a preprocess run in this process, as
    /// a worker takes it for the server: the command line's way, which runs no
    /// workers.
    void take_tree(const TreeRequest& request);

    // command/self_contained.cpp

    /// The self-containment check's result for `key`; nullopt when no check
    /// stands for it. `open`: the header is a document the user edits, whose
    /// text moves with every keystroke: a result holds while its disk content
    /// stays as it was when the check ran, as typing never runs the check
    /// again; otherwise while the disk holds the text the check compiled.
    std::optional<bool> self_contained(const SelfContainmentKey& key, bool open);

    /// Remember what the self-containment check of `key` found: `diagnostics`,
    /// what the header's compile without its includer's context raised over
    /// `text`, having read `inputs`.
    void commit_self_containment(const SelfContainmentKey& key,
                                 llvm::ArrayRef<feature::FileDiagnostic> diagnostics,
                                 VersionID text,
                                 Inputs inputs);

    /// The header's buffer was saved as `text`: the check of `key`, when one
    /// found it self-contained and `leans` says the compile of that text shows
    /// what an includer provides, is forgotten, to run again; any other result
    /// holds for the saved text. Whether it was forgotten.
    bool header_saved(const SelfContainmentKey& key, VersionID text, bool leans);

    // command/synthesis.cpp

    /// The context `header` compiles in under `host`, its command `command`
    /// borrowed, with the files it embeds: the includer's state up to where
    /// the compile enters it, read from disk — never from open buffers: open
    /// files are no dependency of other files. Null when the synthesis cannot
    /// follow the chain under the command; remembered either way while the
    /// chain files hold.
    Memo<Fid, HeaderContext>::Entry synthesize(Fid header,
                                               const Host& host,
                                               const CommandRef& command);

    // command/borrow.cpp

    /// The command a file with neither an entry nor a host borrows
    /// (CommandSource::Borrowed), from a member other than the file, in a
    /// language the file can be part of (a `.h` matches any, a `.c` borrows
    /// C++ only from a `.c` unit compiled as C++, a C++ source and a C++
    /// module unit borrow from each other): one in the file's directory —
    /// same stem first, then by name — with its first command; else, for a
    /// header, the member whose command's header search directories contain
    /// it, nearest directory first, with that command; else the member
    /// closest by path. A provisional member lends the command it borrows
    /// only when `provisional`: the members' own commands are chosen among
    /// the units the build declares, so none rests on another's. Nullopt
    /// when there is no such member.
    std::optional<Candidate> borrowed_command(Fid file, bool provisional);

    // command/contexts.cpp

    /// The contexts a file can compile in, in this project: its hosts, ranked,
    /// each with the commands it lends and the places it enters the header at,
    /// then the file's own database entries. Contexts compiling the same under
    /// one command list once.
    std::vector<ListedContext> list_contexts(Fid file);

    /// The listing's item for the context the file compiles in: `choice`, its
    /// user's active context choice, else what `resolution` picked — the file's
    /// own entry, else the host it borrows a command from. None when the file
    /// compiles under a rule's default command or a lender's.
    std::optional<ListedContext> current_context_item(Fid file,
                                                      const ContextChoice* choice,
                                                      const Resolution& resolution);

    /// The context choice of the item `listed` names (switchContext's
    /// parameters): a host at an occurrence, under one of its commands when it
    /// names one, or one of the file's own entries.
    ContextChoice choice_of(Fid file,
                            Fid listed,
                            std::optional<std::uint32_t> occurrence,
                            const std::optional<std::string>& command_hash);

    /// Whether `choice` holds against the build and the include trees: its host
    /// still compiles and enters the file, at its occurrence when it names one,
    /// and its command still exists — an own entry's, or the host's it names.
    /// A host whose include tree no run took yet holds.
    bool choice_holds(Fid file, const ContextChoice& choice);

    /// The choices encoded as the project's database carries them, files named
    /// portably.
    std::string serialize_choices(llvm::ArrayRef<std::pair<Fid, const ContextChoice*>> choices);

    /// The choices `bytes` encodes; none from bytes that do not parse.
    std::vector<std::pair<Fid, ContextChoice>> parse_choices(llvm::StringRef bytes);

    // graph/pch.cpp

    /// How to build the PCH `request` names: an attempt revalidates the pair
    /// registered under its key or builds it with the request's inputs, and a
    /// clean node's pair is checked complete, in the store and read from
    /// unchanged inputs — anything else is a change for its dependents.
    Work pch_work(PCHRequest request);

    /// Wait for the PCH from outside the graph, foreground, one attempt;
    /// retry policy stays with the caller.
    async::task<Result> acquire_pch(PCHRequest request);

    /// The request for clice's own build of the precompiled header the command
    /// of `unit` writes (Build::pch_unit): the unit's whole file, under that
    /// command. The editor's build skips function bodies, the index's keeps
    /// them: they are two PCHs.
    PCHRequest declared_pch_request(const CommandRef& unit, bool skip_bodies);

    /// Whether the PCH registered under `key` read `file`: a parse of that very
    /// file cannot load it — clang refuses a PCH whose input the parse serves
    /// from a buffer, and the guard the PCH defines would skip the file's text.
    bool pch_reads(llvm::StringRef key, Fid file) const;

    /// The crash of the key's last build, while the inputs it is known to have
    /// read hold: an attempt's crash is recorded whatever became of the request
    /// that spawned it (contract 12), and every consumer that finds it books it
    /// against its own document instead of burning a worker of its own. One
    /// document's records cannot contain it alone: the PCH is shared.
    const FailureRecord* build_crash(llvm::StringRef pch_key);

    /// Forget the key's failed build, a crash or errors in the user's code, so
    /// the next request builds it again: a consumer whose own record lets it
    /// retry, or that was saved, retries it, and one moving to another key
    /// releases it. Returns whether it had failed.
    bool forget_build_failure(llvm::StringRef pch_key);

    /// An attempt for the key is in flight; its commit will republish over
    /// concurrent store retractions.
    bool pch_building(llvm::StringRef pch_key) const;

    /// Discard a pair the frontend could not consume: remove both blobs from
    /// the store and drop the idle cache entry, so the next acquisition misses
    /// and rebuilds instead of trusting corrupt bytes for the life of the
    /// store.
    void discard_pch(llvm::StringRef pch_key);

    /// A parse reading the pair died or found it unreadable: discard it AND
    /// count a failure of its consumption — unless the pair was gone from the
    /// store by then: evicted under the parse, which says nothing of its bytes,
    /// or discarded by a failure counted already. Each successful rebuild
    /// clears the build's failure, so the consumption keeps a record of its
    /// own; enough failures stop the key and acquisitions fail fast, so
    /// consumers fall back to compiling without a PCH instead of rebuilding and
    /// failing forever over failing storage.
    void pch_consume_failed(llvm::StringRef pch_key);

    /// A parse read the key's pair and completed: its consumption failures were
    /// transient — without this, two unrelated hiccups far apart would stop a
    /// key that rebuilds fine in between.
    void pch_consume_succeeded(llvm::StringRef pch_key);

    /// Open the pch.idx index of a cached PCH. The single consumption gate for
    /// `.pch.idx` blobs: when the blob turns out unreadable, the on-disk pair
    /// is discarded from the store as well — otherwise every later session
    /// re-adopts the corrupt pair from the precompiled records and silently
    /// degrades again. With the pair gone the next acquisition is a miss and
    /// rebuilds both halves. Loads count against the loaded-state budget (see
    /// enforce_loaded_budget).
    std::shared_ptr<index::TUIndex> preamble_index(llvm::StringRef pch_key);

    /// Unload pch.idx indexes beyond the budget (two per open document, plus
    /// 2), least recently used first. Without this every preamble key ever
    /// touched keeps its blob mapped for the server's lifetime — tens of MB per
    /// key on real projects, released by neither didClose nor store eviction.
    /// Unloading only drops the entry's reference: consumers holding the
    /// shared_ptr finish safely, and the next use reopens the blob from disk.
    void enforce_loaded_budget();

    // graph/pcm.cpp

    /// How to build a module unit's PCM: an attempt declares the unit's
    /// imports as the node's edges, waits on them and on the precompiled header
    /// its command declares, keeps the last build while it holds, else
    /// dispatches a new one. A clean node holds while its PCM, every PCM below
    /// it and the PCHs they were built over are on disk: the store's LRU, or
    /// another process sharing the cache, can remove one behind the graph's
    /// back. Only the module that lost it builds again, the ones above it
    /// keep their builds: its content did not change.
    Work module_work(Fid module);

    /// Whether `modules` and every module below them have their PCMs on disk,
    /// and the PCHs those were built over. A wait on one can outlast another:
    /// the store's LRU evicts it under the budget, or another process sharing
    /// the cache removes it. One found gone is forgotten (its build no longer
    /// names a PCM), so waiting on it again builds it again. Every one is
    /// looked at: a gone one found later must not survive an early return.
    bool imports_present(llvm::ArrayRef<Fid> modules);

    /// Scan the imports of a request that compiles under `arguments` with
    /// `content` as the focus file and the resolution's synthesized context
    /// served from memory (the stateless requests' builds — the scan must see
    /// the buffer's imports under the request's command), declare them as the
    /// edges of `consumer`, the file's node, and wait for their modules,
    /// foreground. A module that did not build is the compile's to report, as
    /// the stateful compile does; none only when the graph shut down.
    async::task<std::optional<Imports>> build_imports(NodeId consumer,
                                                      Fid path_id,
                                                      const Resolution& resolution,
                                                      llvm::ArrayRef<std::string> arguments,
                                                      llvm::StringRef directory,
                                                      llvm::StringRef content);

    /// Whether the graph has a node for this module unit (it was built or
    /// depended on before).
    bool pcm_tracked(Fid path_id) const;

    /// Mark a module unit and its transitive importers dirty, overtaking
    /// in-flight attempts: their next attempts check their builds again, and a
    /// build whose key and inputs still hold is kept — the single write point
    /// for PCM content invalidation. A product-only loss (cache eviction) is
    /// the node's check instead (module_work), which re-dirties the module alone:
    /// what its importers read is unchanged. Returns the dirtied nodes' files,
    /// of every kind.
    llvm::SmallVector<Fid> invalidate_module(Fid path_id);

    /// A closed document's buffer scans no more.
    void forget_buffer_scan(Fid path_id);

    /// The crash of the module's last build, while what it is known to have
    /// read holds: every importer that finds it books it against its own
    /// document instead of burning a worker of its own.
    const FailureRecord* build_crash(Fid module);

    /// Why the module's last build failed, when it failed on the user's code;
    /// nullopt for any other failure (a crash, an outage).
    std::optional<ModuleBuildFailure> module_build_failure(Fid module) const;

    /// Forget the module's failed build, a crash or errors in the user's code,
    /// so the next attempt builds it again: an importer whose own record lets it
    /// retry, or that was saved, retries it. A failed build is otherwise kept
    /// until its key or one of its inputs changes. Returns whether it had
    /// failed.
    bool forget_build_failure(Fid module);

    /// A module name gained a provider it lacked: invalidate the dependents of
    /// its unresolved import (their keys name the import as unresolved, so
    /// their next attempts rebuild) and return them for serving-side treatment.
    llvm::SmallVector<NodeId> provider_appeared(llvm::StringRef name);

    /// The direct module dependencies of the unit under exactly the arguments
    /// the caller compiles it with, rendered by `resolution`: a context choice
    /// or a donated header host cannot diverge between the scan and the parse.
    /// Consumers declare the full edge set and wait on the resolved subset. The
    /// header context the arguments name is served to the scan from memory. An
    /// engaged `content` scans it in place of the file's on-disk text — even
    /// when empty (an open buffer's imports count before they are saved, and
    /// an emptied buffer has none).
    ///
    /// The scan is a preprocessor run over the whole unit: it runs on the
    /// thread pool, the event loop only resolves the names it found. A unit
    /// that can import nothing pays none: its own text has no module syntax,
    /// neither it nor the host whose command it borrows reaches an import
    /// candidate, and a buffer's directives are the ones the dependency scan
    /// saw. A scan is reused while its arguments and the project's disk state
    /// (context_epoch) stay the same, a buffer's while its directive stream
    /// does too.
    async::task<Imports> scan_imports(Fid path_id,
                                      const Resolution& resolution,
                                      llvm::ArrayRef<std::string> arguments,
                                      llvm::StringRef directory,
                                      std::optional<llvm::StringRef> content);

    /// The PCMs a compile importing `modules` reads, under their modules'
    /// names: theirs and, below each that built, those of the modules it
    /// imports in turn — clang loads every module below an import, and a built
    /// module's imports are built too.
    std::unordered_map<std::string, std::string> imported_pcms(llvm::ArrayRef<Fid> modules) const;

    /// What decides whether the module's last PCM build still holds: what it
    /// read, or — for a build an import failed — what the import's failure
    /// rests on, down to the module whose own build failed. Null for a module
    /// never built and for a cycle of imports failing each other.
    const Inputs* module_inputs(Fid module) const;

    /// The module_inputs of `modules`, each once: what their PCMs were built
    /// from, their imports' inputs included, or what keeps them failing.
    Inputs module_inputs(llvm::ArrayRef<Fid> modules) const;

    // graph/unit.cpp

    /// Compile the unit once through its graph node. The node is invalidated
    /// first: a request's existence means work is owed, and a clean node left by
    /// an earlier success would otherwise satisfy the join without compiling.
    /// Each consumer compiles a file once at a time (the background index's
    /// queue, the lint pass). None when the graph shut down.
    async::task<std::optional<UnitResult>> compile_unit(Fid path_id, UnitRequest request);

    // graph/precompiled.cpp

    /// The precompiled records as the index database carries them (blob kind
    /// PrecompiledRecords): the PCH and PCM validity metadata and the
    /// self-containment checks, serialized from live state; empty on
    /// serialization failure.
    std::string serialize_precompiled_records();

    /// Restore the precompiled records a database held: the PCH and PCM entries
    /// whose blobs the cache store still has, and the self-containment checks.
    void load_precompiled_records(llvm::StringRef bytes);

    /// What a PCH or PCM build read: what the worker reports, and `before`,
    /// what the modules it imports and the PCH it is built over were built from
    /// as it started — one rebuilt meanwhile stales it.
    Inputs read_inputs(const worker::PrecompiledBuildResult& built, const Inputs& before);

    /// Send a PCH or PCM build, `label` naming what it builds in the log (a
    /// file, a module), and keep what did not build: a build that
    /// crashed its worker is recorded through `fail` on what it was known to
    /// read (`known`), and one that failed on the user's code on what it read
    /// (read_inputs over `before`); any other failure is clice's own breakage,
    /// an anomaly. The interest class is read at dispatch time: a foreground
    /// waiter may have joined after the attempt started. The advisory token
    /// rides into the pool, which cancels the request over IPC while this frame
    /// keeps awaiting the real reply (contract 2). The reply of a build that
    /// succeeded; else the attempt's result — Stale for a build the attempt
    /// withdrew, which is no decision on it.
    async::task<std::expected<worker::PrecompiledBuildResult, Result>>
        send_precompiled(Attempt& attempt,
                         const worker::BuildPCHParams& params,
                         llvm::StringRef label,
                         Inputs known,
                         const Inputs& before,
                         llvm::function_ref<void(FailureRecord, Inputs)> fail);

    async::task<std::expected<worker::PrecompiledBuildResult, Result>>
        send_precompiled(Attempt& attempt,
                         const worker::BuildPCMParams& params,
                         llvm::StringRef label,
                         Inputs known,
                         const Inputs& before,
                         llvm::function_ref<void(FailureRecord, Inputs)> fail);

    // indexer/background.cpp

    /// Add a file to the background indexing queue. A file enqueued twice
    /// keeps a single queue entry; its reason is upgraded to ContentChanged if
    /// either enqueue says so (a file both cascaded onto and edited is as stale
    /// as the edit makes it). False when a rule keeps the file out of the
    /// index: nothing is queued and no attempt will ever finish.
    bool enqueue_reindex(Fid path_id, ReindexReason reason);

    /// Someone is reading the file through the index and nothing serves it
    /// yet: enqueue it at the front of the queue and start a pass without the
    /// idle delay. A running pass finishes undisturbed; the file then leads
    /// the next one.
    void index_first(Fid path_id);

    /// Wait until the pending (re)index attempt observed at call time finishes
    /// — the merge committed, the attempt gave up, or the entry was cleared —
    /// and return immediately when nothing is pending. One attempt, not an
    /// idle file: a requeue arriving during the flight does not extend the
    /// wait. For replies clients cache with no refresh request (outline,
    /// links): answered while the didOpen index_first is still running, the
    /// empty result would freeze until the next edit.
    async::task<> await_index_attempt(Fid path_id);

    /// Wake the file's waiters whose observed sequence number the finished
    /// attempt covers (`seq` and older) and drop their events. The default
    /// wakes every waiter — for the paths where no further attempt will come
    /// (entry cleared, shutdown).
    void wake_attempt_waits(Fid path_id, std::uint64_t seq = -1);

    /// Forget a file's pending reindex: used when the file is removed from
    /// disk — nothing is left to reindex, and a lingering ContentChanged reason
    /// would suppress its deliberately still-serving shard forever. Its
    /// attempt's waiters wake.
    void clear_reindex(Fid path_id);

    /// Schedule background indexing (respects idle timeout and dedup).
    /// `immediate` skips the idle batching window — used by the pass tail for
    /// work requeued during the pass that just ended, and by index_first, which re-arms an
    /// already-armed timer to fire now (waiting out a previously scheduled idle window would starve
    /// the reader).
    void schedule_index(bool immediate = false);

    /// One pass, once the idle delay schedule_index armed has passed: index
    /// the files queued when it starts, save, and schedule the next pass for
    /// what it queued.
    async::task<> run_index_pass();

    /// At the start of a run writing the index: queue every member the index
    /// holds nothing of and every unit depending on a file that changed while
    /// no server ran, DepsOnly — a cold start with a warm index keeps serving
    /// the stored shards, not blanking every query until the pass drains — and
    /// the units owed the variants a rebuild discarded, read fresh by content;
    /// then schedule. With indexing off it queues nothing, but the units whose
    /// runs walked a changed file are still marked outdated.
    void enqueue_outdated(llvm::ArrayRef<Fid> members);

    /// Take an index report: book its reindexes — before the current attempt
    /// finishes and its waiters wake, so a waker never observes an index as
    /// current that the report just declared stale — and report its
    /// serving-index changes through on_index_changed.
    void apply_report(const index::IndexReport& report);

    /// Save the index and take the report, again while a save owes a retry (a
    /// grown map can still be too small); an idle save then shrinks the
    /// database file when that is worth it (shrink_database). Reindexes the
    /// saves queued start a pass when none runs.
    async::task<> save(index::SearchRebuild search = index::SearchRebuild::Behind);

    /// Cancel background indexing and wait for all its tasks to finish.
    async::task<> stop_background_index();

    /// No pass runs and nothing is queued.
    bool index_idle() const;

    /// Wait until index_idle(): every pass, requeue passes included, has run
    /// and saved its results.
    async::task<> wait_index_idle();

    /// Temporarily pause background indexing to give priority to user
    /// requests. Indexing tasks already dispatched to workers continue, but no
    /// new tasks are sent until resume_indexing() is called.
    void pause_indexing();

    /// Resume background indexing after a pause.
    void resume_indexing();

    /// Pause indexing once `units` more files are dispatched, so exactly that
    /// many finish before the pause holds (CLICE_TEST_PAUSE).
    void pause_indexing_after(std::size_t units);

    // save.cpp

    /// Open what the project's database holds: the index (ProjectIndex::open),
    /// the precompiled records into the project, the context choices; then, with
    /// `reconcile` (the build is loaded), drop the index of the units a rule
    /// keeps out or the build no longer compiles and owe a reindex to those
    /// whose command changed while no server ran (BuildSnapshot::reconcile) —
    /// in memory only on a read-only database.
    /// The writer replaces a database of another format or found corrupt by a
    /// fresh one; one whose header cannot be read is detached, persistence off
    /// for the session.
    SavedLoad load_saved(bool reconcile);

    /// Persist what the project's database carries: the index's dirty state
    /// (ProjectIndex::begin_save), then the build snapshot, the precompiled
    /// records and the context choices, in one write offloaded to the kotatsu
    /// thread pool. What the write fails to commit stays dirty for the next
    /// save; a database the write finds corrupt is replaced by a fresh one,
    /// and the reindexes that owes come back in the report with owes_retry
    /// set.
    async::task<index::IndexReport>
        write_database(index::SearchRebuild search = index::SearchRebuild::Behind);

    /// Whether state remains that no save committed. After a final save this
    /// means write failures whose retry never came — the one-shot `clice index`
    /// must not report a durable index from this.
    bool has_unsaved_state() const;

    /// The recovery from a database found corrupt after it was opened —
    /// damaged pages, or a record failing its checks: nothing in it survives,
    /// so the project starts over as a first run, every unit of the build and
    /// every unit of its own the index held owed a reindex.
    void recover_corrupt_database(index::IndexReport& report);

    /// Replace the database file by a compact copy when shrink_due, no save
    /// running meanwhile, and move the index's views onto it. Meant for when
    /// indexing is idle: the copy takes seconds on a large project, and it
    /// replaces the file only while no other process has the database open —
    /// else the file shrinks at a later call.
    async::task<index::IndexReport> shrink_database();

    /// Confirmed corruption heals through rebuildability: condemn the corrupt
    /// database (deleted on close) and continue on a freshly opened empty
    /// one, the index and the persisted build snapshot starting over with
    /// it, so the session's rebuild persists instead of waiting for the next
    /// start. A failed reopen leaves persistence disabled for the session.
    void reopen_fresh_database();

    // queries.cpp

    /// The build's files, `filter` one of all, source, header, module; the
    /// error says why the question cannot be answered.
    std::expected<ProjectFilesResult, std::string> project_files(llvm::StringRef filter);

    /// The files reachable from `path` along include edges, `direction` one of
    /// includes, includers, both; `depth` levels at most, 0 meaning unbounded.
    std::expected<FileDepsResult, std::string> file_deps(const Spelling& path,
                                                         llvm::StringRef direction,
                                                         int depth);

    /// What a change to `path` reaches: its direct includers, the sources
    /// hosting it, and the modules among them.
    std::expected<ImpactAnalysisResult, std::string> impact_analysis(const Spelling& path);

    /// Whether some unit of the build the index is to hold has no record in
    /// it yet (see feature::RenameScope::units_pending).
    bool units_pending();
};

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

}  // namespace clice
