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

/// Top-level LSP server — the single orchestration point for the language
/// server process.
///
/// Responsibilities:
///   - Owns the two-layer state model: Workspace (disk truth) and Sessions
///     (per-open-file volatile state).
///   - Manages Session lifecycle directly: didOpen creates, didChange mutates,
///     didSave syncs to Workspace, didClose destroys.
///   - Dispatches compilation and feature queries to Compiler.
///   - Dispatches index lookups and background indexing to Indexer.
///
/// Design principle:
///   Open files are never depended upon by other files.  Dependencies always
///   point to disk files.  The only path from Session to Workspace is didSave.
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

    /// Compilation lifecycle manager (reads/writes workspace and sessions).
    Compiler compiler;

    /// Index query and background scheduling (reads from workspace and sessions).
    Indexer indexer;

    ServerLifecycle lifecycle = ServerLifecycle::Uninitialized;
    std::string self_path;
    std::string workspace_root;
    std::string session_log_dir;

    et::task<> load_workspace();

    using RawResult = et::task<et::serde::RawValue, et::ipc::Error>;
};

}  // namespace clice
