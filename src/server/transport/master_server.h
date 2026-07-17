#pragma once

#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <vector>

#include "server/compiler/compiler.h"
#include "server/compiler/context_resolver.h"
#include "server/compiler/indexer.h"
#include "server/service/feature_router.h"
#include "server/service/query.h"
#include "server/state/config.h"
#include "server/state/invalidator.h"
#include "server/state/session.h"
#include "server/state/session_store.h"
#include "server/state/workspace.h"
#include "server/worker/worker_pool.h"
#include "support/anomaly.h"
#include "support/signal.h"

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
           help = "TCP port (pipe mode: agentic only; socket mode: LSP + agentic)",
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

/// Core server state — owns the two-layer state model (Workspace + Sessions),
/// the worker pool, compilation engine, index query, and background indexer.
///
/// Does NOT own any transport or peer.  Protocol-specific handler registration
/// is done by LSPClient and AgentClient, which drive the server through its
/// public members; the composition itself lives entirely here.
class MasterServer {
public:
    MasterServer(kota::event_loop& loop, std::string self_path);
    ~MasterServer();

    void initialize();
    void initialize(llvm::StringRef root);

    kota::task<> shutdown_and_cleanup();

    std::shared_ptr<Session> find_session(std::uint32_t path_id);
    std::shared_ptr<Session> open_session(std::uint32_t path_id);

    /// Close the session. The diagnostics clear travels through the
    /// session's output + on_output signal; a transport whose client has
    /// not completed the handshake drops it (nothing was ever pushed, so
    /// there is nothing to clear).
    void close_session(std::uint32_t path_id);

    /// The single entry point for file events: fold the batch through the
    /// Invalidator, then execute the resulting effects against the mutable
    /// services (sessions, context resolver, background indexer).
    void dispatch(llvm::ArrayRef<FileEvent> events);

    /// Called by the agentic index-query handlers before answering. The
    /// first call turns on open-file indexing (sticky) and enqueues the
    /// currently open files, so agents get shards for files whose
    /// sessions otherwise satisfied every consumer.
    void on_agentic_query();

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
    Workspace workspace;
    WorkerPool pool;
    ContextResolver contexts;
    Compiler compiler;
    Indexer indexer;
    IndexQuery index_query;

    /// The agentic transport's view of the index: disk truth only.
    /// Agents read files from disk, so buffer state must not leak into
    /// their answers — see IndexQueryOptions::disk_only.
    IndexQuery agent_query;

    FeatureRouter features;
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

    void load_workspace();

    /// Open the CacheStore under cache_dir and register the blob
    /// namespaces.  No-op if already open or caching is disabled.
    void open_cache_store();

    /// Periodically checkpoint the cache store manifest so last-accessed
    /// times survive crashes (the store itself is passive by design).
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
