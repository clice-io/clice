#pragma once

#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <list>
#include <memory>

#include "server/protocol/worker.h"

#include "kota/async/async.h"
#include "kota/ipc/codec/bincode.h"
#include "kota/ipc/peer.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"

namespace clice {

namespace testing {

struct WorkerPoolFixture;

}

using kota::ipc::RequestResult;

/// Information about a worker crash, delivered via WorkerPool::on_crash.
struct WorkerCrashInfo {
    std::size_t worker_index;
    bool stateful;
    int exit_code = 0;
    int exit_signal = 0;
    unsigned restart_count;
    bool will_restart;
    /// For stateful workers: path_ids of documents that were owned by this worker.
    llvm::SmallVector<std::uint32_t> lost_documents;
};

struct WorkerPoolOptions {
    std::string self_path;
    std::uint32_t stateless_count = 2;
    std::uint32_t stateful_count = 2;
    std::uint64_t worker_memory_limit = 4ULL * 1024 * 1024 * 1024;  // 4GB default
    std::string log_dir;
    unsigned max_restarts = 5;
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

    /// Send a request to a stateless worker with priority-aware scheduling.
    template <typename Params>
    RequestResult<Params> send_stateless(const Params& params,
                                         kota::ipc::request_options opts = {});

    /// Send a notification to the stateful worker owning path_id (if any).
    template <typename Params>
    void notify_stateful(std::uint32_t path_id, const Params& params);

    /// Remove path_id from ownership tracking (e.g. when the master learns a
    /// document was evicted).
    void remove_owner(std::uint32_t path_id);

    /// Callback invoked when a worker process crashes.
    std::function<void(const WorkerCrashInfo&)> on_crash;

    /// Callback invoked when a stateful worker sends an EvictedParams notification.
    /// The master should translate the path to a path_id and call remove_owner().
    std::function<void(const std::string& path)> on_evicted;

private:
    struct WorkerProcess {
        kota::process proc;
        std::unique_ptr<kota::ipc::BincodePeer> peer;
        std::size_t owned_documents = 0;
        bool alive = true;
        bool busy = false;
        unsigned restart_count = 0;
    };

    kota::event_loop& loop;
    llvm::SmallVector<WorkerProcess> stateless_workers;
    llvm::SmallVector<WorkerProcess> stateful_workers;

    // Stateful worker routing: path_id -> worker index with LRU tracking
    llvm::DenseMap<std::uint32_t, std::size_t> owner;
    std::list<std::uint32_t> owner_lru;
    llvm::DenseMap<std::uint32_t, std::list<std::uint32_t>::iterator> owner_lru_index;

    std::size_t assign_worker(std::uint32_t path_id);
    void clear_owner(std::size_t worker_index);
    std::size_t pick_least_loaded();

    // Priority-aware stateless scheduling
    struct PendingStateless {
        worker::Priority priority;
        kota::event ready{};
        std::size_t assigned_worker = 0;

        explicit PendingStateless(worker::Priority p) : priority(p) {}
    };

    struct StatelessSlot {
        WorkerPool& pool;
        std::size_t worker_index;

        StatelessSlot(WorkerPool& p, std::size_t idx) : pool(p), worker_index(idx) {}

        StatelessSlot(const StatelessSlot&) = delete;
        StatelessSlot& operator=(const StatelessSlot&) = delete;

        ~StatelessSlot() {
            pool.release_stateless_slot(worker_index);
        }
    };

    std::deque<PendingStateless*> high_queue;
    std::deque<PendingStateless*> low_queue;
    std::size_t stateless_busy_count = 0;
    std::size_t alive_stateless_count = 0;
    std::size_t low_limit = 0;
    std::size_t max_low_limit = 0;

    kota::task<std::size_t> acquire_stateless_slot(worker::Priority priority);
    void release_stateless_slot(std::size_t worker_index);
    void try_dispatch_pending();
    std::size_t pick_idle_stateless();
    kota::task<> monitor_memory();

    /// Handle worker crash: update state, fire on_crash callback.
    /// Returns true if the worker should be restarted.
    bool process_crash(std::size_t index, bool stateful, int exit_code, int exit_signal);

    /// AIMD multiplicative decrease on stateless concurrency limit.
    void apply_crash_backoff();

    bool shutting_down_ = false;
    kota::task_group<> monitor_group{loop};
    kota::task_group<> io_group{loop};
    WorkerPoolOptions options_;
    std::string log_dir_;

    /// Peers moved here during respawn so their coroutines can finish
    /// before the object is destroyed.
    llvm::SmallVector<std::unique_ptr<kota::ipc::BincodePeer>> retired_peers;

    bool spawn_worker(const std::string& self_path, bool stateful, std::uint64_t memory_limit);
    bool respawn_worker(std::size_t index, bool stateful);
    kota::task<> monitor_worker(std::size_t index, bool stateful);

    friend struct testing::WorkerPoolFixture;
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

    // Try up to 2 times: if the worker crashes mid-request, retry on another.
    for(int attempt = 0; attempt < 2; ++attempt) {
        auto idx = co_await acquire_stateless_slot(params.priority);
        StatelessSlot slot(*this, idx);

        if(!stateless_workers[idx].alive)
            continue;

        auto result = co_await stateless_workers[idx].peer->send_request(params, opts);

        // Success, or application-level error (worker still alive) — return as-is.
        if(result.has_value() || stateless_workers[idx].alive)
            co_return std::move(result);

        // Worker crashed during this request — retry on next iteration.
    }
    co_return kota::outcome_error(kota::ipc::Error{"Stateless request failed after retries"});
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
