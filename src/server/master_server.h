#pragma once

#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <vector>

#include "sched/index/pump.h"
#include "server/extension.h"
#include "server/project_server.h"
#include "server/session.h"
#include "support/anomaly.h"
#include "support/signal.h"
#include "worker/pool.h"

#include "kota/async/async.h"
#include "kota/deco/deco.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/StringRef.h"

namespace clice {

namespace deco = kota::deco;

enum class ServerMode : std::uint8_t { Pipe, Socket };

struct ServerOptions {
    DecoFlag(names = {"-h", "--help"}, help = "Show help", required = false)
    help;

    DecoKV(style = deco::decl::KVStyle::JoinedOrSeparate,
           help = "Server mode: pipe (default) or socket (debug)",
           required = false)
    <ServerMode> mode = ServerMode::Pipe;

    DecoKV(style = deco::decl::KVStyle::JoinedOrSeparate,
           help = "Socket mode address",
           required = false)
    <std::string> host = "127.0.0.1";

    DecoKV(style = deco::decl::KVStyle::JoinedOrSeparate,
           help = "Socket mode TCP port",
           required = false)
    <int> port = 0;

    DecoKV(style = deco::decl::KVStyle::JoinedOrSeparate,
           help = "Record LSP input to file for replay testing",
           required = false)
    <std::string> record;

    DecoKV(style = deco::decl::KVStyle::JoinedOrSeparate,
           help = "Workspace root directory (optional, skips LSP initialize)",
           required = false)
    <std::string> workspace;

    DecoKV(style = deco::decl::KVStyle::JoinedOrSeparate,
           help =
               "Build configuration to activate, one of the tags declared on rules "
               "(default: the selected one, else default_configuration)",
           required = false)
    <std::string> configuration;

    DecoKV(style = deco::decl::KVStyle::JoinedOrSeparate,
           names = {"--log-level", "--log-level="},
           help = "Log level: trace, debug, info, warn, error, off",
           required = false)
    <std::string> log_level = "info";
};

enum class ServerLifecycle : std::uint8_t {
    Uninitialized,
    Initialized,
    Ready,
    ShuttingDown,
    Exited,
};

/// A guidance or anomaly message materialized for transport delivery.
/// The log file is the durable record; this copy exists so a client that
/// attaches after the message fired can still be shown it.
struct NotifyMessage {
    logging::NotifyLevel level;
    std::string text;
};

/// The server process: the worker pool, the file table and the projects it
/// serves, with every file routed to one of them.
///
/// A project (ProjectServer) owns everything that is per project — the
/// project on disk, its scheduling stack, the open documents routed to it
/// and the services over them. What stays here is process-wide: the pool
/// the projects share, the routing of files to projects, the lifecycle,
/// and the signals transports subscribe to. Does NOT own any transport or
/// peer: LSPClient and the control channel drive the server through its
/// public members.
class MasterServer {
public:
    MasterServer(kota::event_loop& loop,
                 std::string self_path,
                 std::string requested_configuration);
    ~MasterServer();

    /// Start serving the projects of `workspace_roots` (see
    /// project_roots): load each project's configuration, start the pool
    /// sized for all of them, and load the projects. Documents opened
    /// before move to the projects routing picks for them.
    void initialize();
    void initialize(llvm::StringRef root);

    kota::task<> shutdown_and_cleanup();

    /// The project serving a file: the one its open document was routed
    /// to, else the one routing picks now.
    ProjectServer& owner_of(Fid path_id);

    std::shared_ptr<Session> find_session(Fid path_id);

    /// Open a document with its buffer in the project routing picks, once
    /// every project holding it discovered the databases above it (see
    /// FileTracker::discover_around). A file no project can compile starts
    /// serving the project found nearest above it (a clice.toml or a
    /// compilation database) — when no folder holds the file, or when that
    /// root sits deeper than the folders that do — as if it were open.
    void open_session(Fid path_id, std::string text, int version);

    /// Close the file's session in the project it was routed to.
    void close_session(Fid path_id);

    /// Stop serving the `removed` folders and serve the `added` ones
    /// (didChangeWorkspaceFolders); before initialize, edit the folders it
    /// will serve. The projects follow the folders (see project_roots);
    /// open documents of a project that stops serving move to the project
    /// routing picks for them now. A folder both removed and added (a
    /// rename) keeps serving.
    void change_folders(std::vector<std::string> removed, std::vector<std::string> added);

    /// A project's indexing progress moved: fold every project's round
    /// into index_progress and wake the transports.
    void index_progress_changed(ProjectServer& project);

    /// A project revoked stamps of the shared file table: every project's
    /// persisted stamps rewrite at its next save.
    void stamps_revoked();

    /// A project's database gained or lost entries: move the open
    /// documents routing now sends to another project.
    void builds_changed();

    /// didSave: the file's own project takes the save; the others knowing
    /// the file (listing or including it) see its disk content change.
    void saved(Fid path_id);

    /// clice/queryContext over every project, paginated: the contexts the
    /// file's own project offers, then the ones other projects do —
    /// choosing one of those moves the file there (switch_context). The
    /// listing's epoch covers every project (context_epoch).
    ext::QueryContextResult query_contexts(llvm::StringRef path,
                                           Fid path_id,
                                           const ext::QueryContextParams& params);

    /// clice/switchContext: pin the context in the project offering it —
    /// the file's own first — moving the open document there first. Only
    /// that project keeps a choice for the file, and routing keeps the file
    /// with it (compiler).
    kota::task<ext::SwitchContextResult> switch_context(llvm::StringRef path,
                                                        Fid path_id,
                                                        llvm::StringRef context_path,
                                                        Fid context_path_id,
                                                        ext::SwitchContextParams params);

    /// workspace/symbol over every project: each project's ranked matches,
    /// interleaved rank by rank, a symbol two projects index listed once.
    std::vector<protocol::SymbolInformation> workspace_symbol(llvm::StringRef query);

    void schedule_shutdown();

    kota::cancellation_token shutdown_token() const {
        return shutdown_source.token();
    }

    kota::event_loop& loop;

    /// The process's fid space, shared by every project.
    FileTable files;

    /// The workers every project compiles and indexes on.
    WorkerPool pool;

    /// The projects served, in folder order; never empty. The first also
    /// serves files no project claims. Shared with the requests running in
    /// a project, which keep a removed one alive until they complete.
    std::vector<std::shared_ptr<ProjectServer>> projects;

    /// A project published a document's compile output.
    Signal<ProjectServer&, std::shared_ptr<Session>> on_output;

    /// The projects' indexing rounds as one: it begins with the first
    /// project's round and ends with the last one's, and a project whose
    /// round ended keeps its counts in it until then.
    IndexPump::Progress index_progress{.stage = IndexPump::Progress::Stage::End};

    /// index_progress moved.
    Signal<> on_index_progress;

    /// A project started or stopped serving.
    Signal<> on_projects_changed;

    /// Emitted when rows an open index-served session is serving changed:
    /// results the client already pulled describe the old rows, and only a
    /// refresh request makes it re-pull them — index-only sessions never
    /// compile, so the compile-driven refresh in the output push path
    /// cannot cover them.
    Signal<> on_serving_rows_changed;

    /// Wakes subscribers after a new message landed in notify_log. Pure
    /// wake-up per the Signal contract: subscribers keep a sequence cursor
    /// and read the messages from the log, so a late subscriber (or a
    /// missed signal) simply catches up on its next drain. The constructor
    /// owns the process-wide logging notify hook for the server's lifetime
    /// and forwards every report here; transports subscribe instead of
    /// touching the hook themselves.
    Signal<> on_notify;

    /// Recent guidance/anomaly messages (window/logMessage material),
    /// bounded by dropping the oldest. notify_seq numbers the next message,
    /// so notify_seq - notify_log.size() is the oldest retained sequence; a
    /// subscriber lagging further behind than the retention window loses
    /// the evicted messages (the log file keeps the durable record).
    std::deque<NotifyMessage> notify_log;
    std::uint64_t notify_seq = 0;

    /// Lifecycle state, advanced by the LSP initialize/shutdown handlers.
    ServerLifecycle lifecycle = ServerLifecycle::Uninitialized;

    /// The folders served, canonical: the client's workspace folders (or
    /// the command line's --workspace), and the roots open_session found
    /// above files no folder claims.
    std::vector<std::string> workspace_roots;

    /// The client's initializationOptions (JSON), applied to every project.
    std::string init_options_json;

    /// The `--configuration` argument: the build configuration this
    /// session runs, over the persisted selection; empty takes the
    /// selection, else the default.
    std::string requested_configuration;

private:
    /// A project over `root`, its cross-project queries wired to the
    /// others.
    std::shared_ptr<ProjectServer> make_project(std::string root);

    /// The root of the project serving a folder: the folder itself when it
    /// is a project of its own (defines_project), else the project of the
    /// nearest folder enclosing it, else the folder itself.
    std::string root_of(llvm::StringRef folder) const;

    /// The roots of the projects the folders need, in folder order; the
    /// rootless project's when there is no folder.
    std::vector<std::string> project_roots() const;

    /// Make the projects served those project_roots names: start the
    /// missing ones, retire the ones no folder needs, and move the open
    /// documents to the projects routing picks now. A root whose previous
    /// project is still alive starts once that is gone (make_project runs
    /// this again).
    void serve_folders();

    /// Shut a project that stopped serving down in the background.
    void retire(std::shared_ptr<ProjectServer> project);

    /// The cache directories in use, resolved (path::resolved): a cache
    /// directory belongs to one project at a time, a removed one's until
    /// it is gone.
    std::vector<std::string> taken_cache_dirs() const;

    /// Before a file's first compile: every project whose root holds the
    /// file registers the databases between it and the root, so routing
    /// finds its entry.
    void discover_around(Fid path_id);

    /// The generation of the context listings: it advances whenever a
    /// project's context epoch does or the projects served change.
    std::uint64_t context_epoch();
    std::uint64_t context_generation = 0;
    std::pair<std::uint64_t, std::uint64_t> context_seen;

    /// Advances with every change of the projects served.
    std::uint64_t projects_generation = 0;

    /// The project that can compile a file: the one holding the user's
    /// context choice for it, else the one whose build lists it (its own
    /// entry or a rule's default command), else one whose include graph
    /// reaches it — the deepest whose root holds it, else any — to borrow
    /// a host there; null when none can.
    ProjectServer* compiler(Fid path_id);

    /// The project a file belongs to: its compiler, else the deepest root
    /// holding it; null when none claims it.
    ProjectServer* claimant(Fid path_id);

    /// The claimant, else the first project.
    ProjectServer& route(Fid path_id);

    /// Move the open documents of `from` routing now sends elsewhere,
    /// buffer and version intact.
    void rehome_sessions(ProjectServer& from);

    /// The project each open document was routed to.
    llvm::DenseMap<Fid, ProjectServer*> owners;

    /// The last progress of each project taking part in the current
    /// index_progress round, and the counts of the rounds that ended within
    /// it and were followed by another (or whose project was removed).
    llvm::DenseMap<ProjectServer*, IndexPump::Progress> round;
    IndexPump::Progress carried;
    void carry(const IndexPump::Progress& progress);
    void fold_index_progress();

    /// The pool's callbacks, routed to the projects owning the documents.
    void wire();

    /// Cancellation scope of the serving phase. run_serve_mode bounds its
    /// transport tasks with with_token(..., shutdown_token());
    /// schedule_shutdown() cancels the source, unwinding them so the root
    /// task proceeds to shutdown_and_cleanup().
    kota::cancellation_source shutdown_source;

    /// Shutdowns of removed projects; joined in shutdown_and_cleanup().
    kota::task_group<> bg_tasks;

    /// Removed projects, shutting down or kept alive after by the requests
    /// still running in them.
    std::vector<std::weak_ptr<ProjectServer>> retired;

    std::string self_path;
    std::string session_log_dir;
};

int run_serve_mode(const ServerOptions& opts, const char* self_path);

}  // namespace clice
