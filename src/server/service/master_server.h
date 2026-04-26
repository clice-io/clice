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
/// is done by LSPClient and AgentClient, which access server state via friend.
class MasterServer {
    friend class LSPClient;
    friend class AgentClient;

public:
    MasterServer(kota::event_loop& loop, std::string self_path);
    ~MasterServer();

private:
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
    int port = 50051;
    std::string self_path;
    std::string record;
};

int run_server_mode(const ServerOptions& opts);

}  // namespace clice
