#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "command/command.h"
#include "eventide/async/async.h"
#include "eventide/ipc/lsp/protocol.h"
#include "eventide/ipc/peer.h"
#include "eventide/serde/serde/raw_value.h"
#include "server/compiler.h"
#include "server/config.h"
#include "server/indexer.h"
#include "server/worker_pool.h"
#include "support/path_pool.h"
#include "syntax/dependency_graph.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/StringRef.h"

namespace clice {

namespace et = eventide;
namespace protocol = et::ipc::protocol;

enum class CompletionContext { None, IncludeQuoted, IncludeAngled, Import };

struct PreambleCompletionContext {
    CompletionContext kind = CompletionContext::None;
    std::string prefix;
};

struct DocumentState {
    int version = 0;

    std::string text;

    std::uint64_t generation = 0;

    bool ast_dirty = true;

    /// Non-null while a compile is in flight.  Callers wait on the event;
    /// the compile task runs independently and cannot be cancelled by LSP
    /// $/cancelRequest.
    struct PendingCompile {
        et::event done;
        bool succeeded = false;
    };

    std::shared_ptr<PendingCompile> compiling;
};

enum class ServerLifecycle : std::uint8_t {
    Uninitialized,
    Initialized,
    Ready,
    ShuttingDown,
    Exited,
};

class MasterServer {
public:
    MasterServer(et::event_loop& loop, et::ipc::JsonPeer& peer, std::string self_path);
    ~MasterServer();

    void register_handlers();

private:
    /// Event loop for scheduling async tasks.
    et::event_loop& loop;

    /// JSON-RPC peer for LSP communication.
    et::ipc::JsonPeer& peer;

    /// Pool of stateful/stateless worker processes.
    WorkerPool pool;

    /// Interning pool for file paths (path string -> uint32_t ID).
    PathPool path_pool;

    /// Cross-file symbol index.
    Indexer indexer;

    /// Current server lifecycle state.
    ServerLifecycle lifecycle = ServerLifecycle::Uninitialized;

    /// Path to the clice binary itself.
    std::string self_path;

    /// Root directory of the opened workspace.
    std::string workspace_root;

    /// User/project configuration.
    CliceConfig config;

    /// Session-specific log directory.
    std::string session_log_dir;

    /// Compilation database (compile_commands.json).
    CompilationDatabase cdb;

    /// Include/module dependency graph built from fast lexer scanning.
    DependencyGraph dependency_graph;

    /// Compilation artifacts, dependency preparation, header context.
    Compiler compiler;

    // === Indexing scheduling state ===

    std::vector<std::uint32_t> index_queue;
    std::size_t index_queue_pos = 0;
    bool indexing_active = false;
    bool indexing_scheduled = false;
    std::shared_ptr<et::timer> index_idle_timer;

    // === Document state ===

    llvm::DenseMap<std::uint32_t, DocumentState> documents;

    // === Helpers ===

    std::string uri_to_path(const std::string& uri);

    void publish_diagnostics(const std::string& uri,
                             int version,
                             const eventide::serde::RawValue& diagnostics_json);

    void clear_diagnostics(const std::string& uri);

    et::task<bool> ensure_compiled(std::uint32_t path_id);

    et::task<> load_workspace();

    void schedule_indexing();
    et::task<> run_background_indexing();

    // === Include/import completion ===

    PreambleCompletionContext detect_completion_context(const std::string& text, uint32_t offset);
    et::serde::RawValue complete_include(const PreambleCompletionContext& ctx,
                                         llvm::StringRef path);
    et::serde::RawValue complete_import(const PreambleCompletionContext& ctx);

    // === Feature request forwarding ===

    using RawResult = et::task<et::serde::RawValue, et::ipc::Error>;

    template <typename WorkerParams>
    RawResult forward_stateful(const std::string& uri);

    template <typename WorkerParams>
    RawResult forward_stateful(const std::string& uri, const protocol::Position& position);

    template <typename WorkerParams>
    RawResult forward_stateless(const std::string& uri, const protocol::Position& position);
};

}  // namespace clice
