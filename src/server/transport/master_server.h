#pragma once

#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <vector>

#include "config/config.h"
#include "sched/index/pump.h"
#include "server/state/session.h"
#include "server/transport/project_server.h"
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

    /// Start serving `workspace_roots` (the client's folders, or the
    /// command line's --workspace; none serves a single rootless project):
    /// load each project's configuration, start the pool sized for all of
    /// them, and load the projects.
    void initialize();
    void initialize(llvm::StringRef root);

    kota::task<> shutdown_and_cleanup();

    /// The project serving a file: the one its open document was routed
    /// to, else the one routing picks now.
    ProjectServer& owner_of(Fid path_id);

    std::shared_ptr<Session> find_session(Fid path_id);

    /// Route the file to its project and open its session there. A file no
    /// project claims starts serving the project found above it (a
    /// clice.toml or a compilation database), as if that folder were open.
    std::shared_ptr<Session> open_session(Fid path_id);

    /// Close the file's session in the project it was routed to.
    void close_session(Fid path_id);

    /// Before a file's first compile: every project whose root holds the
    /// file registers the databases between it and the root (see
    /// FileTracker::discover_around), so routing finds its entry.
    void discover_around(Fid path_id);

    /// Serve another folder / stop serving one (didChangeWorkspaceFolders).
    /// Open documents of a removed project move to the project routing
    /// picks for them now.
    void add_folder(std::string root);
    void remove_folder(llvm::StringRef root);

    /// The indexing progress of every project, as one round: the counts
    /// add up, and it ends when the last project's round ends.
    IndexPump::Progress index_progress() const;

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
    /// serves files no project claims.
    std::vector<std::unique_ptr<ProjectServer>> projects;

    /// A project published a document's compile output.
    Signal<std::shared_ptr<Session>> on_output;

    /// A project's indexing progress moved; read index_progress().
    Signal<> on_index_progress;

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

    /// Initialization parameters captured from the LSP initialize request (or
    /// serve-mode options), consumed when loading the projects.
    std::vector<std::string> workspace_roots;
    std::string init_options_json;

    /// The `--configuration` argument: the build configuration this
    /// session runs, over the persisted selection; empty takes the
    /// selection, else the default.
    std::string requested_configuration;

private:
    /// The project a file belongs to: the one whose build compiles it (its
    /// own entry or a rule's default command), else one whose include graph
    /// reaches it (it borrows a host there), else the deepest root holding
    /// it; null when none claims it.
    ProjectServer* claimant(Fid path_id);

    /// The claimant, else the first project.
    ProjectServer& route(Fid path_id);

    /// Move the open documents of `from` routing now sends elsewhere,
    /// buffer and version intact.
    void rehome_sessions(ProjectServer& from);

    /// The project each open document was routed to.
    llvm::DenseMap<Fid, ProjectServer*> owners;

    /// The pool's callbacks, routed to the projects owning the documents.
    void wire();

    /// Cancellation scope of the serving phase. run_serve_mode bounds its
    /// transport tasks with with_token(..., shutdown_token());
    /// schedule_shutdown() cancels the source, unwinding them so the root
    /// task proceeds to shutdown_and_cleanup().
    kota::cancellation_source shutdown_source;

    /// Shutdowns of removed projects; joined in shutdown_and_cleanup().
    kota::task_group<> bg_tasks;

    std::string self_path;
    std::string session_log_dir;
};

int run_serve_mode(const ServerOptions& opts, const char* self_path);

}  // namespace clice
