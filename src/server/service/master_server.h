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
/// is done by LSPClient and AgentClient, which call public methods here.
class MasterServer {
public:
    MasterServer(kota::event_loop& loop, std::string self_path);
    ~MasterServer();

    // --- Lifecycle ---

    ServerLifecycle get_lifecycle() const {
        return lifecycle;
    }

    void set_lifecycle(ServerLifecycle state) {
        lifecycle = state;
    }

    // --- Initialization ---

    void set_workspace_root(std::string root) {
        workspace_root = std::move(root);
    }

    llvm::StringRef get_workspace_root() const {
        return workspace_root;
    }

    void set_init_options(std::string json) {
        init_options_json = std::move(json);
    }

    void set_session_log_dir(std::string dir) {
        session_log_dir = std::move(dir);
    }

    llvm::StringRef get_session_log_dir() const {
        return session_log_dir;
    }

    llvm::StringRef get_self_path() const {
        return self_path;
    }

    /// Apply initialization options to config and prepare workspace.
    /// Called from the LSP `initialized` handler.
    void initialize();

    // --- Path operations ---

    std::uint32_t intern_path(llvm::StringRef path) {
        return workspace.path_pool.intern(path);
    }

    llvm::StringRef resolve_path(std::uint32_t path_id) {
        return workspace.path_pool.resolve(path_id);
    }

    // --- Session management ---

    Session* find_session(std::uint32_t path_id);

    Session& open_session(std::uint32_t path_id);

    void close_session(std::uint32_t path_id, kota::ipc::JsonPeer& peer);

    /// Called on didSave — marks dependent sessions dirty and enqueues
    /// affected files for re-indexing.
    void on_file_saved(std::uint32_t path_id);

    /// Iterate all sessions (e.g. for header context invalidation).
    template <typename F>
    void for_each_session(F&& fn) {
        for(auto& [id, session]: sessions) {
            fn(id, session);
        }
    }

    // --- Shutdown ---

    /// Schedule graceful shutdown (stop compiler, pool, event loop).
    void schedule_shutdown();

    // --- Sub-component access ---

    Workspace& get_workspace() {
        return workspace;
    }

    Compiler& get_compiler() {
        return compiler;
    }

    Indexer& get_indexer() {
        return indexer;
    }

    WorkerPool& get_pool() {
        return pool;
    }

    kota::event_loop& get_loop() {
        return loop;
    }

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
    int port = 0;
    std::string self_path;
    std::string record;
};

int run_server_mode(const ServerOptions& opts);

}  // namespace clice
