#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "server/compiler/compiler.h"
#include "server/compiler/indexer.h"
#include "server/service/session.h"
#include "server/worker/worker_pool.h"
#include "server/workspace/config.h"
#include "server/workspace/workspace.h"

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

    // TODO: add periodic stat-based file watching
    kota::task<> shutdown_and_cleanup();

    std::shared_ptr<Session> find_session(std::uint32_t path_id);
    std::shared_ptr<Session> open_session(std::uint32_t path_id);
    void close_session(std::uint32_t path_id, kota::ipc::JsonPeer& peer);

    /// The preamble include links of a session's active PCH, or nullptr.
    const std::vector<feature::DocumentLink>* find_preamble_links(const Session& session);

    /// Resolve go-to-definition on a preamble include line that the worker
    /// AST cannot see: the include is compiled into the PCH, so the target
    /// is answered from the PCH's cached preamble links. Module names go
    /// through the ordinary index pipeline, not this path.
    std::vector<protocol::Location>
        resolve_directive_definition(Session& session, const protocol::Position& position);

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

    /// Problems found while loading clice.toml during initialize(), kept so
    /// LSPClient can publish them as diagnostics on the config file's URI.
    std::vector<ConfigIssue> config_issues;
    /// Path of the config file that was found (empty when none).
    std::string config_path;
};

int run_serve_mode(const ServerOptions& opts, const char* self_path);

}  // namespace clice
