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

    /// Dynamic scaling bounds for stateless workers.
    /// min_stateless: floor — never retire below this count.
    /// max_stateless: ceiling — never spawn above this count (0 = auto = CPU cores).
    std::uint32_t min_stateless = 1;
    std::uint32_t max_stateless = 0;
};

class WorkerPool {
public:
    WorkerPool(kota::event_loop& loop) : loop(loop) {}

    /// Spawn all worker processes. Returns false on failure.
    bool start(const WorkerPoolOptions& opts);

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
    /// The master translates the path to a path_id and calls remove_owner() so the
    /// owner table shrinks together with the worker's document set.
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

        /// Stateless only: true if the current in-flight request is low-priority.
        bool low_priority = false;

        /// How many times this slot has been respawned.
        unsigned restart_count = 0;

        /// True when this worker is being intentionally shut down (scale-down),
        /// as opposed to an unexpected crash.
        bool retiring = false;
    };

    kota::event_loop& loop;
    llvm::SmallVector<WorkerProcess> stateless_workers;
    llvm::SmallVector<WorkerProcess> stateful_workers;

    // Stateful routing: each open document (path_id) is pinned to one worker.
    llvm::DenseMap<std::uint32_t, std::size_t> owner;  // path_id -> worker index

    std::size_t assign_worker(std::uint32_t path_id);
    void clear_owner(std::size_t worker_index);
    std::size_t pick_least_loaded();

    /// A coroutine waiting for a stateless worker slot.  Lives on the coroutine
    /// frame of acquire_stateless_slot(); the destructor handles two cancellation
    /// scenarios:
    ///   - Still queued (queue != nullptr): removes itself from the queue.
    ///   - Dispatched but coroutine cancelled before StatelessSlot was created
    ///     (pool != nullptr): releases the assigned slot to prevent leak.
    struct PendingStateless {
        worker::Priority priority;

        /// Signalled by try_dispatch_pending() when a worker becomes available.
        kota::event ready{};

        /// Set by try_dispatch_pending() before signalling ready.
        /// SIZE_MAX means the pool is dead and no worker was assigned.
        std::size_t assigned_worker = 0;

        /// restart_count of the assigned worker at dispatch time.
        unsigned assigned_gen = 0;

        /// Points to whichever queue (high/low) this entry sits in; nullptr
        /// once popped or if never enqueued.
        std::deque<PendingStateless*>* queue = nullptr;

        /// Non-null while the slot hasn't been claimed by the coroutine.
        /// Cleared after co_await returns so the destructor doesn't release
        /// a slot that StatelessSlot now owns.
        WorkerPool* pool = nullptr;

        explicit PendingStateless(worker::Priority p) : priority(p) {}

        PendingStateless(const PendingStateless&) = delete;
        PendingStateless& operator=(const PendingStateless&) = delete;

        ~PendingStateless() {
            if(queue) {
                std::erase(*queue, this);
            } else if(assigned_worker != SIZE_MAX && pool) {
                // Only release if the slot hasn't been crash-replaced.
                if(pool->stateless_workers[assigned_worker].restart_count == assigned_gen)
                    pool->release_stateless_slot(assigned_worker);
            }
        }
    };

    /// RAII guard that releases a stateless worker slot on scope exit.
    /// Captures restart_count at construction so that a crash-and-respawn
    /// on the same index won't accidentally clear the new occupant's busy flag.
    struct StatelessSlot {
        WorkerPool& pool;
        std::size_t worker_index;
        unsigned gen;

        StatelessSlot(WorkerPool& p, std::size_t idx) :
            pool(p), worker_index(idx), gen(p.stateless_workers[idx].restart_count) {}

        StatelessSlot(const StatelessSlot&) = delete;
        StatelessSlot& operator=(const StatelessSlot&) = delete;

        ~StatelessSlot() {
            if(pool.stateless_workers[worker_index].restart_count == gen)
                pool.release_stateless_slot(worker_index);
        }
    };

    /// Pending requests waiting for a worker, split by priority.
    /// High queue is drained first; low queue respects low_limit.
    std::deque<PendingStateless*> high_queue;
    std::deque<PendingStateless*> low_queue;

    std::size_t stateless_busy_count = 0;
    std::size_t low_busy_count = 0;
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
    /// @param exclude  worker index to skip (e.g. a peer that just failed
    ///                 but whose crash hasn't been processed yet).
    kota::task<std::size_t> acquire_stateless_slot(worker::Priority priority,
                                                   std::size_t exclude = SIZE_MAX);
    void release_stateless_slot(std::size_t worker_index);

    /// Wake queued requests when a worker becomes available.
    void try_dispatch_pending();

    /// Wake all queued requests with SIZE_MAX (pool is dead).
    void fail_pending_requests();

    std::size_t pick_idle_stateless(std::size_t exclude = SIZE_MAX);

    /// Periodically adjusts low_limit based on system memory pressure
    /// and triggers dynamic scaling checks.
    kota::task<> monitor_memory();

    /// SIGKILL any worker still alive after the SIGTERM grace period, so a
    /// wedged worker can't block stop()'s join forever.
    kota::task<> kill_stragglers();

    /// Handle worker crash: update state, fire on_crash callback.
    /// Returns true if the worker should be restarted.
    bool process_crash(std::size_t index, bool stateful, int exit_code, int exit_signal);

    /// AIMD multiplicative decrease on stateless concurrency limit.
    void apply_crash_backoff();

    // --- Dynamic scaling ---

    /// Spawn an additional stateless worker. Returns true on success.
    bool scale_up_worker();

    /// Find the highest-index idle worker above min_stateless, mark it
    /// retiring, and close its output pipe + send SIGTERM.
    void retire_idle_worker();

    /// Evaluate scaling conditions and act (called from monitor_memory).
    void check_scaling();

    /// Cancel up to `count` in-flight low-priority requests to relieve
    /// memory pressure.
    void cancel_low_priority_requests(std::size_t count);

    /// CUBIC-style fast recovery target: last low_limit before a reduction.
    std::size_t w_max = 0;

    /// Consecutive monitor ticks where all workers were busy with queued work.
    unsigned saturated_cycles = 0;

    /// Consecutive monitor ticks where workers were idle with no queued work.
    unsigned idle_cycles = 0;

    constexpr static unsigned scale_up_ticks = 5;
    constexpr static unsigned scale_down_ticks = 10;

    /// Per-slot cancellation source for in-flight low-priority requests.
    /// Indexed by stateless worker index; null when no low-priority request
    /// is in flight.
    llvm::SmallVector<std::shared_ptr<kota::cancellation_source>> slot_cancel_sources;

    /// Cancelled by stop(). Unwinds monitor_memory() at its poll sleep
    /// instead of waiting out the 3s interval, and lets
    /// monitor_worker() attribute the SIGTERM-induced exits that follow to
    /// intentional shutdown (no anomaly, no respawn).
    kota::cancellation_source stop_scope;

    /// All long-lived pool coroutines: monitor_worker() exit observers, the
    /// peer->run() / drain_stderr() IO pumps, and the wrapped memory poller.
    /// Never cancelled as a group — stop() joins it, so shutdown waits until
    /// every worker process actually exited and its final output (crash
    /// stacktraces, sanitizer reports) was drained to EOF.
    kota::task_group<> worker_tasks{loop};
    WorkerPoolOptions options;
    std::string log_dir;

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
        co_return kota::outcome_error(kota::ipc::Error{worker::dispatch_errc::worker_unavailable,
                                                       "No stateful workers available"});
    }
    auto idx = assign_worker(path_id);
    if(!stateful_workers[idx].alive) {
        co_return kota::outcome_error(kota::ipc::Error{worker::dispatch_errc::worker_unavailable,
                                                       "Assigned stateful worker is down"});
    }
    co_return co_await stateful_workers[idx].peer->send_request(params, opts);
}

template <typename Params>
RequestResult<Params> WorkerPool::send_stateless(const Params& params,
                                                 kota::ipc::request_options opts) {
    if(stateless_workers.empty()) {
        co_return kota::outcome_error(kota::ipc::Error{worker::dispatch_errc::worker_unavailable,
                                                       "No stateless workers available"});
    }

    // Retry once on transport error, excluding the failed peer so the
    // retry doesn't hit the same dead worker before process_crash runs.
    std::size_t exclude = SIZE_MAX;
    for(int attempt = 0; attempt < 2; ++attempt) {
        auto idx = co_await acquire_stateless_slot(params.priority, exclude);
        if(idx >= stateless_workers.size())
            co_return kota::outcome_error(
                kota::ipc::Error{worker::dispatch_errc::worker_unavailable,
                                 "All stateless workers are down"});

        StatelessSlot slot(*this, idx);

        if(!stateless_workers[idx].alive)
            continue;

        // For low-priority requests, install a cancellation source so
        // monitor_memory() can preempt under severe memory pressure.
        // FIXME: cancelling only aborts the IPC wait — the worker process
        // continues compiling until it finishes. The slot is released here
        // but the worker won't accept new work until the in-flight RPC
        // completes, so the next request dispatched to it queues at the
        // IPC layer. Not a correctness bug, but limits preemption efficacy.
        std::shared_ptr<kota::cancellation_source> preempt_src;
        if(params.priority == worker::Priority::Low) {
            preempt_src = std::make_shared<kota::cancellation_source>();
            if(idx < slot_cancel_sources.size())
                slot_cancel_sources[idx] = preempt_src;
            if(!opts.token)
                opts.token = preempt_src->token();
        }

        auto result = co_await stateless_workers[idx].peer->send_request(params, opts);

        if(result.has_value())
            co_return std::move(result);

        // If deliberately cancelled by memory pressure, don't retry.
        if(preempt_src && preempt_src->cancelled())
            co_return kota::outcome_error(
                kota::ipc::Error{worker::dispatch_errc::cancelled,
                                 "Request cancelled due to memory pressure"});

        // Transport error — retry on a different worker.
        exclude = idx;
    }
    co_return kota::outcome_error(kota::ipc::Error{worker::dispatch_errc::worker_unavailable,
                                                   "Stateless request failed after retries"});
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
