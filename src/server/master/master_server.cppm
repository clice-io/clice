module clice:server.master;

import :feature.semantic_tokens;
import :index.query;
import :project.project;
import :sched.index.pump;
import :server.extension;
import :server.served_project;
import :server.session;
import :support.anomaly;
import :support.prelude;
import :syntax.position;
import :vfs.disk_state;
import :worker.pool;
import :worker.probe;

namespace clice {

class LSPClient;

enum class ServerMode : std::uint8_t { Pipe, Socket };

struct ServerOptions {
    DecoKV(help = "Server mode: pipe (default) or socket (debug)", required = false)
    <ServerMode> mode = ServerMode::Pipe;

    DecoKV(help = "Socket mode address", required = false)
    <std::string> host = "127.0.0.1";

    DecoKV(help = "Socket mode TCP port", required = false)
    <int> port = 0;

    DecoKV(help = "Record LSP input to file for replay testing", required = false)
    <std::string> record;

    DecoKV(help = "Workspace root directory (optional, skips LSP initialize)", required = false)
    <std::string> workspace;
};

enum class ServerLifecycle : std::uint8_t {
    Uninitialized,
    Initialized,
    Ready,
    ShuttingDown,
    Exited,
};

/// Why the server must restart (MasterServer::ask_restart).
struct RestartReason {
    /// The configuration file the change concerns, as it is now; empty for
    /// none.
    std::string file;

    std::string message;

    /// What a later reason of the same topic replaces, as a project's next
    /// selection replaces its last; empty for a reason of its own.
    std::string topic;
};

/// A guidance or anomaly message materialized for transport delivery.
/// The log file is the durable record; this copy exists so a client that
/// attaches after the message fired can still be shown it.
struct NotifyMessage {
    logging::NotifyKind kind;
    std::string text;
};

/// The server process: the file table, the worker pool, the editor's state —
/// the open documents, their owners and the users' context choices — and the
/// projects it serves, with every file routed to one of them. A project
/// (ServedProject) holds what it derives from the documents it is given; the
/// editor's state is one per process, so a document moving between projects
/// changes owner and nothing is carried. Its work is split in files:
/// startup and shutdown (lifecycle.cpp), the projects and routing
/// (projects.cpp), the documents (documents.cpp), the context choices
/// (contexts.cpp), polling (polling.cpp), test hooks (test_hooks.cpp) and
/// the transports (serve.cpp).
///
/// It is also every open document's live index source, each through its
/// owner: what a request reads of another project's open document (rename,
/// code actions) it reads as that project serves it.
class MasterServer final : public index::LiveSources {
public:
    MasterServer(kota::event_loop& loop, std::string requested_configuration);
    ~MasterServer();

    // lifecycle.cpp

    /// Start serving the projects of `folders` (see project_roots): load
    /// each project's settings, start the pool sized for all of them, and
    /// start the projects; the documents opened before go to the projects
    /// routing picks for them.
    void initialize();

    /// The `[semantic_tokens]` options the legend is built from, read as
    /// the client initializes, before any project is configured: those of
    /// the first project's settings (load_settings).
    feature::SemanticTokensOptions startup_token_options() const;

    /// Bring every project to rest with its index committed while the
    /// transport still answers: the pool stops first, which breaks every
    /// wait on a worker (a hung one's included), then the background work
    /// joins and each project quiesces and saves. Runs once; a later caller
    /// waits for the first.
    kota::task<> drain();

    /// After the serving phase: drain, then close the projects.
    kota::task<> shutdown_and_cleanup();

    /// Shut down once the client process `pid` is gone: LSP asks a server
    /// to exit with the process that started it, which may die without
    /// closing the transport.
    void watch_client(std::uint32_t pid);

    void schedule_shutdown();

    /// The server began shutting down.
    bool shutting_down() const {
        return lifecycle == ServerLifecycle::ShuttingDown || lifecycle == ServerLifecycle::Exited;
    }

    /// Whether the test hooks are on: project.test_hooks of the first
    /// project at initialize, kept whatever folders come and go — the
    /// pool's probe is attached then.
    bool test_hooks() const;

    kota::cancellation_token shutdown_token() const {
        return shutdown_source.token();
    }

    // projects.cpp

    /// Stop serving the `removed` folders and serve the `added` ones
    /// (didChangeWorkspaceFolders); before initialize, edit the folders it
    /// will serve. The projects follow the folders (see project_roots);
    /// open documents of a project that stops serving move to the project
    /// routing picks for them now. A folder both removed and added (a
    /// rename) keeps serving.
    void change_folders(std::vector<Spelling> removed, std::vector<Spelling> added);

    /// The project serving a file: the owner of its open document, else
    /// the one routing picks now.
    ServedProject& owner_of(Fid path_id);

    /// Every open document routing now sends to another project moves there
    /// (change_owner).
    void reroute();

    /// A project's indexing progress moved: fold every project's round
    /// into index_progress and tell the client.
    void index_progress_changed(ServedProject& project);

    /// What the server was started on changed: a configuration file, the
    /// build the databases resolve to, the build configuration selected.
    /// Only a new process applies it, and only the editor can start one: it
    /// is asked to (clice/restartServer) when it can be, the user told
    /// otherwise. The same reason is given once.
    void ask_restart(RestartReason reason);

    /// The reason of `topic` no longer holds: what it asked for is what
    /// the server runs on again.
    void withdraw_restart(llvm::StringRef topic);

    /// Look at the configuration files of every folder served, as each of
    /// the master's ticks does whatever the polling setting: a change asks
    /// for a restart. clice.toml is the one in use where it exists, else
    /// .clice/config.toml.
    void look_at_settings();

    /// The index queries of the projects but `project`: its peers.
    llvm::SmallVector<const index::IndexQuery*> peer_queries(const ServedProject& project) const;

    /// The index query of the project an open document belongs to; null for
    /// a file not open.
    const index::IndexQuery* owner_query(Fid path_id) const;

    // documents.cpp

    /// A document naming a file already open under another name: the first
    /// name owns the file's buffer, this one keeps its own text and takes
    /// over when the owner closes. While both texts are equal it shares the
    /// first name's answers; diverged, it gets none, and the user is told
    /// to close one.
    struct SecondName {
        std::string spelling;

        Session buffer;

        /// The user was told the texts diverged; reset once they agree.
        bool warned = false;
    };

    /// An open file: the document it is open as, the project serving it and
    /// the documents open under a second name.
    struct Document {
        std::shared_ptr<Session> session;

        /// Null until the projects start: before initialize a document waits
        /// with no owner.
        ServedProject* owner = nullptr;

        llvm::SmallVector<SecondName, 1> second_names;
    };

    Document* find_document(Fid path_id);

    /// The open document's session, or null.
    std::shared_ptr<Session> find_session(Fid path_id);

    /// The second name `spelling` of the open file `path_id`, or null.
    SecondName* find_second_name(Fid path_id, llvm::StringRef spelling);

    /// didOpen of the file `path_id` spelled `spelling`: a second name of a
    /// file open already waits with its own buffer; else the document goes
    /// to the project routing picks, once every project holding it
    /// discovered the databases above it. A file no project can compile
    /// starts serving the project found nearest above it (a clice.toml or a
    /// compilation database) — when no folder holds the file, or when that
    /// root sits deeper than the folders that do — as if it were open. An
    /// open applies the disk changes the file table saw first: the file may
    /// be new, and its host's edge to it with it.
    void open_document(Fid path_id, llvm::StringRef spelling, std::string text, int version);

    /// didChange: the buffer takes the edit, and its owner cancels the
    /// compile in flight. A second name's buffer takes it too, agreeing with
    /// the first name's or not.
    void change_document(Fid path_id,
                         llvm::StringRef spelling,
                         llvm::ArrayRef<protocol::TextDocumentContentChangeEvent> changes,
                         int version);

    /// didClose: the owner releases the document, its diagnostics are taken
    /// down, and a document still open under a second name takes the file
    /// over; then a look at the file's disk content, which the editor no
    /// longer shows in place of the file.
    void close_document(Fid path_id, llvm::StringRef spelling);

    /// didSave: whatever crashed on the document runs again on its next
    /// request; a look at the file's disk content, which every project
    /// knowing the file cascades if it changed. A save is also when users
    /// expect everything to be current: the files every open document's
    /// compile depends on, installed headers included, are looked at too.
    void save_document(Fid path_id);

    bool is_open(Fid file) const override;
    std::optional<index::IndexSource> buffer_index(Fid file) const override;
    void for_each_session(llvm::function_ref<bool(const index::IndexSource&)> visit) const override;
    void for_each_session_index(
        llvm::function_ref<bool(const index::TUIndex&)> visit) const override;
    void
        for_each_preamble(llvm::function_ref<bool(const index::IndexSource&)> visit) const override;
    void for_each_overlay(llvm::function_ref<bool(const index::TUIndex&)> visit) const override;
    std::shared_ptr<index::TUIndex> preamble_index(Fid file) const override;
    void for_each_pch_index(Fid file,
                            llvm::function_ref<void(const index::TUIndex&)> visit) const override;

    // contexts.cpp

    /// The user's context choice for a file: the project it belongs to,
    /// which persists it and checks it against its build, and whether it
    /// holds there now (holds). One that stops holding waits, inactive, and
    /// applies again once it holds; only an active one routes the file and
    /// resolves its command.
    struct ChoiceEntry {
        ContextChoice choice;

        ServedProject* project = nullptr;

        bool active = true;
    };

    /// The file's choice, active or not; null for none.
    const ChoiceEntry* choice(Fid path_id) const;

    /// Load the choices `project` saved, as it starts: of two projects
    /// holding one for the same file, the deeper wins, and the other's is
    /// dropped from its database at its next save.
    void load_choices(ServedProject& project);

    /// Check again whether the file's choice holds in its project: as an
    /// open document is adopted there, and as the project's build or include
    /// graph changes. True when that flipped.
    bool recheck_choice(Fid path_id);

    /// clice/queryContext over every project, paginated: the contexts the
    /// file's own project offers, then the ones other projects do —
    /// choosing one of those moves the file there (switch_context). The
    /// listing's epoch covers every project (context_epoch).
    ext::QueryContextResult query_contexts(Fid path_id, const ext::QueryContextParams& params);

    /// clice/switchContext: make the listed context the file's choice in the
    /// project offering it — the file's own first — moving the open
    /// document there. Answers at once: the project's next save persists
    /// the choice.
    kota::task<ext::SwitchContextResult> switch_context(Fid path_id,
                                                        Fid context_path_id,
                                                        ext::SwitchContextParams params);

    /// clice/currentContext: the listing's item for the context the open
    /// document compiles in, whether the user chose it, and its command.
    kota::task<ext::CurrentContextResult> current_context(Fid path_id);

    /// clice/resetContext: drop the file's choice and serve it under the
    /// automatic context — moving it back to the project it routes to when
    /// the choice had moved it.
    ext::SwitchContextResult reset_context(Fid path_id);

    // polling.cpp

    /// Hand the disk changes the file table saw to every project knowing
    /// the file (ServedProject::knows); how many there were.
    std::size_t apply_disk_changes();

    // test_hooks.cpp

    /// Test hook (clice/internal/sync): wait until nothing is in flight.
    /// One look, taken in one turn after the disk changes the file table
    /// saw are applied, at read-only views: `editor` has no request
    /// unanswered but syncs (kota lists a request from its dispatch on), no
    /// task-graph round is live, the index pump is idle, and no removed
    /// folder's project is still shutting down (its end may start the
    /// folder again). Work starts inside one of these or from a new
    /// message, so the look is exact — with periodic ticks off, as tests
    /// run: workspace polling and checkpoints start work on their own. The
    /// metadata save that follows a build is not waited for.
    kota::task<> wait_idle(const ipc::JSONPeer& editor);

    /// What wait_idle() still waits for, one line each; empty once idle.
    std::vector<std::string> pending_work(const ipc::JSONPeer& editor);

    kota::event_loop& loop;

    /// The process's fid space, shared by every project.
    FileTable files;

    /// Fed by the pool while test hooks are on (see WorkerPool::probe).
    BuildProbe probe;

    /// The master's ticks look at the files (vfs::DiskState::tick), each
    /// followed by the projects' look at their databases
    /// (ServedProject::tick_databases): set once a project polls the disk.
    bool polling_files = false;

    /// The workers every project compiles and indexes on.
    WorkerPool pool;

    /// The connection to the editor: set by the client as it is made and
    /// cleared as it goes — a socket's client goes with its connection while
    /// the server runs on — and null in unit tests.
    LSPClient* client = nullptr;

    /// The projects served, in folder order; never empty after initialize.
    /// The first also serves files no project covers. Shared with the
    /// requests running in a project, which keep a removed one alive until
    /// they complete.
    std::vector<std::shared_ptr<ServedProject>> projects;

    /// The open documents.
    llvm::DenseMap<Fid, Document> documents;

    /// The users' context choices, at most one per file.
    llvm::DenseMap<Fid, ChoiceEntry> choices;

    /// The projects' indexing rounds as one: it begins with the first
    /// project's round and ends with the last one's, and a project whose
    /// round ended keeps its counts in it until then.
    IndexPump::Progress index_progress{.stage = IndexPump::Progress::Stage::End};

    /// Why the server must restart, oldest first (ask_restart).
    std::vector<RestartReason> restart_reasons;

    /// Recent guidance/anomaly messages for the client, bounded by dropping
    /// the oldest. notify_seq numbers the next message, so notify_seq -
    /// notify_log.size() is the oldest retained sequence; a client lagging
    /// further behind than the retention window loses the evicted messages
    /// (the log file keeps the durable record).
    std::deque<NotifyMessage> notify_log;
    std::uint64_t notify_seq = 0;

    /// Lifecycle state, advanced by the LSP initialize/shutdown handlers.
    ServerLifecycle lifecycle = ServerLifecycle::Uninitialized;

    /// The folders served: the client's workspace folders (or
    /// the command line's --workspace), and the roots open_document found
    /// above files no folder covers.
    std::vector<CanonicalPath> folders;

    /// The client's initializationOptions, applied to every project.
    std::optional<codec::dyn::Value> init_options;

    /// What every position the client sends or receives counts in, chosen
    /// once at initialize.
    PositionEncoding position_encoding = PositionEncoding::UTF16;

    /// The `--configuration` argument: the build configuration this
    /// session runs, over the persisted selection; empty takes the
    /// selection, else the default.
    std::string requested_configuration;

    /// The client talks over stdio, so it runs on this host and the
    /// processId it names is a process here; a socket client may run on
    /// another machine.
    bool client_on_host = false;

    /// The client sent `exit` without a `shutdown` first, which LSP answers
    /// with exit code 1.
    bool exit_before_shutdown = false;

private:
    // lifecycle.cpp

    /// The pool's callbacks, routed to the projects owning the documents.
    void route_pool_callbacks();

    /// Dump every configuration layer of a project — the configuration file verbatim,
    /// the client's initializationOptions overlay, and the merged result after
    /// defaults. Absence is stated explicitly so "were my settings even read?"
    /// never needs a support round-trip; the stderr mirror puts all of it in
    /// the editor's output panel.
    void log_settings(const ServedProject& project) const;

    kota::task<> drain_task();

    // projects.cpp

    /// A project over `root`.
    std::shared_ptr<ServedProject> make_project(CanonicalPath root);

    /// The root of the project serving a folder: the folder itself when it
    /// is a project of its own (defines_project), else the project of the
    /// nearest folder enclosing it, else the folder itself.
    CanonicalPath root_of(CanonicalRef folder) const;

    /// The roots of the projects the folders need, in folder order; the
    /// rootless project's when there is no folder.
    std::vector<CanonicalPath> project_roots() const;

    /// Make the projects served those project_roots names: make the
    /// missing ones, stop the ones no folder needs, and move the open
    /// documents to the projects routing picks now. A project made while
    /// its root's previous one is still alive (a folder removed and added
    /// again) holds the root's documents, their compiles waiting
    /// (ServedProject::loaded), and starts once that is gone (make_project
    /// runs this again): the previous one saves its index and frees the
    /// cache directory first.
    void serve_folders();

    /// Watch the configuration files of the folders served now (see
    /// look_at_settings), keeping what earlier looks saw.
    void watch_settings();

    /// The cache directories in use: a cache directory belongs to one
    /// project at a time, a removed one's until it is gone — but for the
    /// project serving its root again, `successor`, which opens it then.
    std::vector<CanonicalPath> taken_cache_dirs(CanonicalRef successor = {}) const;

    /// Before a file's first compile: every project whose root holds the
    /// file registers the databases between it and the root, so routing
    /// finds its entry.
    void discover_around(Fid path_id);

    /// The project that can compile a file: a broken project holding it,
    /// else the one its active context choice belongs to, else the one
    /// whose build lists it (its own entry or a rule's default command),
    /// else one whose include graph reaches it — the deepest whose root
    /// holds it, else any — to borrow a host there; null when none can.
    ServedProject* compiler(Fid path_id);

    /// The project a file belongs to: its compiler, else the deepest root
    /// holding it, else the first project.
    ServedProject& route(Fid path_id);

    /// Move the document to `to`: its generation moves on, so what a
    /// request computed under the old owner is void, the old owner releases
    /// it and `to` adopts it. The client keeps what it shows until `to`'s
    /// answers replace it (replace_shown).
    void change_owner(Document& document, ServedProject& to);

    /// The client shows what the document's previous project or context
    /// gave it, and no request of its own replaces it: compile it again,
    /// or have the index's answers pulled again. A broken project serves
    /// nothing, so what the client shows is taken down.
    void replace_shown(Document& document);

    /// Fold every project's round into index_progress.
    void fold_index_progress();

    // documents.cpp

    /// Open the file's document under its first name: in the table, and
    /// with its owner once the projects run.
    void open_first_name(Fid path_id, std::string text, int version);

    // contexts.cpp

    /// Serialize the choices `project` holds into its database's bytes,
    /// for the next save.
    void save_choices(ServedProject& project);

    /// `project` stopped serving: its choices leave the table, saved in its
    /// database as they are.
    void forget_choices(ServedProject& project);

    /// The generation of the context listings: it advances whenever a
    /// project's context epoch does or the projects served change.
    std::uint64_t context_epoch();

    // polling.cpp

    kota::task<> poll_task();

private:
    std::uint64_t context_generation = 0;
    std::pair<std::uint64_t, std::uint64_t> context_seen;

    /// Advances with every change of the projects served.
    std::uint64_t projects_generation = 0;

    /// The last progress of each project taking part in the current
    /// index_progress round, and the counts of the rounds that ended within
    /// it and were followed by another (or whose project was removed).
    llvm::DenseMap<ServedProject*, IndexPump::Progress> round;
    IndexPump::Progress finished;

    /// Cancellation scope of the serving phase. run_serve_mode serves until
    /// it fires; schedule_shutdown() cancels the source, unwinding the
    /// transport tasks so the root task proceeds to shutdown_and_cleanup().
    kota::cancellation_source shutdown_source;

    /// Shutdowns of removed projects and deferred applications of the file
    /// table's changes; joined in shutdown_and_cleanup().
    kota::task_group<> bg_tasks;

    /// The background looks at files and databases, and the ends of the
    /// file table's turns, until the drain cancels them.
    kota::task_group<> polling;

    /// The watch on the client process, which outlives the drain: a client
    /// may die between the shutdown reply and its exit.
    kota::task_group<> client_watch;

    /// The drain runs here, not in whoever asked first: the shutdown
    /// request that starts it is cancelled by the exit that follows.
    kota::task_group<> draining;
    bool drain_started = false;
    kota::event drained{false};

    /// Removed projects, shutting down or kept alive after by the requests
    /// still running in them.
    std::vector<std::weak_ptr<ServedProject>> stopping;

    /// The configuration files of a folder served.
    struct SettingsWatch {
        CanonicalPath folder;

        /// clice.toml, and .clice/config.toml, which applies without it.
        vfs::Flag file;
        vfs::Flag hidden;
    };

    std::vector<SettingsWatch> settings_watches;

    /// Projects made but not started yet: their root's previous project is
    /// still shutting down (see serve_folders).
    std::vector<ServedProject*> waiting;
};

int run_serve_mode(const ServerOptions& opts, llvm::StringRef configuration);

/// Serve the connections `acceptor` takes for as long as the task runs, each
/// a JSON peer: `accepted` registers its handlers and returns what serves it
/// (none for nothing), kept beside the peer and destroyed first when the
/// connection ends. `what` names the clients in the log.
kota::task<> accept_peers(kota::tcp::acceptor acceptor,
                          llvm::StringRef what,
                          llvm::unique_function<std::shared_ptr<void>(ipc::JSONPeer&)> accepted);

}  // namespace clice
