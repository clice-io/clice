module;

#include "modules/prelude.h"

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

/// IO pump wrapper owning a peer reference, so the peer object outlives its
/// own run() coroutine no matter when the slot drops its copy.
kota::task<> run_peer(std::shared_ptr<kota::ipc::BincodePeer> peer) {
    co_await peer->run();
}

}  // namespace

std::size_t WorkerPool::alive_stateless() const {
    return std::ranges::count_if(stateless_workers, [](const WorkerProcess& w) {
        return w.state == SlotState::Alive;
    });
}

std::size_t WorkerPool::schedulable_stateless() const {
    return std::ranges::count_if(stateless_workers, [](const WorkerProcess& w) {
        return w.state == SlotState::Alive && !w.retiring;
    });
}

std::size_t WorkerPool::busy_stateless() const {
    return std::ranges::count_if(stateless_workers, [](const WorkerProcess& w) {
        return w.state == SlotState::Alive && w.busy;
    });
}

std::size_t WorkerPool::low_busy_count() const {
    return std::ranges::count_if(stateless_workers, [](const WorkerProcess& w) {
        return w.state == SlotState::Alive && w.busy && w.low_priority;
    });
}

std::size_t WorkerPool::high_busy_count() const {
    return std::ranges::count_if(stateless_workers, [](const WorkerProcess& w) {
        return w.state == SlotState::Alive && w.busy && !w.low_priority;
    });
}

std::size_t WorkerPool::stateless_capacity() const {
    return std::ranges::count_if(stateless_workers, [](const WorkerProcess& w) {
        return w.state != SlotState::Dead && w.state != SlotState::Retired && !w.retiring;
    });
}

std::size_t WorkerPool::stateless_footprint() const {
    return std::ranges::count_if(stateless_workers, [](const WorkerProcess& w) {
        return w.state != SlotState::Dead && w.state != SlotState::Retired;
    });
}

std::optional<WorkerPool::SpawnedProcess> WorkerPool::spawn_process(const std::string& name,
                                                                    bool stateful) {
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
    auto transport = std::make_unique<kota::ipc::StreamTransport>(std::move(spawn.stdout_pipe),
                                                                  std::move(spawn.stdin_pipe));
    transport->set_remote_max_payload(kota::ipc::default_max_payload);
    auto peer = std::make_shared<kota::ipc::BincodePeer>(loop, std::move(transport));

    auto stderr_tail = std::make_shared<StderrTail>();
    worker_tasks.spawn(drain_stderr(std::move(spawn.stderr_pipe), "[" + name + "]", stderr_tail));
    worker_tasks.spawn(run_peer(peer));

    return SpawnedProcess{std::move(spawn.proc), std::move(peer), std::move(stderr_tail)};
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

bool WorkerPool::spawn_worker(bool stateful) {
    auto& workers = stateful ? stateful_workers : stateless_workers;
    auto index = workers.size();
    auto name = std::string(stateful ? "SF-" : "SL-") + std::to_string(index);

    auto spawned = spawn_process(name, stateful);
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
    worker_tasks.spawn(monitor_worker(index, stateful));
    if(!stateful)
        on_stateless_capacity.emit();
    capacity_returned.set();
    return true;
}

bool WorkerPool::respawn_worker(std::size_t index, bool stateful) {
    auto& workers = stateful ? stateful_workers : stateless_workers;
    auto name = std::string(stateful ? "SF-" : "SL-") + std::to_string(index);

    auto spawned = spawn_process(name, stateful);
    if(!spawned)
        return false;

    auto& w = workers[index];
    w.proc = std::move(spawned->proc);
    w.peer = std::move(spawned->peer);
    w.stderr_tail = std::move(spawned->stderr_tail);
    w.state = SlotState::Alive;
    w.owned_documents = 0;
    w.busy = false;
    w.low_priority = false;
    w.retiring = false;
    w.preempted = false;
    w.cancel_requested_at = {};
    w.spawn_time = std::chrono::steady_clock::now();
    w.death = std::make_shared<WorkerDeath>();
    // generation was bumped at death; crash_streak carries across restarts
    // until a healthy uptime resets it.

    if(stateful)
        install_evict_handler(w, index);
    worker_tasks.spawn(monitor_worker(index, stateful));

    if(!stateful) {
        try_dispatch_pending();
        on_stateless_capacity.emit();
    }
    capacity_returned.set();

    LOG_INFO("Worker {} restarted (crash streak {})", w.name, w.crash_streak);
    return true;
}

bool WorkerPool::start(const WorkerPoolOptions& opts) {
    options = opts;
    log_dir = opts.log_dir;

    if(options.max_stateless == 0)
        options.max_stateless = default_max_stateless_worker_count();
    options.max_stateless = std::max(options.max_stateless, options.stateless_count);

    // A stateless worker can hold a whole TU's AST, a gigabyte and more for
    // heavy code. More workers than memory holds only feed the memory
    // controller's kill-and-redo cycle — or the OOM killer, which may take
    // the whole session with it — so the memory limit (a cgroup's, when
    // tighter) caps both the start and the ceiling.
    constexpr std::uint64_t worker_footprint = 1536ull * 1024 * 1024;
    auto mem = kota::sys::memory();
    auto limit = (mem.constrained > 0 && mem.constrained < mem.total) ? mem.constrained : mem.total;
    if(limit > 0) {
        auto fit = static_cast<std::uint32_t>(std::max<std::uint64_t>(1, limit / worker_footprint));
        if(fit < options.max_stateless) {
            LOG_INFO("Capping stateless workers at {} for {} MiB of memory",
                     fit,
                     limit / (1024 * 1024));
            options.max_stateless = fit;
            options.stateless_count = std::min(options.stateless_count, fit);
        }
    }

#ifdef __linux__
    if(auto adj = llvm::MemoryBuffer::getFileAsStream("/proc/self/oom_score_adj")) {
        int value = 0;
        if(!(*adj)->getBuffer().trim().getAsInteger(10, value)) {
            oom_base = value;
        }
    }
#endif

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
    if(options.min_stateless == 0)
        options.min_stateless = 1;
    // The configured floor is honored even above the startup count: idle
    // scale-down must not shrink a scaled-up pool below it. Only the
    // ceiling bounds it.
    options.min_stateless = std::min(options.min_stateless, options.max_stateless);

    low_limit = max_low_limit();

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
    auto& w = stateful ? stateful_workers[index] : stateless_workers[index];
    if(w.state != SlotState::Alive)
        return;

    w.state = SlotState::Dying;
    w.generation += 1;
    w.busy = false;
    w.low_priority = false;
    w.preempt_source.reset();
    w.death->in_flight = w.dispatches.size();
    w.dispatches.clear();
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
        // proc.wait() is guaranteed to deliver a verdict.
        w.proc.kill();
    }
}

kota::task<> WorkerPool::monitor_worker(std::size_t index, bool stateful) {
    auto& workers = stateful ? stateful_workers : stateless_workers;

    // Every exit settles this incarnation's record, shutdown included:
    // senders whose link broke are parked on it.
    struct Settle {
        std::shared_ptr<WorkerDeath> death;

        ~Settle() {
            death->settled.set();
        }
    } settle{workers[index].death};

    auto result = co_await workers[index].proc.wait();

    if(stop_scope.cancelled())
        co_return;

    // Intentional retirement (scale-down): the slot becomes permanently
    // vacant without crash processing.
    if(!stateful && workers[index].retiring) {
        auto& w = workers[index];
        LOG_INFO("Worker {} retired gracefully", w.name);
        w.state = SlotState::Retired;
        w.retiring = false;
        w.generation += 1;
        w.busy = false;
        w.low_priority = false;
        if(w.peer) {
            w.peer->close();
            w.peer.reset();
        }
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
        // The pool killed this worker on purpose to reclaim its memory; the
        // work it lost was signalled to its sender. Bring the capacity back
        // immediately, without touching the crash budget.
        LOG_INFO("Worker {} preempted for memory pressure, respawning", workers[index].name);
        workers[index].state = SlotState::Respawning;
        worker_tasks.spawn(respawn_after(index, stateful, std::chrono::milliseconds(0)));
        co_return;
    }

    // The final diagnostic can still sit in the stderr pipe when the exit
    // is observed; give the drain a bounded moment to reach EOF — a
    // grandchild holding the pipe open must not stall the respawn. The
    // slot is already dying, so a shutdown meanwhile skips its reaped pid.
    if(auto tail = workers[index].stderr_tail) {
        auto drained = [](std::shared_ptr<StderrTail> tail) -> kota::task<> {
            co_await tail->drained.wait();
        };
        co_await kota::with_timeout(drained(std::move(tail)), std::chrono::seconds(1), loop);
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

    auto& death = *w.death;
    if(death.culprit.empty() && w.stderr_tail) {
        death.culprit = w.stderr_tail->crashed_in;
    }
    if(death.cause.empty()) {
        death.cause = describe_exit(exit_code, exit_signal);
    }

    reset_streak_if_healthy(w);
    // A death that names its request is that request's content's doing:
    // the caller blames the content, and the slot respawns with its streak
    // untouched, like a preemption. A terminated session says nothing about
    // the slot, nor does a kill of an idle worker (the OOM killer, a user).
    // A worker killed while running requests or holding documents counts:
    // none of them is blamed when several were in flight, or when none
    // was, and only the budget slows a load that keeps getting killed.
    bool idle_kill = exit_signal == sigkill && death.in_flight == 0 && w.lost_documents.empty();
    if(death.culprit.empty() && !terminated && !idle_kill) {
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
    } else {
        // The dead worker's claim was released by mark_worker_dead; a queued
        // low-priority waiter may now fit on another idle worker instead of
        // stalling until the respawn lands.
        try_dispatch_pending();
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

kota::ipc::Error WorkerPool::death_error(const WorkerDeath& death,
                                         llvm::StringRef tag,
                                         kota::codec::dyn::Value identity) {
    namespace errc = worker::dispatch_errc;
    auto named = !death.culprit.empty();
    auto code = named && death.culprit == tag                        ? errc::worker_crashed
                : !named && death.in_flight <= 1 && !death.reclaimed ? errc::worker_died
                                                                     : errc::worker_lost;
    return kota::ipc::Error{code, death.cause, std::move(identity)};
}

kota::task<bool> WorkerPool::await_capacity(bool stateful) {
    while(!stop_scope.cancelled()) {
        auto& workers = stateful ? stateful_workers : stateless_workers;
        auto serving = std::ranges::any_of(workers, [](const WorkerProcess& w) {
            return w.state == SlotState::Alive && !w.retiring;
        });
        if(serving) {
            co_return true;
        }
        auto revives = revives_slots();
        auto returning = std::ranges::any_of(workers, [&](const WorkerProcess& w) {
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
    if(!stateful) {
        try_dispatch_pending();
    }
    // Revival is a running-pool concern: unit fixtures drive slot state
    // without an event loop, and a coroutine queued on a never-run loop
    // outlives the pool.
    if(revives_slots()) {
        worker_tasks.spawn(revive_slot(index, stateful));
    }
}

kota::task<> WorkerPool::revive_slot(std::size_t index, bool stateful) {
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

std::size_t WorkerPool::claim_stateless(std::size_t index, worker::Priority priority) {
    auto& w = stateless_workers[index];
    w.busy = true;
    w.low_priority = priority == worker::Priority::Low;
    next_claim_epoch += 1;
    w.claim_epoch = next_claim_epoch;
    set_oom_score(w, true);
    return index;
}

kota::task<std::size_t> WorkerPool::acquire_stateless_slot(worker::Priority priority,
                                                           kota::cancellation_token cancel) {
    using P = worker::Priority;
    while(true) {
        if(stop_scope.cancelled() || !has_future_capacity() || cancel.cancelled())
            co_return SIZE_MAX;

        // Claim directly only when no earlier request is queued (FIFO
        // fairness); high priority never waits behind queued low.
        bool fifo_clear =
            priority == P::High ? high_queue.empty() : high_queue.empty() && low_queue.empty();
        if(fifo_clear && (priority == P::High || low_busy_count() < effective_low_limit())) {
            if(auto idx = pick_idle_stateless(); idx != SIZE_MAX)
                co_return claim_stateless(idx, priority);
        }

        // A queued High request needs a process, not just CPU: with every
        // slot busy, cooperatively cancel one low compile so a slot frees
        // at the next declaration boundary instead of at end-of-TU. The
        // deficit already counts slots asked and highs queued, so
        // re-entering this loop cannot over-cancel.
        if(priority == P::High && pick_idle_stateless() == SIZE_MAX)
            cancel_low_priority(low_reclaim_deficit(1));

        // Queue up and suspend until try_dispatch_pending() claims a worker
        // for us or wakes us to observe pool death. The destructor covers
        // cancellation while queued or between dispatch and resume.
        PendingStateless pending(*this, priority);
        auto& queue = priority == P::High ? high_queue : low_queue;
        queue.push_back(&pending);
        pending.queue = &queue;

        // A round shut down or overtaken while it waits gives up its place
        // instead of holding it until a slot frees: a zero background budget
        // under memory pressure would otherwise hold it, and every shutdown
        // joining it, for as long as the pressure lasts.
        auto wake = cancel.on_cancel([&pending] { pending.ready.set(); });
        co_await pending.ready.wait();
        if(cancel.cancelled())
            co_return SIZE_MAX;

        if(pending.assigned_worker == SIZE_MAX)
            continue;

        auto idx = std::exchange(pending.assigned_worker, SIZE_MAX);
        // If the claimed worker died between dispatch and resume, the claim
        // was already cleaned up with the slot — go around again.
        if(stateless_workers[idx].generation != pending.assigned_gen)
            continue;
        co_return idx;
    }
}

void WorkerPool::release_stateless_slot(std::size_t worker_index) {
    auto& w = stateless_workers[worker_index];
    if(!w.busy)
        return;
    w.busy = false;
    w.low_priority = false;
    w.preempt_source.reset();
    w.cancel_requested_at = {};
    set_oom_score(w, false);
    LOG_DEBUG("Release {} (busy={}, low_busy={})", w.name, busy_stateless(), low_busy_count());
    try_dispatch_pending();
}

void WorkerPool::try_dispatch_pending() {
    if(stop_scope.cancelled() || !has_future_capacity()) {
        fail_pending_requests();
        return;
    }

    auto dispatch = [&](std::deque<PendingStateless*>& queue, worker::Priority priority) {
        while(!queue.empty()) {
            if(priority == worker::Priority::Low && low_busy_count() >= effective_low_limit())
                break;
            auto idx = pick_idle_stateless();
            if(idx == SIZE_MAX)
                break;
            auto* next = queue.front();
            queue.pop_front();
            next->queue = nullptr;
            claim_stateless(idx, priority);
            next->assigned_worker = idx;
            next->assigned_gen = stateless_workers[idx].generation;
            LOG_DEBUG("Dispatch {} -> {} (busy={}, low_busy={})",
                      priority == worker::Priority::Low ? "low" : "high",
                      stateless_workers[idx].name,
                      busy_stateless(),
                      low_busy_count());
            next->ready.set();
        }
    };

    dispatch(high_queue, worker::Priority::High);
    dispatch(low_queue, worker::Priority::Low);
}

void WorkerPool::fail_pending_requests() {
    // Waiters are woken without a claim (assigned_worker stays SIZE_MAX);
    // they re-check the pool state and return an error.
    auto drain = [](std::deque<PendingStateless*>& queue) {
        while(!queue.empty()) {
            auto* next = queue.front();
            queue.pop_front();
            next->queue = nullptr;
            next->ready.set();
        }
    };
    drain(high_queue);
    drain(low_queue);
}

std::size_t WorkerPool::pick_idle_stateless() {
    for(std::size_t i = 0; i < stateless_workers.size(); ++i) {
        auto& w = stateless_workers[i];
        if(w.state == SlotState::Alive && !w.busy && !w.retiring)
            return i;
    }
    return SIZE_MAX;
}

kota::task<> WorkerPool::monitor_loop() {
    while(true) {
        co_await kota::with_token(kota::sleep(std::chrono::milliseconds(3000), loop),
                                  stop_scope.token());
        if(stop_scope.cancelled())
            co_return;

        tick_foreground();
        tick_cancel_grace();
        tick_deadlines();

        auto mem = kota::sys::memory();
        if(mem.total == 0)
            continue;

        auto limited = mem.constrained > 0 && mem.constrained < mem.total;
        auto limit = limited ? mem.constrained : mem.total;
        auto available = mem.available;
        if(limited) {
            // Read afresh every tick, as libuv reads the limit: the process
            // can move between cgroups.
            if(auto proc_cgroup = llvm::MemoryBuffer::getFileAsStream("/proc/self/cgroup")) {
                auto cgroup = find_cgroup_memory((*proc_cgroup)->getBuffer());
                available = cgroup_available(cgroup, limit).value_or(available);
            }
        }
        auto ratio = static_cast<double>(available) / static_cast<double>(limit);

        tick_memory(ratio);
        tick_scaling(ratio);
        tick_oom_scores(limit);
    }
}

WorkerPool::CgroupMemory WorkerPool::find_cgroup_memory(llvm::StringRef proc_cgroup) {
    // Like libuv: the file is a single "0::/path" on cgroup v2, else the v1
    // memory controller's "N:memory:/path" line.
    if(proc_cgroup.consume_front("0::")) {
        auto dir = ("/sys/fs/cgroup" + proc_cgroup.split('\n').first).str();
        return {dir + "/memory.current", dir + "/memory.stat", "inactive_file"};
    }
    llvm::StringRef path;
    llvm::SmallVector<llvm::StringRef> lines;
    proc_cgroup.split(lines, '\n', -1, false);
    for(auto line: lines) {
        auto [controller, rest] = line.split(':').second.split(':');
        if(controller == "memory") {
            path = rest;
            break;
        }
    }
    // libuv falls back to the hierarchy's root when the process's own
    // memory cgroup is not there: a container without a cgroup namespace
    // mounts its own cgroup at the root.
    auto dir = ("/sys/fs/cgroup/memory" + path).str();
    if(path.empty() || !llvm::sys::fs::exists(dir + "/memory.usage_in_bytes")) {
        dir = "/sys/fs/cgroup/memory";
    }
    return {dir + "/memory.usage_in_bytes", dir + "/memory.stat", "total_inactive_file"};
}

std::optional<std::uint64_t> WorkerPool::cgroup_available(const CgroupMemory& cgroup,
                                                          std::uint64_t limit) {
    auto usage = llvm::MemoryBuffer::getFileAsStream(cgroup.usage);
    auto stat = llvm::MemoryBuffer::getFileAsStream(cgroup.stat);
    std::uint64_t used = 0;
    if(!usage || !stat || (*usage)->getBuffer().trim().getAsInteger(10, used)) {
        return std::nullopt;
    }
    llvm::SmallVector<llvm::StringRef> lines;
    (*stat)->getBuffer().split(lines, '\n', -1, false);
    for(auto line: lines) {
        auto [name, value] = line.split(' ');
        std::uint64_t cache = 0;
        if(name == cgroup.cache_key && !value.trim().getAsInteger(10, cache)) {
            used -= std::min(used, cache);
            break;
        }
    }
    return limit - std::min(limit, used);
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
    for(auto& w: stateless_workers) {
        set_oom_score(w, w.holds_work());
    }
    for(auto& w: stateful_workers) {
        set_oom_score(w, w.holds_work());
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

void WorkerPool::note_foreground() {
    last_fg_activity = std::chrono::steady_clock::now();
    if(foreground_active)
        return;
    foreground_active = true;
    if(auto deficit = low_reclaim_deficit()) {
        LOG_INFO("Foreground active: low budget -> {}, cancelling {} in-flight",
                 effective_low_limit(),
                 deficit);
        cancel_low_priority(deficit);
    }
}

std::size_t WorkerPool::low_reclaim_deficit(std::size_t pending_high) {
    auto cap = effective_low_limit();
    auto busy_low = low_busy_count();
    std::size_t need = busy_low > cap ? busy_low - cap : 0;
    // Queued High requests each need a freed process; with a slot idle the
    // queue is a transient the next dispatch drains without a sacrifice.
    auto high_need = high_queue.size() + pending_high;
    if(high_need > 0 && pick_idle_stateless() == SIZE_MAX) {
        need = std::max(need, high_need);
    }
    auto asked = static_cast<std::size_t>(
        std::ranges::count_if(stateless_workers, [](const WorkerProcess& w) {
            return w.state == SlotState::Alive && w.busy && w.low_priority &&
                   w.cancel_requested_at != std::chrono::steady_clock::time_point{};
        }));
    return need > asked ? need - asked : 0;
}

void WorkerPool::tick_foreground() {
    if(!foreground_active)
        return;
    if(foreground_busy()) {
        last_fg_activity = std::chrono::steady_clock::now();
        return;
    }
    if(std::chrono::steady_clock::now() - last_fg_activity < fg_hold)
        return;
    foreground_active = false;
    LOG_DEBUG("Foreground idle: low budget -> {}", effective_low_limit());
    // The reopened budget can admit queued low work right away.
    try_dispatch_pending();
}

void WorkerPool::tick_cancel_grace() {
    // A cancelled request that ignored its stop flag past the grace
    // window is stuck inside one declaration; reclaim the process.
    auto now = std::chrono::steady_clock::now();
    for(std::size_t i = 0; i < stateless_workers.size(); i += 1) {
        auto& w = stateless_workers[i];
        if(w.state == SlotState::Alive && w.busy &&
           w.cancel_requested_at != std::chrono::steady_clock::time_point{} &&
           now - w.cancel_requested_at > cancel_grace) {
            LOG_WARN("Worker {} ignored a cooperative cancel; killing it", w.name);
            w.preempted = true;
            reset_streak_if_healthy(w);
            mark_worker_dead(i, false, true);
        }
    }
}

void WorkerPool::tick_deadlines() {
    auto now = std::chrono::steady_clock::now();
    for(bool stateful: {false, true}) {
        auto& workers = stateful ? stateful_workers : stateless_workers;
        for(std::size_t i = 0; i < workers.size(); i += 1) {
            auto& w = workers[i];
            if(w.state != SlotState::Alive) {
                continue;
            }
            auto building = std::ranges::any_of(w.dispatches, &Dispatch::build);
            auto deadline = [&](const Dispatch* dispatch) {
                return dispatch->build ? options.build_deadline : options.query_deadline;
            };
            auto overdue = std::ranges::find_if(w.dispatches, [&](const Dispatch* dispatch) {
                return (dispatch->build || !building) &&
                       now - dispatch->started > deadline(dispatch);
            });
            if(overdue == w.dispatches.end()) {
                continue;
            }
            auto seconds =
                std::chrono::duration_cast<std::chrono::seconds>(deadline(*overdue)).count();
            LOG_WARN("Worker {} ran {} for over {}s; killing it", w.name, (*overdue)->tag, seconds);
            w.death->culprit = (*overdue)->tag;
            w.death->cause = std::format("killed after running for over {} seconds", seconds);
            mark_worker_dead(i, stateful, true);
        }
    }
}

void WorkerPool::cancel_low_priority(std::size_t count) {
    // Newest claim first: the youngest compile has the least work to lose,
    // and a fixed scan order would keep sacrificing the same slot's file.
    llvm::SmallVector<std::size_t> victims;
    for(std::size_t i = 0; i < stateless_workers.size(); i += 1) {
        auto& w = stateless_workers[i];
        if(w.state != SlotState::Alive || !w.busy || !w.low_priority)
            continue;
        if(w.cancel_requested_at != std::chrono::steady_clock::time_point{})
            continue;
        // A claim whose sender has not resumed yet has no cancellation
        // source installed; skip it entirely — stamping it would leave a
        // request that never saw a cancel to be grace-killed as if it had
        // ignored one. The demand is not dropped: the sender re-checks the
        // deficit when it arms and gives the claim back on the spot.
        if(!w.preempt_source)
            continue;
        victims.push_back(i);
    }
    std::ranges::sort(victims, std::greater{}, [this](std::size_t i) {
        return stateless_workers[i].claim_epoch;
    });
    std::size_t cancelled = 0;
    for(auto i: victims) {
        if(cancelled >= count)
            break;
        auto& w = stateless_workers[i];
        // Cancels the request on the wire; a slot that dies before the
        // grace expires has its stamp cleared by the respawn.
        w.preempt_source->cancel();
        w.cancel_requested_at = std::chrono::steady_clock::now();
        cancelled += 1;
    }
    if(cancelled > 0)
        LOG_INFO("Cooperatively cancelled {} low-priority requests", cancelled);
}

void WorkerPool::tick_memory(double available_ratio) {
    LOG_DEBUG(
        "Memory: {:.0f}% available, low_limit={}/{}, busy={} (low={}), "
        "queued=hi:{}/lo:{}, alive={}, sat={}/idle={}",
        available_ratio * 100,
        low_limit,
        max_low_limit(),
        busy_stateless(),
        low_busy_count(),
        high_queue.size(),
        low_queue.size(),
        alive_stateless(),
        saturated_cycles,
        idle_cycles);

    // The ceiling falls as workers retire; a budget left above it would
    // make the steps below change nothing for several ticks.
    low_limit = std::min(low_limit, max_low_limit());

    // Severe pressure: zero the low allowance and preempt all running
    // low-priority work — killing the workers releases their memory
    // immediately, and they respawn without crash accounting. Zero, not
    // one: with any allowance left, the preemption's own dispatch kick
    // would admit a fresh compile into the very pressure being relieved.
    if(available_ratio < severe_ratio) {
        if(low_limit > 0) {
            low_limit = 0;
            LOG_WARN("low_limit -> 0 (severe memory pressure: {:.0f}% available)",
                     available_ratio * 100);
        }
        if(auto busy_low = low_busy_count())
            preempt_low_priority(busy_low);
        if(alive_stateless() > options.min_stateless)
            retire_idle_worker();
        return;
    }

    // Tight but not exhausted: shrink, but keep one, so background work
    // still moves a TU at a time; one that alone exhausts memory is killed
    // above and spends its sender's retries.
    if(available_ratio < pressure_ratio) {
        auto shrunk = std::max<std::size_t>(low_limit, 2) - 1;
        if(shrunk != low_limit) {
            low_limit = shrunk;
            LOG_INFO("low_limit -> {} (memory pressure: {:.0f}% available)",
                     low_limit,
                     available_ratio * 100);
            try_dispatch_pending();
        }
        return;
    }

    // Grow while the budget is in use: one more TU per tick, which the next
    // tick sees. A budget the foreground limit masks, or nothing uses, does
    // not climb past what was tried, so its release admits no untested burst.
    if(low_limit < max_low_limit() && low_busy_count() >= low_limit) {
        low_limit += 1;
        LOG_DEBUG("low_limit -> {} (memory OK: {:.0f}% available)",
                  low_limit,
                  available_ratio * 100);
        try_dispatch_pending();
    }
}

void WorkerPool::tick_scaling(double available_ratio) {
    // Two kinds of demand grow the pool. Interactive work queued with no
    // idle slot, up to max_stateless: the user is waiting. Background work
    // filling its whole budget with more queued, only back up to
    // stateless_count after idle scale-down, and only while the user is
    // idle: a zeroed budget (severe memory pressure) is deliberate shutdown,
    // and the foreground limit binding is that limit doing its job.
    auto low_budget = effective_low_limit();
    bool high_starved = !high_queue.empty() && pick_idle_stateless() == SIZE_MAX;
    bool low_starved = low_budget > 0 && low_busy_count() >= low_budget && !low_queue.empty() &&
                       !foreground_active && stateless_footprint() < options.stateless_count;

    if(high_starved || low_starved) {
        saturated_cycles += 1;
        idle_cycles = 0;
    } else if(busy_stateless() == 0 && high_queue.empty() && low_queue.empty()) {
        idle_cycles += 1;
        saturated_cycles = 0;
    } else {
        saturated_cycles = 0;
        idle_cycles = 0;
    }

    if(saturated_cycles >= scale_up_ticks && available_ratio >= pressure_ratio) {
        if(scale_up_worker())
            saturated_cycles = 0;
    }

    if(idle_cycles >= scale_down_ticks) {
        retire_idle_worker();
        idle_cycles = 0;
    }
}

bool WorkerPool::scale_up_worker() {
    // The ceiling bounds live processes, not schedulable slots: a retiring
    // worker still holds its process until the monitor reaps it, and a
    // replacement spawned beside it would push the pool past max_stateless
    // during exactly the memory pressure that triggered the retirement.
    if(stateless_footprint() >= options.max_stateless)
        return false;

    // A vacant slot — Dead and waiting out its revival cooldown, or Retired
    // by an earlier scale-down — is capacity already allocated; refill it
    // instead of appending a fresh slot beside it. Appending would grow the
    // slot table, worker names and log files by one per retire/scale-up
    // cycle, and for a Dead slot the cooldown revival would later fire too
    // and grow the pool past what saturation asked for. The pending
    // revive_slot task no-ops once the state is no longer Dead.
    auto vacant = std::ranges::find_if(stateless_workers, [](const WorkerProcess& w) {
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
        LOG_INFO("Scaled up: revived {} (alive={})", w.name, alive_stateless());
    } else if(!spawn_worker(false)) {
        LOG_WARN("scale_up: spawn_worker failed");
        return false;
    } else {
        LOG_INFO("Scaled up: spawned {} (alive={})",
                 stateless_workers.back().name,
                 alive_stateless());
    }

    // Grow the low allowance by exactly the added capacity, within its
    // ceiling, instead of resetting whatever memory pressure took off it.
    low_limit = std::min(low_limit + 1, max_low_limit());
    try_dispatch_pending();
    return true;
}

void WorkerPool::retire_idle_worker() {
    // Workers already marked retiring are still counted alive until
    // monitor_worker observes their exit; respect the floor including them.
    std::size_t pending_retires = 0;
    for(auto& w: stateless_workers)
        if(w.retiring && w.state == SlotState::Alive)
            pending_retires += 1;
    if(alive_stateless() - pending_retires <= options.min_stateless)
        return;

    for(std::size_t i = stateless_workers.size(); i-- > 0;) {
        auto& w = stateless_workers[i];
        if(w.state == SlotState::Alive && !w.busy && !w.retiring) {
            w.retiring = true;
            w.peer->close_output();
            w.proc.kill(SIGTERM);
            LOG_INFO("Retiring worker {} (alive={})", w.name, alive_stateless());
            return;
        }
    }
}

void WorkerPool::preempt_low_priority(std::size_t count) {
    std::size_t preempted = 0;
    for(std::size_t i = 0; i < stateless_workers.size() && preempted < count; ++i) {
        auto& w = stateless_workers[i];
        if(w.state != SlotState::Alive || !w.busy || !w.low_priority)
            continue;

        // The kill is what actually frees the memory and fails the
        // in-flight request; its sender learns from the death record that
        // the pool reclaimed the worker.
        w.preempted = true;
        w.death->reclaimed = true;
        w.death->cause = "killed to relieve memory pressure";
        // The preempt respawn skips crash accounting, so credit a healthy
        // run here the same way a crash would — the slot must not carry a
        // stale streak past the healthy interval that already cleared it.
        reset_streak_if_healthy(w);
        mark_worker_dead(i, false, true);
        ++preempted;
    }
    if(preempted > 0) {
        LOG_WARN("Preempted {} low-priority workers (memory pressure)", preempted);
        // The victims' claims are gone; queued work may fit under the
        // (reduced) limit on the remaining idle workers.
        try_dispatch_pending();
    }
}

}  // namespace clice
