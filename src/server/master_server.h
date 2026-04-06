#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "eventide/async/async.h"
#include "eventide/ipc/peer.h"
#include "eventide/serde/serde/raw_value.h"
#include "server/compiler.h"
#include "server/config.h"
#include "server/indexer.h"
#include "server/worker_pool.h"
#include "support/path_pool.h"
#include "syntax/dependency_graph.h"

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
/// Registers LSP handlers and delegates all compilation, document management,
/// and index queries to Compiler and Indexer respectively.  MasterServer
/// itself only owns workspace initialization and background indexing scheduling.
class MasterServer {
public:
    MasterServer(et::event_loop& loop, et::ipc::JsonPeer& peer, std::string self_path);
    ~MasterServer();

    void register_handlers();

private:
    et::event_loop& loop;
    et::ipc::JsonPeer& peer;
    WorkerPool pool;
    PathPool path_pool;
    Indexer indexer;

    ServerLifecycle lifecycle = ServerLifecycle::Uninitialized;
    std::string self_path;
    std::string workspace_root;
    CliceConfig config;
    std::string session_log_dir;

    CompilationDatabase cdb;
    DependencyGraph dependency_graph;

    Compiler compiler;

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
