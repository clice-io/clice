module;

#include "kotatsu/ipc/framing.h"

module clice;

import :support.anomaly;
import :support.logging;
import :support.process;
import :vfs.path;
import :worker.pool;
import :worker.resources;
import :worker.test_hooks;

namespace clice {

namespace {

/// Coroutine that drains a worker's stderr pipe.
/// Workers write their own log files, so this only captures unexpected output
/// (crash stacktraces, assertion failures, sanitizer reports, etc.).
async::task<> drain_stderr(async::pipe stderr_pipe,
                           std::string prefix,
                           std::shared_ptr<StderrTail> tail) {
    while(true) {
        auto read = co_await stderr_pipe.read_line();
        if(!read.has_value() || !read.value()) {
            break;
        }
        auto& line = *read.value();
        if(line.starts_with(worker::crashed_in_marker)) {
            tail->crashed_in = line.substr(worker::crashed_in_marker.size());
        }
        // LLVM's handler of a failed allocation, before it aborts.
        if(line.starts_with("LLVM ERROR: out of memory")) {
            tail->out_of_memory = true;
        }
        if(!line.empty()) {
            LOG_WARN("{} {}", prefix, line);
            tail->add(std::move(line));
        }
    }
    tail->drained.set();
}

/// How a worker died, worded for the user.
std::string describe_exit(std::int64_t exit_code, int exit_signal) {
    async::process::exit_status exit{.code = exit_code, .term_signal = exit_signal};
    if(exit_signal != 0) {
        return "killed by " + exit.to_string();
    }
#ifdef _WIN32
    // A Windows process that crashed exits with the NTSTATUS of its
    // exception.
    if(static_cast<std::uint32_t>(exit_code) >= 0x8000'0000) {
        return "terminated by " + exit.to_string();
    }
#endif
    return std::format("exited with code {}", exit_code);
}

}  // namespace

std::vector<std::uint32_t> WorkerPool::worker_pids(bool stateful) const {
    std::vector<std::uint32_t> pids;
    for(auto& w: workers(stateful)) {
        if(w.state == SlotState::Alive) {
            pids.push_back(static_cast<std::uint32_t>(w.proc.pid()));
        }
    }
    return pids;
}

std::size_t WorkerPool::schedulable_stateless() const {
    // A stopped pool serves nothing more, though its slots read Alive until
    // the workers are reaped: a dispatcher counting them would feed its
    // whole queue into instant failures.
    if(stop_scope.cancelled()) {
        return 0;
    }
    return ranges::count_if(stateless_workers, &WorkerProcess::serves);
}

std::size_t WorkerPool::capacity(bool stateful) const {
    return ranges::count_if(workers(stateful), [](const WorkerProcess& w) {
        return w.serves() || w.state == SlotState::Dying || w.state == SlotState::Respawning;
    });
}

std::size_t WorkerPool::stateless_processes() const {
    return ranges::count_if(stateless_workers, [](const WorkerProcess& w) {
        return w.state != SlotState::ScaledDown;
    });
}

bool WorkerPool::launch(std::size_t index, bool stateful) {
    auto& w = slot(index, stateful);
    async::process::options opts;
    // Asked at every spawn: on macOS it fails once an update has replaced
    // the file this program runs from.
    opts.program = options.program;
    if(opts.program.empty()) {
        if(auto error = sys::self_executable(opts.program)) {
            LOG_ANOMALY(WorkerSpawnFail,
                        "Failed to spawn worker {}: cannot find this program: {}",
                        w.name,
                        error.message());
            return false;
        }
        // On Linux the program is /proc/self/exe, which ps would show.
        if(auto& arguments = sys::arguments(); !arguments.empty()) {
            opts.arg0 = arguments.front();
        }
    }
    opts.args = {"worker"};
    if(stateful) {
        opts.args.push_back("--stateful");
    }
    opts.args.push_back("--worker-name");
    opts.args.push_back(w.name);
    std::uint64_t log_offset = 0;
    if(!options.log_dir.empty()) {
        opts.args.push_back("--log-dir");
        opts.args.push_back(options.log_dir);
        if(auto status = fs::status(path::join(options.log_dir, w.name + ".log"))) {
            log_offset = status->size;
        }
    }
    // A worker lives and dies with its master: an orphan answers no one and
    // nothing stops it when it hangs. Its own group keeps a terminal's
    // Ctrl-C and SIGHUP for the master, which stops it. macOS has no way to
    // tie a child to its parent's exit: the worker watches the master there.
    opts.creation.process_group = true;
    opts.creation.exit_with_parent = can_exit_with_parent;
    opts.streams = {
        async::process::stdio::pipe(),
        async::process::stdio::pipe(),
        async::process::stdio::pipe(),
    };

    auto spawned = async::process::spawn(opts, loop);
    if(!spawned) {
        LOG_ANOMALY(WorkerSpawnFail,
                    "Failed to spawn worker {}: {}",
                    w.name,
                    spawned.error().message());
        return false;
    }

    // StreamTransport: input = child's stdout (parent reads), output = child's
    // stdin (parent writes).
    auto transport = std::make_unique<ipc::StreamTransport>(std::move(spawned->stdout_pipe),
                                                            std::move(spawned->stdin_pipe));
    transport->set_remote_max_payload(ipc::default_max_payload);
    w.peer = std::make_shared<ipc::BincodePeer>(loop, std::move(transport));
    if(stateful) {
        w.peer->on_notification([this](const worker::StartedParams& params) {
            scheduler.stateful_started(params.id, now());
        });
    } else {
        w.peer->on_notification([this, index](const worker::PeakParams& params) {
            scheduler.reported_peak(index, params.memory);
        });
    }
    w.proc = std::move(spawned->proc);
    w.stderr_tail = std::make_shared<StderrTail>();
    w.state = SlotState::Alive;
    w.scaling_down = false;
    w.killed_on_purpose = false;
    w.memory = 0;
    w.swapped = 0;
    w.spawn_time = chrono::steady_clock::now();
    w.log_offset = log_offset;
    w.death = std::make_shared<WorkerDeath>();
    // The generation was bumped at the death of the incarnation before;
    // the crash streak carries across until a healthy uptime resets it.

    worker_tasks.spawn(
        drain_stderr(std::move(spawned->stderr_pipe), "[" + w.name + "]", w.stderr_tail));
    worker_tasks.spawn(run_peer(w.peer, index, stateful, w.generation));
    worker_tasks.spawn(monitor_worker(index, stateful));
    if(!stateful) {
        scheduler.worker_ready(index, now());
        execute();
    }
    capacity_returned.set();
    return true;
}

async::task<> WorkerPool::run_peer(std::shared_ptr<ipc::BincodePeer> peer,
                                   std::size_t index,
                                   bool stateful,
                                   unsigned generation) {
    co_await peer->run();
    // The worker closed its end, as it does once it dies: no request may be
    // dispatched to it in the while before its exit is observed.
    auto& w = slot(index, stateful);
    if(!stop_scope.cancelled() && w.generation == generation && w.serves()) {
        mark_worker_dead(index, stateful, true);
    }
}

bool WorkerPool::start(const WorkerPoolOptions& opts) {
    options = opts;
    auto max_resident = options.max_resident.value_or(default_max_resident(memory_state({}).limit));
    residency = Residency(max_resident);

    if(options.max_stateless == 0)
        options.max_stateless = default_max_stateless_worker_count();
    options.max_stateless = std::max(options.max_stateless, options.stateless_count);
    if(options.min_stateless == 0)
        options.min_stateless = 1;
    // The configured floor is honored even above the startup count: idle
    // scale-down must not shrink a scaled-up pool below it. Only the
    // ceiling bounds it.
    options.min_stateless = std::min(options.min_stateless, options.max_stateless);
    scheduler = WorkerScheduler({
        .background_jobs = options.stateless_count,
        .min_workers = options.min_stateless,
        .max_workers = options.max_stateless,
        .build_deadline = options.build_deadline,
        .query_deadline = options.query_deadline,
        .stall_deadline = options.stall_deadline,
    });

    if(auto adj = sys::oom_score_adjustment()) {
        oom_base = *adj;
    }

    // The scheduler admits background work by memory from the first
    // sample on.
    sample();
    for(bool stateful: {false, true}) {
        auto& slots = workers(stateful);
        for(std::uint32_t i = 0; i < (stateful ? options.stateful_count : options.stateless_count);
            i += 1) {
            slots.push_back({.name = std::format("{}-{}", stateful ? "SF" : "SL", slots.size())});
            if(!launch(slots.size() - 1, stateful)) {
                return false;
            }
        }
    }

    worker_tasks.spawn(monitor_loop());

    LOG_INFO("WorkerPool started: {} stateless, {} stateful workers keeping {} documents compiled",
             stateless_workers.size(),
             stateful_workers.size(),
             max_resident);
    return true;
}

async::task<> WorkerPool::stop() {
    LOG_INFO("WorkerPool stopping...");
    stop_scope.cancel();
    fail_pending_requests();
    capacity_returned.set();

    // Dying slots were already SIGKILLed; vacant slots have no process.
    for(bool stateful: {false, true}) {
        for(auto& w: workers(stateful)) {
            if(w.peer) {
                w.peer->close_output();
            }
            if(w.state == SlotState::Alive) {
                w.proc.kill(SIGTERM);
            }
        }
    }

    // A wedged worker that ignores SIGTERM would otherwise block the join
    // below forever; escalate after a grace period.
    async::task_group<> watchdog;
    watchdog.spawn(kill_stragglers());

    co_await worker_tasks.join();
    watchdog.cancel();
    co_await watchdog.join();

    LOG_INFO("WorkerPool stopped");
}

async::task<> WorkerPool::kill_stragglers() {
    // Time is the feature: the grace a worker gets after SIGTERM.
    co_await async::sleep(chrono::milliseconds(5000), loop);
    LOG_WARN("Workers still alive 5s after SIGTERM; escalating to SIGKILL");
    for(bool stateful: {false, true}) {
        for(auto& w: workers(stateful)) {
            if(w.state == SlotState::Alive) {
                w.proc.kill();
            }
        }
    }
}

std::optional<Residency::Assignment> WorkerPool::assign_worker(std::uint32_t path_id,
                                                               llvm::StringRef path) {
    if(auto assignment = residency.assignment(path_id)) {
        return assignment;
    }
    std::optional<std::size_t> fewest;
    for(std::size_t i = 0; i < stateful_workers.size(); i += 1) {
        if(stateful_workers[i].state == SlotState::Alive &&
           (!fewest || residency.documents_on(i) < residency.documents_on(*fewest))) {
            fewest = i;
        }
    }
    if(!fewest) {
        return std::nullopt;
    }
    set_oom_score(stateful_workers[*fewest], true);
    return residency.assign(path_id, *fewest, path);
}

void WorkerPool::forget_document(std::uint32_t path_id) {
    if(auto eviction = residency.forget(path_id)) {
        send_evict(std::move(*eviction));
    }
}

void WorkerPool::evict_over_limit() {
    for(auto& eviction: residency.evictions()) {
        LOG_DEBUG("Evicting {} from {}", eviction.path, stateful_workers[eviction.worker].name);
        send_evict(std::move(eviction));
    }
}

void WorkerPool::send_evict(Residency::Eviction eviction) {
    auto& w = stateful_workers[eviction.worker];
    w.peer->send_notification(worker::EvictParams{std::move(eviction.path)});
}

DocumentHold::DocumentHold(WorkerPool& pool, std::uint32_t path_id) : pool(pool), path_id(path_id) {
    pool.residency.hold(path_id);
}

DocumentHold::~DocumentHold() {
    pool.residency.release(path_id);
    pool.evict_over_limit();
}

void WorkerPool::mark_worker_dead(std::size_t index, bool stateful, bool kill_process) {
    auto& w = slot(index, stateful);
    if(w.state != SlotState::Alive)
        return;

    w.state = SlotState::Dying;
    w.generation += 1;
    w.death->in_flight = scheduler.running_on(index, stateful);
    w.death->memory = w.memory;
    if(!stateful) {
        // Its memory may have fallen since the request's peak.
        if(auto reached = scheduler.reached(index)) {
            w.death->memory = reached;
        }
        w.death->alone = scheduler.runs_alone(index);
    }
    if(stateful) {
        w.death->documents = residency.worker_died(index);
    }
    w.peer->close();
    w.peer.reset();
    if(kill_process) {
        // Make sure the process is really gone so monitor_worker's
        // proc.wait() is guaranteed to deliver a result.
        w.proc.kill();
    }
    scheduler.worker_gone(index, stateful, now());
    execute();
}

async::task<> WorkerPool::monitor_worker(std::size_t index, bool stateful) {
    // Every exit marks this incarnation's record done, shutdown included:
    // senders whose link broke are parked on it.
    struct MarkDone {
        std::shared_ptr<WorkerDeath> death;

        ~MarkDone() {
            death->done.set();
        }
    } mark_done{slot(index, stateful).death};

    auto result = co_await slot(index, stateful).proc.wait();

    if(stop_scope.cancelled())
        co_return;

    if(!stateful) {
        scheduler.worker_exited(index);
    }

    // A scale-down: the slot becomes permanently vacant without crash
    // processing.
    if(!stateful && stateless_workers[index].scaling_down) {
        auto& w = stateless_workers[index];
        LOG_INFO("Worker {} scaled down", w.name);
        w.state = SlotState::ScaledDown;
        w.scaling_down = false;
        w.generation += 1;
        w.peer->close();
        w.peer.reset();
        scheduler.worker_gone(index, false, now());
        execute();
        co_return;
    }

    std::int64_t exit_code = 0;
    int exit_signal = 0;
    if(result.has_value()) {
        exit_code = result.value().code;
        exit_signal = result.value().term_signal;
    } else {
        LOG_ERROR("Worker {} lost: {}", slot(index, stateful).name, result.error().message());
        exit_signal = 9;
    }

    auto& w = slot(index, stateful);
    bool killed_on_purpose = w.killed_on_purpose;
    mark_worker_dead(index, stateful, false);
    w.killed_on_purpose = false;

    if(killed_on_purpose) {
        // The pool killed this worker on purpose; the work it lost was
        // signalled to its sender. Bring the capacity back immediately,
        // without touching the crash budget.
        LOG_INFO("Worker {} killed on purpose, respawning", w.name);
        w.state = SlotState::Respawning;
        mark_done.death->done.set();
        worker_tasks.spawn(respawn_after(index, stateful, chrono::milliseconds(0)));
        co_return;
    }

    // The final diagnostic, the crash marker that names the request among
    // it, can still sit in the stderr pipe when the exit is observed: the
    // drain reaches EOF first. The slot is already dying, so a shutdown
    // meanwhile skips its reaped pid.
    auto drained = [](std::shared_ptr<StderrTail> tail) -> async::task<> {
        co_await tail->drained.wait();
    };
    // Hang detection: a short bound runs out on a stalled master (a
    // loaded machine), and the crash then names no request.
    co_await async::with_timeout(drained(w.stderr_tail), chrono::minutes(1), loop);
    if(stop_scope.cancelled())
        co_return;

    bool restart = process_crash(index, stateful, exit_code, exit_signal);
    // The dead incarnation's senders learn what became of their requests
    // before a replacement takes the work waiting in the queue: a crash they
    // book stops the requests like theirs among it.
    mark_done.death->done.set();
    if(restart) {
        auto& respawning = slot(index, stateful);
        respawning.state = SlotState::Respawning;
        worker_tasks.spawn(respawn_after(index, stateful, backoff_delay(respawning.crash_streak)));
    }
}

void WorkerPool::reset_streak_if_healthy(WorkerProcess& w) {
    if(chrono::steady_clock::now() - w.spawn_time >= options.healthy_uptime) {
        w.fresh_budget();
    }
}

void WorkerPool::exempt_death(const ipc::Error& error) {
    // The identity worker::death_identity wrote: "sf" or "sl", the slot,
    // the incarnation.
    llvm::SmallVector<llvm::StringRef, 3> parts;
    llvm::StringRef(worker::death_of(error)).split(parts, ':');
    assert(parts.size() == 3 && "a worker death carries its identity");
    std::size_t index = 0;
    unsigned generation = 0;
    [[maybe_unused]] bool malformed =
        parts[1].getAsInteger(10, index) || parts[2].getAsInteger(10, generation);
    assert(!malformed);
    auto& w = slot(index, parts[0] == "sf");
    if(auto it = ranges::find(w.counted_deaths, generation); it != w.counted_deaths.end()) {
        w.counted_deaths.erase(it);
        w.crash_streak -= 1;
    }
}

bool WorkerPool::process_crash(std::size_t index,
                               bool stateful,
                               std::int64_t exit_code,
                               int exit_signal) {
    auto& w = slot(index, stateful);
    mark_worker_dead(index, stateful, false);
    auto& death = *w.death;
    auto& tail = *w.stderr_tail;
    death.out_of_memory = tail.out_of_memory;
    // An allocation failure's abort names the request it ran, whose content
    // is not charged with it.
    if(death.out_of_memory) {
        death.cause = "ran out of memory";
    } else if(death.named.empty() && !tail.crashed_in.empty()) {
        death.named.push_back(tail.crashed_in);
    }
    if(death.cause.empty()) {
        death.cause = describe_exit(exit_code, exit_signal);
    }

    // POSIX values: Windows' <csignal> does not define SIGHUP or SIGKILL,
    // and a worker can only receive them on POSIX anyway.
    constexpr int sighup = 1;
    constexpr int sigkill = 9;
    bool terminated = exit_signal == SIGTERM || exit_signal == SIGINT || exit_signal == sighup;

    reset_streak_if_healthy(w);
    // A death that names its request is that request's content's doing:
    // the caller charges the content, and the slot respawns with its streak
    // untouched, like a kill on purpose; so does running out of memory. A
    // terminated session says nothing about the slot, nor does a kill of
    // an idle worker (the OOM killer, a user). A worker killed while
    // running requests or holding documents counts: none of them is charged
    // when several were in flight, or when none was, and only the budget
    // slows a load that keeps getting killed.
    bool idle_kill = exit_signal == sigkill && death.in_flight == 0 && death.documents == 0;
    if(death.named.empty() && !death.out_of_memory && !terminated && !idle_kill) {
        w.crash_streak += 1;
        w.counted_deaths.push_back(w.generation - 1);
    }

    // Ahead of the anomaly report, which aborts Debug builds.
    if(!tail.lines.empty()) {
        LOG_ERROR("Last stderr of crashed worker {}:\n{}", w.name, llvm::join(tail.lines, "\n"));
    }
    // Relay the tail of the dead incarnation's own log into the master log
    // so CI, which only sees the master's output, still gets the worker's
    // final assert/error message. The LLVM crash backtrace itself stays in
    // the worker log: truncate the relay at the first "CRASH STACK TRACE" or
    // "Stack dump" marker so the master log carries the diagnosis, not the
    // (noisy, non-portable) stack frames.
    std::string content;
    if(!options.log_dir.empty() &&
       !fs::read_file(content, path::join(options.log_dir, w.name + ".log"))) {
        llvm::StringRef output = llvm::StringRef(content).drop_front(w.log_offset);
        for(llvm::StringRef marker: {"CRASH STACK TRACE", "Stack dump"}) {
            if(auto pos = output.find(marker); pos != llvm::StringRef::npos)
                output = output.substr(0, pos);
        }
        output = output.rtrim();

        constexpr std::size_t max_output = 4096;
        if(output.size() > max_output) {
            output = output.take_back(max_output);
            if(auto nl = output.find('\n'); nl != llvm::StringRef::npos) {
                output = output.drop_front(nl + 1);
            }
        }
        if(!output.empty())
            LOG_ERROR("Last output of crashed worker {}:\n{}", w.name, output);
    }

    if(terminated) {
        // Termination requested from outside — e.g. the editor tearing the
        // whole process group down on a hard restart. The worker did not
        // crash; don't alarm the user through the anomaly channel.
        LOG_WARN("Worker {} terminated by signal {} (crash streak: {})",
                 w.name,
                 exit_signal,
                 w.crash_streak);
    } else if(death.out_of_memory) {
        // The machine ran out, not clice.
        LOG_WARN("Worker {} ran out of memory at {} MiB", w.name, death.memory >> 20);
    } else if(exit_signal != 0) {
        LOG_ANOMALY(WorkerCrash,
                    "Worker {} killed by signal {} (crash streak: {})",
                    w.name,
                    exit_signal,
                    w.crash_streak);
    } else {
        LOG_ANOMALY(WorkerCrash,
                    "Worker {} exited with code {} (crash streak: {})",
                    w.name,
                    exit_code,
                    w.crash_streak);
    }

    if(w.crash_streak > options.max_crash_streak) {
        LOG_ERROR("Worker {} exceeded crash budget ({} consecutive), giving up",
                  w.name,
                  options.max_crash_streak);
        give_up_slot(index, stateful);
        return false;
    }
    return true;
}

ipc::Error WorkerPool::death_error(const WorkerDeath& death,
                                   llvm::StringRef tag,
                                   codec::dyn::Value identity) {
    namespace errc = worker::dispatch_errc;
    if(death.out_of_memory) {
        return ipc::Error{
            errc::out_of_memory,
            death.cause,
            codec::dyn::Value{{"memory", death.memory}, {"alone", death.alone}},
        };
    }
    auto named = !death.named.empty();
    auto code = named && ranges::contains(death.named, tag)               ? errc::worker_crashed
                : !named && death.in_flight <= 1 && !death.ignored_cancel ? errc::worker_died
                                                                          : errc::worker_lost;
    return ipc::Error{code, death.cause, std::move(identity)};
}

bool WorkerPool::will_serve(bool stateful) const {
    return capacity(stateful) > 0 ||
           ranges::contains(workers(stateful), SlotState::GivenUp, &WorkerProcess::state);
}

async::task<bool> WorkerPool::await_capacity(bool stateful) {
    while(!stop_scope.cancelled() && will_serve(stateful)) {
        if(ranges::any_of(workers(stateful), &WorkerProcess::serves)) {
            co_return true;
        }
        capacity_returned.reset();
        co_await capacity_returned.wait();
    }
    co_return false;
}

chrono::milliseconds WorkerPool::backoff_delay(unsigned crash_streak) const {
    if(crash_streak <= 1)
        return chrono::milliseconds(0);
    auto shift = std::min(crash_streak - 2, 4u);
    return options.respawn_backoff * (1u << shift);
}

async::task<> WorkerPool::respawn_after(std::size_t index,
                                        bool stateful,
                                        chrono::milliseconds delay) {
    while(true) {
        if(delay.count() > 0) {
            LOG_INFO("Worker {} respawn delayed by {}ms (crash streak {})",
                     slot(index, stateful).name,
                     delay.count(),
                     slot(index, stateful).crash_streak);
            // Time is the feature: the crash streak's backoff.
            co_await async::with_token(async::sleep(delay, loop), stop_scope.token());
        }
        if(stop_scope.cancelled())
            co_return;

        auto& w = slot(index, stateful);
        if(launch(index, stateful)) {
            LOG_INFO("Worker {} started (crash streak {})", w.name, w.crash_streak);
            co_return;
        }

        // The spawn itself failed (e.g. transient resource exhaustion):
        // treat it like another crash in the streak and back off harder.
        w.crash_streak += 1;
        if(w.crash_streak > options.max_crash_streak) {
            LOG_ERROR("Worker {} respawn failed permanently, giving up", w.name);
            give_up_slot(index, stateful);
            co_return;
        }
        delay = backoff_delay(w.crash_streak);
    }
}

void WorkerPool::give_up_slot(std::size_t index, bool stateful) {
    auto& w = slot(index, stateful);
    w.state = SlotState::GivenUp;
    w.revive_at = chrono::steady_clock::now() + options.revive_after;
    if(capacity(stateful) == 0) {
        auto seconds = chrono::duration_cast<chrono::seconds>(options.revive_after).count();
        if(stateful) {
            LOG_GUIDANCE(
                "clice's workers for open files keep crashing and are stopped for {} seconds: "
                "compiles of the open files wait until then. The server log has the crash reports",
                seconds);
        } else {
            // Queued requests fail now instead of waiting for a dispatch.
            fail_pending_requests();
            LOG_GUIDANCE(
                "clice's background workers keep crashing and are stopped for {} seconds: "
                "completion, signature help, formatting and indexing wait until then. The "
                "server log has the crash reports",
                seconds);
        }
    }
}

void WorkerPool::revive_due() {
    auto now = chrono::steady_clock::now();
    for(bool stateful: {false, true}) {
        for(auto [index, w]: views::enumerate(workers(stateful))) {
            if(w.state != SlotState::GivenUp || now < w.revive_at) {
                continue;
            }
            // A fresh budget, not a pardon: if the workload that killed the
            // slot is still around, the budget bounds the damage again and
            // the next revival is one cooldown away.
            LOG_INFO("Worker {} reviving after cooldown", w.name);
            w.fresh_budget();
            w.state = SlotState::Respawning;
            worker_tasks.spawn(
                respawn_after(static_cast<std::size_t>(index), stateful, chrono::milliseconds(0)));
        }
    }
}

std::uint64_t WorkerPool::PendingStateless::finish() {
    finished = true;
    pool.pending.erase(id);
    auto peak = pool.scheduler.finish(id, now());
    if(slot) {
        auto& w = pool.stateless_workers[*slot];
        if(w.generation == generation) {
            pool.set_oom_score(w, pool.holds_work(*slot, false));
        }
    }
    pool.execute();
    return peak;
}

void WorkerPool::user_activity() {
    scheduler.user_activity(now());
    execute();
}

void WorkerPool::fail_pending_requests() {
    // Waiters are woken without a slot; they re-check the pool state and
    // return an error.
    for(auto& [_, request]: pending) {
        if(!request->slot) {
            request->ready.set();
        }
    }
}

void WorkerPool::execute() {
    if(executing) {
        return;
    }
    executing = true;
    while(true) {
        auto actions = scheduler.take_actions();
        if(actions.empty()) {
            break;
        }
        for(auto& action: actions) {
            if(auto* dispatch = std::get_if<scheduler::Dispatch>(&action)) {
                // An earlier action of the batch may have ended the request.
                auto it = pending.find(dispatch->request);
                if(it == pending.end()) {
                    continue;
                }
                auto& request = *it->second;
                // A stopped pool serves nothing more: the request wakes to
                // fail.
                if(stop_scope.cancelled()) {
                    request.ready.set();
                    continue;
                }
                auto& w = stateless_workers[dispatch->slot];
                request.slot = dispatch->slot;
                request.generation = w.generation;
                set_oom_score(w, true);
                LOG_DEBUG("Dispatch {} to {}", dispatch->request, w.name);
                request.ready.set();
            } else if(auto* cancel = std::get_if<scheduler::Cancel>(&action)) {
                if(auto it = pending.find(cancel->request); it != pending.end()) {
                    it->second->preempt.cancel();
                }
            } else if(auto* kill = std::get_if<scheduler::Kill>(&action)) {
                kill_worker(*kill);
            } else if(std::holds_alternative<scheduler::Spawn>(action)) {
                scale_up_worker();
            } else if(auto* scale_down = std::get_if<scheduler::ScaleDown>(&action)) {
                scale_down_worker(scale_down->slot);
            }
        }
    }
    executing = false;
}

void WorkerPool::kill_worker(const scheduler::Kill& kill) {
    auto& w = slot(kill.slot, kill.stateful);
    if(w.state != SlotState::Alive) {
        return;
    }
    using scheduler::KillReason;
    if(kill.reason == KillReason::Memory) {
        LOG_WARN("Worker {} killed to free memory at {} MiB", w.name, w.memory >> 20);
        w.killed_on_purpose = true;
        w.death->out_of_memory = true;
        w.death->cause = "killed to free memory";
    } else if(kill.reason == KillReason::CancelGrace) {
        LOG_WARN("Worker {} ignored a cooperative cancel; killing it", w.name);
        w.killed_on_purpose = true;
        w.death->ignored_cancel = true;
        w.death->cause = std::format(
            "killed after ignoring a cancel for {} seconds",
            chrono::duration_cast<chrono::seconds>(WorkerScheduler::cancel_grace).count());
    } else {
        // Timed only once their sends started.
        auto* request = flights.at(kill.requests.front());
        auto limit = request->build ? options.build_deadline : options.query_deadline;
        auto seconds = [](auto duration) {
            return chrono::duration_cast<chrono::seconds>(duration).count();
        };
        if(kill.stateful) {
            w.death->cause =
                std::format("killed after running for over {} seconds", seconds(limit));
        } else if(kill.reason == KillReason::Deadline) {
            w.death->cause =
                std::format("killed after over {} seconds of CPU time", seconds(limit));
        } else {
            w.death->cause = std::format("killed after {} seconds without using the CPU",
                                         seconds(options.stall_deadline));
        }
        for(auto id: kill.requests) {
            auto& tag = flights.at(id)->tag;
            LOG_WARN("Worker {} ran {}: {}", w.name, tag, w.death->cause);
            w.death->named.push_back(tag);
        }
    }
    if(w.killed_on_purpose) {
        // The respawn skips crash accounting: credit a healthy run here the
        // same way a crash would, or the slot carries a stale streak past
        // the healthy interval that already cleared it.
        reset_streak_if_healthy(w);
    }
    mark_worker_dead(kill.slot, kill.stateful, true);
}

void WorkerPool::scale_down_worker(std::size_t index) {
    auto& w = stateless_workers[index];
    if(w.state != SlotState::Alive) {
        return;
    }
    w.scaling_down = true;
    w.peer->close_output();
    w.proc.kill(SIGTERM);
    LOG_INFO("Scaling down worker {}", w.name);
}

async::task<> WorkerPool::monitor_loop() {
    while(true) {
        // Time is the feature: the pace of the samples.
        co_await async::with_token(async::sleep(scheduler.sample_interval(), loop),
                                   stop_scope.token());
        if(stop_scope.cancelled())
            co_return;
        sample();
        revive_due();
    }
}

void WorkerPool::sample() {
    SchedulerSample sample;
    sample.stateless_processes = stateless_processes();
    std::vector<sys::ProcessStats> usage;
    std::uint64_t swapped = 0;
    for(bool stateful: {false, true}) {
        for(auto [index, w]: views::enumerate(workers(stateful))) {
            if(w.state != SlotState::Alive) {
                continue;
            }
            auto read = sys::process_stats(w.proc.pid());
            if(!read) {
                continue;
            }
            w.memory = read->memory;
            swapped += read->swapped > w.swapped ? read->swapped - w.swapped : 0;
            w.swapped = read->swapped;
            usage.push_back(*read);
            if(!stateful) {
                auto cpu = chrono::duration_cast<chrono::milliseconds>(read->user_time +
                                                                       read->system_time);
                sample.workers.push_back(
                    {.slot = static_cast<std::size_t>(index), .memory = read->memory, .cpu = cpu});
            }
        }
    }
    auto memory = memory_state(usage);
    sample.headroom = memory.headroom;
    sample.reserve = memory.reserve;
    sample.swapping = swapped > swap_threshold;
    LOG_DEBUG("Memory: {} MiB headroom, {} MiB reserve, {} stateless processes",
              memory.headroom >> 20,
              memory.reserve >> 20,
              sample.stateless_processes);
    scheduler.sample(std::move(sample), now());
    execute();
    if(memory.limit > 0) {
        tick_oom_scores(memory.limit);
    }
}

bool WorkerPool::holds_work(std::size_t index, bool stateful) const {
    return scheduler.running_on(index, stateful) > 0 ||
           (stateful && residency.documents_on(index) > 0);
}

void WorkerPool::tick_oom_scores(std::uint64_t memory_limit) {
    if(!oom_base) {
        return;
    }
    if(auto master = sys::resident_memory()) {
        // The kernel ranks rss + adj * pages / 1000 over at least
        // `memory_limit` worth of pages, so this much adj outweighs the
        // master's whole RSS without lifting the worker over anything bigger.
        auto above_master = static_cast<int>(1000 * *master / memory_limit) + 2;
        oom_holding = std::min(*oom_base + above_master, 1000);
    }
    for(bool stateful: {false, true}) {
        for(auto [index, w]: views::enumerate(workers(stateful))) {
            set_oom_score(w, holds_work(static_cast<std::size_t>(index), stateful));
        }
    }
}

void WorkerPool::set_oom_score(const WorkerProcess& w, bool holds) {
    if(!oom_base || w.state != SlotState::Alive) {
        return;
    }
    // An exited worker awaiting its reaping refuses it; the monitor is
    // about to replace it.
    sys::set_oom_score_adjustment(holds ? oom_holding : *oom_base, w.proc.pid());
}

void WorkerPool::scale_up_worker() {
    // The ceiling bounds processes, not schedulable slots: a worker scaling
    // down still holds its process until the monitor reaps it, and a
    // replacement spawned beside it would push the pool past max_stateless
    // during exactly the memory pressure that triggered the scale-down. A
    // given-up slot counts too: its revival brings it back after its
    // cooldown, which a scale-up does not cut short.
    if(stop_scope.cancelled() || stateless_processes() >= options.max_stateless) {
        return;
    }
    // A scaled-down slot is capacity already allocated: refill it instead of
    // appending a fresh slot beside it, which would grow the slot table,
    // worker names and log files by one per scale-down/scale-up cycle.
    auto vacant = ranges::find(stateless_workers, SlotState::ScaledDown, &WorkerProcess::state);
    auto index = static_cast<std::size_t>(vacant - stateless_workers.begin());
    if(vacant == stateless_workers.end()) {
        stateless_workers.push_back({.name = std::format("SL-{}", index)});
    }
    auto& w = stateless_workers[index];
    w.fresh_budget();
    w.state = SlotState::Respawning;
    LOG_INFO("Scaling up: {}", w.name);
    worker_tasks.spawn(respawn_after(index, false, chrono::milliseconds(0)));
}

}  // namespace clice
