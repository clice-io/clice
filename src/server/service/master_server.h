#pragma once

#include <cstdint>
#include <string>

#include "server/compiler/compiler.h"
#include "server/compiler/indexer.h"
#include "server/service/session.h"
#include "server/worker/worker_pool.h"
#include "server/workspace/workspace.h"

#include "kota/async/async.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/StringRef.h"

namespace clice {

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

    void start_file_watcher();

    Session* find_session(std::uint32_t path_id);
    Session& open_session(std::uint32_t path_id);
    void close_session(std::uint32_t path_id, kota::ipc::JsonPeer& peer);

    void on_file_saved(std::uint32_t path_id);

    void schedule_shutdown();

    kota::event& get_shutdown_event() {
        return shutdown_event;
    }

private:
    kota::event shutdown_event;
    void load_workspace();

    kota::event_loop& loop;

    Workspace workspace;
    llvm::DenseMap<std::uint32_t, Session> sessions;
    WorkerPool pool;
    Compiler compiler;
    Indexer indexer;

    ServerLifecycle lifecycle = ServerLifecycle::Uninitialized;
    std::string self_path;
    std::string workspace_root;
    std::string session_log_dir;
    std::string init_options_json;
};

struct ServerOptions {
    std::string mode;
    std::string host = "127.0.0.1";
    int port = 0;
    std::string self_path;
    std::string record;
};

int run_server_mode(const ServerOptions& opts);

struct DaemonOptions {
    std::string socket_path;
    std::string workspace;
    std::string self_path;
};

int run_daemon_mode(const DaemonOptions& opts);

}  // namespace clice
