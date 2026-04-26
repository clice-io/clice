#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <list>
#include <memory>

#include "protocol/worker.h"

#include "kota/async/async.h"
#include "kota/ipc/codec/bincode.h"
#include "kota/ipc/peer.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"

namespace clice {

using kota::ipc::RequestResult;

struct WorkerPoolOptions {
    std::string self_path;
    std::uint32_t stateless_count = 2;
    std::uint32_t stateful_count = 2;
    std::uint64_t worker_memory_limit = 4ULL * 1024 * 1024 * 1024;  // 4GB default
    std::string log_dir;
};

class WorkerPool {
public:
    WorkerPool(kota::event_loop& loop) : loop(loop) {}

    /// Spawn all worker processes. Returns false on failure.
    bool start(const WorkerPoolOptions& options);

    /// Gracefully stop all workers.
    kota::task<> stop();

    /// Send a request to a stateful worker with path_id affinity routing.
    template <typename Params>
    RequestResult<Params> send_stateful(std::uint32_t path_id,
                                        const Params& params,
                                        kota::ipc::request_options opts = {});

    /// Send a request to a stateless worker with round-robin dispatch.
    template <typename Params>
    RequestResult<Params> send_stateless(const Params& params,
                                         kota::ipc::request_options opts = {});

    /// Send a notification to the stateful worker owning path_id (if any).
    template <typename Params>
    void notify_stateful(std::uint32_t path_id, const Params& params);

    /// Remove path_id from ownership tracking (e.g. when the master learns a
    /// document was evicted).
    void remove_owner(std::uint32_t path_id);

    /// Callback invoked when a stateful worker sends an EvictedParams notification.
    /// The master should translate the path to a path_id and call remove_owner().
    std::function<void(const std::string& path)> on_evicted;

private:
    struct WorkerProcess {
        kota::process proc;
        std::unique_ptr<kota::ipc::BincodePeer> peer;
        std::size_t owned_documents = 0;
        bool alive = true;
        unsigned restart_count = 0;
    };

    kota::event_loop& loop;
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

    bool shutting_down_ = false;
    std::size_t alive_count_ = 0;
    kota::event all_exited_{true};  // Signalled when alive_count_ reaches 0.
    WorkerPoolOptions options_;
    std::string log_dir_;

    /// Peers moved here during respawn so their coroutines can finish
    /// before the object is destroyed.
    llvm::SmallVector<std::unique_ptr<kota::ipc::BincodePeer>> retired_peers;

    bool spawn_worker(const std::string& self_path, bool stateful, std::uint64_t memory_limit);
    bool respawn_worker(std::size_t index, bool stateful);
    kota::task<> monitor_worker(std::size_t index, bool stateful);
};

template <typename Params>
RequestResult<Params> WorkerPool::send_stateful(std::uint32_t path_id,
                                                const Params& params,
                                                kota::ipc::request_options opts) {
    if(stateful_workers.empty()) {
        co_return kota::outcome_error(kota::ipc::Error{"No stateful workers available"});
    }
    auto idx = assign_worker(path_id);
    if(!stateful_workers[idx].alive) {
        co_return kota::outcome_error(kota::ipc::Error{"Assigned stateful worker is down"});
    }
    co_return co_await stateful_workers[idx].peer->send_request(params, opts);
}

template <typename Params>
RequestResult<Params> WorkerPool::send_stateless(const Params& params,
                                                 kota::ipc::request_options opts) {
    if(stateless_workers.empty()) {
        co_return kota::outcome_error(kota::ipc::Error{"No stateless workers available"});
    }
    // Round-robin, skipping dead workers.
    auto start = next_stateless;
    for(std::size_t i = 0; i < stateless_workers.size(); ++i) {
        auto idx = (start + i) % stateless_workers.size();
        if(stateless_workers[idx].alive) {
            next_stateless = (idx + 1) % stateless_workers.size();
            co_return co_await stateless_workers[idx].peer->send_request(params, opts);
        }
    }
    co_return kota::outcome_error(kota::ipc::Error{"All stateless workers are down"});
}

template <typename Params>
void WorkerPool::notify_stateful(std::uint32_t path_id, const Params& params) {
    auto it = owner.find(path_id);
    if(it == owner.end())
        return;
    if(!stateful_workers[it->second].alive)
        return;
    stateful_workers[it->second].peer->send_notification(params);
}

}  // namespace clice
