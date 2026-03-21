#pragma once

#include "server/protocol.h"

#include "eventide/async/io/loop.h"
#include "eventide/async/io/process.h"
#include "eventide/ipc/peer.h"
#include "eventide/ipc/transport.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"

#include <cstdint>
#include <functional>
#include <list>
#include <memory>

namespace clice {

namespace et = eventide;
using et::ipc::RequestResult;

struct WorkerPoolOptions {
    std::string self_path;
    std::uint32_t stateless_count = 2;
    std::uint32_t stateful_count = 2;
    std::uint64_t worker_memory_limit = 4ULL * 1024 * 1024 * 1024;  // 4GB default
};

class WorkerPool {
public:
    WorkerPool(et::event_loop& loop) : loop(loop) {}

    /// Spawn all worker processes. Returns false on failure.
    bool start(const WorkerPoolOptions& options);

    /// Gracefully stop all workers.
    et::task<> stop();

    /// Send a request to a stateful worker with path_id affinity routing.
    template <typename Params>
    RequestResult<Params> send_stateful(std::uint32_t path_id, const Params& params,
                                        et::ipc::request_options opts = {});

    /// Send a request to a stateless worker with round-robin dispatch.
    template <typename Params>
    RequestResult<Params> send_stateless(const Params& params,
                                         et::ipc::request_options opts = {});

    /// Send a notification to the stateful worker owning path_id (if any).
    template <typename Params>
    void notify_stateful(std::uint32_t path_id, const Params& params);

    /// Remove path_id from ownership tracking (e.g. when the master learns a
    /// document was evicted).
    void remove_owner(std::uint32_t path_id);

    /// Callback invoked when a stateful worker sends an EvictedParams notification.
    /// The master should translate the uri to a path_id and call remove_owner().
    std::function<void(const std::string& uri)> on_evicted;

private:
    struct WorkerProcess {
        et::process proc;
        std::unique_ptr<et::ipc::StreamTransport> transport;
        std::unique_ptr<et::ipc::BincodePeer> peer;
        std::size_t owned_documents = 0;
    };

    et::event_loop& loop;
    llvm::SmallVector<WorkerProcess> stateless_workers;
    llvm::SmallVector<WorkerProcess> stateful_workers;
    std::size_t next_stateless = 0;

    // Stateful worker routing: path_id -> worker index with LRU tracking
    llvm::DenseMap<std::uint32_t, std::size_t> owner;
    std::list<std::uint32_t> owner_lru;
    llvm::DenseMap<std::uint32_t, std::list<std::uint32_t>::iterator> owner_lru_index;

    std::size_t assign_worker(std::uint32_t path_id);
    void clear_owner(std::size_t worker_index);
    std::size_t pick_least_loaded();

    bool spawn_worker(const std::string& self_path, bool stateful,
                      std::uint64_t memory_limit);
};

// --- Template implementations ---------------------------------------------------

template <typename Params>
RequestResult<Params> WorkerPool::send_stateful(std::uint32_t path_id, const Params& params,
                                                 et::ipc::request_options opts) {
    auto idx = assign_worker(path_id);
    return stateful_workers[idx].peer->send_request(params, opts);
}

template <typename Params>
RequestResult<Params> WorkerPool::send_stateless(const Params& params,
                                                  et::ipc::request_options opts) {
    auto idx = next_stateless;
    next_stateless = (next_stateless + 1) % stateless_workers.size();
    return stateless_workers[idx].peer->send_request(params, opts);
}

template <typename Params>
void WorkerPool::notify_stateful(std::uint32_t path_id, const Params& params) {
    auto it = owner.find(path_id);
    if(it == owner.end()) return;
    stateful_workers[it->second].peer->send_notification(params);
}

}  // namespace clice
