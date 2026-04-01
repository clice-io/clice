#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "command/command.h"
#include "eventide/async/async.h"
#include "eventide/ipc/lsp/protocol.h"
#include "eventide/ipc/peer.h"
#include "eventide/serde/serde/raw_value.h"
#include "server/compile_graph.h"
#include "server/config.h"
#include "server/worker_pool.h"
#include "support/path_pool.h"
#include "syntax/dependency_graph.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringRef.h"

namespace clice {

namespace et = eventide;
namespace protocol = et::ipc::protocol;

struct DocumentState {
    int version = 0;
    std::string text;
    std::uint64_t generation = 0;
    bool build_running = false;
    bool build_requested = false;
    bool drain_scheduled = false;
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
    et::event_loop& loop;
    et::ipc::JsonPeer& peer;
    WorkerPool pool;
    PathPool path_pool;
    ServerLifecycle lifecycle = ServerLifecycle::Uninitialized;

    std::string self_path;
    std::string workspace_root;
    CliceConfig config;

    CompilationDatabase cdb;
    DependencyGraph dependency_graph;

    // Module compilation graph (lazy dependency resolution).
    std::unique_ptr<CompileGraph> compile_graph;

    // path_id -> built PCM output path (set after successful module build).
    llvm::DenseMap<std::uint32_t, std::string> pcm_paths;

    // path_id -> module name (for files that provide a module interface).
    llvm::DenseMap<std::uint32_t, std::string> path_to_module;

    // path_id -> built PCH file path.
    llvm::DenseMap<std::uint32_t, std::string> pch_paths;

    // path_id -> preamble bound (byte offset) used when building the PCH.
    llvm::DenseMap<std::uint32_t, std::uint32_t> pch_bounds;

    // path_id -> hash of preamble content at PCH build time (for staleness detection).
    llvm::DenseMap<std::uint32_t, std::uint64_t> pch_hashes;

    // path_id -> in-flight PCH build event (later arrivals co_await the same build).
    llvm::DenseMap<std::uint32_t, std::shared_ptr<et::event>> pch_building;

    // Document state: path_id -> DocumentState
    llvm::DenseMap<std::uint32_t, DocumentState> documents;

    // Per-document debounce timers (shared_ptr so drain coroutines survive didClose)
    llvm::DenseMap<std::uint32_t, std::shared_ptr<et::timer>> debounce_timers;

    // Helper: convert URI to file path
    std::string uri_to_path(const std::string& uri);

    // Publish diagnostics to client
    void publish_diagnostics(const std::string& uri,
                             int version,
                             const eventide::serde::RawValue& diagnostics_json);
    void clear_diagnostics(const std::string& uri);

    // Schedule a build after debounce
    void schedule_build(std::uint32_t path_id, const std::string& uri);

    // Build drain coroutine: waits for debounce, then runs compile loop
    et::task<> run_build_drain(std::uint32_t path_id, std::string uri);

    // Ensure a file has been compiled before servicing feature requests
    et::task<bool> ensure_compiled(std::uint32_t path_id, const std::string& uri);

    // Load CDB and build initial include graph
    et::task<> load_workspace();

    // Helper: fill compile arguments from CDB into worker params
    bool fill_compile_args(llvm::StringRef path,
                           std::string& directory,
                           std::vector<std::string>& arguments);

    // Build or reuse PCH for a source file. Returns true if PCH is available.
    et::task<bool> ensure_pch(std::uint32_t path_id,
                              llvm::StringRef path,
                              const std::string& text,
                              const std::string& directory,
                              const std::vector<std::string>& arguments);

    // Forwarding helpers for feature requests (RawValue passthrough)
    using RawResult = et::task<et::serde::RawValue, et::ipc::Error>;

    /// Forward a simple stateful request (path-only worker params).
    template <typename WorkerParams>
    RawResult forward_stateful(const std::string& uri);

    /// Forward a stateful request with position-to-offset conversion.
    template <typename WorkerParams>
    RawResult forward_stateful(const std::string& uri, const protocol::Position& position);

    /// Forward a stateless request with document content and compile args.
    template <typename WorkerParams>
    RawResult forward_stateless(const std::string& uri, const protocol::Position& position);
};

}  // namespace clice
