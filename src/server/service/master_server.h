#pragma once

#include <cstdint>
#include <string>

#include "server/compiler/compiler.h"
#include "server/compiler/indexer.h"
#include "server/service/session.h"
#include "server/worker/worker_pool.h"
#include "server/workspace/workspace.h"

#include "kota/async/async.h"
#include "kota/deco/deco.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/StringRef.h"

namespace clice {

namespace deco = kota::deco;

enum class ServerMode : std::uint8_t { Pipe, Socket, Relay, Daemon };

struct ServerOptions {
    DecoFlag(names = {"-h", "--help"}, help = "Show help", required = false)
    help;

    DecoKV(style = deco::decl::KVStyle::JoinedOrSeparate,
           help = "Server mode: pipe (default), socket, relay, or daemon",
           required = false)
    <ServerMode> mode = ServerMode::Pipe;

    DecoKV(style = deco::decl::KVStyle::JoinedOrSeparate,
           help = "Socket mode address",
           required = false)
    <std::string> host = "127.0.0.1";

    DecoKV(style = deco::decl::KVStyle::JoinedOrSeparate,
           help = "Agentic TCP port (0 = disabled)",
           required = false)
    <int> port = 0;

    DecoKV(style = deco::decl::KVStyle::JoinedOrSeparate,
           help = "Record LSP input to file for replay testing",
           required = false)
    <std::string> record;

    DecoKV(style = deco::decl::KVStyle::JoinedOrSeparate,
           help = "Unix domain socket path (relay/daemon mode)",
           required = false)
    <std::string> socket;

    DecoKV(style = deco::decl::KVStyle::JoinedOrSeparate,
           help = "Workspace root directory (daemon mode)",
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
/// the worker pool, compilation engine, and indexer.
///
/// Does NOT own any transport or peer.  Protocol-specific handler registration
/// is done by LSPClient and AgentClient, which access private members directly.
class MasterServer {
    friend class LSPClient;
    friend class AgentClient;

public:
    MasterServer(kota::event_loop& loop, std::string self_path);
    ~MasterServer();

    void initialize();
    void initialize(llvm::StringRef root);

    kota::task<> file_watcher_task();
    kota::task<> shutdown_and_cleanup();

    std::shared_ptr<Session> find_session(std::uint32_t path_id);
    std::shared_ptr<Session> open_session(std::uint32_t path_id);
    void close_session(std::uint32_t path_id, kota::ipc::JsonPeer& peer);

    void on_file_saved(std::uint32_t path_id);

    void schedule_shutdown();

    kota::event& get_shutdown_event() {
        return shutdown_event;
    }

private:
    kota::event shutdown_event;
    void load_workspace();

    /// Open the CacheStore under cache_dir and register the blob
    /// namespaces.  No-op if already open or caching is disabled.
    void open_cache_store();

    /// Periodically checkpoint the cache store manifest so last-accessed
    /// times survive crashes (the store itself is passive by design).
    kota::task<> cache_checkpoint_task();

    kota::event_loop& loop;

    /// Server-owned background tasks (cache checkpoint); cancelled and
    /// joined in shutdown_and_cleanup().
    kota::task_group<> bg_tasks;

    Workspace workspace;
    llvm::DenseMap<std::uint32_t, std::shared_ptr<Session>> sessions;
    WorkerPool pool;
    Compiler compiler;
    Indexer indexer;

    ServerLifecycle lifecycle = ServerLifecycle::Uninitialized;
    std::string self_path;
    std::string workspace_root;
    std::string session_log_dir;
    std::string init_options_json;
};

int run_server_mode(const ServerOptions& opts, const char* self_path);

int run_daemon_mode(const ServerOptions& opts, const char* self_path);

}  // namespace clice
