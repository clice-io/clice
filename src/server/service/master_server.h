#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "server/compiler/compiler.h"
#include "server/context/context_resolver.h"
#include "server/feature/feature_router.h"
#include "server/index/background_indexer.h"
#include "server/index/query.h"
#include "server/session/session.h"
#include "server/session/session_store.h"
#include "server/worker/worker_pool.h"
#include "server/workspace/config.h"
#include "server/workspace/invalidator.h"
#include "server/workspace/workspace.h"

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
    void close_session(std::uint32_t path_id, kota::ipc::JsonPeer& peer);

    /// The single entry point for file events: fold the batch through the
    /// Invalidator, then execute the resulting effects against the mutable
    /// services (sessions, context resolver, background indexer).
    void dispatch(llvm::ArrayRef<FileEvent> events);

    void schedule_shutdown();

    kota::event& get_shutdown_event() {
        return shutdown_event;
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
    IndexQuery index_query;
    BackgroundIndexer background_indexer;
    FeatureRouter features;
    Invalidator invalidator;

    /// Stat-polling discovery of CDB and on-disk file changes. Created by
    /// initialize() once the workspace is loaded (null before that and in
    /// workspace-less sessions); its polling loops run in bg_tasks, and the
    /// clice/internal/poll test hook drives ticks directly.
    std::unique_ptr<FileTracker> tracker;

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

    /// The file tracker's polling loops: each tick hands the tracker's
    /// event batch to dispatch(). Spawned by initialize() when the
    /// configured interval is non-zero.
    kota::task<> cdb_poll_task();
    kota::task<> workspace_poll_task();

    kota::event shutdown_event;

    /// Server-owned background tasks (cache checkpoint); cancelled and
    /// joined in shutdown_and_cleanup().
    kota::task_group<> bg_tasks;

    std::string self_path;
    std::string session_log_dir;
};

int run_serve_mode(const ServerOptions& opts, const char* self_path);

}  // namespace clice
