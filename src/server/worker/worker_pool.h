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

    /// Non-zero when the worker was killed by a signal (e.g. 9 = SIGKILL).
    int exit_signal = 0;

    /// How many times this slot had restarted before this crash.
    unsigned restart_count;

    /// Whether the pool will attempt to respawn this worker.
    bool will_restart;

    /// Stateful only: path_ids of documents owned by the crashed worker.
    /// The on_crash handler should mark these dirty for recompilation.
    llvm::SmallVector<std::uint32_t> lost_documents;
};

struct WorkerPoolOptions {
    std::string self_path;
    std::uint32_t stateless_count = 2;
    std::uint32_t stateful_count = 2;
    std::uint64_t worker_memory_limit = 4ULL * 1024 * 1024 * 1024;  // 4GB default
    std::string log_dir;

    /// Per-worker restart cap before the pool gives up on that slot.
    unsigned max_restarts = 2;
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

        /// Display name for logging, e.g. "SL-0" or "SF-1".
        std::string name;

        /// Stateful only: number of documents routed to this worker.
        std::size_t owned_documents = 0;

        bool alive = true;

        /// Stateless only: true while a request is in-flight on this worker.
        bool busy = false;

        /// How many times this slot has been respawned.
        unsigned restart_count = 0;
    };

    kota::event_loop& loop;
    llvm::SmallVector<WorkerProcess> stateless_workers;
    llvm::SmallVector<WorkerProcess> stateful_workers;

    // Stateful routing: each open document (path_id) is pinned to one worker.
    // LRU tracks access order so stale assignments can be evicted.
    llvm::DenseMap<std::uint32_t, std::size_t> owner;  // path_id -> worker index
    std::list<std::uint32_t> owner_lru;                // most-recent at front
    llvm::DenseMap<std::uint32_t, std::list<std::uint32_t>::iterator> owner_lru_index;

    std::size_t assign_worker(std::uint32_t path_id);
    void clear_owner(std::size_t worker_index);
    std::size_t pick_least_loaded();

    /// A coroutine waiting for a stateless worker slot.  Lives on the coroutine
    /// frame of acquire_stateless_slot(); the destructor auto-removes it from
    /// the queue so cancellation never leaves a dangling pointer.
    struct PendingStateless {
        worker::Priority priority;

        /// Signalled by try_dispatch_pending() when a worker becomes available.
        kota::event ready{};

        /// Set by try_dispatch_pending() before signalling ready.
        /// SIZE_MAX means the pool is dead and no worker was assigned.
        std::size_t assigned_worker = 0;

        /// Points to whichever queue (high/low) this entry sits in; nullptr
        /// once popped or if never enqueued.
        std::deque<PendingStateless*>* queue = nullptr;

        explicit PendingStateless(worker::Priority p) : priority(p) {}

        PendingStateless(const PendingStateless&) = delete;
        PendingStateless& operator=(const PendingStateless&) = delete;

        ~PendingStateless() {
            if(queue)
                std::erase(*queue, this);
        }
    };

    /// RAII guard that releases a stateless worker slot on scope exit.
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

    /// Pending requests waiting for a worker, split by priority.
    /// High queue is drained first; low queue respects low_limit.
    std::deque<PendingStateless*> high_queue;
    std::deque<PendingStateless*> low_queue;

    std::size_t stateless_busy_count = 0;
    std::size_t alive_stateless_count = 0;

    /// Max concurrent low-priority tasks.  Dynamically adjusted by
    /// monitor_memory() and apply_crash_backoff().
    std::size_t low_limit = 0;

    /// Ceiling for low_limit recovery (set once at start).
    std::size_t max_low_limit = 0;

    /// Remaining monitor_memory cycles to skip after a crash backoff,
    /// prevents crash AIMD and memory pressure from compounding.
    unsigned backoff_cooldown = 0;

    /// Wait for an idle stateless worker. Returns SIZE_MAX when the pool
    /// is dead (all workers down, no restarts left).
    kota::task<std::size_t> acquire_stateless_slot(worker::Priority priority);
    void release_stateless_slot(std::size_t worker_index);

    /// Wake queued requests when a worker becomes available.
    void try_dispatch_pending();

    /// Wake all queued requests with SIZE_MAX (pool is dead).
    void fail_pending_requests();

    std::size_t pick_idle_stateless();

    /// Periodically adjusts low_limit based on system memory pressure.
    kota::task<> monitor_memory();

    /// Handle worker crash: update state, fire on_crash callback.
    /// Returns true if the worker should be restarted.
    bool process_crash(std::size_t index, bool stateful, int exit_code, int exit_signal);

    /// AIMD multiplicative decrease on stateless concurrency limit.
    void apply_crash_backoff();

    bool shutting_down_ = false;

    /// Runs monitor_worker() and monitor_memory() coroutines.
    kota::task_group<> monitor_group{loop};

    /// Runs peer->run() and drain_stderr() coroutines.
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

    // Retry once: if the worker crashes mid-request, acquire a fresh slot.
    for(int attempt = 0; attempt < 2; ++attempt) {
        auto idx = co_await acquire_stateless_slot(params.priority);
        if(idx >= stateless_workers.size())
            co_return kota::outcome_error(kota::ipc::Error{"All stateless workers are down"});

        StatelessSlot slot(*this, idx);

        if(!stateless_workers[idx].alive)
            continue;

        // Snapshot restart_count to detect crash-and-respawn on the same slot:
        // respawn_worker() reuses the index, so checking alive alone would
        // see the NEW worker as alive and return the stale IPC error.
        auto gen = stateless_workers[idx].restart_count;
        auto result = co_await stateless_workers[idx].peer->send_request(params, opts);

        if(result.has_value() || stateless_workers[idx].restart_count == gen)
            co_return std::move(result);
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
