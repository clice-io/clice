#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "eventide/async/async.h"
#include "eventide/ipc/peer.h"
#include "eventide/serde/serde/raw_value.h"
#include "server/compiler.h"
#include "server/indexer.h"
#include "server/session.h"
#include "server/worker_pool.h"
#include "server/workspace.h"

#include "llvm/ADT/DenseMap.h"

namespace clice {

namespace et = eventide;

enum class ServerLifecycle : std::uint8_t {
    Uninitialized,
    Initialized,
    Ready,
    ShuttingDown,
    Exited,
};

/// Top-level LSP server.
///
/// Owns all persistent state and registers LSP handlers.  Document lifecycle
/// (open/change/close/save) is handled directly here on the sessions map.
/// Compilation and feature-request forwarding are delegated to Compiler;
/// index queries are delegated to Indexer.
class MasterServer {
public:
    MasterServer(et::event_loop& loop, et::ipc::JsonPeer& peer, std::string self_path);
    ~MasterServer();

    void register_handlers();

private:
    et::event_loop& loop;
    et::ipc::JsonPeer& peer;

    /// Persistent project-wide state (config, CDB, path pool, dependency
    /// graphs, compilation caches, symbol index).
    Workspace workspace;

    /// Per-file editing sessions, keyed by server-level path_id.
    llvm::DenseMap<std::uint32_t, Session> sessions;

    /// Worker process pool for offloading compilation and queries.
    WorkerPool pool;

    /// Index query layer (reads from workspace and sessions).
    Indexer indexer;

    /// Compilation lifecycle manager (reads/writes workspace and sessions).
    Compiler compiler;

    ServerLifecycle lifecycle = ServerLifecycle::Uninitialized;
    std::string self_path;
    std::string workspace_root;
    std::string session_log_dir;

    /// Background indexing state.
    std::vector<std::uint32_t> index_queue;
    std::size_t index_queue_pos = 0;
    bool indexing_active = false;
    bool indexing_scheduled = false;
    std::shared_ptr<et::timer> index_idle_timer;

    et::task<> load_workspace();
    void schedule_indexing();
    et::task<> run_background_indexing();

    using RawResult = et::task<et::serde::RawValue, et::ipc::Error>;
};

}  // namespace clice
