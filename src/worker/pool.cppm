module;

#include "modules/prelude.h"

#include "support/logging.macros.h"

module clice:worker.pool;

import :support.logging;
import :support.signal;
import :worker.probe;
import :worker.protocol;
import :worker.scheduler;

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
/// it. A sender whose link broke waits for the monitor to mark the record
/// done — the exit observed, the stderr drained — and only then learns whether
/// its own request is the one that killed the worker.
struct WorkerDeath {
    kota::event done;

    /// The crash tags (worker::crash_tag) of the request the worker named in
    /// its last words, or of the ones that outlived the pool's deadline;
    /// empty when nothing names one — an outside kill, a crash outside any
    /// request.
    llvm::SmallVector<std::string, 1> culprits;

    /// How it died, worded for the user ("signal 11 (SIGSEGV)").
    std::string cause;

    /// A stateless worker's memory ran out: the pool killed it to free
    /// memory, or an allocation failed in it. Its request is not to blame.
    bool out_of_memory = false;

    /// The pool killed the worker because its request ignored a cancel past
    /// the grace period. The request is not to blame, but the run counts as
    /// lost: a free requeue would repeat it for as long as the user keeps
    /// preempting it.
    bool ignored_cancel = false;

    /// A stateless worker's own memory as last sampled, and whether its
    /// request was the only stateless one running: one whose memory ran out
    /// alone does not fit this machine.
    std::uint64_t memory = 0;
    bool alone = false;

    /// Requests in flight when the worker died. A death that names no
    /// request is held against a request only when it ran alone: among
    /// several, nothing tells which one killed the worker.
    std::size_t in_flight = 0;
};

/// The last lines a worker wrote to stderr, kept for its crash report:
/// the fatal diagnosis (an LLVM ERROR, an assertion) arrives there seconds
/// before the exit is observed, far from the report in the log.
struct StderrTail {
    constexpr static std::size_t capacity = 8;

    llvm::SmallVector<std::string, capacity> lines;

    /// The crash tag the dying worker wrote after worker::crashed_in_marker;
    /// kept apart from `lines`, which a sanitizer report would roll over.
    std::string crashed_in;

    /// The worker wrote that an allocation failed.
    bool out_of_memory = false;

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
    /// never should. A stateless request is timed by its worker's CPU time,
    /// and taken for hung too once that stands still for stall_deadline
    /// (see SchedulerOptions).
    std::chrono::milliseconds build_deadline{std::chrono::minutes(10)};
    std::chrono::milliseconds query_deadline{std::chrono::minutes(2)};
    std::chrono::milliseconds stall_deadline{std::chrono::minutes(5)};

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

/// What a finished stateless request measured.
struct StatelessReport {
    /// The peak of its worker's own memory while it ran.
    std::uint64_t peak = 0;
};

/// Multi-process scheduler for clice worker processes.
///
/// Two kinds of workers are managed:
///   - Stateless workers execute independent build tasks (PCH/PCM builds,
///     indexing, completion, formatting); which runs when is the
///     WorkerScheduler's decision (worker/scheduler.cppm), which the pool
///     carries out.
///   - Stateful workers keep per-document ASTs; each open document is pinned
///     to one worker (path_id affinity), balanced by document count.
///
/// Responsibility contract — the pool is mechanism, callers are policy:
///
///   - The pool owns PROCESSES and CAPACITY: spawn, monitor, respawn with
///     backoff, per-slot crash budget (streak, healthy-uptime reset),
///     give-up -> cooldown revival, and the scheduler's actions: dispatch,
///     cooperative cancels, kills for memory and deadlines, scaling. Its
///     guarantee: capacity is never permanently zero.
///   - The pool ATTRIBUTES every death. A dying worker names the request it
///     was running (see worker/crash_report.cppm), or the deadline watchdog
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
///     request's crash took it along, or the death named none among several
///     requests in flight — blameless. worker_died: the death named no
///     request and this one ran alone. out_of_memory: the stateless
///     worker's memory ran out — blameless, Error::data says how much it
///     reached and whether it ran alone. worker_unavailable: never
///     dispatched, a capacity window — await_capacity() tells when it
///     closes. cancelled: deliberate preemption, requeue freely.
class WorkerPool {
public:
    WorkerPool(kota::event_loop& loop) : loop(loop), scheduler(SchedulerOptions{}) {}

    /// Spawn all worker processes. Returns false on failure.
    bool start(const WorkerPoolOptions& opts);

    /// Gracefully stop all workers.
    kota::task<> stop();

    /// Whether foreground work is in flight right now: a stateful request
    /// dispatched and unanswered, or high-priority stateless work queued or
    /// running.
    bool foreground_busy() const {
        return stateful_inflight > 0 || high_inflight > 0;
    }

    /// Send a request to a stateful worker with path_id affinity routing.
    template <typename Params>
    RequestResult<Params> send_stateful(std::uint32_t path_id,
                                        const Params& params,
                                        kota::ipc::request_options opts = {});

    /// Send a request to a stateless worker, placed by the scheduler.
    ///
    /// `cancel` is an advisory cancellation: when it fires, the request is
    /// cancelled over IPC and this call KEEPS awaiting the worker's real
    /// answer — the slot frees only when the process is actually idle, and
    /// crash accounting keeps observing the real outcome. A cancelled
    /// result is reported as dispatch_errc::cancelled. `cost` is what the
    /// request is expected to take; a finished request reports what it took
    /// into `report`.
    template <typename Params>
    RequestResult<Params> send_stateless(const Params& params,
                                         worker::Priority priority,
                                         kota::cancellation_token cancel = {},
                                         RequestCost cost = {},
                                         StatelessReport* report = nullptr);

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
    /// slots after a cooldown, so "no capacity" is a window, not a decision.
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

    /// The process ids of the workers of the kind in service.
    std::vector<std::uint32_t> worker_pids(bool stateful) const;

    /// Background requests allowed at once now; the index feeders size
    /// their in-flight windows from it.
    std::size_t background_limit() const {
        return scheduler.background_limit(std::chrono::steady_clock::now());
    }

    /// Foreground evidence from outside the pool: didOpen/didChange/didSave
    /// and friends. Requests need no call of their own — every stateful
    /// dispatch and every High stateless dispatch counts.
    void foreground_pulse();

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
    /// acquisition no longer describes the current occupant.
    enum class SlotState : std::uint8_t {
        /// Process running and accepting requests.
        Alive,
        /// Process declared dead (transport failure, a kill, or an observed
        /// exit); monitor_worker has not delivered its result yet.
        Dying,
        /// A respawn is scheduled, possibly sleeping out a backoff delay.
        Respawning,
        /// Intentionally scaled down; vacant until scale-up refills it.
        Retired,
        /// Crash budget exhausted; vacant until revival or scale-up.
        Dead,
    };

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

        /// True when this worker is being intentionally shut down (scale-down),
        /// as opposed to an unexpected crash.
        bool retiring = false;

        /// True when the pool killed this worker on purpose — for memory, or
        /// past an ignored cancel: respawn immediately, without crash
        /// accounting.
        bool preempted = false;

        /// Consecutive fast deaths that count against the slot's budget
        /// (see process_crash); resets after healthy uptime.
        unsigned crash_streak = 0;

        /// The death record of the current incarnation, fresh at every
        /// spawn; senders hold it across their await.
        std::shared_ptr<WorkerDeath> death = std::make_shared<WorkerDeath>();

        /// Stateful only: the documents the dead incarnation owned, taken
        /// off the owner table the moment the death is declared — a request
        /// routed to the corpse in the window before the crash report
        /// would fail instead of moving on — and reported with the crash.
        std::vector<std::uint32_t> lost_documents;

        std::chrono::steady_clock::time_point spawn_time{};

        /// The process's own memory as last sampled, and the part of it in
        /// swap.
        std::uint64_t memory = 0;
        std::uint64_t swapped = 0;
    };

    /// A stateless request between its submission and its end: what the
    /// scheduler's actions reach it by.
    struct PendingStateless {
        WorkerPool& pool;
        RequestId id;

        /// Fired by the scheduler's Cancel: cancels the request over IPC.
        kota::cancellation_source preempt;

        /// Signalled by the scheduler's Dispatch, or to observe that the
        /// request can never run (pool stopped, all slots given up, its
        /// sender cancelled it while it waited).
        kota::event ready{};

        /// The slot and its generation the Dispatch named.
        std::optional<std::size_t> slot;
        unsigned generation = 0;

        bool finished = false;

        PendingStateless(WorkerPool& pool, RequestId id) : pool(pool), id(id) {
            pool.pending[id] = this;
        }

        PendingStateless(const PendingStateless&) = delete;
        PendingStateless& operator=(const PendingStateless&) = delete;

        /// Its end, however it came: the scheduler forgets it and frees its
        /// slot. Returns the peak of its worker's own memory while it ran.
        std::uint64_t finish();

        ~PendingStateless() {
            if(!finished) {
                finish();
            }
        }
    };

    /// A request in flight on a slot, registered for the span of its send:
    /// the crash tag a death is matched against, and whether it is a build
    /// (see worker::is_build), which picks the deadline a kill reports.
    struct Inflight {
        WorkerPool& pool;
        RequestId id;
        std::string tag;
        bool build;

        Inflight(WorkerPool& pool, RequestId id, std::string tag, bool build) :
            pool(pool), id(id), tag(std::move(tag)), build(build) {
            pool.flights[id] = this;
        }

        Inflight(const Inflight&) = delete;
        Inflight& operator=(const Inflight&) = delete;

        ~Inflight() {
            pool.flights.erase(id);
        }
    };

    WorkerProcess& slot(std::size_t index, bool stateful) {
        return stateful ? stateful_workers[index] : stateless_workers[index];
    }

    /// The error a request that died with its worker reports: worker_crashed
    /// when the death names this very request; out_of_memory when the
    /// worker's memory ran out; worker_lost when it names another, names
    /// none while other requests were in flight, or the request ignored a
    /// cancel; worker_died when it names none and this request ran alone.
    static kota::ipc::Error death_error(const WorkerDeath& death,
                                        llvm::StringRef tag,
                                        kota::codec::dyn::Value identity);

    static SchedulerTime now() {
        return std::chrono::steady_clock::now();
    }

    /// Carry out the scheduler's actions, those they lead to included.
    void execute();

    void kill_worker(const scheduler::Kill& kill);

    /// Close the idle stateless worker's pipe and terminate it; its exit
    /// retires the slot.
    void retire_worker(std::size_t index);

    /// Returns the worker owning path_id, assigning the least-loaded live
    /// worker on first use. SIZE_MAX when no stateful worker is alive, or
    /// once the pool stops: its workers are still Alive then, but their
    /// links are closed, and a request failing on one would read as a
    /// nameless death and be blamed. An owner is always alive: a death
    /// takes its documents off the table.
    std::size_t assign_worker(std::uint32_t path_id);
    std::size_t pick_least_loaded();

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

    /// Wake every queued stateless request: it observes that it can never run.
    void fail_pending_requests();

    /// Declare a worker's process dead right now: bump the generation, drop
    /// the peer reference, record what the scheduler knew of it in its
    /// death and tell the scheduler. Idempotent — safe to call from a
    /// failed send before the monitor observed the exit.
    /// `kill_process` must be true when death is declared ahead of the
    /// result (failed send, a kill) so monitor_worker's proc.wait() is
    /// guaranteed to deliver; the monitor itself passes false — its process
    /// was already reaped, and signalling the freed pid could hit an
    /// unrelated process.
    void mark_worker_dead(std::size_t index, bool stateful, bool kill_process);

    /// Handle a crash result: log/relay, update crash accounting, fire
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
    /// accounting and on a kill on purpose — such a slot must not carry a
    /// stale streak past the healthy interval that would have cleared it.
    void reset_streak_if_healthy(WorkerProcess& w);

    /// Permanently vacate a slot and fail waiters if it was the last one.
    void give_up_slot(std::size_t index, bool stateful);

    /// The pool's pace: samples memory and the workers for the scheduler.
    kota::task<> monitor_loop();

    /// Take a sample and hand it to the scheduler.
    void sample();

    /// What the workers have had moved out to swap between two samples
    /// that tells memory is short: below it, the kernel trims pages nothing
    /// touches.
    constexpr static std::uint64_t swap_threshold = 64ull << 20;

    /// Whether the worker holds work, and with it memory: a request in
    /// flight, or documents (whose last one can close while its compile
    /// still runs).
    bool holds_work(const WorkerProcess& w, std::size_t index, bool stateful) const;

    /// Rank each worker for the kernel's OOM killer (Linux): one holding a
    /// request or documents just above the master, which holds every
    /// session; an idle one at the master's own score, so that killing it —
    /// which frees nothing — is never the killer's first choice.
    void tick_oom_scores(std::uint64_t memory_limit);

    /// Write a worker's OOM score: oom_holding when it holds work, else
    /// the master's. Taking on work raises it and a stateless release
    /// lowers it at once, since a compile can exhaust memory before the
    /// next sample.
    void set_oom_score(const WorkerProcess& w, bool holds);

    /// Start another stateless process, in a vacant slot when there is one.
    bool scale_up_worker();

    /// SIGKILL any worker still alive after the SIGTERM grace period, so a
    /// wedged worker can't block stop()'s join forever.
    kota::task<> kill_stragglers();

    struct SpawnedProcess {
        kota::process proc;
        std::shared_ptr<kota::ipc::BincodePeer> peer;
        std::shared_ptr<StderrTail> stderr_tail;
    };

    /// Launch the process of a slot's incarnation and start its IO pumps.
    /// Shared by initial spawn, scale-up, and respawn.
    std::optional<SpawnedProcess> spawn_process(std::size_t index,
                                                bool stateful,
                                                unsigned generation);

    /// The IO pump, owning a peer reference so the peer outlives its own
    /// run() no matter when the slot drops its copy. The incarnation dies
    /// when its link ends.
    kota::task<> run_peer(std::shared_ptr<kota::ipc::BincodePeer> peer,
                          std::size_t index,
                          bool stateful,
                          unsigned generation);

    /// Append a new slot (initial start and scale-up).
    bool spawn_worker(bool stateful);

    /// Refill an existing slot with a fresh process.
    bool respawn_worker(std::size_t index, bool stateful);

    /// The slot entered service: tell the scheduler and the waiters.
    void serving(std::size_t index, bool stateful);

    /// After `revive_after`, grant a given-up slot a fresh crash budget
    /// and respawn it: the pool never stays at zero workers forever.
    kota::task<> revive_slot(std::size_t index, bool stateful);

    void install_evict_handler(WorkerProcess& worker, std::size_t index);

    /// Hand the worker's gate parks (a test gate, see BuildProbe::gate) to
    /// the probe.
    void install_gate_handler(WorkerProcess& worker);

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
                                             RequestCost cost,
                                             StatelessReport* report,
                                             bool& sent);

    friend struct testing::WorkerPoolFixture;

private:
    kota::event_loop& loop;

    llvm::SmallVector<WorkerProcess> stateless_workers;

    llvm::SmallVector<WorkerProcess> stateful_workers;

    /// Every decision of which request runs where and which process dies.
    WorkerScheduler scheduler;

    /// execute() is carrying out actions: one that leads to more leaves
    /// them to its loop.
    bool executing = false;

    /// The stateless requests between submission and end, by id.
    llvm::DenseMap<RequestId, PendingStateless*> pending;

    /// The requests in flight, by id.
    llvm::DenseMap<RequestId, const Inflight*> flights;

    RequestId next_request = 1;

    /// Set whenever a slot (re)enters service, and at stop; await_capacity
    /// waits on it.
    kota::event capacity_returned;

    // Stateful routing: each open document (path_id) is pinned to one worker.
    llvm::DenseMap<std::uint32_t, std::size_t> owner;  // path_id -> worker index

    std::size_t stateful_inflight = 0;

    std::size_t high_inflight = 0;

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
};

/// The build a worker ran to its end, as the probe counts it: its kind and
/// file. The request reached the worker, and it answered or died running
/// it; a build the master withdrew — preempted, superseded — comes back
/// cancelled whatever the worker had done, or as a compile the worker
/// stopped. None for the other requests.
template <typename Params, typename Result>
std::optional<std::pair<BuildKind, llvm::StringRef>> ran_build(const Params& params,
                                                               const Result& result,
                                                               bool sent) {
    if(!sent || (!result.has_value() && result.error().code == worker::dispatch_errc::cancelled)) {
        return std::nullopt;
    }
    if constexpr(std::same_as<Params, worker::CompileParams>) {
        if(result.has_value() && result->status == worker::CompileStatus::Cancelled) {
            return std::nullopt;
        }
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
    if(auto build = ran_build(params, result, sent); build && probe) {
        co_await probe->returned(build->first, build->second);
    }
    co_return std::move(result);
}

template <typename Params>
RequestResult<Params> WorkerPool::send_stateless(const Params& params,
                                                 worker::Priority priority,
                                                 kota::cancellation_token cancel,
                                                 RequestCost cost,
                                                 StatelessReport* report) {
    bool sent = false;
    auto result =
        co_await dispatch_stateless(params, priority, std::move(cancel), cost, report, sent);
    if(auto build = ran_build(params, result, sent); build && probe) {
        co_await probe->returned(build->first, build->second);
    }
    co_return std::move(result);
}

template <typename Params>
RequestResult<Params> WorkerPool::dispatch_stateful(std::uint32_t path_id,
                                                    const Params& params,
                                                    kota::ipc::request_options opts,
                                                    bool& sent) {
    stateful_inflight += 1;

    struct InflightGuard {
        std::size_t& count;

        ~InflightGuard() {
            count -= 1;
        }
    } inflight_guard{stateful_inflight};

    auto idx = assign_worker(path_id);
    if(idx == SIZE_MAX) {
        scheduler.user_activity(now());
        execute();
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
    auto id = next_request;
    next_request += 1;
    Inflight inflight(*this, id, worker::crash_tag(params), worker::is_build<Params>);
    // Every stateful request is user-facing: it counts as activity, and is
    // timed while it flies.
    scheduler.stateful_begin(id, idx, path_id, worker::is_build<Params>, now());
    execute();

    struct Timed {
        WorkerPool& pool;
        RequestId id;

        ~Timed() {
            pool.scheduler.stateful_end(id, now());
        }
    } timed{*this, id};

    auto gate = probe ? probe->sending(inflight.tag, peer) : std::nullopt;
    auto result = co_await peer->send_request(params, opts);
    if(gate) {
        probe->answered(*gate);
    }
    if(result.has_value() || !worker::is_transport_error(result.error()))
        co_return std::move(result);

    // The worker link broke mid-request. Declare the slot dead now so
    // follow-up requests fail fast instead of piling onto a corpse;
    // monitor_worker reconciles with the real exit status.
    if(stateful_workers[idx].generation == gen)
        mark_worker_dead(idx, true, true);
    co_await death->done.wait();
    co_await kota::fail(death_error(*death, inflight.tag, worker::death_identity(idx, gen, true)));
}

template <typename Params>
RequestResult<Params> WorkerPool::dispatch_stateless(const Params& params,
                                                     worker::Priority priority,
                                                     kota::cancellation_token cancel,
                                                     RequestCost cost,
                                                     StatelessReport* report,
                                                     bool& sent) {
    using worker::dispatch_errc::cancelled;
    using worker::dispatch_errc::worker_unavailable;
    if(priority == worker::Priority::High) {
        high_inflight += 1;
    }

    struct HighGuard {
        WorkerPool& pool;
        bool high;

        ~HighGuard() {
            if(high) {
                pool.high_inflight -= 1;
            }
        }
    } high_guard{*this, priority == worker::Priority::High};

    while(true) {
        if(stop_scope.cancelled() || !has_future_capacity() || cancel.cancelled()) {
            co_await kota::fail(
                cancel.cancelled()
                    ? kota::ipc::Error{cancelled, "Request cancelled by its round"}
                    : kota::ipc::Error{worker_unavailable, "No stateless workers available"});
        }

        auto id = next_request;
        next_request += 1;
        PendingStateless request(*this, id);
        // The caller's cancel: a queued request is dropped and its waiter
        // woken, a running one cancelled over IPC and killed past the grace
        // period.
        auto relay = cancel.on_cancel([this, &request] {
            scheduler.cancel(request.id, now());
            execute();
            if(!request.slot) {
                request.ready.set();
            }
        });
        scheduler.submit(id, priority, cost, worker::is_build<Params>, now());
        execute();
        if(!request.slot) {
            co_await request.ready.wait();
        }
        if(!request.slot) {
            continue;
        }
        auto idx = *request.slot;
        // The worker died between the dispatch and this resumption; the
        // scheduler forgot the request with it — go around again.
        if(stateless_workers[idx].generation != request.generation) {
            continue;
        }
        // Asked to stop before it reached the worker: giving the slot back
        // costs nothing.
        if(request.preempt.cancelled()) {
            co_await kota::fail(kota::ipc::Error{cancelled, "Request preempted by the scheduler"});
        }

        auto peer = stateless_workers[idx].peer;
        auto gen = stateless_workers[idx].generation;
        auto death = stateless_workers[idx].death;
        sent = true;
        Inflight inflight(*this, id, worker::crash_tag(params), worker::is_build<Params>);
        auto gate = probe ? probe->sending(inflight.tag, peer) : std::nullopt;
        auto result = co_await peer->send_request(params, {.token = request.preempt.token()});
        if(gate) {
            probe->answered(*gate);
        }
        // The worker link broke mid-request: declare the slot dead now so a
        // caller-side retry cannot arrive at the same corpse before the
        // monitor observed the exit.
        bool transport_dead = !result.has_value() && worker::is_transport_error(result.error());
        if(transport_dead && stateless_workers[idx].generation == gen) {
            mark_worker_dead(idx, false, true);
            execute();
        }

        // A cancel comes back as whatever the worker answered —
        // RequestCancelled, the result it had already, or the killed
        // process's transport error: the request awaits the real answer so
        // the slot frees only once the process is actually idle again. Every
        // shape is reported as cancelled, so the indexer requeues instead of
        // recording a failure — unless the worker's memory ran out before it
        // stopped, or it never stopped, which its sender learns of.
        if(request.preempt.cancelled() && !death->out_of_memory && !death->ignored_cancel) {
            co_await kota::fail(kota::ipc::Error{cancelled, "Request preempted by the scheduler"});
        }
        // An error returned by the worker's handler leaves the worker
        // healthy; pass it through untouched.
        if(result.has_value() || !transport_dead) {
            auto peak = request.finish();
            if(report) {
                report->peak = peak;
            }
            co_return std::move(result);
        }

        co_await death->done.wait();
        co_await kota::fail(
            death_error(*death, inflight.tag, worker::death_identity(idx, gen, false)));
    }
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
/// took the attempt along (worker_lost), the death named no request
/// (worker_died) or the worker's memory ran out (out_of_memory), and waits
/// out capacity windows (worker_unavailable). One resend suffices: the
/// culprit of the first death is barred from running again by then. A
/// request that dies naming no request twice, alone on its worker both
/// times, is taken for the killer — an OOM kill, a stack overflow the
/// crash report cannot run on.
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
        if((died || code == errc::worker_lost || code == errc::out_of_memory) && !resent) {
            resent = true;
            nameless = died;
            continue;
        }
        co_return std::move(result);
    }
}

}  // namespace clice
