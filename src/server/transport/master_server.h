#pragma once

#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <vector>

#include "config/config.h"
#include "project/command_resolver.h"
#include "project/index_store.h"
#include "project/project.h"
#include "sched/stack.h"
#include "server/service/ast_family.h"
#include "server/service/context_service.h"
#include "server/service/dispatcher.h"
#include "server/service/features.h"
#include "server/service/live_sources.h"
#include "server/state/editor_context.h"
#include "server/state/invalidator.h"
#include "server/state/session.h"
#include "server/state/session_store.h"
#include "support/anomaly.h"
#include "support/signal.h"
#include "worker/pool.h"

#include "kota/async/async.h"
#include "kota/deco/deco.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/StringRef.h"

namespace clice {

class FileTracker;

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
    <std::string> project;

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

/// Core server state — owns the two-layer state model (Project + Sessions),
/// the worker pool, compilation engine, index query, and background indexer.
///
/// Does NOT own any transport or peer.  Protocol-specific handler registration
/// is done by LSPClient and the control channel (serve_control), which drive
/// the server through its public members; the composition itself lives
/// entirely here.
class MasterServer {
public:
    MasterServer(kota::event_loop& loop,
                 std::string self_path,
                 std::string requested_configuration);
    ~MasterServer();

    void initialize();
    void initialize(llvm::StringRef root);

    kota::task<> shutdown_and_cleanup();

    std::shared_ptr<Session> find_session(Fid path_id);
    std::shared_ptr<Session> open_session(Fid path_id);

    /// Before a file's first compile: register the databases discovery
    /// finds between its directory and the workspace root (see
    /// FileTracker::discover_around) so the compile finds its entry.
    void discover_around(Fid path_id);

    /// Settle a freshly opened index-only buffer: escalate one that
    /// already diverged from its shard, or boost the file's background
    /// indexing when nothing can serve it. Escalated sessions need no
    /// settlement — builds are pull-driven.
    void settle_open_serving(std::shared_ptr<Session> session);

    /// Close the session. The diagnostics clear travels through the
    /// session's output + on_output signal; a transport whose client has
    /// not completed the handshake drops it (nothing was ever pushed, so
    /// there is nothing to clear).
    void close_session(Fid path_id);

    /// The single entry point for file events: fold the batch through the
    /// Invalidator, then execute the resulting effects against the mutable
    /// services (sessions, editor context, background indexer).
    void dispatch(llvm::ArrayRef<FileEvent> events);

    void schedule_shutdown();

    kota::cancellation_token shutdown_token() const {
        return shutdown_source.token();
    }

    /// The table of open documents and the buffer-sync logic. Public so
    /// transports and features can reach open sessions directly (e.g.
    /// sessions.find(path_id)); MasterServer's open/close methods layer the
    /// non-map orchestration (pool eviction, diagnostics, indexing) on top.
    SessionStore sessions;

    /// The composed services that make up the server, declared (and thus
    /// constructed) in dependency order. Transports and features drive the
    /// server through these directly; the wiring between them lives in wire().
    kota::event_loop& loop;
    FileTable files;
    Project project{files};
    CommandResolver commands{project};
    EditorContext contexts{project, commands};

    /// The scheduling core the batch driver runs too, its families
    /// registered at construction — nodes materialize on demand, so a
    /// module-free project pays nothing. The store and the pump are
    /// serving-neutral; the session-side policy — admission vetoes,
    /// unservable escalation, serving-row refresh — lives on this class
    /// and is installed into the pump's hooks by wire(). The AST family is
    /// assembled here in the server: its rounds capture sessions,
    /// quarantine and publishing.
    SchedulingStack sched{loop, project, commands};
    ASTFamily ast{project, contexts, sched.graph, sched.pcm, sched.pch, sched.pool, sessions, loop};

    Dispatcher dispatcher{project, contexts, ast, sched.pool};
    ContextService context_service{project, contexts, ast};

    /// Emitted when rows an open index-served session is serving changed:
    /// results the client already pulled describe the old rows, and only a
    /// refresh request makes it re-pull them — index-only sessions never
    /// compile, so the compile-driven refresh in the output push path
    /// cannot cover them.
    Signal<> on_serving_rows_changed;

    ServerLiveSources live_sources{project, sched.pch, sessions, ast.projections};
    PumpGate freshness{sched.pump, project.config};
    index::IndexQuery index_query;

    Features features;
    Invalidator invalidator;

    /// Stat-polling discovery of CDB and on-disk file changes. Created by
    /// initialize() once the workspace is loaded (null before that and in
    /// workspace-less sessions); its polling loops run in bg_tasks, and the
    /// clice/internal/poll test hook drives ticks directly.
    std::unique_ptr<FileTracker> tracker;

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
    /// serve-mode options), consumed when loading the workspace and publishing
    /// config diagnostics.
    std::string workspace_root;
    std::string init_options_json;
    /// The `--configuration` argument: the build configuration this
    /// session runs, over the persisted selection; empty takes the
    /// selection, else the default.
    std::string requested_configuration;
    /// Problems found while loading clice.toml during initialize(), kept so
    /// LSPClient can publish them as diagnostics on the config file's URI.
    std::vector<ConfigIssue> config_issues;
    /// Path of the config file that was found (empty when none).
    std::string config_path;

private:
    /// The server's wiring diagram: every domain→domain callback hook
    /// (pool crash/eviction, indexing scheduling, ...) is assigned here
    /// and nowhere else, so the composition root shows all cross-component
    /// plumbing in one place. domain→transport communication does not go
    /// through here — it uses Signal members that transports subscribe to.
    void wire();

    /// Dispatch- and landing-time admission on one claimed pump file: the
    /// serving side's veto (open sessions, index-only disk divergence).
    Admission index_admission(Fid server_path_id);

    /// An index attempt settled with no retry pending; a session waiting
    /// on the index with nothing servable will never be served by it —
    /// escalate instead of letting it answer empty forever.
    void index_attempt_settled(Fid server_path_id);

    /// Whether an open session serves this file's project rows (freshness
    /// clause 4) and a client already pulled some of them — the emit
    /// condition of on_serving_rows_changed.
    bool serves_session_rows(Fid path_id) const;

    /// Filter a store row-change report down to the sessions actually
    /// serving those rows and wake the transports.
    void index_rows_changed(llvm::ArrayRef<Fid> path_ids);

    Signal<llvm::ArrayRef<Fid>>::Connection index_rows_conn;

    void load_root_project();

    /// When this server holds the cache directory's writer lock, the
    /// commands that find it taken ask this server to index for them:
    /// listen on a loopback port and record it next to the lock (see
    /// index/writer_lock.h). The record is removed at shutdown.
    void start_control_listener();
    bool endpoint_recorded = false;

    /// Periodically checkpoint the cache store manifest so last-accessed
    /// times survive crashes (the store itself is passive by design).
    /// Schedule a save carrying dirty artifact/context metadata; no-op
    /// when one is already scheduled or the server is shutting down (the
    /// final shutdown save covers it).
    void schedule_metadata_flush();
    kota::task<> metadata_flush_task();
    bool metadata_flush_scheduled = false;

    kota::task<> cache_checkpoint_task();

    /// Drop pch_cache metadata for blobs the store's LRU evicted from
    /// disk (see cache_checkpoint_task).
    void drain_store_evictions();

    /// The file tracker's polling loops: each tick hands the tracker's
    /// event batch to dispatch(). Spawned by initialize() when the
    /// configured interval is non-zero.
    kota::task<> cdb_poll_task();
    kota::task<> workspace_poll_task();

    /// Cancellation scope of the serving phase. run_serve_mode bounds its
    /// transport tasks with with_token(..., shutdown_token());
    /// schedule_shutdown() cancels the source, unwinding them so the root
    /// task proceeds to shutdown_and_cleanup().
    kota::cancellation_source shutdown_source;

    /// Server-owned background tasks (cache checkpoint); cancelled and
    /// joined in shutdown_and_cleanup().
    kota::task_group<> bg_tasks;

    std::string self_path;
    std::string session_log_dir;
};

int run_serve_mode(const ServerOptions& opts, const char* self_path);

}  // namespace clice
