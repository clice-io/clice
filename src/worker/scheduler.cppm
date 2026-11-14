module clice:worker.scheduler;

import :support.prelude;

namespace clice::worker {

/// Scheduling class of a stateless dispatch. Deliberately not part of the
/// IPC protocol: priority is the pool's business, passed explicitly to
/// send_stateless — the worker process never sees it.
enum class Priority : uint8_t { High, Low };

}  // namespace clice::worker

namespace clice {

/// The scheduler's clock. Its owner passes the time with every event and
/// the scheduler never reads a clock itself, so a test drives it in
/// virtual time.
using SchedulerTime = chrono::steady_clock::time_point;

/// A request, numbered by the pool.
using RequestId = std::uint64_t;

/// What a request is expected to cost.
struct RequestCost {
    /// The peak of its worker's own memory, in bytes, as the request's last
    /// finished run measured it, or one killed for memory before reached;
    /// zero when nothing is known.
    std::uint64_t peak = 0;

    /// The memory a later run reached when it was killed for memory: the
    /// peak is at least this.
    std::uint64_t floor = 0;
};

struct SchedulerOptions {
    /// Background (Low) requests at once while the user is idle.
    std::uint32_t background_jobs = 2;

    /// The stateless processes the pool keeps.
    std::uint32_t min_workers = 1;

    /// The most it grows to for foreground work.
    std::uint32_t max_workers = 2;

    /// A stateless request is taken for hung past this much of its
    /// worker's CPU time, a stateful one past this much time since its
    /// worker began its work.
    chrono::milliseconds build_deadline{chrono::minutes(10)};

    chrono::milliseconds query_deadline{chrono::minutes(2)};

    /// A stateless request is also taken for hung when its worker's CPU
    /// time stands still this long.
    chrono::milliseconds stall_deadline{chrono::minutes(5)};
};

/// One look at memory and at the pool's processes, taken by the pool.
struct SchedulerSample {
    /// The bytes the machine, or the tightest memory cgroup clice runs in,
    /// can still give before reclaim starts to hurt (worker/resources).
    std::uint64_t headroom = 0;

    /// The headroom kept free: below it background work is killed, and no
    /// background request starts unless it leaves this much.
    std::uint64_t reserve = 0;

    /// The system pushed the workers' memory out to swap since the last
    /// sample: it keeps its file cache, which the headroom counts as free,
    /// and memory is short for several background requests whatever the
    /// headroom says.
    bool swapping = false;

    /// The stateless processes the pool holds or is bringing back: in
    /// service, starting, dead and about to respawn, or given up until
    /// their cooldown ends.
    std::size_t stateless_processes = 0;
};

/// One look at a stateless worker in service, taken with a SchedulerSample.
struct WorkerSample {
    std::size_t slot = 0;

    /// The process's own memory: anonymous on Linux, its footprint on
    /// macOS, private bytes on Windows. Shared mappings (PCHs, the binary)
    /// are left out: every worker maps the same ones.
    std::uint64_t memory = 0;

    /// The process's CPU time so far.
    chrono::milliseconds cpu{0};
};

namespace scheduler {

/// Run the request on the slot's worker.
struct Dispatch {
    RequestId request = 0;

    std::size_t slot = 0;
};

/// Cancel the running request cooperatively: its worker stops at the next
/// declaration boundary and answers.
struct Cancel {
    RequestId request = 0;
};

enum class KillReason : std::uint8_t {
    /// Memory ran short: the slot's background request was the newest.
    Memory,
    /// The request ran past its deadline.
    Deadline,
    /// The stateless request's worker used no CPU for stall_deadline.
    Stalled,
    /// The request ignored a cooperative cancel past the grace period.
    CancelGrace,
};

/// Kill the slot's process.
struct Kill {
    std::size_t slot = 0;

    bool stateful = false;

    KillReason reason = KillReason::Memory;

    /// The requests the kill is for: one, or every overdue request of a
    /// stateful worker.
    std::vector<RequestId> requests;
};

/// Start another stateless process; the pool reports it with
/// worker_ready.
struct Spawn {};

/// Scale the slot's idle stateless process down.
struct ScaleDown {
    std::size_t slot = 0;
};

}  // namespace scheduler

using SchedulerAction = std::variant<scheduler::Dispatch,
                                     scheduler::Cancel,
                                     scheduler::Kill,
                                     scheduler::Spawn,
                                     scheduler::ScaleDown>;

/// Every decision of the worker pool, as a function of what it was told:
/// which queued stateless request runs where, which running one yields to
/// the user or to memory, which process dies for a deadline, how many
/// stateless processes run. It reads no clock and touches no process: the
/// pool reports events and samples, takes the actions out and carries them
/// out. Rules:
///
///   - Foreground (High) requests run in arrival order on any idle
///     worker; with none idle the newest background request is asked to
///     stop and the pool grows up to max_workers. Memory never holds or
///     kills foreground work.
///   - Background (Low) requests run in arrival order while fewer than
///     background_jobs run (one while the user is active) and memory
///     admits the next: the headroom less the growth still due from the
///     running background requests must leave its peak plus the reserve.
///     One memory holds back lets those behind it that fit go first, for
///     long_wait; then the queue waits for it. With no background request
///     running it starts at two thirds of the reserve with nothing else
///     running, a third above where it would be found too big, and at the
///     reserve beside other work, where it would be killed.
///   - Below the reserve, one background request is killed at a time, the
///     newest; the next only once that process has exited.
///   - A stateless request's deadline is its worker's CPU time; a stateful
///     one's is the time since its worker began its work, which may wait
///     behind its document's earlier requests and for a thread. One kill
///     names every overdue request of the worker.
///   - A background request foreground interest joins becomes foreground,
///     queued or running.
///
/// Invariants: a slot runs at most one stateless request; a request is
/// queued, running on one slot, or gone; at most one memory kill is in
/// flight.
class WorkerScheduler {
public:
    explicit WorkerScheduler(SchedulerOptions options) : options(options) {}

    /// A stateless request to place. `build` selects the deadline.
    void submit(RequestId id,
                worker::Priority priority,
                RequestCost cost,
                bool build,
                SchedulerTime now);

    /// Its sender cancelled it: a queued request is dropped, a running one
    /// is cancelled cooperatively, and killed if it ignores that.
    void cancel(RequestId id, SchedulerTime now);

    /// The user waits for a background request now: it becomes foreground,
    /// queued or running. A cancel already asked of it stands.
    void raise(RequestId id, SchedulerTime now);

    /// Its answer arrived, or its sender went away. Returns the peak of the
    /// worker's own memory sampled while it ran.
    std::uint64_t finish(RequestId id, SchedulerTime now);

    /// A stateful request the pool sent to `slot`: counted as foreground
    /// activity until it ends, and timed once its worker began its work
    /// (stateful_started).
    void stateful_begin(RequestId id, std::size_t slot, bool build, SchedulerTime now);
    void stateful_started(RequestId id, SchedulerTime now);
    void stateful_end(RequestId id, SchedulerTime now);

    /// The stateless slot's worker held at most `memory` of its own while
    /// its request ran, as it measured itself.
    void reported_peak(std::size_t slot, std::uint64_t memory);

    /// A stateless slot entered service: spawned, respawned or revived.
    void worker_ready(std::size_t slot, SchedulerTime now);

    /// A slot's process left service: it died, was killed or scaled down. The
    /// requests on it are gone; the pool fails their senders.
    void worker_gone(std::size_t slot, bool stateful, SchedulerTime now);

    /// A stateless slot's process exited, after it left service: the memory
    /// it held is free again.
    void worker_exited(std::size_t slot);

    /// The user did something: an edit, a request.
    void user_activity(SchedulerTime now);

    void sample(SchedulerSample sample, llvm::ArrayRef<WorkerSample> workers, SchedulerTime now);

    /// The actions decided since the last call, in order.
    std::vector<SchedulerAction> take_actions() {
        return std::exchange(actions, {});
    }

    /// Background requests allowed at once now: what the index feeders size
    /// their windows by.
    std::size_t background_limit(SchedulerTime now) const;

    /// How soon the pool should take the next sample: every second while
    /// stateless work runs or waits, as a worker grows by up to a few
    /// hundred megabytes a second.
    chrono::milliseconds sample_interval() const;

    /// Requests running on the slot, for the pool's death accounting.
    std::size_t running_on(std::size_t slot, bool stateful) const;

    /// Whether the stateless slot runs the only request running, stateless
    /// or stateful: one whose memory ran out then does not fit this machine.
    bool runs_alone(std::size_t slot) const;

    /// The most memory sampled while the stateless slot's request ran; zero
    /// when it runs none.
    std::uint64_t reached(std::size_t slot) const;

    /// The estimate of a background request whose peak is unknown: the
    /// 75th percentile of the peaks of the background requests finished so
    /// far and of the memory the ones killed for memory reached, at least
    /// default_peak until trusted_peaks are known.
    std::uint64_t unknown_peak() const;

    /// The peak assumed before anything finished, and the floor of every
    /// estimate of an unknown peak until trusted_peaks are known.
    constexpr static std::uint64_t default_peak = 1536ull << 20;

    /// The peaks that estimate an unknown one alone: fewer may all be the
    /// small units a project lists first, ahead of big ones.
    constexpr static std::size_t trusted_peaks = 3;

    /// The requests of unknown peak that run at once on a learned estimate
    /// below default_peak; those dispatched after them are estimated at
    /// default_peak. A wrong estimate shows only as they outgrow it, and
    /// the memory kills that stop them come one at a time.
    constexpr static std::size_t learned_runs = 3;

    /// The user counts as active this long after their last activity, and
    /// while a foreground request is in flight, for at most
    /// foreground_request_hold of its age: one older is either stuck or
    /// long, and must not pin the background to one forever.
    constexpr static chrono::seconds foreground_hold{10};

    constexpr static chrono::seconds foreground_request_hold{60};

    /// A cooperatively cancelled request still running after this is
    /// stuck inside one declaration: its process is killed.
    constexpr static chrono::seconds cancel_grace{10};

    /// No stateless request for this long scales one idle process down, to
    /// no fewer than min_workers; at min_workers, it starts a fresh process
    /// beside one that ran requests and holds more than replace_above.
    constexpr static chrono::seconds idle_scale_down{30};

    /// What an idle stateless process may hold of its own before a fresh
    /// one, which holds a few MiB, replaces it. After a few requests an idle
    /// worker held 58 to 104 MiB on macOS and Windows, in pages mimalloc keeps
    /// past every collect and only the end of the process frees, and 28 MiB
    /// on Linux.
    constexpr static std::uint64_t replace_above = 32ull << 20;

    /// A foreground request waiting this long with no idle worker grows the
    /// pool; a shorter wait is the cooperative cancel freeing a worker.
    constexpr static chrono::seconds starved_spawn{2};

    /// The head of the background queue memory holds back lets later
    /// requests that fit run beside the ones running for this long; then
    /// the queue drains for it. Alone, it starts once a lone request would
    /// not be killed, and a kill then shows it does not fit this machine.
    constexpr static chrono::seconds long_wait{30};

    /// How far behind the head a request that fits is looked for.
    constexpr static std::size_t backfill_depth = 16;

    /// While background work waits for memory, the figures that hold it
    /// back are logged at most this often.
    constexpr static chrono::seconds memory_log_interval{30};

    /// The only request running is killed only once the headroom falls, or
    /// is about to fall, below this share of the reserve.
    constexpr static std::uint64_t lone_share = 3;

    /// How far ahead memory is projected at the rate the running background
    /// requests grow: until the next sample and a killed process is gone.
    /// Four workers grow by up to half a gigabyte a second together, more
    /// than the reserve between two samples.
    constexpr static chrono::seconds memory_horizon{2};

private:
    struct Request {
        RequestId id = 0;

        worker::Priority priority = worker::Priority::Low;

        bool stateful = false;

        bool build = false;

        /// See RequestCost.
        std::uint64_t peak = 0;

        std::uint64_t floor = 0;

        SchedulerTime submitted;

        /// Running: its slot.
        std::optional<std::size_t> slot;

        /// Stateful: when its worker began its work.
        std::optional<SchedulerTime> started;

        /// Dispatch order, the age victims are chosen by.
        std::uint64_t order = 0;

        /// Its worker's CPU time when it started, and the last time that
        /// CPU time moved.
        std::optional<chrono::milliseconds> start_cpu;

        chrono::milliseconds last_cpu{0};

        SchedulerTime cpu_moved;

        /// The worker's memory as last sampled while it ran, when, and the
        /// most.
        std::uint64_t memory = 0;

        std::optional<SchedulerTime> sampled;

        std::uint64_t max_memory = 0;

        /// How fast its memory grew between the last two samples, in bytes
        /// a second.
        double growth = 0;

        /// When a cooperative cancel was asked; unset when none was.
        std::optional<SchedulerTime> cancel_asked;

        /// A kill was asked for its process.
        bool killed = false;
    };

    struct Slot {
        std::size_t slot = 0;

        /// A scale-down was asked: it takes no work until it is gone.
        bool scaling_down = false;

        /// It ran a request since its process started.
        bool ran = false;

        /// The request it runs.
        std::optional<RequestId> request;

        /// Last sampled.
        std::uint64_t memory = 0;
    };

    bool user_active(SchedulerTime now) const;

    std::size_t running(worker::Priority priority) const;

    /// The requests running, stateless or stateful.
    std::size_t requests_running() const;

    /// The peak a background request is expected to reach.
    std::uint64_t estimate(const Request& request) const;

    /// The running background requests of unknown peak dispatched before
    /// the request: all of them for a queued one.
    std::size_t unknown_ahead(const Request& request) const;

    /// The memory the running background requests will still take.
    std::uint64_t growth_due() const;

    /// The memory they will take within memory_horizon, at their rate,
    /// bounded by their estimates.
    std::uint64_t growth_soon() const;

    /// Whether memory admits the queued background request on `slot`.
    bool memory_admits(const Request& request, const Slot& slot) const;

    Slot* idle_slot();

    /// The idle process holding the most, the newest of equals: the one a
    /// scale-down takes.
    Slot* heaviest_idle();

    /// Stateless requests queued or running.
    bool busy() const;

    void start(Request& request, Slot& slot, SchedulerTime now);

    /// Ask the newest running background requests to stop until `count`
    /// are asked, counting those asked already.
    void cancel_newest_low(std::size_t count, SchedulerTime now);

    /// Scale down the idle stateless processes above min_workers: their memory
    /// is the cheapest to free. Returns whether any was.
    bool scale_down_idle();

    /// Kill the requests' worker; they all run on one.
    void kill(llvm::ArrayRef<const Request*> killed, scheduler::KillReason reason);

    /// Take every decision the state now calls for.
    void decide(SchedulerTime now);

    void check_memory(SchedulerTime now);

    void check_deadlines(SchedulerTime now);

    void dispatch(SchedulerTime now);

    void scale(SchedulerTime now);

private:
    SchedulerOptions options;

    std::map<RequestId, Request> requests;

    /// Queued stateless requests by priority, in arrival order.
    std::deque<RequestId> high_queue;

    std::deque<RequestId> low_queue;

    /// Stateless slots in service, by slot.
    std::map<std::size_t, Slot> slots;

    std::uint64_t next_order = 0;

    /// When the user last did something; unset until they do.
    std::optional<SchedulerTime> last_activity;

    /// When a stateless request last ran or waited.
    SchedulerTime last_busy;

    /// The slot of the memory kill in flight.
    std::optional<std::size_t> memory_kill;

    /// The head of the background queue memory holds back, and since when.
    std::optional<std::pair<RequestId, SchedulerTime>> memory_wait;

    /// When the wait for memory was last logged.
    std::optional<SchedulerTime> memory_logged;

    /// Since when a foreground request has waited with no idle worker.
    std::optional<SchedulerTime> starved_since;

    /// The pool samples once before it takes requests.
    SchedulerSample last_sample;

    /// Peaks of the finished background requests and the memory of the
    /// ones killed for memory, sorted.
    std::vector<std::uint64_t> peaks;

    std::vector<SchedulerAction> actions;
};

}  // namespace clice
