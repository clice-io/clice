module clice:worker.pool;

import :support.logging;
import :support.prelude;
import :support.signal;
import :worker.protocol;
import :worker.residency;
import :worker.resources;
import :worker.scheduler;
import :worker.test_hooks;

namespace clice {

namespace testing {

struct WorkerPoolFixture;

}

using ipc::RequestResult;

/// How one worker incarnation died, shared with every request in flight on
/// it. A sender whose link broke waits for the monitor to mark the record
/// done — the exit observed, the stderr drained — and only then learns whether
/// its own request is the one that killed the worker.
struct WorkerDeath {
    async::event done;

    /// The crash tags (worker::crash_tag) of the request the worker named in
    /// its last words, or of the ones that outlived the pool's deadline;
    /// empty when nothing names one — an outside kill, a crash outside any
    /// request.
    llvm::SmallVector<std::string, 1> named;

    /// How it died, worded for the user ("signal 11 (SIGSEGV)").
    std::string cause;

    /// The worker's memory ran out: the pool killed it to free memory, or
    /// an allocation failed in it. None of its requests is charged with it.
    bool out_of_memory = false;

    /// The pool killed the worker because its request ignored a cancel past
    /// the grace period. The request is not charged with it, but the run counts as
    /// lost: a free requeue would repeat it for as long as the user keeps
    /// preempting it.
    bool ignored_cancel = false;

    /// The worker's own memory as last sampled, or the most its stateless
    /// request reached.
    std::uint64_t memory = 0;

    /// A stateless worker's request was the only request running: one whose
    /// memory ran out alone does not fit this machine.
    bool alone = false;

    /// Requests in flight when the worker died. A death that names no
    /// request is held against a request only when it ran alone: among
    /// several, nothing tells which one killed the worker.
    std::size_t in_flight = 0;

    /// Stateful only: the documents assigned to it, taken off the
    /// residency table the moment the death is declared — a request routed
    /// to the corpse in the window before the crash report would fail
    /// instead of moving on.
    std::size_t documents = 0;
};

/// What a worker's stderr said of its death, read as the lines come: the
/// exit is observed independently, often before the last of them.
struct StderrReport {
    /// The crash tag the dying worker wrote after worker::crashed_in_marker.
    std::string crashed_in;

    /// The worker wrote that an allocation failed.
    bool out_of_memory = false;

    /// The pipe reached EOF: the death's report waits for it.
    async::event drained;
};

struct WorkerPoolOptions {
    /// The program workers run as `<program> worker`: this program itself
    /// when empty.
    std::string program;

    /// Stateless workers started, and the most that background (Low) work
    /// keeps busy; only interactive work that finds no idle slot grows the
    /// pool past it, up to max_stateless.
    std::uint32_t stateless_count = 2;
    std::uint32_t stateful_count = 2;
    std::string session_dir;

    /// A slot is given up once it crashes more than this many times in a
    /// row. The streak resets after `healthy_uptime` of stable operation,
    /// so the budget bounds crash loops, not lifetime bad luck.
    unsigned max_crash_streak = 3;

    /// Base delay for exponential respawn backoff. The first crash of a
    /// streak respawns immediately; each further one doubles the delay,
    /// capped at 16x the base.
    chrono::milliseconds respawn_backoff{500};

    /// Uptime after which a worker is considered healthy and its crash
    /// streak resets.
    chrono::milliseconds healthy_uptime{30'000};

    /// Cooldown before a slot that gave up is revived with a fresh crash
    /// budget. The pool must never stay at zero workers forever: the
    /// documents' failure records stop the poison, and this bounds how long
    /// a fully burnt pool stays dark.
    chrono::milliseconds revive_after{30'000};

    /// A request running past its deadline is taken for a hung worker: the
    /// worker is killed and every request of it past its deadline charged,
    /// like a crash. A build may take long on a big translation unit (see
    /// worker::is_build); a query never should. A stateless request is
    /// timed by its worker's CPU time, a stateful one by the time since its
    /// worker began its work (see SchedulerOptions).
    chrono::milliseconds build_deadline{chrono::minutes(10)};

    chrono::milliseconds query_deadline{chrono::minutes(2)};

    /// A stateless request is also taken for hung once its worker's CPU
    /// time stands still this long.
    chrono::milliseconds stall_deadline{chrono::minutes(5)};

    /// The documents the stateful workers keep compiled at once; unset
    /// leaves it to the machine's memory (default_max_resident).
    std::optional<std::size_t> max_resident;

    /// Dynamic scaling bounds for stateless workers.
    /// min_stateless: floor — never scale down below this count.
    /// max_stateless: ceiling — never spawn above this count (0 = auto = the
    /// physical cores, see default_max_stateless_worker_count).
    std::uint32_t min_stateless = 1;
    std::uint32_t max_stateless = 0;
};

/// How a stateless request is sent (WorkerPool::send_stateless).
struct StatelessOptions {
    worker::Priority priority = worker::Priority::High;

    /// Emitted when the user comes to wait for a Low request: it becomes
    /// High, queued or running.
    Signal<>* raised = nullptr;

    /// An advisory cancellation: when it fires, the request is cancelled
    /// over IPC and the send KEEPS awaiting the worker's real answer — the
    /// slot frees only when the process is actually idle, and crash
    /// accounting keeps observing the real reply. A cancelled result is
    /// reported as dispatch_errc::cancelled.
    async::cancellation_token cancel = {};

    /// What the request is expected to take.
    RequestCost cost = {};

    /// Where a finished request puts the most of its worker's own memory
    /// while it ran.
    std::uint64_t* reached = nullptr;

    /// Asked once the request has its slot, right before it reaches the
    /// worker: a request that waited in the queue while a request like it
    /// crashed is not sent, and reports dispatch_errc::cancelled.
    llvm::function_ref<bool()> admitted = {};
};

class WorkerPool;

/// A request's hold on an open document: while one lives, the pool evicts
/// none of the document's AST, a worker's death notwithstanding.
class [[nodiscard]] DocumentHold {
public:
    DocumentHold(WorkerPool& pool, std::uint32_t path_id);

    DocumentHold(const DocumentHold&) = delete;
    DocumentHold& operator=(const DocumentHold&) = delete;

    ~DocumentHold();

private:
    WorkerPool& pool;

    std::uint32_t path_id;
};

/// Multi-process scheduler for clice worker processes.
///
/// Two kinds of workers are managed:
///   - Stateless workers execute independent build tasks (PCH/PCM builds,
///     indexing, completion, formatting); which runs when is the
///     WorkerScheduler's decision (worker/scheduler.cppm), which the pool
///     carries out.
///   - Stateful workers keep per-document ASTs; a document compiled is
///     assigned to one worker, the one holding the fewest documents, and
///     the pool keeps at most max_resident ASTs, evicting the least recently
///     used one nobody holds (Residency).
///
/// Responsibility contract — the pool is mechanism, callers are policy:
///
///   - The pool owns PROCESSES and CAPACITY: spawn, monitor, respawn with
///     backoff, per-slot crash budget (streak, healthy-uptime reset),
///     give-up -> cooldown revival, and the scheduler's actions: dispatch,
///     cooperative cancels, kills for memory and deadlines, scaling. Its
///     guarantee: capacity is never permanently zero, and the user is told
///     when every worker of a kind is given up for its cooldown.
///   - The pool ATTRIBUTES every death. A dying worker names the request it
///     was running (see worker/crash_report.cppm), or the deadline watchdog
///     names every one past its deadline it killed the worker for; each
///     request in flight learns whether it is among them. A death that
///     names a request is that request's content's doing and spends no slot
///     budget; neither does a terminated session (SIGTERM, SIGINT, SIGHUP)
///     or a SIGKILL (the OOM killer, a user) of a worker holding no work.
///     Every other death naming no request counts toward the streak.
///   - The pool NEVER retries a request. Requests do not survive a crash;
///     slots do. Retry policy is semantic and lives with the caller —
///     deliver() is the shared form of it.
///   - The dispatch_errc taxonomy is the contract language. worker_crashed:
///     this request killed its worker — the caller charges its content
///     (Error::data carries the dead incarnation's identity so one death is
///     never charged twice, the message how it died). worker_lost: another
///     request's crash took it along, or the death named none among several
///     requests in flight — not its doing. worker_died: the death named no
///     request and this one ran alone. out_of_memory: the worker's memory
///     ran out — not its doing, Error::data says how much it reached and
///     whether it ran the only request running.
///     worker_unavailable: never dispatched, a capacity window —
///     await_capacity() tells when it closes. cancelled: deliberate
///     preemption, requeue freely.
class WorkerPool {
public:
    WorkerPool(async::event_loop& loop) : loop(loop), scheduler(SchedulerOptions{}) {}

    /// Spawn all worker processes. Returns false on failure.
    bool start(const WorkerPoolOptions& opts);

    /// Gracefully stop all workers.
    async::task<> stop();

    /// Send a request to the stateful worker the document is assigned to,
    /// holding the document while it flies. A compile assigns it if it is
    /// not; a query of a document whose AST no worker holds — gone with its
    /// worker since its compile — fails as worker_lost without being sent.
    template <typename Params>
    RequestResult<Params> send_stateful(std::uint32_t path_id,
                                        const Params& params,
                                        ipc::request_options opts = {});

    /// Send a request to a stateless worker, placed by the scheduler.
    template <typename Params>
    RequestResult<Params> send_stateless(const Params& params, StatelessOptions options = {});

    /// Whether a stateful worker holds the document's AST.
    bool resident(std::uint32_t path_id) const {
        return residency.resident(path_id);
    }

    /// A request about the document arrived: whatever answers it, the
    /// editor shows the document, which is evicted the later for it.
    void used(std::uint32_t path_id) {
        residency.used(path_id);
    }

    /// The workers log into `dir` from now on: those running are told, those
    /// started later are given it.
    void log_to(std::string dir);

    /// The document closed or moved to another project: its worker drops
    /// its AST, and a compile of it still running leaves none behind.
    void forget_document(std::uint32_t path_id);

    /// Whether a worker of the kind serves or will with no request asking:
    /// one alive and staying, dying, respawning, or given up until its
    /// revival. None will in a pool with no slot of the kind — the command
    /// line's has no stateful worker.
    bool will_serve(bool stateful) const;

    /// Wait until a worker of the kind can take a request. False when none
    /// ever will again: the pool is stopping, or none will serve.
    /// FIXME: takes no cancellation: a request its client cancels while every
    /// worker of the kind is down still waits here until one returns, up to
    /// revive_after.
    async::task<bool> await_capacity(bool stateful);

    /// Alive stateless slots that also accept new work: a slot scaling
    /// down stays alive to finish its request but is skipped by dispatch.
    std::size_t schedulable_stateless() const;

    /// The process ids of the workers of the kind in service.
    std::vector<std::uint32_t> worker_pids(bool stateful) const;

    /// Background requests allowed at once now; the index feeders size
    /// their in-flight windows from it.
    std::size_t background_limit() const {
        return scheduler.background_limit(chrono::steady_clock::now());
    }

    /// The user did something the pool does not see: didOpen, didChange,
    /// didSave and friends. Requests need no call of their own — every
    /// stateful dispatch and every High stateless dispatch counts.
    void user_activity();

    /// The death `error` reports, which named no request, was its request's
    /// doing after all (see deliver): like a death that names its request,
    /// it spends none of its slot's budget.
    void exempt_death(const ipc::Error& error);

    /// The server's test hooks while project.test_hooks is on, null
    /// otherwise.
    TestHooks* hooks = nullptr;

private:
    friend class DocumentHold;

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
        /// A process is on its way: a respawn, possibly sleeping out a
        /// backoff delay, a revival or a scale-up.
        Respawning,
        /// Intentionally scaled down; vacant until a scale-up refills it.
        ScaledDown,
        /// Crash budget exhausted; vacant until its revival, a cooldown
        /// later, and nothing else brings it back.
        GivenUp,
    };

    struct WorkerProcess {
        void fresh_budget() {
            crash_streak = 0;
            counted_deaths.clear();
        }

        /// Alive and staying: it takes requests.
        bool serves() const {
            return state == SlotState::Alive && !scaling_down;
        }

        async::process proc;

        /// Shared with every coroutine that awaits on it (request senders,
        /// the IO pump), so a slot can drop its reference the moment the
        /// process dies without racing in-flight users.
        std::shared_ptr<ipc::BincodePeer> peer;

        /// Written by the stderr drain, read by the crash report.
        std::shared_ptr<StderrReport> stderr_report = std::make_shared<StderrReport>();

        /// Display name for logging, e.g. "SL-0" or "SF-1"; its log file is
        /// named after it.
        std::string name;

        SlotState state = SlotState::Respawning;

        /// Bumped on every death; see SlotState.
        unsigned generation = 0;

        /// True when this worker is being intentionally shut down (scale-down),
        /// as opposed to an unexpected crash.
        bool scaling_down = false;

        /// True when the pool killed this worker on purpose — for memory, or
        /// past an ignored cancel: respawn immediately, without crash
        /// accounting.
        bool killed_on_purpose = false;

        /// Consecutive fast deaths that count against the slot's budget
        /// (see process_crash); resets after healthy uptime.
        unsigned crash_streak = 0;

        /// The incarnations (generations) whose deaths the streak counts, so
        /// a death found later to be its request's doing gives its count
        /// back (exempt_death).
        llvm::SmallVector<unsigned, 4> counted_deaths;

        /// The death record of the current incarnation, fresh at every
        /// spawn; senders hold it across their await.
        std::shared_ptr<WorkerDeath> death = std::make_shared<WorkerDeath>();

        chrono::steady_clock::time_point spawn_time{};

        /// Where the current incarnation's lines start in the slot's log
        /// file, which every incarnation appends to.
        std::uint64_t log_offset = 0;

        /// The process's own memory as last sampled.
        std::uint64_t memory = 0;

        /// The part of it in swap.
        std::uint64_t swapped = 0;

        /// Given up: when its cooldown ends.
        chrono::steady_clock::time_point revive_at{};
    };

    /// A stateless request between its submission and its end: what the
    /// scheduler's actions reach it by.
    struct PendingStateless {
        WorkerPool& pool;

        RequestId id;

        /// Fired by the scheduler's Cancel: cancels the request over IPC.
        async::cancellation_source preempt;

        /// Signalled by the scheduler's Dispatch, or to observe that the
        /// request can never run (pool stopped, all slots given up, its
        /// sender cancelled it while it waited).
        async::event ready{};

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

    llvm::SmallVector<WorkerProcess>& workers(bool stateful) {
        return stateful ? stateful_workers : stateless_workers;
    }

    const llvm::SmallVector<WorkerProcess>& workers(bool stateful) const {
        return stateful ? stateful_workers : stateless_workers;
    }

    WorkerProcess& slot(std::size_t index, bool stateful) {
        return workers(stateful)[index];
    }

    /// The error a request that died with its worker reports: worker_crashed
    /// when the death names this very request; out_of_memory when the
    /// worker's memory ran out; worker_lost when it names another, names
    /// none while other requests were in flight, or the request ignored a
    /// cancel; worker_died when it names none and this request ran alone.
    static ipc::Error death_error(const WorkerDeath& death,
                                  llvm::StringRef tag,
                                  codec::dyn::Value identity);

    /// What of a crashed worker's log the master's log quotes, of what the
    /// dead incarnation wrote from `from` on.
    struct CrashOutput {
        /// The last few kilobytes of its lines before the crash.
        llvm::StringRef lines;

        /// The crash's line and stack trace, their first kilobytes; empty
        /// when it wrote none, as a killed process cannot.
        llvm::StringRef trace;
    };

    static CrashOutput crash_output(llvm::StringRef log, std::size_t from);

    static SchedulerTime now() {
        return chrono::steady_clock::now();
    }

    /// Carry out the scheduler's actions, those they lead to included.
    void execute();

    void kill_worker(const scheduler::Kill& kill);

    /// Close the idle stateless worker's pipe and terminate it; its exit
    /// scales the slot down.
    void scale_down_worker(std::size_t index);

    /// The document's assignment, or a new one to the live worker holding
    /// the fewest documents. None when no stateful worker is alive. An
    /// assigned worker is always alive: a death takes its documents off the
    /// table.
    std::optional<Residency::Assignment> assign_worker(std::uint32_t path_id, llvm::StringRef path);

    /// Tell the workers to drop the ASTs the residency table evicts.
    void evict_over_limit();

    void send_evict(Residency::Eviction eviction);

    /// The slots of the kind that serve or are about to: alive and staying,
    /// dying, or respawning. A given-up slot serves only after its
    /// cooldown.
    std::size_t capacity(bool stateful) const;

    /// Stateless slots holding a process or owed one: all but the scaled
    /// down. A given-up slot counts, as its revival brings it back.
    std::size_t stateless_processes() const;

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

    /// Handle a crash result: crash accounting, then the log and the
    /// relay of the dead incarnation's last output. Returns true if the
    /// worker should be respawned; on false the slot has been given up.
    bool process_crash(std::size_t index, bool stateful, std::int64_t exit_code, int exit_signal);

    /// Bring a slot that is Respawning back after `delay`, retrying with
    /// growing backoff if the spawn itself fails; gives the slot up once the
    /// streak is exhausted.
    async::task<> respawn_after(std::size_t index, bool stateful, chrono::milliseconds delay);

    /// Backoff before the respawn for the given crash streak: immediate for
    /// the first crash, exponential from the second on.
    chrono::milliseconds backoff_delay(unsigned crash_streak) const;

    /// A worker that ran healthily for a while starts a fresh streak: the
    /// budget bounds crash loops, not lifetime bad luck. Called on crash
    /// accounting and on a kill on purpose — such a slot must not carry a
    /// stale streak past the healthy interval that would have cleared it.
    void reset_streak_if_healthy(WorkerProcess& w);

    /// Vacate a slot for the cooldown, after which its revival brings it
    /// back; fail the stateless waiters, and tell the user, when no slot
    /// of the kind is left to serve.
    void give_up_slot(std::size_t index, bool stateful);

    /// Grant the given-up slots whose cooldown is over a fresh crash budget
    /// and respawn them: the pool never stays at zero workers forever.
    void revive_due();

    /// The pool's pace: samples memory and the workers for the scheduler,
    /// and revives the slots due.
    async::task<> monitor_loop();

    /// Take a sample and hand it to the scheduler.
    void sample();

    /// Whether the worker holds work, and with it memory: a request in
    /// flight, or documents (whose last one can close while its compile
    /// still runs).
    bool holds_work(std::size_t index, bool stateful) const;

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

    /// Start another stateless process, in a scaled-down slot when there
    /// is one.
    void scale_up_worker();

    /// SIGKILL any worker still alive after the SIGTERM grace period, so a
    /// wedged worker can't block stop()'s join forever.
    async::task<> kill_stragglers();

    /// Start the process of a slot's next incarnation: its IO pumps, its
    /// monitor, and its entry into service. The one way a slot gets a
    /// process; false when the spawn failed.
    bool launch(std::size_t index, bool stateful);

    /// The IO pump, owning a peer reference so the peer outlives its own
    /// run() no matter when the slot drops its copy. The incarnation dies
    /// when its link ends.
    async::task<> run_peer(std::shared_ptr<ipc::BincodePeer> peer,
                           std::size_t index,
                           bool stateful,
                           unsigned generation);

    async::task<> monitor_worker(std::size_t index, bool stateful);

    /// Send a request to the current incarnation of the slot, registered as
    /// in flight while it is; a stateful worker is told the id its start
    /// report names it by first. When the link breaks under it, the
    /// incarnation is declared dead at once, so follow-up requests fail
    /// fast instead of piling onto a corpse.
    template <typename Params>
    RequestResult<Params> send_to(RequestId id,
                                  std::size_t index,
                                  bool stateful,
                                  const Params& params,
                                  ipc::request_options opts);

    /// The sends without the test hooks, which hold a reply only once its
    /// dispatch released the worker.
    template <typename Params>
    RequestResult<Params> dispatch_stateful(std::uint32_t path_id,
                                            const Params& params,
                                            ipc::request_options opts);

    template <typename Params>
    RequestResult<Params> dispatch_stateless(const Params& params, StatelessOptions options);

    friend struct testing::WorkerPoolFixture;

private:
    /// What the workers have had moved out to swap between two samples
    /// that tells memory is short: below it, the kernel trims pages nothing
    /// touches.
    constexpr static std::uint64_t swap_threshold = 64ull << 20;

    async::event_loop& loop;

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
    async::event capacity_returned;

    /// Which documents' ASTs the stateful workers keep; the limit is set at
    /// start.
    Residency residency{default_max_resident(0)};

    /// Cancelled by stop(). Unwinds monitor_loop() and respawn backoff sleeps
    /// at their poll points, and lets monitor_worker() attribute the
    /// SIGTERM-induced exits that follow to intentional shutdown (no anomaly,
    /// no respawn).
    async::cancellation_source stop_scope;

    /// All long-lived pool coroutines: monitor_worker() exit observers, the
    /// peer IO pumps and stderr drains, respawn tasks, and the monitor loop.
    /// Never cancelled as a group — stop() joins it, so shutdown waits until
    /// every worker process actually exited and its final output (crash
    /// stacktraces, sanitizer reports) was drained to EOF.
    async::task_group<> worker_tasks;

    WorkerPoolOptions options;

    /// The master's own oom_score_adj, which workers inherit; unset where
    /// there is none to adjust (not Linux).
    std::optional<int> oom_base;

    /// The score of a worker holding work, just above the master as the
    /// last tick measured it; the highest until the first tick.
    int oom_holding = 1000;
};

template <typename Params>
RequestResult<Params> WorkerPool::send_stateful(std::uint32_t path_id,
                                                const Params& params,
                                                ipc::request_options opts) {
    auto result = co_await dispatch_stateful(path_id, params, std::move(opts));
    if(hooks) {
        co_await hooks->replied(params, result);
    }
    co_return std::move(result);
}

template <typename Params>
RequestResult<Params> WorkerPool::send_stateless(const Params& params, StatelessOptions options) {
    auto result = co_await dispatch_stateless(params, std::move(options));
    if(hooks) {
        co_await hooks->replied(params, result);
    }
    co_return std::move(result);
}

template <typename Params>
RequestResult<Params> WorkerPool::send_to(RequestId id,
                                          std::size_t index,
                                          bool stateful,
                                          const Params& params,
                                          ipc::request_options opts) {
    // Own a peer reference across the await: the slot drops its copy the
    // moment the worker dies.
    auto& w = slot(index, stateful);
    auto peer = w.peer;
    auto generation = w.generation;
    Inflight inflight(*this, id, worker::crash_tag(params), worker::is_build<Params>);
    if(stateful) {
        peer->send_notification(worker::NextRequestParams{id});
    }
    auto result = hooks ? co_await hooks->send(peer, params, std::move(opts))
                        : co_await peer->send_request(params, std::move(opts));
    if(!result.has_value() && worker::is_transport_error(result.error()) &&
       slot(index, stateful).generation == generation) {
        mark_worker_dead(index, stateful, true);
    }
    co_return std::move(result);
}

template <typename Params>
RequestResult<Params> WorkerPool::dispatch_stateful(std::uint32_t path_id,
                                                    const Params& params,
                                                    ipc::request_options opts) {
    // Once the pool stops its workers are still Alive, but their links are
    // closed: a request failing on one would read as a nameless death and
    // be charged with it.
    constexpr bool compile = std::same_as<Params, worker::CompileParams>;
    std::optional<Residency::Assignment> assignment;
    if(!stop_scope.cancelled()) {
        if constexpr(compile) {
            assignment = assign_worker(path_id, params.path);
        } else if(residency.resident(path_id)) {
            assignment = residency.assignment(path_id);
        } else {
            co_await async::fail(ipc::Error{worker::dispatch_errc::worker_lost,
                                            "The document's AST died with its worker"});
        }
    }
    if(!assignment) {
        user_activity();
        co_await async::fail(
            ipc::Error{worker::dispatch_errc::worker_unavailable, "No stateful workers available"});
    }
    DocumentHold flight(*this, path_id);
    residency.used(path_id);

    auto index = assignment->worker;
    auto generation = stateful_workers[index].generation;
    auto death = stateful_workers[index].death;
    auto id = next_request;
    next_request += 1;
    // Every stateful request is user-facing: it counts as activity, and is
    // timed while it runs.
    scheduler.stateful_begin(id, index, worker::is_build<Params>, now());
    execute();

    struct Timed {
        WorkerPool& pool;

        RequestId id;

        ~Timed() {
            pool.scheduler.stateful_end(id, now());
        }
    } timed{*this, id};

    auto result = co_await send_to(id, index, true, params, std::move(opts));
    if constexpr(compile) {
        residency.compiled(path_id,
                           assignment->id,
                           result.has_value() && result->status == worker::CompileStatus::Done);
    }
    if(result.has_value() || !worker::is_transport_error(result.error())) {
        co_return std::move(result);
    }
    co_await death->done.wait();
    co_await async::fail(death_error(*death,
                                     worker::crash_tag(params),
                                     worker::death_identity(index, generation, true)));
}

template <typename Params>
RequestResult<Params> WorkerPool::dispatch_stateless(const Params& params,
                                                     StatelessOptions options) {
    using worker::dispatch_errc::cancelled;
    using worker::dispatch_errc::worker_unavailable;
    auto priority = options.priority;
    while(true) {
        if(stop_scope.cancelled() || capacity(false) == 0 || options.cancel.cancelled()) {
            co_await async::fail(
                options.cancel.cancelled()
                    ? ipc::Error{cancelled, "Request cancelled by its attempt"}
                    : ipc::Error{worker_unavailable, "No stateless workers available"});
        }

        auto id = next_request;
        next_request += 1;
        PendingStateless request(*this, id);
        // The caller's cancel: a queued request is dropped and its waiter
        // woken, a running one cancelled over IPC and killed past the grace
        // period.
        auto relay = options.cancel.on_cancel([this, &request] {
            scheduler.cancel(request.id, now());
            execute();
            if(!request.slot) {
                request.ready.set();
            }
        });
        // The user came to wait for it: it runs as foreground work, and so
        // does a resubmission.
        Signal<>::Connection raise;
        if(options.raised) {
            raise = options.raised->connect([this, &request, &priority] {
                priority = worker::Priority::High;
                scheduler.raise(request.id, now());
                execute();
            });
        }
        scheduler.submit(id, priority, options.cost, worker::is_build<Params>, now());
        execute();
        if(!request.slot) {
            co_await request.ready.wait();
        }
        if(!request.slot) {
            continue;
        }
        auto index = *request.slot;
        // The worker died between the dispatch and this resumption; the
        // scheduler forgot the request with it — go around again.
        if(stateless_workers[index].generation != request.generation) {
            continue;
        }
        // Asked to stop before it reached the worker: giving the slot back
        // costs nothing.
        if(request.preempt.cancelled()) {
            co_await async::fail(ipc::Error{cancelled, "more urgent work came first"});
        }
        if(options.admitted && !options.admitted()) {
            co_await async::fail(ipc::Error{cancelled, "a crash stopped it"});
        }

        auto death = stateless_workers[index].death;
        auto result =
            co_await send_to(id, index, false, params, {.token = request.preempt.token()});

        // A cancel comes back as whatever the worker answered —
        // RequestCancelled, the result it had already, or the killed
        // process's transport error: the request awaits the real answer so
        // the slot frees only once the process is actually idle again. Every
        // shape is reported as cancelled, so the indexer requeues instead of
        // recording a failure — unless the worker's memory ran out before it
        // stopped, or it never stopped, which its sender learns of.
        // FIXME: a request whose own crash meets a cancel reads as cancelled too:
        // its death record is not done yet, so it is requeued uncharged and crashes
        // its worker once more.
        if(request.preempt.cancelled() && !death->out_of_memory && !death->ignored_cancel) {
            co_await async::fail(ipc::Error{cancelled, "more urgent work came first"});
        }
        // An error returned by the worker's handler leaves the worker
        // healthy; pass it through untouched.
        if(result.has_value() || !worker::is_transport_error(result.error())) {
            auto peak = request.finish();
            if(options.reached) {
                *options.reached = peak;
            }
            co_return std::move(result);
        }

        co_await death->done.wait();
        co_await async::fail(death_error(*death,
                                         worker::crash_tag(params),
                                         worker::death_identity(index, request.generation, false)));
    }
}

/// Send a request through the worker deaths that are not its own doing.
/// `send` makes one attempt (and may fail on its own to stop early, a
/// outdated request); deliver resends once when another request's crash
/// took the attempt along (worker_lost), the death named no request
/// (worker_died) or the worker's memory ran out (out_of_memory), and waits
/// out capacity windows (worker_unavailable). One resend suffices: the
/// request the first death named is stopped from running again by then. A
/// request that dies naming no request twice, alone on its worker both
/// times, is taken for the killer — an OOM kill, a stack overflow the
/// crash report cannot run on.
///
/// `crashed` fires once, when the request is found to have killed its
/// worker: its own crash, or the second worker_died, whose two deaths then
/// spend no slot budget. The result is the last attempt's.
template <typename Send, typename Crashed>
auto deliver(WorkerPool& pool, bool stateful, Send send, Crashed crashed)
    -> std::invoke_result_t<Send&> {
    namespace errc = worker::dispatch_errc;
    bool resent = false;
    std::optional<ipc::Error> nameless;
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
            if(died) {
                pool.exempt_death(*nameless);
                pool.exempt_death(result.error());
            }
            crashed(result.error());
            co_return std::move(result);
        }
        if((died || code == errc::worker_lost || code == errc::out_of_memory) && !resent) {
            resent = true;
            if(died) {
                nameless = result.error();
            }
            continue;
        }
        co_return std::move(result);
    }
}

}  // namespace clice
