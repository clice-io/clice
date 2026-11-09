module clice:project.project;

import :command.database;
import :command.edit;
import :command.invocation;
import :command.normalize;
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
import :project.background;
import :project.command;
import :project.discovery;
import :project.graph.pch;
import :project.graph.pcm;
import :project.graph.task_graph;
import :project.graph.unit;
import :project.members;
import :project.memo;
import :project.saved;
import :semantic.symbol;
import :settings.rules;
import :settings.settings;
import :support.failure;
import :support.prelude;
import :support.signal;
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

/// The editor's context choices of the project, as its index database
/// carries them beside the index. A save moves bytes only: the server keeps
/// its choices in them whenever they change and reads them after a load
/// (Project::save_choices, saved_choices), so a process without editor state
/// writes back the bytes it loaded, and only into a database replacing a
/// corrupt one.
struct ContextChoices {
    /// The serialized choices; empty when there are none, or when
    /// serializing failed (the save then writes nothing).
    std::string bytes;

    bool dirty = false;
};

/// Who compiles a unit (Project::compiled_by).
enum class CompiledBy : std::uint8_t {
    /// An open document's own compile: its index stands for the unit's
    /// until the document closes (BackgroundIndex::leave_to_document).
    Document,
    /// A build: its PCM's, or that of the precompiled header it writes.
    Build,
    /// Its own compile (compile_unit).
    Itself,
};

/// An open document, as a change of the project's inputs needs it: the
/// project does not know the documents its owner has open, which the owner
/// hands it.
struct OpenFile {
    Fid file;

    /// The files its last compile read.
    llvm::SmallVector<Fid> read;
};

/// What a change of the project's inputs means for the open documents
/// (files_changed, update_members).
struct Affected {
    /// The open files whose compile the change made stale through what they
    /// read. One whose own command changed is in `commands`: whether its
    /// document compiles under it is the owner's to tell.
    llvm::SmallVector<Fid> stale;

    /// The open files it queued to reindex, their skipped-body diagnostics
    /// forgotten.
    llvm::SmallVector<Fid> reindexed;

    /// The files whose commands appeared, changed or left — the provisional
    /// members' among them.
    CDBDiff commands;
};

/// A project: its build — the rules, the compilation databases and the
/// members they make, the commands each file compiles under — and what it
/// builds from it: its PCHs, its modules and its units' index, persisted in
/// the cache directory. All of it derives from files on disk, never from an
/// unsaved buffer: open files are never depended upon by other files, so a
/// buffer's state stays with its document (Session) while the project
/// tracks the disk, shared by every file.
///
/// Its owner — the server, or a command-line run — opens it once (open),
/// tells it what changed on disk (files_changed) and in the databases
/// (update_members), starts its background index when the run indexes, and
/// shuts it down (shutdown). A project builds through `loop`
/// and `pool`, which an owner that builds (the server, `clice index`,
/// `clice lint`) hands it at construction and keeps alive past it; the
/// read-only entry points construct it without them and never reach the
/// build functions (graph/pch.cpp, graph/pcm.cpp, graph/unit.cpp,
/// background/background.cpp). The task graph's attempts and the background
/// index's tasks use the members before them: an owner awaits shutdown()
/// before the project dies.
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

    /// The rules of clice.toml under the configuration open() activated;
    /// those of the settings a Project is born with until then.
    Rules rules{settings, root, {}};

    /// The commands the rules write by hand, and the builtin ones.
    WrittenCommands written_commands{commands, file_table};

    /// The commands the rules' edits make of others.
    EditedCommands edited_commands{commands, file_table};

    /// The sources the default-command rules cover, enumerated on first use
    /// and again by refresh_default_sources.
    std::optional<std::vector<Fid>> covered_sources;

    ProvisionalMembers provisional;

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

    /// What the project watches on disk besides the files it reads; nothing
    /// until its owner watches the databases (watch_databases).
    DiskWatch disk_watch;

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

    /// The reads over the index, gated by `freshness`; a server reads the
    /// documents it serves through it (IndexQuery::reading).
    index::IndexQuery index_query{index, file_table, &freshness};

    /// What the index database records of the build's members.
    SavedMembers saved_members;

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

    /// The generation of context-affecting project state: the include graph,
    /// the include trees — a tree moves its host's contexts — and the
    /// self-containment results — a header found self-contained lists the
    /// hosts giving the same command once. clice/queryContext tags its
    /// results with it and clice/switchContext rejects requests made against
    /// an older epoch, so a client can never apply a context picked from a
    /// stale listing without noticing.
    std::uint64_t context_epoch() const {
        return dep_graph.generation() + hosting.trees.generation() +
               hosting.self_containment.generation();
    }

    /// Generation of the build's commands: bumped when a database reloads
    /// or a member appears, not on saves like the dependency graph's.
    std::uint64_t commands_epoch = 1;

    /// What the project knows of the headers borrowing a unit's command.
    Hosting hosting;

    /// Persistence signal for the precompiled records (the PCH and PCM
    /// records, self-containment results) the index database carries
    /// beyond the index itself: producers mark, the single write pipeline
    /// (write_database) flushes it on its next run.
    bool precompiled_dirty = false;

    void mark_precompiled_dirty() {
        precompiled_dirty = true;
        on_metadata_changed.emit();
    }

    /// Emitted when what the database carries beside the index changed — a
    /// precompiled record, the context choices: an owner saving on its own
    /// cadence saves it soon.
    Signal<> on_metadata_changed;

    /// Emitted when shards a read may serve changed — merged, re-masked,
    /// dropped or shed.
    Signal<llvm::ArrayRef<Fid>> on_index_changed;

    /// The PCHs the cache holds, keyed by content key (preamble text +
    /// canonical flags), so files with identical preambles share one PCH:
    /// a mirror of the cache store's state, which names their paths, with
    /// what failed of building and reading them.
    llvm::StringMap<PCHEntry> pchs;

    /// The PCMs of the module units, by PCM node (module_node).
    llvm::DenseMap<NodeId, PCMEntry> pcms;

    /// The contexts each module unit's PCM builds in, as importers pick
    /// them (module_build).
    llvm::DenseMap<Fid, ModuleContexts> module_contexts;

    /// module_compatibility of the arguments a command renders, by their
    /// hash with the directory's.
    llvm::DenseMap<std::uint64_t, ModuleCompatibility> compatibilities;

    /// The import scans of units' disk text and of open buffers
    /// (scan_imports), each while the files it read hold.
    Memo<Fid, ImportScan> disk_scans;

    Memo<Fid, ImportScan> buffer_scans;

    /// The preprocessor runs the import scans took.
    std::uint64_t import_scan_runs = 0;

    /// What each unit's compiles in this session showed.
    llvm::DenseMap<Fid, UnitHistory> unit_history;

    /// The hash of each file's command as resolve_command last logged it:
    /// a file's command is logged again when it changed.
    llvm::DenseMap<Fid, std::uint64_t> logged_commands;

    /// The include-tree runs on the worker pool, by tree: a second asker
    /// waits for the first (take_tree).
    llvm::DenseMap<UnitCommand, std::shared_ptr<async::event>> tree_runs;

    async::task_group<> tree_tasks;

    /// The task graph the PCH, PCM and unit nodes build on, the server's
    /// AST nodes beside them. Its attempts use everything above.
    TaskGraph graph;

    BackgroundIndex background_index{*this};

    // open.cpp

    /// How much of the project a run opens (open()).
    struct OpenOptions {
        /// What of the build loads.
        enum class Load : std::uint8_t {
            /// No database: the rules alone, for commands converted under
            /// them.
            Rules,
            /// The databases alone, for the commands of one file.
            Databases,
            /// The databases, the provisional members the index remembers,
            /// the members and the dependency graph.
            Members,
        };

        /// How the cache — the store of PCHs and PCMs, the index database in
        /// it — opens.
        enum class Cache : std::uint8_t {
            None,
            /// Both read-only, without the writer lock, for a run reading
            /// what others built: a concurrent writer keeps owning them, and
            /// what the run changes stays in memory.
            Read,
            /// The store writable, for a run building PCHs and PCMs itself,
            /// over the index read as Read reads it.
            Build,
            /// Both writable, the index database as its writer, under the
            /// cache directory's writer lock.
            Write,
        };

        /// The build configuration to activate (LoadedSettings::configuration).
        llvm::StringRef configuration;

        Load load = Load::Members;

        Cache cache = Cache::None;

        /// What the reads of the index withhold (FreshnessGate).
        index::FreshnessOptions freshness;

        /// The files a run is about, or the documents open as a server
        /// starts: the databases in their directories and above them up to
        /// the root join the ones discovered.
        llvm::SmallVector<CanonicalPath> files;
    };

    /// What open() found.
    struct Opened {
        /// The build's translation units: the indexing pass's work list,
        /// so no driver walks the tree a second time.
        std::vector<Fid> members;

        /// The dependency scan from them.
        ScanReport scan;

        /// Why the index could not be read, for a run reading it: it opens
        /// with none then. Empty when it was read.
        std::string unreadable;
    };

    /// Open the project over its final settings (load_settings), once,
    /// in a fixed order: the rules under the configuration; the cache store
    /// and the index database; what the index remembers (the databases of
    /// earlier sessions, the provisional members); the build; the saved
    /// state — the index, the precompiled records, the context choices —
    /// reconciled with the build when that is loaded (reconcile_index), the
    /// reindexes that owes booked; for the index's writer, the units that
    /// depend on a file that changed while no run wrote the index marked
    /// outdated (ProjectIndex::stale_units). A run reading the index without
    /// the build binds its tables alone, fetched on first use.
    Opened open(const OpenOptions& options);

    /// What a reader says of an index database the index reports damaged: a
    /// writer discards one only when it meets the damage itself, which may sit
    /// in records no `clice index` run reads, so the reader names the directory
    /// to delete.
    std::string damaged_index_error() const;

    // members.cpp

    /// Every registered database in the priority order `path` sees: those
    /// the rules declare (Rules::database_order); then those no active rule
    /// declares (discovered ones) — those still on disk before the
    /// vanished, shallower before deeper, then by path. While the rules
    /// declare sources, one only inactive rules declare is left out.
    llvm::SmallVector<SourceID, 4> source_order(CanonicalRef path) const;

    /// Whether discovery registered the source: no rule hides it
    /// (Rules::hiding_databases).
    bool discovered(SourceID id) const;

    /// A file's database entries in source_order, within a source in file
    /// order. The first is the default selection.
    llvm::SmallVector<CompilationEntry, 2> entries(Fid file) const;

    /// The commands a file compiles under, in build order: its entries, or
    /// the default command of the first matching rule that declares one, or
    /// a provisional member's command. The first is the default
    /// selection; empty when the file has none — such a file is never a
    /// header's host.
    llvm::SmallVector<Candidate, 2> candidates(Fid file);

    /// The interned default command of the first matching rule declaring
    /// one; nullopt when no rule does or its command is not a compile
    /// command.
    std::optional<CommandID> default_command(CanonicalRef path);

    /// The builtin command of a file nothing gives one
    /// (CommandSource::Fallback), run from the file's directory: the driver
    /// follows the language clang assigns to the file's extension, an
    /// ambiguous `.h` and an extension clang does not know counting as C++.
    CommandID fallback_command(CanonicalRef path);

    /// The edits the rules make of `candidate` for `file`: those matching
    /// the files the candidate is from (Candidate::rules_from) and `file`,
    /// each once in declaration order.
    Edits rule_edits(Fid file, const Candidate& candidate) const;

    /// `candidate` as `file` compiles under it: edited by rule_edits and a
    /// run's extras; in the language of the file it is from for a header, or
    /// for a file compiling in its host, else in its own.
    CommandRef apply_rules(Fid file,
                           const Candidate& candidate,
                           llvm::ArrayRef<std::string> extra_prepend = {},
                           llvm::ArrayRef<std::string> extra_append = {});

    /// Every translation unit of the project: files with entries, plus the
    /// source files on disk that a default-command rule matches — enumerated
    /// once and again by refresh_default_sources — then the provisional
    /// members with a command, by path.
    std::vector<Fid> members();

    /// The scan units of `members`: every command of every member, so a
    /// header reachable through only one of a file's entries still finds
    /// that host.
    llvm::SmallVector<CommandRef> scan_commands(llvm::ArrayRef<Fid> members);

    /// Whether `file` is a translation unit of its own: declared() or a
    /// provisional member with a command — the members() filter for one
    /// file, so candidates() is never empty for a unit. A header is no unit;
    /// its default command is the resolver's last resort after its hosts,
    /// never its own command.
    bool is_unit(Fid file);

    /// Whether the build declares `file` a unit: it has a database entry,
    /// or a default-command rule covers it and it is a source.
    bool declared(Fid file);

    /// Record that the user opened or saved `file`: a provisional member
    /// while no unit hosts it (see refresh_provisional), until an entry or a
    /// rule covers it. Only a source of the project's own the build does not
    /// declare is one — none in a project nested in the root, a build tree
    /// or the cache (project_file). False when it is none, or recorded
    /// already.
    bool record_provisional(Fid file);

    /// Bring the provisional members in line with the build: drop the
    /// record of a file the build declares now, and give every other
    /// recorded file the command it borrows — a declared unit's, while no
    /// declared unit includes it, by the dependency graph; the builtin one
    /// when it borrows none — reporting the files that joined (`added`),
    /// borrow another command (`changed`) or left (`removed`), as a database
    /// reload reports its entries. The only writer of their commands.
    CDBDiff refresh_provisional();

    /// The directories the default-command rules cover sources under, walked
    /// again (walk_sources): report the sources that appeared since the last
    /// enumeration — a file created after startup joins the members; a
    /// deleted one just leaves them. The server calls it every workspace poll
    /// (ServedProject::poll_sources).
    llvm::SmallVector<Fid> refresh_default_sources(llvm::ArrayRef<CanonicalPath> walked);

    /// Whether some unit of the build the index is to hold has no record in
    /// it yet, but those `covered` says another index answers for (an open
    /// buffer's own compile's) (see feature::RenameScope::units_pending).
    bool units_pending(llvm::function_ref<bool(Fid unit)> covered = nullptr);

    // changes.cpp

    /// The files whose compiles read `file`: the units including it, also
    /// through a file their commands force in, by the dependency graph; the
    /// units whose indexed compiles read it, looked for it in vain, or
    /// imported a module built from it, by the index; the modules whose PCM
    /// was built from it, or whose build failed on it.
    llvm::SmallVector<Fid> readers(Fid file) const;

    /// Whether anything of the project derives from the file: one of the
    /// owner's `open` files, a command, an include edge, index entries, a
    /// build or an open file's compile that read it or looked for it. The
    /// disk changes of files it does not know are not its to follow.
    /// FIXME: a rule with a default command and no patterns gives every file a
    /// command, so such a project takes every other project's changes too.
    bool knows(Fid file, llvm::ArrayRef<OpenFile> open);

    /// Files it knows changed on disk: their include edges and module
    /// declarations are scanned again, the PCMs built from them and the
    /// units reading them go stale, the closed units reindex, and a file
    /// seen missing is forgotten. `open` are the owner's open documents: one
    /// whose last compile read a changed file goes stale even where the scan
    /// cannot tell (a macro include).
    Affected files_changed(llvm::ArrayRef<Fid> files, llvm::ArrayRef<OpenFile> open);

    /// What rebuilding the dependency graph changed.
    struct Rebuilt {
        /// Module names that gained their first provider.
        llvm::SmallVector<std::string> appeared;

        /// The previous providers of names whose providers changed: an
        /// importer picks the nearest of them, which may now be another.
        llvm::SmallVector<Fid> replaced;

        /// How the provisional members followed (see refresh_provisional).
        CDBDiff provisional;

        /// The last scan of the rebuild.
        ScanReport scan;
    };

    /// Rebuild the dependency graph from scratch against the current
    /// database: entry additions, removals and flag changes all funnel into
    /// one uniform rescan instead of per-entry graph surgery. Still cheap —
    /// per-file scan results are content-keyed in the file table, so
    /// unchanged files re-resolve without a read or lex. The provisional
    /// members then follow the rebuilt graph, which alone tells whether a
    /// unit includes one, and the graph takes them in.
    Rebuilt rebuild_dependency_graph();

    /// Follow a change of the build's commands: `commands`, what a database
    /// reload or a walk of the default sources changed; none when only the
    /// provisional members may have (a file just recorded). The provisional
    /// members follow, the dependency graph is rebuilt, and the index follows
    /// each file whose command changed: a unit the build stopped compiling
    /// leaves it, one whose command changed keeps serving until its reindex
    /// merges. The PCMs whose providers or commands changed go stale with
    /// their importers. `open` as files_changed's. The only writer of
    /// commands_epoch.
    Affected update_members(CDBDiff commands, llvm::ArrayRef<OpenFile> open = {});

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

    /// What a refactoring may edit of the project (EditableFiles).
    EditableFiles editable_files() const;

    /// Whether the build's databases resolve elsewhere than at the load
    /// (resolved_databases): another build, whose commands a reload would
    /// record as the ones this index was built with.
    bool databases_moved() const;

    /// Watch the databases registered so far, from their last loads, and
    /// the HEAD and index of the git checkout the project lies in.
    void watch_databases();

    /// Look at the databases (`force` skips the content gate and the
    /// two-tick debounce, the half-written-file guard): register the ones
    /// found since (discover_databases, `open` the owner's open files) and
    /// reload the watched ones that changed. Returns what the reloads changed,
    /// for update_members. A discovered database vanishing or returning
    /// flips its presence, and the files whose default entry moves with it
    /// change command (see source_order); its entries keep serving
    /// meanwhile.
    CDBDiff look_at_databases(bool force, llvm::ArrayRef<Fid> open);

    /// When no rule declares a source, register and watch the databases at
    /// the places a file may compile from that startup discovery did not
    /// look at: the root and its direct subdirectories (listed again when
    /// the root directory moves), and every directory from each of `files`
    /// still without a command, inside the root, up to the root — which is
    /// how a database generated after startup is picked up. The places stay
    /// watched; the next look loads what they hold. Loading now looks above
    /// `files` alone, a registered database whose load failed so far (absent
    /// at startup, unreadable at an earlier open) tried again — with polling
    /// off nothing else would. Returns what the loads changed, for
    /// update_members.
    CDBDiff discover_databases(llvm::ArrayRef<Fid> files, DiscoverOptions options);

    /// The directories below the root whose configuration file decides what
    /// the project loads (nested_project): each direct subdirectory, and
    /// each one on the way down to a database it loads or waits for, a
    /// provisional member or one of the owner's `open` files, the ones the
    /// rule left out among them.
    std::vector<CanonicalPath> configuration_dirs(llvm::ArrayRef<Fid> open) const;

    // command/resolve.cpp

    /// The one of `candidates` whose base is `base` (ContextChoice::base);
    /// none when no candidate is it.
    std::optional<Candidate> chosen_candidate(llvm::ArrayRef<Candidate> candidates,
                                              llvm::StringRef base);

    /// The dialect a file's text is lexed in, `choice` the user's context
    /// choice for it, null for none: the one of the command it compiles under
    /// (select_command).
    const clang::LangOptions& dialect(Fid file, const ContextChoice* choice);

    /// The command a file compiles under, before anything is rendered: tries,
    /// in order, the chosen host, the file's own command, the host ranking
    /// first, a default command, a nearby member's command and finally the
    /// builtin command — so it always succeeds — then the rule edits and a
    /// run's extras. The rules (`rules`) and the members say what a file
    /// compiles as; the hosts (find_chosen_host, find_ranked_host) which unit
    /// stands in for a header without a command of its own, and the
    /// self-containment check whether it needs its includer's context.
    SelectedCommand select_command(Fid file, const CommandRequest& request = {});

    /// `command` as a compile runs it: the arguments the toolchain renders,
    /// from its working directory.
    Rendered render(const CommandRef& command, const RenderOptions& options = {});

    /// A file's final compile command: select_command, then the context
    /// restoring the includer's preprocessor state of a header that needs
    /// it, rendered. Logs where the command came from, once per change.
    Resolution resolve_command(Fid file, const CommandRequest& request = {});

    /// resolve_command until the host search it runs misses no include
    /// tree, each tree it misses taken first, once (take_tree): a header
    /// compiles under the host every process finds for it. `request` gives
    /// the request anew for every resolution — a wait can change the user's
    /// choice it names. Stops with the tree missing when a run takes none,
    /// or once `current` no longer holds.
    async::task<Resolution> resolve_with_trees(Fid file,
                                               llvm::function_ref<CommandRequest()> request,
                                               llvm::function_ref<bool()> current = {});

    /// The identity of the command the build compiles `unit` under, as its
    /// index records it: the default selection (candidates' first) as the
    /// rules edit it (apply_rules), its hash the key of the unit's include
    /// tree (tree_request); 0 when the build compiles it under none. A run's
    /// extras are not part of it.
    std::uint64_t command_identity(Fid unit);

    /// The command of the entry writing the precompiled header that the
    /// command `selection` and `request` give `file` reads
    /// (CommandTable::pch_inputs) — the `cmake_pch.hxx.cxx` entry of a CMake
    /// target with precompiled headers; of the configurations writing one
    /// each from that file, the one writing this very PCH. The PCH on disk
    /// may come from another compiler, so clice builds its own under that
    /// command, edited as the reading command was — by the rules apply_rules
    /// applies and the request's extras: built under other flags, it would
    /// hold other code than that command parses. Nullopt when the build
    /// writes none of them, and for a command writing a precompiled header
    /// itself.
    std::optional<CommandRef> pch_writer(Fid file,
                                         const SelectedCommand& selection,
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
    llvm::SmallVector<Candidate, 2> candidates(Fid header, Fid host);

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
    /// tree: its index manifest's when the index was built under that command
    /// (command_identity), else the one a preprocess run took. Empty when the compile never enters
    /// the header. Nullopt when no tree can tell: the manifest cannot — the unit or a file on its
    /// way changed since, a file appeared where the compile looked and found none, or a forced file
    /// a precompiled header held hides the way in — and no run took one since the inputs it read
    /// last changed.
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

    /// Take the tree `request` names with a preprocess run in this process,
    /// as a worker takes it: the way of a run without workers.
    void take_tree_in_process(const TreeRequest& request);

    /// Take the tree `request` names with a preprocess run, or wait for the
    /// run taking it: on the worker pool when the project builds, in this
    /// process otherwise. False when the run took none: the pool stopped, the
    /// run was cancelled, or the worker failed for a cause not the unit's.
    async::task<bool> take_tree(TreeRequest request);

    // command/self_contained.cpp

    /// The self-containment check's result for `key`; nullopt when no check
    /// stands for it. `open`: the header is a document the user edits, whose
    /// text moves with every keystroke: a result holds while its disk content
    /// stays as it was when the check ran, as typing never runs the check
    /// again; otherwise while the disk holds the text the check compiled.
    std::optional<bool> self_contained(const SelfContainmentKey& key, bool open);

    /// Remember what the self-containment check of `key` found: whether the
    /// header's compile without its includer's context over `text`, having
    /// read `inputs`, missed nothing an includer provides
    /// (feature::missing_context_diagnostics).
    void commit_self_containment(const SelfContainmentKey& key,
                                 bool self_contained,
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
    /// closest by path. A provisional member gives the command it borrows
    /// only when `provisional`: the members' own commands are chosen among
    /// the units the build declares, so none rests on another's. Nullopt
    /// when there is no such member.
    std::optional<Candidate> borrowed_command(Fid file, bool provisional);

    // command/contexts.cpp

    /// The contexts a file can compile in, in this project: its hosts, ranked,
    /// each with the commands it gives and the places it enters the header at,
    /// then the file's own database entries. Contexts compiling the same under
    /// one command list once.
    std::vector<ListedContext> list_contexts(Fid file);

    /// The listing's item for the context the file compiles in as `resolution`
    /// picked it: the host it compiles in, else its own entry; none for a
    /// rule's default command or a borrowed one.
    std::optional<ListedContext> current_context_item(Fid file, const Resolution& resolution);

    /// Whether `choice` holds against the build and the include trees: its host
    /// still compiles and enters the file, at its occurrence when it names one,
    /// and its command still exists — an own entry's, or the host's it names.
    /// A host whose include tree no run took yet holds.
    bool choice_holds(Fid file, const ContextChoice& choice);

    /// Keep `choices` for the next save, as the project's database carries
    /// them (files named portably), replacing those kept before.
    void save_choices(llvm::ArrayRef<std::pair<Fid, const ContextChoice*>> choices);

    /// The choices kept for the database, as a load read them or
    /// save_choices last kept them; none from bytes that do not parse.
    std::vector<std::pair<Fid, ContextChoice>> saved_choices();

    // graph/pch.cpp

    /// The entry of `key` while its PCH is built; null else.
    const PCHEntry* built_pch(llvm::StringRef key) const;

    /// How to build the PCH `request` names: an attempt revalidates the pair
    /// registered under its key or builds it with the request's inputs, and a
    /// clean node's pair is checked complete, in the store and read from
    /// unchanged inputs — anything else is a change for its dependents.
    Work pch_work(PCHRequest request);

    /// The request for clice's own build of the precompiled header the command
    /// of `unit` writes (Project::pch_writer): the unit's whole file, under that
    /// command, function bodies and all — the editor and the index load the
    /// same one.
    PCHRequest declared_pch_request(const CommandRef& unit);

    /// The PCH of the preamble of `text`, the buffer of `file` compiled under
    /// `arguments` from `directory` as `resolution` resolved them, with
    /// `imports` its imports: keyed by the preamble text and the
    /// frontend-relevant flags, so files with the same preamble share it.
    /// `base` is the key of the precompiled header the build declares for
    /// the command, built already; empty for none, and `forced` when the
    /// command forces that header in all the same, which the PCH then
    /// holds without a directive of the text's. `point` is where a
    /// completion's parse stops: the preamble ends before the line holding
    /// it, so the lines typed there keep the PCH of the lines above them.
    PreamblePCH preamble_pch(Fid file,
                             llvm::StringRef text,
                             std::optional<std::uint32_t> point,
                             const Resolution& resolution,
                             const Imports& imports,
                             llvm::StringRef base,
                             bool forced);

    /// Build the PCHs a parse of `text`, the buffer of `file` under
    /// `resolution` with `imports` its imports, loads: the precompiled header
    /// the build declares for the command, then the preamble's over it (see
    /// preamble_pch) — none under readonly = "on", the profile's stated
    /// trade. None when the attempt was overtaken or the graph shut down.
    async::task<std::optional<BuiltPCH>> build_pch(Fid file,
                                                   llvm::StringRef text,
                                                   std::optional<std::uint32_t> point,
                                                   const Resolution& resolution,
                                                   const Imports& imports,
                                                   BuildOptions options);

    /// Whether the PCH registered under `key` read `file`: a parse of that very
    /// file cannot load it — clang refuses a PCH whose input the parse serves
    /// from a buffer, and the guard the PCH defines would skip the file's text.
    bool pch_reads(llvm::StringRef key, Fid file) const;

    /// The crash of the key's last build, while the inputs it is known to have
    /// read hold: an attempt's crash is recorded whatever became of the request
    /// that spawned it (contract 12), and every consumer that finds it books it
    /// against its own document instead of burning a worker of its own. One
    /// document's records cannot contain it alone: the PCH is shared.
    const FailureRecord* pch_build_crash(llvm::StringRef pch_key);

    /// Forget the key's failed build, a crash or errors in the user's code, so
    /// the next request builds it again: a consumer whose own record lets it
    /// retry, or that was saved, retries it, and one moving to another key
    /// releases it. Returns whether it had failed.
    bool forget_pch_failure(llvm::StringRef pch_key);

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
    /// rebuilds both halves. It stays loaded while someone holds it.
    std::shared_ptr<index::TUIndex> preamble_index(llvm::StringRef pch_key);

    /// Forget the entries whose pairs the store's LRU evicted from disk (inside
    /// a commit, maybe on a worker thread), or the content-keyed map grows
    /// for the server's lifetime even for keys never requested again.
    void forget_evicted_pchs();

    // graph/pcm.cpp

    /// The entry of the PCM node while its PCM is built; null else.
    const PCMEntry* built_pcm(NodeId module) const;

    /// How to build a module unit's PCM: an attempt declares the unit's
    /// imports as the node's edges, waits on them and on the precompiled header
    /// its command declares, keeps the last build while it holds, else
    /// dispatches a new one. A clean node holds while its PCM, every PCM below
    /// it and the PCHs they were built over are on disk: the store's LRU, or
    /// another process sharing the cache, can remove one behind the graph's
    /// back. Only the module that lost it builds again, the ones above it
    /// keep their builds: its content did not change.
    Work module_work(NodeId module);

    /// The PCM an import of `module` reads in a compile of `importer` under
    /// `command`: a build the compile can read (ModuleCompatibility::reads),
    /// under an entry of the database the compile's command comes from before
    /// any other, as the real build gives it, and among either the module's
    /// own command first; else the build under its own command anyway, whose reading then fails
    /// with clang's reason — the build no entry describes is never made up. A build system lists a
    /// module once per configuration it builds it in: CMake once per set of
    /// its importers' language flags, a project once per build directory.
    NodeId module_build(Fid module, Fid importer, const Resolution& command);

    /// Whether `modules` and every module below them have their PCMs on disk,
    /// and the PCHs those were built over. A wait on one can outlast another:
    /// the store's LRU evicts it under the budget, or another process sharing
    /// the cache removes it. One found gone is forgotten (its build no longer
    /// names a PCM), so waiting on it again builds it again. Every one is
    /// looked at: a gone one found later must not survive an early return.
    bool imports_present(llvm::ArrayRef<NodeId> modules);

    /// Wait on the PCMs of `modules`, `failed` told every one that failed to
    /// build: in `attempt`, each in turn, or from outside the graph, all at
    /// once and foreground, when it is null; again while all built but one
    /// left the cache by the time the last did — a rebuild can evict another
    /// under the store's budget — import_passes times at most. Stale when
    /// the attempt was overtaken or the graph shut down; Success when every
    /// one built and they are all on disk.
    async::task<Result> wait_modules(Attempt* attempt,
                                     llvm::ArrayRef<NodeId> modules,
                                     llvm::function_ref<void(NodeId)> failed = {});

    /// Scan the imports of a parse of `text`, the buffer of `file` under
    /// `resolution` (scan_imports), declare them as the edges of the file's
    /// node, and build their modules. The import list is scanner truth, not
    /// build output: declared before the wait, a parse that fails stays
    /// reachable from its imports — fixing or providing one re-dirties it —
    /// and the empty list of a buffer that imports nothing is declared too,
    /// so an edge declared earlier stops cascading here. A module that did
    /// not build is the parse's to report, on the import next to the file's
    /// own diagnostics. None when the attempt was overtaken or the graph shut
    /// down.
    async::task<std::optional<BuiltImports>> build_imports(Fid file,
                                                           llvm::StringRef text,
                                                           const Resolution& resolution,
                                                           BuildOptions options);

    /// The PCM nodes of a module unit: its build under its own command, and
    /// those under the other entries importers were given (module_build).
    llvm::SmallVector<NodeId> module_nodes(Fid unit) const;

    /// The crash of the module's last build, while what it is known to have
    /// read holds: every importer that finds it books it against its own
    /// document instead of burning a worker of its own.
    const FailureRecord* module_build_crash(NodeId module);

    /// Why the last builds of `imports` failed, for those that failed on the
    /// user's code; none for any other failure (a crash, an outage).
    std::vector<feature::ModuleFailure> module_failures(llvm::ArrayRef<NodeId> imports) const;

    /// Forget the module's failed build, a crash or errors in the user's code,
    /// so the next attempt builds it again: an importer whose own record lets it
    /// retry, or that was saved, retries it. A failed build is otherwise kept
    /// until its key or one of its inputs changes. Returns whether it had
    /// failed.
    bool forget_module_failure(NodeId module);

    /// The direct module dependencies of the unit under the arguments
    /// `resolution` renders, the ones its compile runs: a context choice or a
    /// header's host cannot diverge between the scan and the parse.
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
    /// saw. A scan is reused while its arguments stay the same and the files
    /// it read and looked for in vain hold, a buffer's while its directive
    /// stream stays the same too.
    async::task<Imports> scan_imports(Fid path_id,
                                      const Resolution& resolution,
                                      std::optional<llvm::StringRef> content);

    /// The PCMs a compile importing `modules` reads, under their modules'
    /// names: theirs and, below each that built, those of the modules it
    /// imports in turn — clang loads every module below an import, and a built
    /// module's imports are built too.
    std::unordered_map<std::string, std::string>
        imported_pcms(llvm::ArrayRef<NodeId> modules) const;

    /// What decides whether the module's last PCM build still holds: what it
    /// read, or — for a build an import failed — what the import's failure
    /// rests on, down to the module whose own build failed. Null for a module
    /// never built and for a cycle of imports failing each other.
    const Inputs* module_inputs(NodeId module) const;

    /// The module_inputs of `modules`, each once: what their PCMs were built
    /// from, their imports' inputs included, or what keeps them failing.
    Inputs module_inputs(llvm::ArrayRef<NodeId> modules) const;

    // graph/unit.cpp

    /// Compile the unit once through its graph node. The node is invalidated
    /// first: a request's existence means work is owed, and a clean node left by
    /// an earlier success would otherwise satisfy the join without compiling.
    /// Each consumer compiles a file once at a time (the background index's
    /// queue, the lint pass). None when the graph shut down.
    async::task<std::optional<UnitResult>> compile_unit(Fid path_id, UnitRequest request);

    /// Whether a build compiles `unit` as its run's compile would, and
    /// indexes it: its PCM's, or that of the precompiled header it writes.
    /// Its run then waits on the build rather than compiling the unit again
    /// (compile_unit), but for the products only a compile brings.
    bool compiled_by_build(Fid unit);

    /// Who compiles `unit`: an open document it was left to, else a build
    /// (compiled_by_build), else its own run.
    CompiledBy compiled_by(Fid unit);

    /// Whether the unit's index holds: built under its command of now, from
    /// inputs that hold (ProjectIndex::needs_reindex).
    bool index_holds(Fid unit);

    /// Ask a compile of `unit` — its run's, or a build that is the unit's
    /// compile (its PCM, the precompiled header it writes) — for the unit's
    /// TU index as the project index takes it: without what the index
    /// stores already of the files it is expected to include, unless its
    /// last result named one the index had lost, and with the standard
    /// library's own instantiations as the settings say. Returns how the
    /// index comes back.
    IndexTransfer request_index(Fid unit, worker::IndexRequest& request);

    /// Merge the TU index of a compile of `unit` (request_index) and take
    /// its report: read over `imports` — what the PCMs and the precompiled
    /// header the parse read were built from — under the command `command`
    /// identifies, its worker reaching `peak`. One naming a variant the
    /// index lost fails Outdated, and the unit's next compile sends every
    /// section in full.
    std::expected<void, index::MergeError> merge_index(Fid unit,
                                                       llvm::StringRef tu_index,
                                                       llvm::ArrayRef<Input> imports,
                                                       std::uint64_t command,
                                                       std::uint64_t peak);

    // graph/precompiled.cpp

    /// What a PCH or PCM build read: what the worker reports, and `before`,
    /// what the modules it imports and the PCH it is built over were built from
    /// as it started — one rebuilt meanwhile stales it.
    Inputs read_inputs(const worker::PrecompiledBuildResult& built, const Inputs& before);

    /// Merge the index a build that is `unit`'s compile sent (request_index)
    /// as the unit's compile's: read over `before` (read_inputs), under the
    /// command `command` identifies. One that does not merge leaves the unit
    /// to its run's own compile.
    void merge_build_index(Fid unit,
                           IndexTransfer& transfer,
                           const worker::PrecompiledBuildResult& built,
                           const Inputs& before,
                           std::uint64_t command);

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

    // saved.cpp

    /// The build's members now, as the index database records them
    /// (SavedMembers).
    SavedMembers::Blob recorded_members();

    /// Bring the loaded index in line with the live build: a unit a rule
    /// keeps out of the index leaves it, but for one a build compiles
    /// (compiled_by_build), and so does one the build stopped
    /// compiling — every database that listed it is healthy and lists it no
    /// more, or the default command that covered it is gone — while one a
    /// database that failed to load listed keeps serving its last-known
    /// index, as the live database reload does; every unit whose command
    /// changed while no server ran is owed a reindex in `report`, its index
    /// serving until the reindex merges.
    void reconcile_index(index::IndexReport& report);

    /// Save the index and take the report, again while a save owes a retry (a
    /// grown map can still be too small); an idle save then shrinks the
    /// database file when that is worth it (shrink_database). Reindexes the
    /// saves queued start a pass when none runs.
    async::task<> save(index::SearchRebuild search = index::SearchRebuild::Behind);

    /// Take an index report — of the load, a save or a unit's merge: book its
    /// reindexes, before the attempt that merged finishes and its waiters
    /// wake, so a waker never observes an index as current that the report
    /// just declared stale; and tell of its serving-index changes
    /// (on_index_changed).
    void take_report(const index::IndexReport& report);

    /// Persist the index and the cache store's manifest as they stand, a
    /// pass running or not: a cold pass covers the whole project, and a
    /// process killed mid-pass loses only what merged since the last
    /// checkpoint; then forget the PCHs the store evicted.
    async::task<> checkpoint();

    /// The shutdown once its owner's compiles are quiesced (contract 11):
    /// stop the include-tree runs and the background index, wind down the
    /// graph's attempts, save a last time and close the cache store.
    async::task<> shutdown();

    /// Open what the project's database holds: the index (ProjectIndex::open),
    /// the precompiled records into the project, the context choices; then, with
    /// `reconcile` (the build is loaded), bring the index in line with the
    /// build into `report` (reconcile_index) — in memory only on a read-only
    /// database. The writer replaces a database of another format or found
    /// corrupt by a fresh one; one whose header cannot be read is detached,
    /// persistence off for the session. False when the database's header is
    /// of another format or corrupt: a reader must report an unusable cache
    /// instead of an empty index.
    bool load_saved(bool reconcile, index::IndexReport& report);

    /// Persist what the project's database carries: the index's dirty state
    /// (ProjectIndex::begin_save), then the members, the precompiled
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
    /// one, the index and the persisted members starting over with
    /// it, so the session's rebuild persists instead of waiting for the next
    /// start. A failed reopen leaves persistence disabled for the session.
    void reopen_fresh_database();

    /// The precompiled records as the index database carries them (blob kind
    /// PrecompiledRecords): the PCH and PCM validity metadata and the
    /// self-containment checks, serialized from live state; empty on
    /// serialization failure.
    std::string serialize_precompiled_records();

    /// Restore the precompiled records a database held: the PCH and PCM entries
    /// whose blobs the cache store still has, and the self-containment checks.
    void load_precompiled_records(llvm::StringRef bytes);
};

/// Whether a database file's free pages are worth a compact copy: a quarter
/// of the file, and 64 MB at least.
bool shrink_due(const index::PageUsage& usage);

/// How often a long-lived owner calls Project::checkpoint().
chrono::milliseconds checkpoint_interval();

}  // namespace clice
