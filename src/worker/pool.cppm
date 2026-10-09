module;

#include "modules/prelude.h"

#include "support/logging.macros.h"

module clice:worker.pool;

import :support.logging;
import :support.signal;
import :worker.probe;
import :worker.protocol;

namespace clice::worker {

/// Scheduling class of a stateless dispatch. Deliberately not part of the
/// wire protocol: priority is the pool's business, passed explicitly to
/// send_stateless — the worker process never sees it.
enum class Priority : uint8_t { High, Low };

}  // namespace clice::worker

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

    /// Consecutive fast deaths of this slot that count against its budget
    /// (see WorkerPool), this one included when it does. Resets after a
    /// healthy uptime, so only genuine crash loops accumulate.
    unsigned crash_streak = 0;

    /// Whether the pool will attempt to respawn this worker.
    bool will_restart = false;

    /// Stateful only: path_ids of documents owned by the crashed worker.
    /// The on_crash handler should mark these dirty for recompilation.
    std::vector<std::uint32_t> lost_documents;
};

/// How one worker incarnation died, shared with every request in flight on
/// it. A sender whose link broke waits for the monitor to settle the record
/// — the exit observed, the stderr drained — and only then learns whether
/// its own request is the one that killed the worker.
struct WorkerDeath {
    kota::event settled;

    /// The crash tag (worker::crash_tag) of the request the worker named in
    /// its last words, or that outlived the pool's deadline; empty when
    /// nothing names one — an outside kill, a crash outside any request.
    std::string culprit;

    /// How it died, worded for the user ("signal 11 (SIGSEGV)").
    std::string cause;

    /// The pool killed the worker to reclaim memory. Its request may be
    /// what exhausted memory, so the sender spends its own bounded retries
    /// (worker_lost) instead of requeueing for free like a deliberate cancel.
    bool reclaimed = false;

    /// Requests in flight when the worker died. A death that names no
    /// request is held against a request only when it ran alone: among
    /// several, nothing tells which one killed the worker.
    std::size_t in_flight = 0;
};

/// The last lines a worker wrote to stderr, kept for its crash report:
/// the fatal diagnosis (an LLVM ERROR, an assertion) lands there seconds
/// before the exit is observed, far from the report in the log.
struct StderrTail {
    constexpr static std::size_t capacity = 8;

    llvm::SmallVector<std::string, capacity> lines;

    /// The crash tag the dying worker wrote after worker::crashed_in_marker;
    /// kept apart from `lines`, which a sanitizer report would roll over.
    std::string crashed_in;

    /// The pipe reached EOF. The exit is observed independently of the
    /// pipe, so the crash report waits for this before reading the lines.
    kota::event drained;

    void add(std::string line) {
        if(lines.size() == capacity) {
            lines.erase(lines.begin());
        }
        lines.push_back(std::move(line));
    }
};

struct WorkerPoolOptions {
    std::string self_path;

    /// Stateless workers started, and the most that background (Low) work
    /// keeps busy; only interactive work that finds no idle slot grows the
    /// pool past it, up to max_stateless.
    std::uint32_t stateless_count = 2;
    std::uint32_t stateful_count = 2;
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

    /// A request running past its deadline is taken for a hung worker: the
    /// worker is killed and the request blamed, like a crash. A build may
    /// take long on a big translation unit (see worker::is_build); a query
    /// never should.
    std::chrono::milliseconds build_deadline{std::chrono::minutes(10)};
    std::chrono::milliseconds query_deadline{std::chrono::minutes(2)};

    /// Documents a stateful worker holds before evicting; unset leaves the
    /// worker's default.
    std::optional<std::size_t> max_documents;

    /// Dynamic scaling bounds for stateless workers.
    /// min_stateless: floor — never retire below this count.
    /// max_stateless: ceiling — never spawn above this count (0 = auto = the
    /// physical cores, see default_max_stateless_worker_count).
    std::uint32_t min_stateless = 1;
    std::uint32_t max_stateless = 0;
};

/// Multi-process scheduler for clice worker processes.
///
/// Two kinds of workers are managed:
///   - Stateless workers execute independent build tasks (PCH/PCM builds,
///     indexing, completion, formatting) with two-level priority scheduling:
///     low-priority concurrency is budgeted — up to stateless_count when the
///     user is idle, one while foreground activity is live, and less under
///     memory pressure. A High request finding no idle slot cooperatively
///     cancels one low build instead of waiting out a whole TU.
///   - Stateful workers keep per-document ASTs; each open document is pinned
///     to one worker (path_id affinity), balanced by document count.
///
/// Responsibility contract — the pool is mechanism, callers are policy:
///
///   - The pool owns PROCESSES and CAPACITY: spawn, monitor, respawn with
///     backoff, per-slot crash budget (streak, healthy-uptime reset),
///     give-up -> cooldown revival, scaling between min/max, preemption
///     under memory pressure, a deadline on every request, scheduling
///     (priority, affinity). Its guarantee: capacity is never permanently
///     zero.
///   - The pool ATTRIBUTES every death. A dying worker names the request it
///     was running (see worker/crash_report.h), or the deadline watchdog
///     names the one it killed the worker for; each request in flight
///     learns whether it is that one. A death that names a request is that
///     request's content's doing and spends no slot budget; neither does a
///     terminated session (SIGTERM, SIGINT, SIGHUP) or a SIGKILL (the OOM
///     killer, a user) of a worker holding no work. Every other death
///     naming no request counts toward the streak.
///   - The pool NEVER retries a request. Requests do not survive a crash;
///     slots do. Retry policy is semantic and lives with the caller —
///     deliver() is the shared form of it.
///   - The dispatch_errc taxonomy is the contract language. worker_crashed:
///     this request killed its worker — the caller blames its content
///     (Error::data carries the dead incarnation's identity so one death is
///     never blamed twice, the message how it died). worker_lost: another
///     request's crash took it along, the death named none among several
///     requests in flight, or the pool reclaimed the worker's memory —
///     blameless. worker_died: the death named no request and this one ran
///     alone. worker_unavailable: never dispatched, a capacity window —
///     await_capacity() tells when it closes. cancelled: deliberate
///     preemption, requeue freely.
class WorkerPool {
public:
    WorkerPool(kota::event_loop& loop) : loop(loop) {}

    /// Spawn all worker processes. Returns false on failure.
    bool start(const WorkerPoolOptions& opts);

    /// Gracefully stop all workers.
    kota::task<> stop();

    /// Whether foreground work is in flight right now — a stateful request
    /// dispatched and unanswered, or high-priority stateless work queued or
    /// running. The hold window only starts counting once this drains.
    bool foreground_busy() const {
        return stateful_inflight > 0 || !high_queue.empty() || high_busy_count() > 0;
    }

    /// Send a request to a stateful worker with path_id affinity routing.
    template <typename Params>
    RequestResult<Params> send_stateful(std::uint32_t path_id,
                                        const Params& params,
                                        kota::ipc::request_options opts = {});

    /// Send a request to a stateless worker with priority-aware scheduling.
    ///
    /// `cancel` is an advisory cancellation: when it fires, the request is
    /// cancelled on the wire and this call KEEPS awaiting the worker's real
    /// answer — the slot frees only when the process is actually idle, and
    /// crash accounting keeps observing the real outcome. A cancelled
    /// result surfaces as dispatch_errc::cancelled.
    template <typename Params>
    RequestResult<Params> send_stateless(const Params& params,
                                         worker::Priority priority,
                                         kota::cancellation_token cancel = {});

    /// Send a notification to the stateful worker owning path_id (if any).
    template <typename Params>
    void notify_stateful(std::uint32_t path_id, const Params& params);

    /// Remove path_id from ownership tracking (e.g. when the master learns a
    /// document was evicted).
    void remove_owner(std::uint32_t path_id);

    /// Remove path_id from ownership only if worker_index is its current
    /// owner. Returns whether it was removed — false means the eviction
    /// concerns a copy the master already let go of (the document closed,
    /// or reopened on another worker) and the current owner's state is
    /// untouched.
    bool remove_owner_from(std::uint32_t path_id, std::size_t worker_index);

    /// Wait until a worker of the kind can take a request. False when none
    /// ever will again: the pool is stopping, or every slot gave up with
    /// revival off.
    kota::task<bool> await_capacity(bool stateful);

    /// True when a Dead slot is not final: the running pool revives dead
    /// slots after a cooldown, so "no capacity" is a window, not a verdict.
    /// Callers (the indexer's requeue) may retry work that failed with
    /// worker_unavailable instead of dropping it. Unit fixtures drive slot
    /// state without an event loop and never start the pool, so revival
    /// stays off for them.
    bool revives_slots() const {
        return started && options.revive_after.count() > 0;
    }

    /// Alive stateless slots that also accept new work: a retiring slot
    /// stays alive to finish its request but is skipped by dispatch.
    std::size_t schedulable_stateless() const;

    /// The current low-priority concurrency budget: the memory controller's
    /// window, additionally clamped to one while the user is active. Public
    /// so the indexer's feeder can size its in-flight window from it.
    std::size_t effective_low_limit() const {
        return std::min(low_limit, foreground_active ? foreground_low_limit : max_low_limit());
    }

    /// Foreground evidence from outside the pool: didOpen/didChange/didSave
    /// and friends. Starts (or refreshes) the hold window; requests need no
    /// call of their own — every stateful dispatch and every High stateless
    /// dispatch notes itself, and in-flight ones hold the window open.
    void foreground_pulse() {
        note_foreground();
    }

    /// Emitted when a stateless slot (re)enters service — spawn, respawn,
    /// revive, scale-up. The indexer's capacity gate wakes on it instead of
    /// spinning dispatch attempts against a pool with no schedulable worker.
    Signal<> on_stateless_capacity;

    /// Callback invoked when a worker process crashes.
    std::function<void(const WorkerCrashInfo&)> on_crash;

    /// The server's test instrumentation while test hooks are on, null
    /// otherwise: it counts the builds sent and parks the replies a test
    /// holds (see BuildProbe).
    BuildProbe* probe = nullptr;

    /// Callback invoked when a stateful worker sends an EvictedParams
    /// notification, with the slot index of the evicting worker. The master
    /// translates the path to a path_id and calls remove_owner_from() so an
    /// eviction of a copy the master already let go of cannot unseat the
    /// current owner.
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
        /// Intentionally scaled down; vacant until scale-up refills it.
        Retired,
        /// Crash budget exhausted; vacant until revival or scale-up.
        Dead,
    };

    struct Dispatch;

    struct WorkerProcess {
        kota::process proc;

        /// Shared with every coroutine that awaits on it (request senders,
        /// the IO pump), so a slot can drop its reference the moment the
        /// process dies without racing in-flight users.
        std::shared_ptr<kota::ipc::BincodePeer> peer;

        /// Written by the stderr drain, read by the crash report.
        std::shared_ptr<StderrTail> stderr_tail;

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

        /// Consecutive fast deaths that count against the slot's budget
        /// (see process_crash); resets after healthy uptime.
        unsigned crash_streak = 0;

        /// The death record of the current incarnation, fresh at every
        /// spawn; senders hold it across their await.
        std::shared_ptr<WorkerDeath> death = std::make_shared<WorkerDeath>();

        /// Requests in flight on the current incarnation, for the deadline
        /// watchdog. Cleared at death.
        std::vector<const Dispatch*> dispatches;

        /// Stateful only: the documents the dead incarnation owned, taken
        /// off the owner table the moment the death is declared — a request
        /// routed to the corpse in the window before the crash report
        /// would fail instead of moving on — and reported with the crash.
        std::vector<std::uint32_t> lost_documents;

        std::chrono::steady_clock::time_point spawn_time{};

        /// Stateless only: cancels the in-flight request, and marks it
        /// scheduler-cancelled so its sender classifies the eventual reply
        /// (cooperative stop or kill) as preemption instead of a failure.
        std::shared_ptr<kota::cancellation_source> preempt_source;

        /// When a cooperative cancel was requested of this slot's in-flight
        /// request; zero when none is outstanding. The monitor kills the
        /// process if it blows past cancel_grace without returning.
        std::chrono::steady_clock::time_point cancel_requested_at{};

        /// Monotonic claim ordinal of the in-flight request. Victim
        /// selection picks the largest — the actual newest claim; the slot
        /// index says nothing about claim order once slots are reused.
        std::uint64_t claim_epoch = 0;

        /// Whether the worker holds work, and with it memory: a claim, a
        /// request in flight, or documents (whose last one can close while
        /// its compile still runs).
        bool holds_work() const {
            return busy || !dispatches.empty() || owned_documents > 0;
        }
    };

    /// A request in flight on a slot: what the deadline watchdog times and
    /// the crash tag a death is matched against. Registered for the span of
    /// the send; a death clears the slot's list, so an outliving frame
    /// leaves the next incarnation's list alone.
    struct Dispatch {
        WorkerPool& pool;
        std::size_t index;
        bool stateful;
        unsigned generation;
        std::string tag;
        /// See worker::is_build.
        bool build;
        std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();

        Dispatch(WorkerPool& pool, std::size_t index, bool stateful, std::string tag, bool build) :
            pool(pool), index(index), stateful(stateful),
            generation(pool.slot(index, stateful).generation), tag(std::move(tag)), build(build) {
            pool.slot(index, stateful).dispatches.push_back(this);
        }

        Dispatch(const Dispatch&) = delete;
        Dispatch& operator=(const Dispatch&) = delete;

        ~Dispatch() {
            auto& w = pool.slot(index, stateful);
            if(w.generation == generation) {
                std::erase(w.dispatches, this);
            }
        }
    };

    kota::event_loop& loop;
    llvm::SmallVector<WorkerProcess> stateless_workers;
    llvm::SmallVector<WorkerProcess> stateful_workers;

    WorkerProcess& slot(std::size_t index, bool stateful) {
        return stateful ? stateful_workers[index] : stateless_workers[index];
    }

    /// Set whenever a slot (re)enters service, and at stop; await_capacity
    /// waits on it.
    kota::event capacity_returned;

    /// The error a request that died with its worker reports: worker_crashed
    /// when the death names this very request; worker_lost when it names
    /// another, names none while other requests were in flight, or was the
    /// pool's memory reclaim; worker_died when it names none and this
    /// request ran alone.
    static kota::ipc::Error death_error(const WorkerDeath& death,
                                        llvm::StringRef tag,
                                        kota::codec::dyn::Value identity);

    // Stateful routing: each open document (path_id) is pinned to one worker.
    llvm::DenseMap<std::uint32_t, std::size_t> owner;  // path_id -> worker index

    /// Returns the worker owning path_id, assigning the least-loaded live
    /// worker on first use. SIZE_MAX when no stateful worker is alive, or
    /// once the pool stops: its workers are still Alive then, but their
    /// links are closed, and a request failing on one would read as a
    /// nameless death and be blamed. An owner is always alive: a death
    /// takes its documents off the table.
    std::size_t assign_worker(std::uint32_t path_id);
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

        /// Set to the claimed worker before signalling ready; stays SIZE_MAX
        /// when the waiter is woken only to observe pool death or stop.
        std::size_t assigned_worker = SIZE_MAX;

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
            } else if(assigned_worker != SIZE_MAX &&
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

    /// Max concurrent low-priority tasks, adjusted by tick_memory(). Reads
    /// go through effective_low_limit(), which clamps to the ceiling.
    std::size_t low_limit = 0;

    // All occupancy numbers are derived from slot state on demand instead of
    // being maintained as counters — with at most a few dozen slots the scans
    // are trivial, and there is no incremental bookkeeping to drift.

    /// Stateless slots that can serve requests now.
    std::size_t alive_stateless() const;

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

    /// The low-priority ceiling with no foreground activity: the
    /// configured stateless count, of the slots schedulable now. Foreground
    /// bursts reclaim capacity through foreground_low_limit and the deficit
    /// cancel in acquire_stateless_slot instead of a standing reservation,
    /// which would leave a slot idle through every fully-idle stretch.
    std::size_t max_low_limit() const {
        return std::min<std::size_t>(schedulable_stateless(), options.stateless_count);
    }

    /// The low budget while the user is active. Background compiles slow
    /// the user's own down well before the CPU runs out — cores share one
    /// power limit — so while they type one TU indexes at a time.
    constexpr static std::size_t foreground_low_limit = 1;

    /// Memory controller thresholds on the available share of memory: below
    /// severe_ratio running low work is killed and the budget zeroed; below
    /// pressure_ratio the budget shrinks; at or above it the budget grows.
    constexpr static double severe_ratio = 0.10;
    constexpr static double pressure_ratio = 0.20;

    /// Rising edge of foreground activity: clamp the budget now and
    /// cooperatively cancel the excess in-flight low work so the CPU frees
    /// within a declaration boundary, not at end-of-TU.
    void note_foreground();

    /// Falling edge, driven by the monitor tick: foreground work drained
    /// and the hold window expired reopen the idle budget.
    void tick_foreground();

    /// Kill workers whose cooperatively cancelled request outlived
    /// cancel_grace; driven by the monitor tick.
    void tick_cancel_grace();

    /// Kill workers running a request past its deadline and name that
    /// request in the death record; driven by the monitor tick. A query is
    /// not timed while a build runs on the same worker: the time is the
    /// sender's, and the query may be queued behind the build.
    void tick_deadlines();

    /// Cooperatively cancel up to `count` in-flight low-priority requests:
    /// the wire cancel trips the worker's stop flag, the compile returns at
    /// the next declaration boundary, and the sender — which
    /// keeps awaiting the worker's own reply, so the slot stays busy until
    /// the process is actually free — observes dispatch_errc::cancelled.
    /// A victim that ignores the cancel past cancel_grace is killed by the
    /// monitor.
    void cancel_low_priority(std::size_t count);

    /// Low slots still owed to foreground/High demand beyond the cancels
    /// already in flight: the excess over the clamped budget, or one per
    /// queued High request while no slot is idle, minus the victims already
    /// asked to stop. Evaluated at every demand edge AND at the Low arming
    /// point in send_stateless — the cancel sweep must skip claims whose
    /// sender has not armed a source yet, so the arming re-check is what
    /// keeps demand from being dropped in that window. `pending_high`
    /// counts a High requester about to queue itself.
    std::size_t low_reclaim_deficit(std::size_t pending_high = 0);

    /// Wait for an idle stateless worker. Returns SIZE_MAX when no slot can
    /// serve the request anymore (pool stopped or all slots given up), or
    /// once `cancel` fires while it waits.
    kota::task<std::size_t> acquire_stateless_slot(worker::Priority priority,
                                                   kota::cancellation_token cancel);
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

    /// 3s tick driving the controllers below.
    kota::task<> monitor_loop();

    /// Adjusts low_limit from memory pressure: zero below severe_ratio,
    /// killing the low work in flight; one step down below pressure_ratio,
    /// never under one; one step up above it while the budget is in use, so
    /// a budget the foreground limit masks or nothing uses never climbs past
    /// what was tried.
    void tick_memory(double available_ratio);

    /// Rank each worker for the kernel's OOM killer (Linux): one holding a
    /// request or documents just above the master, which holds every
    /// session; an idle one at the master's own score, so that killing it —
    /// which frees nothing — is never the killer's first choice.
    void tick_oom_scores(std::uint64_t memory_limit);

    /// Write a worker's OOM score: oom_holding when it holds work, else
    /// the master's. Taking on work raises it and a stateless release
    /// lowers it at once, since a compile can exhaust memory before the
    /// next tick.
    void set_oom_score(const WorkerProcess& w, bool holds);

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
    /// pressure. Their requests fail with dispatch_errc::worker_lost and the
    /// processes respawn immediately without crash accounting.
    void preempt_low_priority(std::size_t count);

    /// Foreground activity state: in-flight foreground work (stateful or
    /// High stateless), or a pulse within fg_hold, keeps the low budget
    /// clamped to foreground_low_limit. Rising edges are synchronous
    /// (note_foreground); the falling edge is evaluated by the monitor tick
    /// so hot paths never read the clock.
    bool foreground_active = false;
    std::size_t stateful_inflight = 0;
    std::chrono::steady_clock::time_point last_fg_activity{};
    std::uint64_t next_claim_epoch = 0;

    /// Alive stateless slots busy with high-priority work.
    std::size_t high_busy_count() const;

    constexpr static std::chrono::seconds fg_hold{10};

    /// Grace before a cooperatively cancelled low request's worker is
    /// killed: covers a compile stuck inside one giant declaration, where
    /// the stop flag is never polled.
    constexpr static std::chrono::seconds cancel_grace{10};

    /// Consecutive monitor ticks with interactive work queued and no idle
    /// slot, or background work queued behind a full allowance while the
    /// pool is below stateless_count.
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
    kota::task_group<> worker_tasks;
    WorkerPoolOptions options;

    /// Set once start() succeeded: gates background concerns (slot
    /// revival) that need a running event loop.
    bool started = false;
    std::string log_dir;

    /// The master's own oom_score_adj, which workers inherit; unset where
    /// there is none to adjust (not Linux).
    std::optional<int> oom_base;

    /// The score of a worker holding work, just above the master as the
    /// last tick measured it; the highest until the first tick.
    int oom_holding = 1000;

    /// The usage counter and memory.stat libuv measures a memory cgroup by,
    /// with the memory.stat key of the cgroup's inactive file cache
    /// (hierarchical on v1, as the usage counter is).
    struct CgroupMemory {
        std::string usage;
        std::string stat;
        llvm::StringRef cache_key;
    };

    /// The files libuv reads for the memory cgroup `proc_cgroup` (the text
    /// of /proc/self/cgroup) names.
    static CgroupMemory find_cgroup_memory(llvm::StringRef proc_cgroup);

    /// What is left of the cgroup's `limit` once its working set (usage
    /// less inactive file cache, as kubelet counts it) is taken out:
    /// libuv counts that cache as used, so a container that read and wrote
    /// some files would look full. Unset when the files cannot be read.
    static std::optional<std::uint64_t> cgroup_available(const CgroupMemory& cgroup,
                                                         std::uint64_t limit);

    struct SpawnedProcess {
        kota::process proc;
        std::shared_ptr<kota::ipc::BincodePeer> peer;
        std::shared_ptr<StderrTail> stderr_tail;
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

    void install_evict_handler(WorkerProcess& worker, std::size_t index);

    kota::task<> monitor_worker(std::size_t index, bool stateful);

    /// The sends without the probe, which parks a reply only once its
    /// dispatch released the slot. `sent` tells whether the request reached
    /// a worker: one that failed before has no build to count or hold.
    template <typename Params>
    RequestResult<Params> dispatch_stateful(std::uint32_t path_id,
                                            const Params& params,
                                            kota::ipc::request_options opts,
                                            bool& sent);

    template <typename Params>
    RequestResult<Params> dispatch_stateless(const Params& params,
                                             worker::Priority priority,
                                             kota::cancellation_token cancel,
                                             bool& sent);

    friend struct testing::WorkerPoolFixture;
};

/// The build a request carries, as the probe counts it: its kind and file;
/// none for the other requests.
template <typename Params>
std::optional<std::pair<BuildKind, llvm::StringRef>> probed_build(const Params& params) {
    if constexpr(std::same_as<Params, worker::CompileParams>) {
        return std::pair{BuildKind::Compile, llvm::StringRef(params.path)};
    } else if constexpr(std::same_as<Params, worker::BuildPCHParams>) {
        return std::pair{BuildKind::PCH, llvm::StringRef(params.file)};
    } else if constexpr(std::same_as<Params, worker::BuildPCMParams>) {
        return std::pair{BuildKind::PCM, llvm::StringRef(params.file)};
    } else if constexpr(std::same_as<Params, worker::TURunParams>) {
        return std::pair{BuildKind::Index, llvm::StringRef(params.file)};
    } else {
        return std::nullopt;
    }
}

template <typename Params>
RequestResult<Params> WorkerPool::send_stateful(std::uint32_t path_id,
                                                const Params& params,
                                                kota::ipc::request_options opts) {
    bool sent = false;
    auto result = co_await dispatch_stateful(path_id, params, std::move(opts), sent);
    if(auto build = probed_build(params); build && sent && probe) {
        co_await probe->returned(build->first, build->second);
    }
    co_return std::move(result);
}

template <typename Params>
RequestResult<Params> WorkerPool::send_stateless(const Params& params,
                                                 worker::Priority priority,
                                                 kota::cancellation_token cancel) {
    bool sent = false;
    auto result = co_await dispatch_stateless(params, priority, std::move(cancel), sent);
    if(auto build = probed_build(params); build && sent && probe) {
        co_await probe->returned(build->first, build->second);
    }
    co_return std::move(result);
}

template <typename Params>
RequestResult<Params> WorkerPool::dispatch_stateful(std::uint32_t path_id,
                                                    const Params& params,
                                                    kota::ipc::request_options opts,
                                                    bool& sent) {
    // Every stateful request is user-facing: note the activity and hold the
    // foreground window open for as long as it flies.
    note_foreground();
    stateful_inflight += 1;

    struct InflightGuard {
        std::size_t& count;

        ~InflightGuard() {
            count -= 1;
        }
    } inflight_guard{stateful_inflight};

    auto idx = assign_worker(path_id);
    if(idx == SIZE_MAX) {
        co_await kota::fail(kota::ipc::Error{worker::dispatch_errc::worker_unavailable,
                                             "No stateful workers available"});
    }

    // Own a peer reference across the await: the slot drops its copy the
    // moment the worker dies.
    auto& assigned = stateful_workers[idx];
    auto peer = assigned.peer;
    auto gen = assigned.generation;
    auto death = assigned.death;
    sent = true;
    Dispatch dispatch(*this, idx, true, worker::crash_tag(params), worker::is_build<Params>);
    auto result = co_await peer->send_request(params, opts);
    if(result.has_value() || !worker::is_transport_error(result.error()))
        co_return std::move(result);

    // The worker link broke mid-request. Declare the slot dead now so
    // follow-up requests fail fast instead of piling onto a corpse;
    // monitor_worker reconciles with the real exit status.
    if(stateful_workers[idx].generation == gen)
        mark_worker_dead(idx, true, true);
    co_await death->settled.wait();
    co_await kota::fail(death_error(*death, dispatch.tag, worker::death_identity(idx, gen, true)));
}

template <typename Params>
RequestResult<Params> WorkerPool::dispatch_stateless(const Params& params,
                                                     worker::Priority priority,
                                                     kota::cancellation_token cancel,
                                                     bool& sent) {
    // High-priority stateless work (PCH, completion builds, foreground
    // PCMs) is foreground by the priority taxonomy; while it runs or
    // queues, foreground_busy() holds the window open.
    if(priority == worker::Priority::High)
        note_foreground();
    auto idx = co_await acquire_stateless_slot(priority, cancel);
    if(idx == SIZE_MAX) {
        co_await kota::fail(cancel.cancelled()
                                ? kota::ipc::Error{worker::dispatch_errc::cancelled,
                                                   "Request cancelled by its round"}
                                : kota::ipc::Error{worker::dispatch_errc::worker_unavailable,
                                                   "No stateless workers available"});
    }

    StatelessSlot slot(*this, idx);
    auto peer = stateless_workers[idx].peer;
    auto gen = stateless_workers[idx].generation;
    auto death = stateless_workers[idx].death;

    if(priority == worker::Priority::Low) {
        // Reclaim demand that arose while this claim's sender was parked
        // (a foreground edge, a queued High) was skipped by the cancel
        // sweep — an unarmed slot must never be stamped (see
        // cancel_low_priority). Honor it here, before the request reaches
        // the wire: giving the claim back costs nothing.
        if(low_reclaim_deficit() > 0) {
            co_await kota::fail(kota::ipc::Error{worker::dispatch_errc::cancelled,
                                                 "Request preempted by the scheduler"});
        }
    }
    // The classification channel for scheduler-initiated cancels: the
    // cooperative cancel and the caller's advisory token both mark it, and
    // the sender consults it when the reply arrives. Firing it cancels the request on the wire,
    // which keeps awaiting the worker's real answer.
    auto preempt_src = std::make_shared<kota::cancellation_source>();
    stateless_workers[idx].preempt_source = preempt_src;

    // Guarded by generation: the worker may have died before this frame
    // resumed, and its respawned successor must see no grace deadline.
    auto relay = cancel.on_cancel([this, idx, gen, preempt_src] {
        auto& w = stateless_workers[idx];
        if(w.generation != gen || w.state != SlotState::Alive || preempt_src->cancelled()) {
            return;
        }
        LOG_DEBUG("Advisory cancel: cancelling the request");
        preempt_src->cancel();
        // Arm the grace deadline like cancel_low_priority does, or
        // tick_cancel_grace() never reclaims a worker whose build
        // ignores the stop flag.
        w.cancel_requested_at = std::chrono::steady_clock::now();
    });

    sent = true;
    Dispatch dispatch(*this, idx, false, worker::crash_tag(params), worker::is_build<Params>);
    auto result = co_await peer->send_request(params, {.token = preempt_src->token()});
    // The worker link broke mid-request: declare the slot dead now so a
    // caller-side retry cannot land on the same corpse before the monitor
    // observed the exit. This must precede the cancel classification — a
    // cooperatively cancelled worker can die on its own inside the grace
    // window, and returning early would release the slot still Alive.
    bool transport_dead = !result.has_value() && worker::is_transport_error(result.error());
    if(transport_dead && stateless_workers[idx].generation == gen) {
        mark_worker_dead(idx, false, true);
        // The dead worker's claim is gone; a queued low-priority waiter may
        // now fit under the limit on another idle worker.
        try_dispatch_pending();
    }

    // A cancel comes back as whatever the worker answered — RequestCancelled,
    // the result it had already, or the killed process's transport error:
    // the request awaits the real answer so the slot frees only once the
    // process is actually idle again, keeping the grace deadline armed and
    // the next request off a still-stuck worker. Every shape must surface as
    // cancelled, so the indexer requeues instead of recording a failure —
    // unless the pool killed the worker for memory before it stopped: the
    // request may be what exhausted memory, and a free requeue would let
    // the user's typing repeat the kill forever.
    if(preempt_src->cancelled() && !death->reclaimed)
        co_await kota::fail(kota::ipc::Error{worker::dispatch_errc::cancelled,
                                             "Request preempted by the scheduler"});
    // An error returned by the worker's handler leaves the worker healthy;
    // pass it through untouched.
    if(result.has_value() || !transport_dead)
        co_return std::move(result);

    co_await death->settled.wait();
    co_await kota::fail(death_error(*death, dispatch.tag, worker::death_identity(idx, gen, false)));
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

/// Send a request through the worker deaths that are not its own doing.
/// `send` makes one attempt (and may fail on its own to stop early, a
/// superseded request); deliver resends once when another request's crash
/// took the attempt along (worker_lost) or the death named no request
/// (worker_died), and waits out capacity windows (worker_unavailable). One
/// resend suffices: the culprit of the first death is barred from running
/// again by then. A request that dies naming no request twice, alone on
/// its worker both times, is taken for the killer — an OOM kill, a stack
/// overflow the crash report cannot run on.
///
/// `blame` fires once, when the request is found to have killed its
/// worker: its own crash, or the second worker_died. The result is the
/// last attempt's.
template <typename Send, typename Blame>
auto deliver(WorkerPool& pool, bool stateful, Send send, Blame blame)
    -> std::invoke_result_t<Send&> {
    namespace errc = worker::dispatch_errc;
    bool resent = false;
    bool nameless = false;
    while(true) {
        auto result = co_await send();
        if(result.has_value()) {
            co_return std::move(result);
        }
        auto code = result.error().code;
        if(code == errc::worker_unavailable) {
            if(co_await pool.await_capacity(stateful)) {
                continue;
            }
            co_return std::move(result);
        }
        bool died = code == errc::worker_died;
        if(code == errc::worker_crashed || (died && nameless)) {
            blame(result.error());
            co_return std::move(result);
        }
        if((died || code == errc::worker_lost) && !resent) {
            resent = true;
            nameless = died;
            continue;
        }
        co_return std::move(result);
    }
}

}  // namespace clice
