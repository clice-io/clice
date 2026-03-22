#pragma once

#include <cstdint>
#include <deque>
#include <memory>
#include <string>

#include "compile/command.h"
#include "eventide/async/async.h"
#include "eventide/ipc/lsp/protocol.h"
#include "eventide/ipc/peer.h"
#include "eventide/serde/serde/raw_value.h"
#include "server/compile_graph.h"
#include "server/config.h"
#include "server/worker_pool.h"
#include "syntax/scan.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Allocator.h"

namespace clice {

namespace et = eventide;

/// Global path interning pool. Maps file paths to uint32_t IDs.
struct ServerPathPool {
    llvm::BumpPtrAllocator allocator;
    llvm::SmallVector<llvm::StringRef> paths;
    llvm::StringMap<std::uint32_t> cache;

    std::uint32_t intern(llvm::StringRef path) {
        auto [it, inserted] = cache.try_emplace(path, paths.size());
        if(inserted) {
            auto saved = path.copy(allocator);
            paths.push_back(saved);
        }
        return it->second;
    }

    llvm::StringRef resolve(std::uint32_t id) const {
        return paths[id];
    }
};

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

    void register_handlers();

private:
    et::event_loop& loop;
    et::ipc::JsonPeer& peer;
    WorkerPool pool;
    CompileGraph compile_graph;
    ServerPathPool path_pool;
    ServerLifecycle lifecycle = ServerLifecycle::Uninitialized;

    std::string self_path;
    std::string workspace_root;
    CliceConfig config;

    CompilationDatabase cdb;
    SharedScanCache scan_cache;

    // Forward include graph: path_id -> set of included path_ids
    llvm::DenseMap<std::uint32_t, llvm::DenseSet<std::uint32_t>> include_forward;
    // Backward include graph: path_id -> set of includer path_ids
    llvm::DenseMap<std::uint32_t, llvm::DenseSet<std::uint32_t>> include_backward;

    // Document state: path_id -> DocumentState
    llvm::DenseMap<std::uint32_t, DocumentState> documents;

    // Per-document debounce timers
    llvm::DenseMap<std::uint32_t, std::unique_ptr<et::timer>> debounce_timers;

    // === Index system ===

    // Queue of path_ids that need background indexing
    std::deque<std::uint32_t> index_queue;

    // Idle timer: fires after 3 seconds of no user activity
    std::unique_ptr<et::timer> idle_timer;

    // Event to wake the background indexer when new work is queued
    et::event index_event{false};

    // Whether background indexing is currently active
    bool indexing_active = false;

    // Number of files indexed so far (for progress reporting)
    std::uint32_t index_progress = 0;
    std::uint32_t index_total = 0;

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

    // Scan a file and update the include graph
    void scan_file(std::uint32_t path_id, llvm::StringRef path);

    // Helper: fill compile arguments from CDB into worker params
    void fill_compile_args(llvm::StringRef path,
                           std::string& directory,
                           std::vector<std::string>& arguments);

    // === Index system ===

    // Reset the idle timer on user activity (didOpen, didChange, feature requests)
    void reset_idle_timer();

    // Background indexer coroutine: waits for idle, processes index queue
    et::task<> run_background_indexer();

    // Populate the index queue with all CDB files
    void populate_index_queue();
};

}  // namespace clice
