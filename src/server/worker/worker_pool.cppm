module;

export module clice.worker:worker_pool;

import stdlib;
import llvm;
import kota;
import clice.protocol;
import clice.support;

export namespace clice::testing {

struct WorkerPoolFixture;

}

export namespace clice {

using kota::ipc::RequestResult;

/// Information about a worker crash, delivered via WorkerPool::on_crash.
struct WorkerCrashInfo {
    std::size_t worker_index;
    bool stateful;
    int exit_code = 0;

    /// Non-zero when the worker was killed by a signal (e.g. 9 = SIGKILL).
    int exit_signal = 0;

    /// Consecutive fast crashes of this slot, including this one. Resets
    /// after a healthy uptime, so only genuine crash loops accumulate.
    unsigned crash_streak = 0;

    /// Whether the pool will attempt to respawn this worker.
    bool will_restart = false;

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

    /// A slot is given up once it crashes more than this many times in a
    /// row. The streak resets after `healthy_uptime` of stable operation,
    /// so the budget bounds crash loops, not lifetime bad luck.
    unsigned max_crash_streak = 3;

    /// Base delay for exponential respawn backoff. The first crash of a
    /// streak respawns immediately; each further one doubles the delay,
    /// capped at 16x the base.
    std::chrono::milliseconds respawn_backoff{500};

    /// Uptime after which a worker is considered healthy and its crash
    /// streak resets.
    std::chrono::milliseconds healthy_uptime{30'000};

    /// Cooldown before a slot that gave up is revived with a fresh crash
    /// budget. The pool must never stay at zero workers forever: document
    /// quarantine isolates the poison, and this bounds how long a fully
    /// burnt pool stays dark. 0 disables revival.
    std::chrono::milliseconds revive_after{30'000};

    /// Dynamic scaling bounds for stateless workers.
    /// min_stateless: floor — never retire below this count.
    /// max_stateless: ceiling — never spawn above this count (0 = auto = CPU cores).
    std::uint32_t min_stateless = 1;
    std::uint32_t max_stateless = 0;
};

/// How much the caller distrusts a stateful dispatch (see the class doc's
/// responsibility contract). Every flavor of distrust exempts the slot's
/// crash budget; they differ in routing.
enum class Suspect : std::uint8_t {
    /// Ordinary work.
    No,
    /// A quarantined document's probe compile: runs only on a worker
    /// hosting no other document, so a crash takes nothing healthy along.
    Isolated,
    /// A quarantined document's recovery query: it must reach the worker
    /// holding the AST, so it keeps owner routing; while it flies, the
    /// worker is avoided by new-document assignment.
    InPlace,
};

/// Multi-process scheduler for clice worker processes.
///
/// Two kinds of workers are managed:
///   - Stateless workers execute independent build tasks (PCH/PCM builds,
///     indexing, completion, formatting) with two-level priority scheduling:
///     one worker's worth of capacity is reserved for high-priority requests
///     by capping low-priority concurrency at capacity - 1, and the cap
///     shrinks further under memory pressure.
///   - Stateful workers keep per-document ASTs; each open document is pinned
///     to one worker (path_id affinity), balanced by document count.
///
/// Responsibility contract — the pool is mechanism, callers are policy:
///
///   - The pool owns PROCESSES and CAPACITY: spawn, monitor, respawn with
///     backoff, per-slot crash budget (streak, healthy-uptime reset),
///     give-up -> cooldown revival, scaling between min/max, preemption
///     under memory pressure, scheduling (priority, affinity, probe
///     isolation). Its guarantee: capacity is never permanently zero.
///   - The pool NEVER retries a request. Requests do not survive a crash;
///     slots do. Retry policy is semantic and lives with the caller: the
///     compiler resends idempotent builds once, the indexer requeues with
///     its own budget, a stateful compile never resends (the crash is
///     evidence about the content — see state/quarantine.h).
///   - The dispatch_errc taxonomy is the contract language. worker_crashed:
///     the request died with its worker — the caller may blame its content
///     (Error::data carries the dead incarnation's identity so one death is
///     never blamed twice). worker_restarting: never dispatched, blameless.
///     worker_unavailable: a capacity window — retryable later when
///     revives_slots(). cancelled: deliberate preemption, requeue freely.
///   - Suspect is the single sanctioned policy hint INTO the pool: the
///     caller already distrusts the workload, so its crash spends no slot
///     budget. An Isolated probe additionally runs only where it can take
///     no healthy document with it; an InPlace recovery query keeps owner
///     routing (the AST lives there) and is merely avoided by new-document
///     assignment while it flies.
class WorkerPool {
public:
    WorkerPool(kota::event_loop& loop) : loop(loop) {}

    /// Spawn all worker processes. Returns false on failure.
    bool start(const WorkerPoolOptions& opts);

    /// Gracefully stop all workers.
    kota::task<> stop();

    /// Send a request to a stateful worker with path_id affinity routing.
    /// A suspect dispatch's crash does not spend the slot's budget — the
    /// failure says something about the document, not the slot; see
    /// Suspect for the routing difference between its flavors.
    template <typename Params>
    RequestResult<Params> send_stateful(std::uint32_t path_id,
                                        const Params& params,
                                        kota::ipc::request_options opts = {},
                                        Suspect suspect = Suspect::No);

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

    /// Remove path_id from ownership only if worker_index is its current
    /// owner. Returns whether it was removed — false means the eviction
    /// came from a stale copy on a worker that lost ownership (probe
    /// reassignment) and the current owner's state is untouched.
    bool remove_owner_from(std::uint32_t path_id, std::size_t worker_index);

    /// True when a Dead slot is not final: the running pool revives dead
    /// slots after a cooldown, so "no capacity" is a window, not a verdict.
    /// Callers (the indexer's requeue) may retry work that failed with
    /// worker_unavailable instead of dropping it. Unit fixtures drive slot
    /// state without an event loop and never start the pool, so revival
    /// stays off for them.
    bool revives_slots() const {
        return started && options.revive_after.count() > 0;
    }

    /// Callback invoked when a worker process crashes.
    std::function<void(const WorkerCrashInfo&)> on_crash;

    /// Callback invoked when a stateful worker sends an EvictedParams
    /// notification, with the slot index of the evicting worker. The master
    /// translates the path to a path_id and calls remove_owner_from() so an
    /// eviction of a stale copy — a quarantine probe moved ownership to
    /// another worker while the old one kept its document entry — cannot
    /// unseat the current owner.
    std::function<void(const std::string& path, std::size_t worker_index)> on_evicted;

private:
    /// Lifecycle of a worker slot. The generation counter is bumped on every
    /// transition out of Alive, so stale references (an in-flight request's
    /// slot guard, a dispatched-but-cancelled waiter) can detect that their
    /// claim no longer describes the current occupant.
    enum class SlotState : std::uint8_t {
        /// Process running and accepting requests.
        Alive,
        /// Process declared dead (transport failure, preemption kill, or
        /// observed exit); monitor_worker has not delivered its verdict yet.
        Dying,
        /// A respawn is scheduled, possibly sleeping out a backoff delay.
        Respawning,
        /// Intentionally scaled down; the slot stays vacant.
        Retired,
        /// Crash budget exhausted; the slot stays vacant.
        Dead,
    };

    struct WorkerProcess {
        kota::process proc;

        /// Shared with every coroutine that awaits on it (request senders,
        /// the IO pump), so a slot can drop its reference the moment the
        /// process dies without racing in-flight users.
        std::shared_ptr<kota::ipc::BincodePeer> peer;

        /// Display name for logging, e.g. "SL-0" or "SF-1".
        std::string name;

        SlotState state = SlotState::Alive;

        /// Bumped on every death; see SlotState.
        unsigned generation = 0;

        /// Stateful only: number of documents routed to this worker.
        std::size_t owned_documents = 0;

        /// Stateless only: true while a request is in-flight on this worker.
        bool busy = false;

        /// Stateless only: true if the current in-flight request is low-priority.
        bool low_priority = false;

        /// True when this worker is being intentionally shut down (scale-down),
        /// as opposed to an unexpected crash.
        bool retiring = false;

        /// True when the pool killed this worker on purpose to relieve
        /// memory pressure: respawn immediately, without crash accounting.
        bool preempted = false;

        /// Consecutive fast crashes; resets after healthy uptime.
        unsigned crash_streak = 0;

        /// In-flight requests whose caller flagged them as suspect (a
        /// quarantined document's probe). While non-zero, a crash does not
        /// spend the slot's budget — see send_stateful.
        unsigned suspect_inflight = 0;

        std::chrono::steady_clock::time_point spawn_time{};

        /// Stateless only: cancels the in-flight low-priority request so its
        /// sender observes preemption instead of a bare transport error.
        std::shared_ptr<kota::cancellation_source> preempt_source;
    };

    kota::event_loop& loop;
    llvm::SmallVector<WorkerProcess> stateless_workers;
    llvm::SmallVector<WorkerProcess> stateful_workers;

    // Stateful routing: each open document (path_id) is pinned to one worker.
    llvm::DenseMap<std::uint32_t, std::size_t> owner;  // path_id -> worker index

    /// Returns the worker owning path_id, assigning the least-loaded live
    /// worker on first use. the max-size_t sentinel when no stateful worker is alive.
    std::size_t assign_worker(std::uint32_t path_id);
    void clear_owner(std::size_t worker_index);
    std::size_t pick_least_loaded();

    /// A coroutine waiting for a stateless worker slot. Lives on the frame of
    /// acquire_stateless_slot(). Dispatch claims a worker on the waiter's
    /// behalf before waking it; the destructor covers cancellation:
    ///   - Still queued: removes itself from the queue.
    ///   - Claimed but cancelled before the coroutine consumed the claim:
    ///     releases the slot (unless the worker died in between — then the
    ///     generation mismatch shows the claim was already cleaned up).
    struct PendingStateless {
        WorkerPool& pool;
        worker::Priority priority;

        /// Signalled by try_dispatch_pending().
        kota::event ready{};

        /// Set to the claimed worker before signalling ready; stays the max-size_t sentinel
        /// when the waiter is woken only to observe pool death or stop.
        std::size_t assigned_worker = std::numeric_limits<std::size_t>::max();

        /// Generation of the claimed worker at dispatch time.
        unsigned assigned_gen = 0;

        /// Points to whichever queue (high/low) this entry sits in; nullptr
        /// once popped.
        std::deque<PendingStateless*>* queue = nullptr;

        PendingStateless(WorkerPool& pool, worker::Priority priority) :
            pool(pool), priority(priority) {}

        PendingStateless(const PendingStateless&) = delete;
        PendingStateless& operator=(const PendingStateless&) = delete;

        ~PendingStateless() {
            if(queue) {
                std::erase(*queue, this);
            } else if(assigned_worker != std::numeric_limits<std::size_t>::max() &&
                      pool.stateless_workers[assigned_worker].generation == assigned_gen) {
                pool.release_stateless_slot(assigned_worker);
            }
        }
    };

    /// RAII guard that releases a stateless worker slot on scope exit.
    /// Captures the generation so a death-and-respawn on the same index
    /// won't accidentally clear the new occupant's busy flag.
    struct StatelessSlot {
        WorkerPool& pool;
        std::size_t worker_index;
        unsigned gen;

        StatelessSlot(WorkerPool& p, std::size_t idx) :
            pool(p), worker_index(idx), gen(p.stateless_workers[idx].generation) {}

        StatelessSlot(const StatelessSlot&) = delete;
        StatelessSlot& operator=(const StatelessSlot&) = delete;

        ~StatelessSlot() {
            if(pool.stateless_workers[worker_index].generation == gen)
                pool.release_stateless_slot(worker_index);
        }
    };

    /// Pending requests waiting for a worker, split by priority.
    /// High queue is drained first; low queue respects low_limit.
    std::deque<PendingStateless*> high_queue;
    std::deque<PendingStateless*> low_queue;

    /// Max concurrent low-priority tasks, adjusted by tick_memory() and
    /// apply_crash_backoff(). Reads go through effective_low_limit(), which
    /// clamps to the capacity-derived ceiling.
    std::size_t low_limit = 0;

    /// Remaining tick_memory cycles to skip after a crash backoff,
    /// prevents crash AIMD and memory pressure from compounding.
    unsigned backoff_cooldown = 0;

    // All occupancy numbers are derived from slot state on demand instead of
    // being maintained as counters — with at most a few dozen slots the scans
    // are trivial, and there is no incremental bookkeeping to drift.

    /// Stateless slots that can serve requests now.
    std::size_t alive_stateless() const;

    /// Alive stateless slots that also accept new work: a retiring slot
    /// stays alive to finish its request but is skipped by dispatch.
    std::size_t schedulable_stateless() const;

    /// Alive stateless slots with a request in flight.
    std::size_t busy_stateless() const;

    /// Alive stateless slots busy with low-priority work.
    std::size_t low_busy_count() const;

    /// Stateless slots that are not permanently vacant (Dead/Retired):
    /// alive ones plus those dying or awaiting respawn.
    std::size_t stateless_capacity() const;

    /// Stateless slots still holding a process: stateless_capacity() plus
    /// retiring workers, which keep theirs until the monitor reaps them.
    std::size_t stateless_footprint() const;

    /// True while at least one stateless slot can still (eventually) serve.
    bool has_future_capacity() const {
        return stateless_capacity() > 0;
    }

    /// Reserve one worker for high-priority requests by capping low-priority
    /// concurrency at one below the slots that can be scheduled right now.
    /// A slot dying, awaiting respawn, or retiring cannot take new work;
    /// counting it would let low-priority requests occupy every schedulable
    /// worker. With a single schedulable worker this collapses to 1: both
    /// priorities share it, and high wins only via queue ordering.
    std::size_t max_low_limit() const {
        auto live = schedulable_stateless();
        return live > 1 ? live - 1 : live;
    }

    std::size_t effective_low_limit() const {
        return std::min(low_limit, max_low_limit());
    }

    /// Wait for an idle stateless worker. Returns the max-size_t sentinel when no slot can
    /// serve the request anymore (pool stopped or all slots given up).
    kota::task<std::size_t> acquire_stateless_slot(worker::Priority priority);
    void release_stateless_slot(std::size_t worker_index);

    /// Mark a claimed worker busy. Returns the index for chaining.
    std::size_t claim_stateless(std::size_t index, worker::Priority priority);

    /// Wake queued requests when a worker becomes available; drains the
    /// queues with a failure signal when no capacity remains.
    void try_dispatch_pending();

    /// Wake all queued requests without a claim so they observe pool death.
    void fail_pending_requests();

    std::size_t pick_idle_stateless();

    /// Declare a worker's process dead right now: bump the generation, drop
    /// the busy claim and the peer reference. Idempotent — safe to call from
    /// a failed send before the monitor observed the exit. `kill_process`
    /// must be true when death is declared ahead of the verdict (failed
    /// send, preemption) so monitor_worker's proc.wait() is guaranteed to
    /// deliver; the monitor itself passes false — its process was already
    /// reaped, and signalling the freed pid could hit an unrelated process.
    void mark_worker_dead(std::size_t index, bool stateful, bool kill_process);

    /// Handle a crash verdict: log/relay, update crash accounting, fire
    /// on_crash. Returns true if the worker should be respawned; on false the
    /// slot has been given up.
    bool process_crash(std::size_t index, bool stateful, int exit_code, int exit_signal);

    /// Respawn a slot after `delay`, retrying with growing backoff if the
    /// spawn itself fails; gives the slot up once the streak is exhausted.
    kota::task<> respawn_after(std::size_t index, bool stateful, std::chrono::milliseconds delay);

    /// Backoff before the respawn for the given crash streak: immediate for
    /// the first crash, exponential from the second on.
    std::chrono::milliseconds backoff_delay(unsigned crash_streak) const;

    /// A worker that ran healthily for a while starts a fresh streak: the
    /// budget bounds crash loops, not lifetime bad luck. Called on crash
    /// accounting and on preemption — a preempted slot must not carry a
    /// stale streak past the healthy interval that would have cleared it.
    void reset_streak_if_healthy(WorkerProcess& w);

    /// Permanently vacate a slot and fail waiters if it was the last one.
    void give_up_slot(std::size_t index, bool stateful);

    /// AIMD multiplicative decrease on stateless concurrency limit.
    void apply_crash_backoff();

    /// 3s tick driving the two controllers below.
    kota::task<> monitor_loop();

    /// Adjusts low_limit from memory pressure (AIMD down, CUBIC-style fast
    /// recovery up) and preempts running low-priority work when severe.
    void tick_memory(double available_ratio);

    /// Scales the stateless pool up/down from saturation/idle streaks.
    void tick_scaling(double available_ratio);

    /// SIGKILL any worker still alive after the SIGTERM grace period, so a
    /// wedged worker can't block stop()'s join forever.
    kota::task<> kill_stragglers();

    // --- Dynamic scaling ---

    /// Spawn an additional stateless worker. Returns true on success.
    bool scale_up_worker();

    /// Find the highest-index idle worker above min_stateless, mark it
    /// retiring, and close its output pipe + send SIGTERM.
    void retire_idle_worker();

    /// Kill up to `count` in-flight low-priority workers to relieve memory
    /// pressure. Their requests fail with dispatch_errc::cancelled and the
    /// processes respawn immediately without crash accounting.
    void preempt_low_priority(std::size_t count);

    /// CUBIC-style fast recovery target: last low_limit before a reduction.
    std::size_t w_max = 0;

    /// Consecutive monitor ticks where all workers were busy with queued work.
    unsigned saturated_cycles = 0;

    /// Consecutive monitor ticks where workers were idle with no queued work.
    unsigned idle_cycles = 0;

    constexpr static unsigned scale_up_ticks = 5;
    constexpr static unsigned scale_down_ticks = 10;

    /// Cancelled by stop(). Unwinds monitor_loop() and respawn backoff sleeps
    /// at their poll points, and lets monitor_worker() attribute the
    /// SIGTERM-induced exits that follow to intentional shutdown (no anomaly,
    /// no respawn).
    kota::cancellation_source stop_scope;

    /// All long-lived pool coroutines: monitor_worker() exit observers, the
    /// peer IO pumps and stderr drains, respawn tasks, and the monitor loop.
    /// Never cancelled as a group — stop() joins it, so shutdown waits until
    /// every worker process actually exited and its final output (crash
    /// stacktraces, sanitizer reports) was drained to EOF.
    kota::task_group<> worker_tasks{loop};
    WorkerPoolOptions options;

    /// Set once start() succeeded: gates background concerns (slot
    /// revival) that need a running event loop.
    bool started = false;
    std::string log_dir;

    struct SpawnedProcess {
        kota::process proc;
        std::shared_ptr<kota::ipc::BincodePeer> peer;
    };

    /// Launch a worker process and start its IO pumps. Shared by initial
    /// spawn, scale-up, and respawn.
    std::optional<SpawnedProcess> spawn_process(const std::string& name, bool stateful);

    /// Append a new slot (initial start and scale-up).
    bool spawn_worker(bool stateful);

    /// Refill an existing slot with a fresh process.
    bool respawn_worker(std::size_t index, bool stateful);

    /// After `revive_after`, grant a given-up slot a fresh crash budget
    /// and respawn it: the pool never stays at zero workers forever.
    kota::task<> revive_slot(std::size_t index, bool stateful);

    /// A stateful worker that may be sacrificed to a suspect compile:
    /// alive and hosting no document other than `path_id` itself, which is
    /// reassigned to it. the max-size_t sentinel when every live worker hosts others.
    std::size_t assign_expendable(std::uint32_t path_id);

    void install_evict_handler(WorkerProcess& worker, std::size_t index);

    kota::task<> monitor_worker(std::size_t index, bool stateful);

    friend struct testing::WorkerPoolFixture;
};

template <typename Params>
RequestResult<Params> WorkerPool::send_stateful(std::uint32_t path_id,
                                                const Params& params,
                                                kota::ipc::request_options opts,
                                                Suspect suspect) {
    // An isolated probe only runs on a worker hosting no other document:
    // its crash must never take healthy sessions with it. With no such
    // worker available right now, the caller keeps the probe armed and
    // tries again later instead of risking one. An in-place suspect stays
    // with the owner — the AST it queries lives there.
    auto idx = suspect == Suspect::Isolated ? assign_expendable(path_id) : assign_worker(path_id);
    if(idx == std::numeric_limits<std::size_t>::max()) {
        co_return kota::outcome_error(kota::ipc::Error{
            worker::dispatch_errc::worker_unavailable,
            suspect == Suspect::Isolated ? "No expendable stateful worker for quarantined probe"
                                         : "No stateful workers available"});
    }

    auto& assigned = stateful_workers[idx];
    if(assigned.state == SlotState::Dying || assigned.state == SlotState::Respawning) {
        co_return kota::outcome_error(kota::ipc::Error{worker::dispatch_errc::worker_restarting,
                                                       "Assigned stateful worker is restarting"});
    }
    if(assigned.state != SlotState::Alive) {
        co_return kota::outcome_error(kota::ipc::Error{worker::dispatch_errc::worker_unavailable,
                                                       "Assigned stateful worker is down"});
    }

    // Own a peer reference across the await: the slot drops its copy the
    // moment the worker dies.
    auto peer = assigned.peer;
    auto gen = assigned.generation;

    // RAII unwind: a cancellation that unwinds the frame mid-await must
    // not leave the worker marked as hosting suspect work forever. On
    // transport death the guard is disarmed instead — the monitor's crash
    // accounting consumes the count — and the generation check skips
    // incarnations already torn down elsewhere.
    struct SuspectGuard {
        WorkerPool& pool;
        std::size_t idx;
        unsigned gen;
        bool armed;

        ~SuspectGuard() {
            if(!armed) {
                return;
            }
            auto& w = pool.stateful_workers[idx];
            if(w.generation == gen && w.suspect_inflight > 0) {
                w.suspect_inflight -= 1;
            }
        }
    };

    if(suspect != Suspect::No) {
        assigned.suspect_inflight += 1;
    }
    SuspectGuard suspect_guard{*this, idx, gen, suspect != Suspect::No};
    auto result = co_await peer->send_request(params, opts);
    bool transport_dead = !result.has_value() && worker::is_transport_error(result.error());
    if(transport_dead) {
        suspect_guard.armed = false;
    }

    if(result.has_value() || !worker::is_transport_error(result.error()))
        co_return std::move(result);

    // The worker link broke mid-request. Declare the slot dead now so
    // follow-up requests fail fast instead of piling onto a corpse;
    // monitor_worker reconciles with the real exit status.
    if(stateful_workers[idx].generation == gen)
        mark_worker_dead(idx, true, true);
    co_return kota::outcome_error(
        kota::ipc::Error{worker::dispatch_errc::worker_crashed,
                         "Stateful worker died during request: " + result.error().message,
                         worker::death_identity(idx, gen, true)});
}

template <typename Params>
RequestResult<Params> WorkerPool::send_stateless(const Params& params,
                                                 kota::ipc::request_options opts) {
    auto idx = co_await acquire_stateless_slot(params.priority);
    if(idx == std::numeric_limits<std::size_t>::max()) {
        co_return kota::outcome_error(kota::ipc::Error{worker::dispatch_errc::worker_unavailable,
                                                       "No stateless workers available"});
    }

    StatelessSlot slot(*this, idx);
    auto peer = stateless_workers[idx].peer;
    auto gen = stateless_workers[idx].generation;

    // For low-priority requests, install a cancellation source so
    // preempt_low_priority() can signal the preemption to this sender
    // before the kill's transport error arrives.
    std::shared_ptr<kota::cancellation_source> preempt_src;
    if(params.priority == worker::Priority::Low) {
        preempt_src = std::make_shared<kota::cancellation_source>();
        stateless_workers[idx].preempt_source = preempt_src;
        if(!opts.token)
            opts.token = preempt_src->token();
    }

    auto result = co_await peer->send_request(params, opts);
    if(result.has_value())
        co_return std::move(result);

    if(preempt_src && preempt_src->cancelled())
        co_return kota::outcome_error(kota::ipc::Error{worker::dispatch_errc::cancelled,
                                                       "Request preempted under memory pressure"});

    // An error returned by the worker's handler leaves the worker healthy;
    // pass it through untouched.
    if(!worker::is_transport_error(result.error()))
        co_return std::move(result);

    // The worker link broke mid-request: declare the slot dead now so a
    // caller-side retry cannot land on the same corpse before the monitor
    // observed the exit.
    if(stateless_workers[idx].generation == gen) {
        mark_worker_dead(idx, false, true);
        // The dead worker's claim is gone; a queued low-priority waiter may
        // now fit under the limit on another idle worker.
        try_dispatch_pending();
    }
    co_return kota::outcome_error(
        kota::ipc::Error{worker::dispatch_errc::worker_crashed,
                         "Stateless worker died during request: " + result.error().message,
                         worker::death_identity(idx, gen, false)});
}

template <typename Params>
void WorkerPool::notify_stateful(std::uint32_t path_id, const Params& params) {
    auto it = owner.find(path_id);
    if(it == owner.end())
        return;
    auto& assigned = stateful_workers[it->second];
    if(assigned.state != SlotState::Alive)
        return;
    assigned.peer->send_notification(params);
}

}  // namespace clice

#ifdef CLICE_ENABLE_TEST

/// Defined here (attached to clice.worker) rather than in the test TU: with
/// the module split, a definition in the test executable would attach to a
/// different module and no longer be the class the friend declaration above
/// names.
export namespace clice::testing {

/// Test fixture with friend access to WorkerPool internals.
struct WorkerPoolFixture {
    using SlotState = WorkerPool::SlotState;

    kota::event_loop loop;
    WorkerPool pool;

    std::vector<WorkerCrashInfo> crash_reports;

    WorkerPoolFixture() : pool(loop) {
        logging::set_anomaly_trap_for_testing([](logging::AnomalyId) {});
        pool.on_crash = [this](const WorkerCrashInfo& info) {
            crash_reports.push_back(info);
        };
    }

    ~WorkerPoolFixture() {
        logging::reset_anomaly_for_testing();
    }

    void add_stateless(bool alive = true, bool busy = false, bool low = true) {
        auto idx = pool.stateless_workers.size();
        pool.stateless_workers.push_back(WorkerPool::WorkerProcess{});
        auto& w = pool.stateless_workers.back();
        w.name = "SL-" + std::to_string(idx);
        w.state = alive ? SlotState::Alive : SlotState::Dead;
        w.busy = busy;
        w.low_priority = busy && low;
    }

    void add_stateful(bool alive = true, std::size_t owned = 0) {
        auto idx = pool.stateful_workers.size();
        pool.stateful_workers.push_back(WorkerPool::WorkerProcess{});
        auto& w = pool.stateful_workers.back();
        w.name = "SF-" + std::to_string(idx);
        w.state = alive ? SlotState::Alive : SlotState::Dead;
        w.owned_documents = owned;
    }

    void set_low_limit(std::size_t low) {
        pool.low_limit = low;
    }

    void set_max_crash_streak(unsigned n) {
        pool.options.max_crash_streak = n;
    }

    void set_crash_streak(std::size_t idx, bool stateful, unsigned streak) {
        auto& workers = stateful ? pool.stateful_workers : pool.stateless_workers;
        workers[idx].crash_streak = streak;
    }

    void set_uptime(std::size_t idx, bool stateful, std::chrono::milliseconds uptime) {
        auto& workers = stateful ? pool.stateful_workers : pool.stateless_workers;
        workers[idx].spawn_time = std::chrono::steady_clock::now() - uptime;
    }

    std::size_t pick_least_loaded() {
        return pool.pick_least_loaded();
    }

    std::size_t assign_worker(std::uint32_t path_id) {
        return pool.assign_worker(path_id);
    }

    std::size_t assign_expendable(std::uint32_t path_id) {
        return pool.assign_expendable(path_id);
    }

    void remove_owner(std::uint32_t path_id) {
        pool.remove_owner(path_id);
    }

    void clear_owner(std::size_t idx) {
        pool.clear_owner(idx);
    }

    std::size_t pick_idle() {
        return pool.pick_idle_stateless();
    }

    void apply_backoff() {
        pool.apply_crash_backoff();
    }

    void release_slot(std::size_t idx) {
        pool.release_stateless_slot(idx);
    }

    kota::task<std::size_t> acquire_slot(worker::Priority p) {
        return pool.acquire_stateless_slot(p);
    }

    bool simulate_crash(std::size_t index, bool stateful, int exit_code = 0, int exit_signal = 9) {
        return pool.process_crash(index, stateful, exit_code, exit_signal);
    }

    void mark_dead(std::size_t idx, bool stateful = false) {
        pool.mark_worker_dead(idx, stateful, false);
    }

    void set_retiring(std::size_t idx) {
        pool.stateless_workers[idx].retiring = true;
    }

    void set_suspect_inflight(std::size_t idx, bool stateful, unsigned n) {
        auto& workers = stateful ? pool.stateful_workers : pool.stateless_workers;
        workers[idx].suspect_inflight = n;
    }

    void set_revive_after(std::chrono::milliseconds cooldown) {
        pool.options.revive_after = cooldown;
    }

    void set_max_stateless(std::size_t n) {
        pool.options.max_stateless = n;
    }

    bool slot_dead(std::size_t idx, bool stateful = false) const {
        auto& workers = stateful ? pool.stateful_workers : pool.stateless_workers;
        return workers[idx].state == WorkerPool::SlotState::Dead;
    }

    std::size_t stateless_count() const {
        return pool.stateless_workers.size();
    }

    /// Manually queue a waiter, as acquire_stateless_slot does when it
    /// cannot claim directly.
    auto enqueue_waiter(worker::Priority p) {
        auto pending = std::make_unique<WorkerPool::PendingStateless>(pool, p);
        auto& queue = p == worker::Priority::High ? pool.high_queue : pool.low_queue;
        queue.push_back(pending.get());
        pending->queue = &queue;
        return pending;
    }

    void dispatch_pending() {
        pool.try_dispatch_pending();
    }

    void preempt(std::size_t count) {
        pool.preempt_low_priority(count);
    }

    void tick_memory(double ratio) {
        pool.tick_memory(ratio);
    }

    std::chrono::milliseconds backoff_delay(unsigned streak) {
        return pool.backoff_delay(streak);
    }

    bool scale_up() {
        return pool.scale_up_worker();
    }

    bool start(llvm::StringRef self_path,
               std::uint32_t stateless = 2,
               std::uint32_t stateful = 0,
               std::uint32_t min_stateless = 0) {
        WorkerPoolOptions opts;
        opts.self_path = self_path.str();
        opts.stateless_count = stateless;
        opts.stateful_count = stateful;
        opts.min_stateless = min_stateless;
        opts.max_stateless = 8;
        return pool.start(opts);
    }

    std::size_t min_stateless() const {
        return pool.options.min_stateless;
    }

    void force_owner(std::uint32_t path_id, std::size_t idx) {
        pool.owner[path_id] = idx;
        pool.stateful_workers[idx].owned_documents += 1;
    }

    unsigned suspect_inflight(std::size_t idx, bool stateful) const {
        auto& workers = stateful ? pool.stateful_workers : pool.stateless_workers;
        return workers[idx].suspect_inflight;
    }

    kota::task<> stop() {
        return pool.stop();
    }

    template <typename F>
    void run(F&& coro_factory) {
        loop.schedule(coro_factory());
        loop.run();
    }

    void kill_worker(std::size_t idx) {
        pool.stateless_workers[idx].proc.kill(9);
    }

    std::size_t low_limit() const {
        return pool.low_limit;
    }

    std::size_t effective_low_limit() const {
        return pool.effective_low_limit();
    }

    std::size_t max_low_limit() const {
        return pool.max_low_limit();
    }

    std::size_t w_max() const {
        return pool.w_max;
    }

    void set_w_max(std::size_t value) {
        pool.w_max = value;
    }

    std::size_t alive_count() const {
        return pool.alive_stateless();
    }

    std::size_t busy_count() const {
        return pool.busy_stateless();
    }

    std::size_t low_busy() const {
        return pool.low_busy_count();
    }

    std::size_t capacity() const {
        return pool.stateless_capacity();
    }

    std::size_t stateful_owned(std::size_t idx) const {
        return pool.stateful_workers[idx].owned_documents;
    }

    bool is_busy(std::size_t idx) const {
        return pool.stateless_workers[idx].busy;
    }

    bool has_owner(std::uint32_t path_id) const {
        return pool.owner.count(path_id);
    }

    std::size_t owner_of(std::uint32_t path_id) const {
        return pool.owner.find(path_id)->second;
    }

    SlotState state(std::size_t idx, bool stateful = false) const {
        return stateful ? pool.stateful_workers[idx].state : pool.stateless_workers[idx].state;
    }

    unsigned generation(std::size_t idx, bool stateful = false) const {
        return stateful ? pool.stateful_workers[idx].generation
                        : pool.stateless_workers[idx].generation;
    }

    bool preempted(std::size_t idx) const {
        return pool.stateless_workers[idx].preempted;
    }

    unsigned crash_streak(std::size_t idx, bool stateful = false) const {
        return stateful ? pool.stateful_workers[idx].crash_streak
                        : pool.stateless_workers[idx].crash_streak;
    }

    bool worker_alive(std::size_t idx) const {
        return pool.stateless_workers[idx].state == SlotState::Alive;
    }

    int worker_pid(std::size_t idx) const {
        return pool.stateless_workers[idx].proc.pid();
    }

    unsigned get_backoff_cooldown() const {
        return pool.backoff_cooldown;
    }

    std::size_t high_queue_size() const {
        return pool.high_queue.size();
    }

    std::size_t low_queue_size() const {
        return pool.low_queue.size();
    }

    struct PriorityResult {
        bool high_dispatched;
        bool low_dispatched;
    };

    PriorityResult test_priority_dispatch(std::size_t release_idx) {
        auto high = std::make_unique<WorkerPool::PendingStateless>(pool, worker::Priority::High);
        auto low = std::make_unique<WorkerPool::PendingStateless>(pool, worker::Priority::Low);
        pool.high_queue.push_back(high.get());
        high->queue = &pool.high_queue;
        pool.low_queue.push_back(low.get());
        low->queue = &pool.low_queue;
        pool.release_stateless_slot(release_idx);
        return {high->ready.is_set(), low->ready.is_set()};
    }

    struct LowDispatchResult {
        std::size_t dispatched;

        /// Busy count observed while the waiters still hold their claims;
        /// destroying an unconsumed dispatched waiter releases its slot.
        std::size_t busy_at_dispatch;
    };

    LowDispatchResult test_low_dispatch(std::size_t count) {
        llvm::SmallVector<std::unique_ptr<WorkerPool::PendingStateless>> pending;
        for(std::size_t i = 0; i < count; ++i) {
            pending.push_back(
                std::make_unique<WorkerPool::PendingStateless>(pool, worker::Priority::Low));
            pool.low_queue.push_back(pending.back().get());
            pending.back()->queue = &pool.low_queue;
        }
        pool.try_dispatch_pending();
        std::size_t dispatched = 0;
        for(auto& p: pending)
            if(p->ready.is_set())
                dispatched += 1;
        return {dispatched, pool.busy_stateless()};
    }

    bool test_slot_raii(std::size_t idx) {
        pool.stateless_workers[idx].busy = true;
        { WorkerPool::StatelessSlot slot(pool, idx); }
        return !pool.stateless_workers[idx].busy && pool.busy_stateless() == 0 &&
               pool.low_busy_count() == 0;
    }

    struct ReleaseResult {
        bool pending_before;
        bool dispatched_after;
    };

    ReleaseResult test_release_dispatches() {
        auto pending = std::make_unique<WorkerPool::PendingStateless>(pool, worker::Priority::High);
        pool.high_queue.push_back(pending.get());
        pending->queue = &pool.high_queue;
        bool before = pending->ready.is_set();
        pool.release_stateless_slot(0);
        bool after = pending->ready.is_set();
        return {before, after};
    }

    bool test_pending_cleanup_high() {
        {
            WorkerPool::PendingStateless pending(pool, worker::Priority::High);
            pool.high_queue.push_back(&pending);
            pending.queue = &pool.high_queue;
            if(pool.high_queue.size() != 1)
                return false;
        }
        return pool.high_queue.empty();
    }

    bool test_pending_cleanup_low() {
        {
            WorkerPool::PendingStateless pending(pool, worker::Priority::Low);
            pool.low_queue.push_back(&pending);
            pending.queue = &pool.low_queue;
            if(pool.low_queue.size() != 1)
                return false;
        }
        return pool.low_queue.empty();
    }

    struct DispatchResult {
        bool dispatched;
        bool queue_cleared;
        bool queue_ptr_nulled;
    };

    struct DrainResult {
        bool drained;
        std::size_t assigned_worker;
    };

    DrainResult test_dead_pool_drain() {
        auto pending = std::make_unique<WorkerPool::PendingStateless>(pool, worker::Priority::High);
        pool.high_queue.push_back(pending.get());
        pending->queue = &pool.high_queue;
        simulate_crash(0, false);
        return {pending->ready.is_set(), pending->assigned_worker};
    }

    bool test_slot_gen_guard() {
        // Old StatelessSlot should NOT release a slot whose occupant died and
        // was replaced (generation bumped) in the meantime.
        {
            WorkerPool::StatelessSlot slot(pool, 0);
            pool.stateless_workers[0].generation += 1;
            pool.stateless_workers[0].busy = true;
        }
        return pool.stateless_workers[0].busy && pool.busy_stateless() == 1;
    }

    bool test_pending_gen_guard() {
        // A dispatched-but-unconsumed waiter must NOT release the slot if the
        // claimed occupant died and a new claim owns it now.
        auto pending = enqueue_waiter(worker::Priority::High);
        pool.try_dispatch_pending();
        if(!pending->ready.is_set() || pending->assigned_worker != 0)
            return false;
        pool.stateless_workers[0].generation += 1;
        pool.stateless_workers[0].busy = true;
        pending.reset();
        return pool.stateless_workers[0].busy;
    }

    DispatchResult test_dispatch_clears_queue() {
        auto pending = std::make_unique<WorkerPool::PendingStateless>(pool, worker::Priority::High);
        pool.high_queue.push_back(pending.get());
        pending->queue = &pool.high_queue;
        pool.release_stateless_slot(0);
        return {
            pending->ready.is_set(),
            pool.high_queue.empty(),
            pending->queue == nullptr,
        };
    }
};

}  // namespace clice::testing

#endif
