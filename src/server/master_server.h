#pragma once

#include "server/worker_pool.h"

#include "eventide/async/io/loop.h"
#include "eventide/ipc/peer.h"
#include "eventide/ipc/lsp/protocol.h"

#include "llvm/Support/Allocator.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringRef.h"

#include <cstdint>
#include <string>

namespace clice {

namespace et = eventide;
namespace protocol = eventide::ipc::protocol;

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
    MasterServer(et::event_loop& loop, et::ipc::JsonPeer& peer);

    void register_handlers();

private:
    et::event_loop& loop;
    et::ipc::JsonPeer& peer;
    WorkerPool pool;
    ServerPathPool path_pool;
    ServerLifecycle lifecycle = ServerLifecycle::Uninitialized;

    std::string workspace_root;

    // Document state: path_id -> DocumentState
    llvm::DenseMap<std::uint32_t, DocumentState> documents;

    // Helper: convert URI to file path
    std::string uri_to_path(const std::string& uri);

    // Publish diagnostics to client
    void publish_diagnostics(const std::string& uri, int version,
                             std::vector<protocol::Diagnostic> diagnostics);
    void clear_diagnostics(const std::string& uri);
};

}  // namespace clice
