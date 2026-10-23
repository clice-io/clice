module;

#include "support/prelude.h"

#include "support/anomaly.macros.h"
#include "support/logging.macros.h"

#include "kota/ipc/framing.h"

module clice;

import :config.config;
import :support.anomaly;
import :support.logging;
import :vfs.file_system;
import :vfs.path;
import :worker.pool;
import :worker.resources;

namespace clice {

namespace {

/// Coroutine that drains a worker's stderr pipe.
/// Workers write their own log files, so this only captures unexpected output
/// (crash stacktraces, assertion failures, sanitizer reports, etc.).
kota::task<> drain_stderr(kota::pipe stderr_pipe,
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
std::string describe_exit(int exit_code, int exit_signal) {
    kota::process::exit_status exit{.status = exit_code, .term_signal = exit_signal};
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
    for(auto& w: stateful ? stateful_workers : stateless_workers) {
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
    return ranges::count_if(stateless_workers, [](const WorkerProcess& w) {
        return w.state == SlotState::Alive && !w.retiring;
    });
}

std::size_t WorkerPool::stateless_capacity() const {
    return ranges::count_if(stateless_workers, [](const WorkerProcess& w) {
        return w.state != SlotState::Dead && w.state != SlotState::Retired && !w.retiring;
    });
}

std::size_t WorkerPool::stateless_footprint() const {
    return ranges::count_if(stateless_workers, [](const WorkerProcess& w) {
        return w.state != SlotState::Dead && w.state != SlotState::Retired;
    });
}

std::optional<WorkerPool::SpawnedProcess> WorkerPool::spawn_process(std::size_t index,
                                                                    bool stateful,
                                                                    unsigned generation) {
    auto name = std::string(stateful ? "SF-" : "SL-") + std::to_string(index);
    kota::process::options opts;
    opts.file = options.self_path;
    opts.args = {options.self_path, "worker"};
    if(stateful) {
        opts.args.push_back("--stateful");
        if(options.max_documents) {
            opts.args.push_back("--max-documents");
            opts.args.push_back(std::to_string(*options.max_documents));
        }
    }
    opts.args.push_back("--worker-name");
    opts.args.push_back(name);
    opts.args.push_back("--master-pid");
    opts.args.push_back(std::to_string(llvm::sys::Process::getProcessId()));
    if(!log_dir.empty()) {
        opts.args.push_back("--log-dir");
        opts.args.push_back(log_dir);
    }

    opts.streams = {
        kota::process::stdio::pipe(true, false),  // stdin: child reads
        kota::process::stdio::pipe(false, true),  // stdout: child writes
        kota::process::stdio::pipe(false, true),  // stderr: child writes
    };

    auto result = kota::process::spawn(opts, loop);
    if(!result) {
        LOG_ANOMALY(WorkerSpawnFail,
                    "Failed to spawn worker {}: {}",
                    name,
                    result.error().message());
        return std::nullopt;
    }

    auto& spawn = *result;

    // StreamTransport: input = child's stdout (parent reads), output = child's
    // stdin (parent writes).
    auto transport = std::make_unique<ipc::StreamTransport>(std::move(spawn.stdout_pipe),
                                                            std::move(spawn.stdin_pipe));
    transport->set_remote_max_payload(ipc::default_max_payload);
    auto peer = std::make_shared<ipc::BincodePeer>(loop, std::move(transport));

    auto stderr_tail = std::make_shared<StderrTail>();
    worker_tasks.spawn(drain_stderr(std::move(spawn.stderr_pipe), "[" + name + "]", stderr_tail));
    worker_tasks.spawn(run_peer(peer, index, stateful, generation));

    return SpawnedProcess{std::move(spawn.proc), std::move(peer), std::move(stderr_tail)};
}

kota::task<> WorkerPool::run_peer(std::shared_ptr<ipc::BincodePeer> peer,
                                  std::size_t index,
                                  bool stateful,
                                  unsigned generation) {
    co_await peer->run();
    // The worker closed its end, as it does once it dies: no request may be
    // dispatched to it in the while before its exit is observed.
    auto& w = slot(index, stateful);
    if(!stop_scope.cancelled() && w.generation == generation && w.state == SlotState::Alive &&
       !w.retiring) {
        mark_worker_dead(index, stateful, true);
    }
}

void WorkerPool::install_evict_handler(WorkerProcess& worker, std::size_t index) {
    worker.peer->on_notification(
        [this, index, gen = worker.generation](const worker::EvictedParams& params) {
            // A buffered eviction from a dead peer can drain after the slot
            // respawned and reacquired the same path; matching by slot index
            // alone would unseat the new owner. A same-generation ABA (an
            // eviction of a closed copy draining after a reopen assigned the
            // path back to the same worker) is accepted: the notification
            // carries no ownership epoch, the window is one notification
            // drain, and the cost is one spurious invalidation-recompile —
            // not corrupted state.
            if(stateful_workers[index].generation != gen) {
                return;
            }
            if(on_evicted) {
                on_evicted(params.path, index);
            }
        });
}

void WorkerPool::install_gate_handler(WorkerProcess& worker) {
    worker.peer->on_notification(
        [this](const worker::GateParkedParams& params) { probe->gate_parked(params.id); });
}

bool WorkerPool::spawn_worker(bool stateful) {
    auto& workers = stateful ? stateful_workers : stateless_workers;
    auto index = workers.size();
    auto name = std::string(stateful ? "SF-" : "SL-") + std::to_string(index);

    auto spawned = spawn_process(index, stateful, 0);
    if(!spawned)
        return false;

    workers.push_back(WorkerProcess{
        .proc = std::move(spawned->proc),
        .peer = std::move(spawned->peer),
        .stderr_tail = std::move(spawned->stderr_tail),
        .name = name,
        .state = SlotState::Alive,
        .spawn_time = std::chrono::steady_clock::now(),
    });

    if(stateful)
        install_evict_handler(workers[index], index);
    install_gate_handler(workers[index]);
    worker_tasks.spawn(monitor_worker(index, stateful));
    serving(index, stateful);
    return true;
}

bool WorkerPool::respawn_worker(std::size_t index, bool stateful) {
    auto& workers = stateful ? stateful_workers : stateless_workers;
    auto& w = workers[index];
    auto spawned = spawn_process(index, stateful, w.generation);
    if(!spawned)
        return false;

    w.proc = std::move(spawned->proc);
    w.peer = std::move(spawned->peer);
    w.stderr_tail = std::move(spawned->stderr_tail);
    w.state = SlotState::Alive;
    w.owned_documents = 0;
    w.retiring = false;
    w.preempted = false;
    w.memory = 0;
    w.swapped = 0;
    w.spawn_time = std::chrono::steady_clock::now();
    w.death = std::make_shared<WorkerDeath>();
    // generation was bumped at death; crash_streak carries across restarts
    // until a healthy uptime resets it.

    if(stateful)
        install_evict_handler(w, index);
    install_gate_handler(w);
    worker_tasks.spawn(monitor_worker(index, stateful));
    serving(index, stateful);

    LOG_INFO("Worker {} restarted (crash streak {})", w.name, w.crash_streak);
    return true;
}

void WorkerPool::serving(std::size_t index, bool stateful) {
    if(!stateful) {
        scheduler.worker_ready(index, now());
        execute();
        on_stateless_capacity.emit();
    }
    capacity_returned.set();
}

bool WorkerPool::start(const WorkerPoolOptions& opts) {
    options = opts;
    log_dir = opts.log_dir;

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

#ifdef __linux__
    if(auto adj = llvm::MemoryBuffer::getFileAsStream("/proc/self/oom_score_adj")) {
        int value = 0;
        if(!(*adj)->getBuffer().trim().getAsInteger(10, value)) {
            oom_base = value;
        }
    }
#endif

    // The scheduler admits background work by memory from the first
    // sample on.
    sample();
    for(std::uint32_t i = 0; i < options.stateless_count; ++i) {
        if(!spawn_worker(false)) {
            return false;
        }
    }
    for(std::uint32_t i = 0; i < options.stateful_count; ++i) {
        if(!spawn_worker(true)) {
            return false;
        }
    }

    worker_tasks.spawn(monitor_loop());

    started = true;
    LOG_INFO("WorkerPool started: {} stateless, {} stateful workers",
             stateless_workers.size(),
             stateful_workers.size());
    return true;
}

kota::task<> WorkerPool::stop() {
    LOG_INFO("WorkerPool stopping...");
    stop_scope.cancel();
    fail_pending_requests();
    // A parked reply would hold its round, and the drain behind it, forever.
    if(probe) {
        probe->release_all();
    }
    capacity_returned.set();

    for(auto& w: stateless_workers)
        if(w.peer)
            w.peer->close_output();
    for(auto& w: stateful_workers)
        if(w.peer)
            w.peer->close_output();

    // Dying slots were already SIGKILLed; vacant slots have no process.
    for(auto& w: stateless_workers)
        if(w.state == SlotState::Alive)
            w.proc.kill(SIGTERM);
    for(auto& w: stateful_workers)
        if(w.state == SlotState::Alive)
            w.proc.kill(SIGTERM);

    // A wedged worker that ignores SIGTERM would otherwise block the join
    // below forever; escalate after a grace period.
    kota::task_group<> watchdog;
    watchdog.spawn(kill_stragglers());

    co_await worker_tasks.join();
    watchdog.cancel();
    co_await watchdog.join();

    LOG_INFO("WorkerPool stopped");
}

kota::task<> WorkerPool::kill_stragglers() {
    // Time is the feature: the grace a worker gets after SIGTERM.
    co_await kota::sleep(std::chrono::milliseconds(5000), loop);
    LOG_WARN("Workers still alive 5s after SIGTERM; escalating to SIGKILL");
    for(auto& w: stateless_workers)
        if(w.state == SlotState::Alive)
            w.proc.kill();
    for(auto& w: stateful_workers)
        if(w.state == SlotState::Alive)
            w.proc.kill();
}

std::size_t WorkerPool::assign_worker(std::uint32_t path_id) {
    if(stop_scope.cancelled()) {
        return SIZE_MAX;
    }

    auto it = owner.find(path_id);
    if(it != owner.end()) {
        return it->second;
    }

    // New assignment: pick the least-loaded live worker.
    auto selected = pick_least_loaded();
    if(selected == SIZE_MAX)
        return SIZE_MAX;
    owner[path_id] = selected;
    stateful_workers[selected].owned_documents += 1;
    set_oom_score(stateful_workers[selected], true);
    return selected;
}

std::size_t WorkerPool::pick_least_loaded() {
    std::size_t best = SIZE_MAX;
    for(std::size_t i = 0; i < stateful_workers.size(); i += 1) {
        auto& w = stateful_workers[i];
        if(w.state != SlotState::Alive)
            continue;
        if(best == SIZE_MAX || w.owned_documents < stateful_workers[best].owned_documents) {
            best = i;
        }
    }
    return best;
}

void WorkerPool::remove_owner(std::uint32_t path_id) {
    auto it = owner.find(path_id);
    if(it == owner.end())
        return;

    auto worker_idx = it->second;
    stateful_workers[worker_idx].owned_documents -= 1;
    owner.erase(it);
}

bool WorkerPool::remove_owner_from(std::uint32_t path_id, std::size_t worker_index) {
    auto it = owner.find(path_id);
    if(it == owner.end() || it->second != worker_index)
        return false;

    stateful_workers[worker_index].owned_documents -= 1;
    owner.erase(it);
    return true;
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
        for(auto& [path_id, widx]: owner) {
            if(widx == index)
                w.lost_documents.push_back(path_id);
        }
        for(auto path_id: w.lost_documents) {
            remove_owner(path_id);
        }
    }
    if(w.peer) {
        w.peer->close();
        w.peer.reset();
    }
    if(kill_process) {
        // Make sure the process is really gone so monitor_worker's
        // proc.wait() is guaranteed to deliver a result.
        w.proc.kill();
    }
    scheduler.worker_gone(index, stateful, now());
    execute();
}

kota::task<> WorkerPool::monitor_worker(std::size_t index, bool stateful) {
    auto& workers = stateful ? stateful_workers : stateless_workers;

    // Every exit marks this incarnation's record done, shutdown included:
    // senders whose link broke are parked on it.
    struct MarkDone {
        std::shared_ptr<WorkerDeath> death;

        ~MarkDone() {
            death->done.set();
        }
    } mark_done{workers[index].death};

    auto result = co_await workers[index].proc.wait();

    if(stop_scope.cancelled())
        co_return;

    if(!stateful) {
        scheduler.worker_exited(index);
    }

    // Intentional retirement (scale-down): the slot becomes permanently
    // vacant without crash processing.
    if(!stateful && workers[index].retiring) {
        auto& w = workers[index];
        LOG_INFO("Worker {} retired gracefully", w.name);
        w.state = SlotState::Retired;
        w.retiring = false;
        w.generation += 1;
        if(w.peer) {
            w.peer->close();
            w.peer.reset();
        }
        scheduler.worker_gone(index, false, now());
        execute();
        co_return;
    }

    int exit_code = 0, exit_signal = 0;
    if(result.has_value()) {
        exit_code = result.value().status;
        exit_signal = result.value().term_signal;
    } else {
        LOG_ERROR("Worker {} lost: {}", workers[index].name, result.error().message());
        exit_signal = 9;
    }

    bool preempted = workers[index].preempted;
    mark_worker_dead(index, stateful, false);
    workers[index].preempted = false;

    if(preempted) {
        // The pool killed this worker on purpose; the work it lost was
        // signalled to its sender. Bring the capacity back immediately,
        // without touching the crash budget.
        LOG_INFO("Worker {} killed on purpose, respawning", workers[index].name);
        workers[index].state = SlotState::Respawning;
        worker_tasks.spawn(respawn_after(index, stateful, std::chrono::milliseconds(0)));
        co_return;
    }

    // The final diagnostic, the crash marker that names the request among
    // it, can still sit in the stderr pipe when the exit is observed: the
    // drain reaches EOF first. The slot is already dying, so a shutdown
    // meanwhile skips its reaped pid.
    if(auto tail = workers[index].stderr_tail) {
        auto drained = [](std::shared_ptr<StderrTail> tail) -> kota::task<> {
            co_await tail->drained.wait();
        };
        // Hang detection: a short bound runs out on a stalled master (a
        // loaded machine), and the crash then blames no request.
        co_await kota::with_timeout(drained(std::move(tail)), std::chrono::minutes(1), loop);
        if(stop_scope.cancelled())
            co_return;
    }

    if(process_crash(index, stateful, exit_code, exit_signal)) {
        workers[index].state = SlotState::Respawning;
        worker_tasks.spawn(
            respawn_after(index, stateful, backoff_delay(workers[index].crash_streak)));
    }
}

void WorkerPool::reset_streak_if_healthy(WorkerProcess& w) {
    auto now = std::chrono::steady_clock::now();
    if(w.spawn_time != std::chrono::steady_clock::time_point{} &&
       now - w.spawn_time >= options.healthy_uptime) {
        w.crash_streak = 0;
    }
}

bool WorkerPool::process_crash(std::size_t index, bool stateful, int exit_code, int exit_signal) {
    auto& workers = stateful ? stateful_workers : stateless_workers;
    auto& w = workers[index];
    mark_worker_dead(index, stateful, false);
    auto& death = *w.death;
    death.out_of_memory = w.stderr_tail && w.stderr_tail->out_of_memory;

    // Ahead of the anomaly report, which aborts Debug builds.
    if(w.stderr_tail && !w.stderr_tail->lines.empty()) {
        LOG_ERROR("Last stderr of crashed worker {}:\n{}",
                  w.name,
                  llvm::join(w.stderr_tail->lines, "\n"));
    }

    // POSIX values: Windows' <csignal> does not define SIGHUP or SIGKILL,
    // and a worker can only receive them on POSIX anyway.
    constexpr int sighup = 1;
    constexpr int sigkill = 9;
    bool terminated = exit_signal == SIGTERM || exit_signal == SIGINT || exit_signal == sighup;
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

    // Relay the tail of the dead worker's own log into the master log so CI,
    // which only sees the master's output, still gets the worker's final
    // assert/error message.  The LLVM crash backtrace itself stays in the
    // worker log: truncate the relay at the first "CRASH STACK TRACE" or
    // "Stack dump" marker so the master log carries the diagnosis, not the
    // (noisy, non-portable) stack frames.
    if(!log_dir.empty()) {
        auto log_path = path::join(log_dir, w.name + ".log");
        if(auto content = vfs::read(log_path, vfs::Read::Bytes)) {
            llvm::StringRef tail = (*content)->getBuffer();
            for(llvm::StringRef marker: {"CRASH STACK TRACE", "Stack dump"}) {
                if(auto pos = tail.find(marker); pos != llvm::StringRef::npos)
                    tail = tail.substr(0, pos);
            }
            tail = tail.rtrim();

            constexpr std::size_t max_tail = 4096;
            if(tail.size() > max_tail) {
                tail = tail.take_back(max_tail);
                if(auto nl = tail.find('\n'); nl != llvm::StringRef::npos) {
                    tail = tail.drop_front(nl + 1);
                }
            }
            if(!tail.empty())
                LOG_ERROR("Last output of crashed worker {}:\n{}", w.name, tail);
        }
    }

    // An allocation failure's abort names the request it ran, whose content
    // is not to blame.
    if(death.out_of_memory) {
        death.cause = "ran out of memory";
    } else if(death.culprits.empty() && w.stderr_tail && !w.stderr_tail->crashed_in.empty()) {
        death.culprits.push_back(w.stderr_tail->crashed_in);
    }
    if(death.cause.empty()) {
        death.cause = describe_exit(exit_code, exit_signal);
    }

    reset_streak_if_healthy(w);
    // A death that names its request is that request's content's doing:
    // the caller blames the content, and the slot respawns with its streak
    // untouched, like a kill on purpose; so does running out of memory. A
    // terminated session says nothing about the slot, nor does a kill of
    // an idle worker (the OOM killer, a user). A worker killed while
    // running requests or holding documents counts: none of them is blamed
    // when several were in flight, or when none was, and only the budget
    // slows a load that keeps getting killed.
    bool idle_kill = exit_signal == sigkill && death.in_flight == 0 && w.lost_documents.empty();
    if(death.culprits.empty() && !death.out_of_memory && !terminated && !idle_kill) {
        w.crash_streak += 1;
    }

    WorkerCrashInfo info;
    info.worker_index = index;
    info.stateful = stateful;
    info.exit_code = exit_code;
    info.exit_signal = exit_signal;
    info.crash_streak = w.crash_streak;
    info.will_restart = w.crash_streak <= options.max_crash_streak;

    if(stateful) {
        info.lost_documents = std::exchange(w.lost_documents, {});
    }

    if(!info.will_restart) {
        LOG_ERROR("Worker {} exceeded crash budget ({} consecutive), giving up",
                  w.name,
                  options.max_crash_streak);
        give_up_slot(index, stateful);
    }

    if(on_crash)
        on_crash(info);

    return info.will_restart;
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
    auto named = !death.culprits.empty();
    auto code = named && ranges::contains(death.culprits, tag)            ? errc::worker_crashed
                : !named && death.in_flight <= 1 && !death.ignored_cancel ? errc::worker_died
                                                                          : errc::worker_lost;
    return ipc::Error{code, death.cause, std::move(identity)};
}

kota::task<bool> WorkerPool::await_capacity(bool stateful) {
    while(!stop_scope.cancelled()) {
        auto& workers = stateful ? stateful_workers : stateless_workers;
        auto serving = ranges::any_of(workers, [](const WorkerProcess& w) {
            return w.state == SlotState::Alive && !w.retiring;
        });
        if(serving) {
            co_return true;
        }
        auto revives = revives_slots();
        auto returning = ranges::any_of(workers, [&](const WorkerProcess& w) {
            return w.state == SlotState::Dying || w.state == SlotState::Respawning ||
                   (revives && w.state == SlotState::Dead);
        });
        if(!returning) {
            co_return false;
        }
        capacity_returned.reset();
        co_await capacity_returned.wait();
    }
    co_return false;
}

std::chrono::milliseconds WorkerPool::backoff_delay(unsigned crash_streak) const {
    if(crash_streak <= 1)
        return std::chrono::milliseconds(0);
    auto shift = std::min(crash_streak - 2, 4u);
    return options.respawn_backoff * (1u << shift);
}

kota::task<> WorkerPool::respawn_after(std::size_t index,
                                       bool stateful,
                                       std::chrono::milliseconds delay) {
    auto& workers = stateful ? stateful_workers : stateless_workers;
    while(true) {
        if(delay.count() > 0) {
            LOG_INFO("Worker {} respawn delayed by {}ms (crash streak {})",
                     workers[index].name,
                     delay.count(),
                     workers[index].crash_streak);
            // Time is the feature: the crash streak's backoff.
            co_await kota::with_token(kota::sleep(delay, loop), stop_scope.token());
        }
        if(stop_scope.cancelled())
            co_return;

        if(respawn_worker(index, stateful))
            co_return;

        // The spawn itself failed (e.g. transient resource exhaustion):
        // treat it like another crash in the streak and back off harder.
        auto& w = workers[index];
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
    auto& w = stateful ? stateful_workers[index] : stateless_workers[index];
    w.state = SlotState::Dead;
    // If this was the last slot with a future, wake all waiters so they
    // can return an error instead of hanging.
    capacity_returned.set();
    if(!stateful && !has_future_capacity()) {
        fail_pending_requests();
    }
    // Revival is a running-pool concern: unit fixtures drive slot state
    // without an event loop, and a coroutine queued on a never-run loop
    // outlives the pool.
    if(revives_slots()) {
        worker_tasks.spawn(revive_slot(index, stateful));
    }
}

kota::task<> WorkerPool::revive_slot(std::size_t index, bool stateful) {
    // Time is the feature: the cooldown of a slot given up.
    co_await kota::with_token(kota::sleep(options.revive_after, loop), stop_scope.token());
    if(stop_scope.cancelled()) {
        co_return;
    }
    auto& w = stateful ? stateful_workers[index] : stateless_workers[index];
    if(w.state != SlotState::Dead) {
        co_return;
    }
    // A fresh budget, not a pardon: if the workload that killed the slot is
    // still around, the budget bounds the damage again and the next revival
    // is one cooldown away.
    LOG_INFO("Worker {} reviving after cooldown", w.name);
    w.crash_streak = 0;
    w.state = SlotState::Respawning;
    worker_tasks.spawn(respawn_after(index, stateful, std::chrono::milliseconds(0)));
}

std::uint64_t WorkerPool::PendingStateless::finish() {
    finished = true;
    pool.pending.erase(id);
    auto peak = pool.scheduler.finish(id, now());
    if(slot) {
        auto& w = pool.stateless_workers[*slot];
        if(w.generation == generation) {
            pool.set_oom_score(w, pool.holds_work(w, *slot, false));
        }
    }
    pool.execute();
    return peak;
}

void WorkerPool::foreground_pulse() {
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
            } else if(auto* retire = std::get_if<scheduler::Retire>(&action)) {
                retire_worker(retire->slot);
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
        w.preempted = true;
        w.death->out_of_memory = true;
        w.death->cause = "killed to free memory";
    } else if(kill.reason == KillReason::CancelGrace) {
        LOG_WARN("Worker {} ignored a cooperative cancel; killing it", w.name);
        w.preempted = true;
        w.death->ignored_cancel = true;
        w.death->cause = std::format(
            "killed after ignoring a cancel for {} seconds",
            std::chrono::duration_cast<std::chrono::seconds>(WorkerScheduler::cancel_grace)
                .count());
    } else {
        // Timed only once their sends started.
        auto* request = flights.at(kill.requests.front());
        auto limit = request->build ? options.build_deadline : options.query_deadline;
        auto seconds = [](auto duration) {
            return std::chrono::duration_cast<std::chrono::seconds>(duration).count();
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
            w.death->culprits.push_back(tag);
        }
    }
    if(w.preempted) {
        // The respawn skips crash accounting: credit a healthy run here the
        // same way a crash would, or the slot carries a stale streak past
        // the healthy interval that already cleared it.
        reset_streak_if_healthy(w);
    }
    mark_worker_dead(kill.slot, kill.stateful, true);
}

void WorkerPool::retire_worker(std::size_t index) {
    auto& w = stateless_workers[index];
    if(w.state != SlotState::Alive) {
        return;
    }
    w.retiring = true;
    w.peer->close_output();
    w.proc.kill(SIGTERM);
    LOG_INFO("Retiring worker {}", w.name);
}

kota::task<> WorkerPool::monitor_loop() {
    while(true) {
        // Time is the feature: the pace of the samples.
        co_await kota::with_token(kota::sleep(scheduler.sample_interval(), loop),
                                  stop_scope.token());
        if(stop_scope.cancelled())
            co_return;
        sample();
    }
}

void WorkerPool::sample() {
    SchedulerSample sample;
    sample.stateless_processes = stateless_footprint();
    std::vector<ProcessUsage> usage;
    std::uint64_t swapped = 0;
    for(bool stateful: {false, true}) {
        auto& workers = stateful ? stateful_workers : stateless_workers;
        for(std::size_t index = 0; index < workers.size(); index += 1) {
            auto& w = workers[index];
            if(w.state != SlotState::Alive) {
                continue;
            }
            auto read = process_usage(static_cast<std::uint32_t>(w.proc.pid()));
            if(!read) {
                continue;
            }
            w.memory = read->memory;
            swapped += read->swapped > w.swapped ? read->swapped - w.swapped : 0;
            w.swapped = read->swapped;
            usage.push_back(*read);
            if(!stateful) {
                sample.workers.push_back({.slot = index, .memory = read->memory, .cpu = read->cpu});
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

bool WorkerPool::holds_work(const WorkerProcess& w, std::size_t index, bool stateful) const {
    return scheduler.running_on(index, stateful) > 0 || (stateful && w.owned_documents > 0);
}

void WorkerPool::tick_oom_scores(std::uint64_t memory_limit) {
    if(!oom_base) {
        return;
    }
    if(auto master = kota::sys::resident_memory()) {
        // The kernel ranks rss + adj * pages / 1000 over at least
        // `memory_limit` worth of pages, so this much adj outweighs the
        // master's whole RSS without lifting the worker over anything bigger.
        auto above_master = static_cast<int>(1000 * *master / memory_limit) + 2;
        oom_holding = std::min(*oom_base + above_master, 1000);
    }
    for(bool stateful: {false, true}) {
        auto& workers = stateful ? stateful_workers : stateless_workers;
        for(std::size_t index = 0; index < workers.size(); index += 1) {
            set_oom_score(workers[index], holds_work(workers[index], index, stateful));
        }
    }
}

void WorkerPool::set_oom_score(const WorkerProcess& w, bool holds) {
    if(!oom_base || w.state != SlotState::Alive) {
        return;
    }
    std::error_code ec;
    llvm::raw_fd_ostream adj(std::format("/proc/{}/oom_score_adj", w.proc.pid()),
                             ec,
                             llvm::sys::fs::OF_None);
    if(ec) {
        return;
    }
    adj << (holds ? oom_holding : *oom_base);
    adj.close();
    // An exited worker awaiting its reaping refuses the write; the monitor
    // is about to replace it.
    adj.clear_error();
}

bool WorkerPool::scale_up_worker() {
    // The ceiling bounds live processes, not schedulable slots: a retiring
    // worker still holds its process until the monitor reaps it, and a
    // replacement spawned beside it would push the pool past max_stateless
    // during exactly the memory pressure that triggered the retirement.
    if(stop_scope.cancelled() || stateless_footprint() >= options.max_stateless)
        return false;

    // A vacant slot — Dead and waiting out its revival cooldown, or Retired
    // by an earlier scale-down — is capacity already allocated; refill it
    // instead of appending a fresh slot beside it. Appending would grow the
    // slot table, worker names and log files by one per retire/scale-up
    // cycle, and for a Dead slot the cooldown revival would later fire too
    // and grow the pool past what was asked for. The pending revive_slot
    // task no-ops once the state is no longer Dead.
    auto vacant = ranges::find_if(stateless_workers, [](const WorkerProcess& w) {
        return w.state == SlotState::Dead || w.state == SlotState::Retired;
    });
    if(vacant != stateless_workers.end()) {
        auto index = static_cast<std::size_t>(vacant - stateless_workers.begin());
        auto& w = *vacant;
        w.crash_streak = 0;
        w.state = SlotState::Respawning;
        if(!respawn_worker(index, false)) {
            // Dead with a fresh cooldown revival armed, so a failed refill
            // does not orphan the slot.
            give_up_slot(index, false);
            LOG_WARN("scale_up: revive of {} failed", w.name);
            return false;
        }
        LOG_INFO("Scaled up: revived {}", w.name);
    } else if(!spawn_worker(false)) {
        LOG_WARN("scale_up: spawn_worker failed");
        return false;
    } else {
        LOG_INFO("Scaled up: spawned {}", stateless_workers.back().name);
    }
    return true;
}

}  // namespace clice
