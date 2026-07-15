#include "server/worker/worker_pool.h"

#include <algorithm>
#include <csignal>
#include <string>

#include "support/anomaly.h"
#include "support/filesystem.h"
#include "support/logging.h"

#include "kota/async/io/system.h"
#include "kota/ipc/transport.h"

namespace clice {

namespace {

/// Coroutine that drains a worker's stderr pipe.
/// Workers write their own log files, so this only captures unexpected output
/// (crash stacktraces, assertion failures, sanitizer reports, etc.).
kota::task<> drain_stderr(kota::pipe stderr_pipe, std::string prefix) {
    std::string buffer;
    while(true) {
        auto result = co_await stderr_pipe.read();
        if(!result.has_value())
            break;
        auto& chunk = result.value();
        if(chunk.empty())
            break;

        buffer += chunk;

        std::size_t pos = 0;
        while(true) {
            auto nl = buffer.find('\n', pos);
            if(nl == std::string::npos)
                break;
            auto line = buffer.substr(pos, nl - pos);
            if(!line.empty()) {
                LOG_WARN("{} {}", prefix, line);
            }
            pos = nl + 1;
        }
        buffer.erase(0, pos);
    }

    if(!buffer.empty()) {
        LOG_WARN("{} {}", prefix, buffer);
    }
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
        opts.args.push_back("--memory-limit");
        opts.args.push_back(std::to_string(options.worker_memory_limit));
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
    auto peer = std::make_shared<kota::ipc::BincodePeer>(loop, std::move(transport));

    worker_tasks.spawn(drain_stderr(std::move(spawn.stderr_pipe), "[" + name + "]"));
    worker_tasks.spawn(run_peer(peer));

    return SpawnedProcess{std::move(spawn.proc), std::move(peer)};
}

void WorkerPool::install_evict_handler(WorkerProcess& worker, std::size_t index) {
    worker.peer->on_notification(
        [this, index, gen = worker.generation](const worker::EvictedParams& params) {
            // A buffered eviction from a dead peer can drain after the slot
            // respawned and reacquired the same path; matching by slot index
            // alone would unseat the new owner. A same-generation ABA (a
            // live worker's stale-copy eviction draining after a probe
            // reassigned the path back to it) is accepted: the notification
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
        .name = name,
        .state = SlotState::Alive,
        .spawn_time = std::chrono::steady_clock::now(),
    });

    if(stateful)
        install_evict_handler(workers[index], index);
    worker_tasks.spawn(monitor_worker(index, stateful));
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
    w.state = SlotState::Alive;
    w.owned_documents = 0;
    w.busy = false;
    w.low_priority = false;
    w.retiring = false;
    w.preempted = false;
    w.spawn_time = std::chrono::steady_clock::now();
    // generation was bumped at death; crash_streak carries across restarts
    // until a healthy uptime resets it.

    if(stateful)
        install_evict_handler(w, index);
    worker_tasks.spawn(monitor_worker(index, stateful));

    if(!stateful)
        try_dispatch_pending();

    LOG_INFO("Worker {} restarted (crash streak {})", w.name, w.crash_streak);
    return true;
}

bool WorkerPool::start(const WorkerPoolOptions& opts) {
    options = opts;
    log_dir = opts.log_dir;

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

    // Resolve auto max_stateless (0 = CPU cores).
    if(options.max_stateless == 0)
        options.max_stateless = kota::sys::parallelism();
    options.max_stateless = std::max(options.max_stateless, options.stateless_count);
    if(options.min_stateless == 0)
        options.min_stateless = 1;
    // The configured floor is honored even above the startup count: idle
    // scale-down must not shrink a scaled-up pool below it. Only the
    // ceiling bounds it.
    options.min_stateless = std::min(options.min_stateless, options.max_stateless);

    low_limit = max_low_limit();

    worker_tasks.spawn(kota::with_token(monitor_loop(), stop_scope.token()));

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
    kota::task_group<> watchdog{loop};
    watchdog.spawn(kill_stragglers());

    co_await worker_tasks.join();
    watchdog.cancel();
    co_await watchdog.join();

    LOG_INFO("WorkerPool stopped");
}

kota::task<> WorkerPool::kill_stragglers() {
    co_await kota::sleep(std::chrono::milliseconds(5000), loop);
    LOG_WARN("Workers still alive 5s after SIGTERM; escalating to SIGKILL");
    // 9 == SIGKILL by value; Windows' <csignal> does not define the macro.
    for(auto& w: stateless_workers)
        if(w.state == SlotState::Alive)
            w.proc.kill(9);
    for(auto& w: stateful_workers)
        if(w.state == SlotState::Alive)
            w.proc.kill(9);
}

std::size_t WorkerPool::assign_worker(std::uint32_t path_id) {
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
    return selected;
}

std::size_t WorkerPool::assign_expendable(std::uint32_t path_id) {
    auto expendable = [&](std::size_t i) {
        if(stateful_workers[i].state != SlotState::Alive) {
            return false;
        }
        for(auto& [pid, widx]: owner) {
            if(widx == i && pid != path_id) {
                return false;
            }
        }
        return true;
    };

    // Keep the current owner when sacrificing it risks nothing.
    if(auto it = owner.find(path_id); it != owner.end() && expendable(it->second)) {
        return it->second;
    }

    auto take = [&](std::size_t i) {
        remove_owner(path_id);
        owner[path_id] = i;
        stateful_workers[i].owned_documents += 1;
        return i;
    };

    for(std::size_t i = 0; i < stateful_workers.size(); ++i) {
        if(expendable(i)) {
            return take(i);
        }
    }

    // A single-worker pool has nothing to preserve by refusing: run the
    // probe there — the occasional respawn beats quarantining the document
    // until the session reopens.
    if(stateful_workers.size() == 1 && stateful_workers[0].state == SlotState::Alive) {
        return take(0);
    }
    return SIZE_MAX;
}

std::size_t WorkerPool::pick_least_loaded() {
    // Two passes: a worker hosting an in-flight quarantine probe is a
    // known crash risk, so new documents are pinned elsewhere while any
    // other live worker exists — a probe crash must not take a freshly
    // opened healthy document with it.
    for(bool allow_suspect: {false, true}) {
        std::size_t best = SIZE_MAX;
        for(std::size_t i = 0; i < stateful_workers.size(); ++i) {
            auto& w = stateful_workers[i];
            if(w.state != SlotState::Alive)
                continue;
            if(!allow_suspect && w.suspect_inflight > 0)
                continue;
            if(best == SIZE_MAX || w.owned_documents < stateful_workers[best].owned_documents) {
                best = i;
            }
        }
        if(best != SIZE_MAX)
            return best;
    }
    return SIZE_MAX;
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

void WorkerPool::clear_owner(std::size_t worker_index) {
    llvm::SmallVector<std::uint32_t> to_remove;
    for(auto& [pid, widx]: owner) {
        if(widx == worker_index) {
            to_remove.push_back(pid);
        }
    }
    for(auto pid: to_remove) {
        remove_owner(pid);
    }
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
    if(w.peer) {
        w.peer->close();
        w.peer.reset();
    }
    if(kill_process) {
        // Make sure the process is really gone so monitor_worker's
        // proc.wait() is guaranteed to deliver a verdict.
        w.proc.kill(9);
    }
}

kota::task<> WorkerPool::monitor_worker(std::size_t index, bool stateful) {
    auto& workers = stateful ? stateful_workers : stateless_workers;

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

    // POSIX SIGHUP == 1 by value: Windows' <csignal> does not define the
    // macro, and a worker can only receive it on POSIX anyway.
    constexpr int sighup = 1;
    if(exit_signal == SIGTERM || exit_signal == SIGINT || exit_signal == sighup) {
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
        if(auto content = fs::read(log_path)) {
            llvm::StringRef tail(*content);
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

    reset_streak_if_healthy(w);
    // A crash while a suspect request (a quarantined document's probe) was
    // in flight says something about the document, not the slot: respawn
    // with the streak untouched, like a preemption.
    bool suspect = w.suspect_inflight > 0;
    w.suspect_inflight = 0;
    if(!suspect) {
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
        // Collect documents owned by this worker so the caller (on_crash)
        // can mark them dirty for recompilation on the next request.
        for(auto& [path_id, widx]: owner) {
            if(widx == index)
                info.lost_documents.push_back(path_id);
        }
        clear_owner(index);
    } else {
        apply_crash_backoff();
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
    if(!stateful) {
        // If this was the last slot with a future, wake all waiters so they
        // can return an error instead of hanging.
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
    return index;
}

kota::task<std::size_t> WorkerPool::acquire_stateless_slot(worker::Priority priority) {
    using P = worker::Priority;
    while(true) {
        if(stop_scope.cancelled() || !has_future_capacity())
            co_return SIZE_MAX;

        // Claim directly only when no earlier request is queued (FIFO
        // fairness); high priority never waits behind queued low.
        bool fifo_clear =
            priority == P::High ? high_queue.empty() : high_queue.empty() && low_queue.empty();
        if(fifo_clear && (priority == P::High || low_busy_count() < effective_low_limit())) {
            if(auto idx = pick_idle_stateless(); idx != SIZE_MAX)
                co_return claim_stateless(idx, priority);
        }

        // Queue up and suspend until try_dispatch_pending() claims a worker
        // for us or wakes us to observe pool death. The destructor covers
        // cancellation while queued or between dispatch and resume.
        PendingStateless pending(*this, priority);
        auto& queue = priority == P::High ? high_queue : low_queue;
        queue.push_back(&pending);
        pending.queue = &queue;

        co_await pending.ready.wait();

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
        co_await kota::sleep(std::chrono::milliseconds(3000), loop);

        auto mem = kota::sys::memory();
        if(mem.total == 0)
            continue;

        auto effective_total =
            (mem.constrained > 0 && mem.constrained < mem.total) ? mem.constrained : mem.total;
        auto ratio = static_cast<double>(mem.available) / static_cast<double>(effective_total);

        tick_memory(ratio);
        tick_scaling(ratio);
    }
}

void WorkerPool::tick_memory(double available_ratio) {
    LOG_DEBUG(
        "Memory: {:.0f}% available, low_limit={}/{}, busy={} (low={}), " "queued=hi:{}/lo:{}, alive={}, sat={}/idle={}",
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

    // Severe pressure: drop the low allowance to its floor and preempt all
    // running low-priority work — killing the workers releases their memory
    // immediately, and they respawn without crash accounting.
    if(available_ratio < 0.10) {
        if(low_limit > 1) {
            if(w_max == 0 || low_limit > w_max)
                w_max = low_limit;
            low_limit = 1;
            LOG_WARN("low_limit -> 1 (severe memory pressure: {:.0f}% available)",
                     available_ratio * 100);
        }
        if(auto busy_low = low_busy_count())
            preempt_low_priority(busy_low);
        if(alive_stateless() > options.min_stateless)
            retire_idle_worker();
        return;
    }

    if(available_ratio < 0.20 && low_limit > 1) {
        if(backoff_cooldown > 0) {
            backoff_cooldown -= 1;
        } else {
            if(w_max == 0 || low_limit > w_max)
                w_max = low_limit;
            low_limit -= 1;
            LOG_INFO("low_limit -> {} (memory pressure: {:.0f}% available)",
                     low_limit,
                     available_ratio * 100);
        }
    } else if(available_ratio > 0.40 && low_limit < max_low_limit()) {
        backoff_cooldown = 0;
        if(w_max > 0 && low_limit < w_max) {
            // CUBIC-style fast recovery: close half the gap to the last
            // known-good limit per tick.
            auto gap = w_max - low_limit;
            auto increment = std::max<std::size_t>(1, gap / 2);
            low_limit = std::min(low_limit + increment, max_low_limit());
        } else {
            low_limit += 1;
            if(low_limit >= max_low_limit())
                w_max = 0;
        }
        LOG_DEBUG("low_limit -> {} (memory OK: {:.0f}% available, w_max={})",
                  low_limit,
                  available_ratio * 100,
                  w_max);
        try_dispatch_pending();
    }
}

void WorkerPool::tick_scaling(double available_ratio) {
    bool has_queued = !high_queue.empty() || !low_queue.empty();
    auto alive = alive_stateless();
    auto busy = busy_stateless();

    // The pool is saturated when all workers are busy with queued work, OR
    // when low-priority slots are at capacity with low-priority work queued.
    // The second condition handles the case where low_limit reserves a worker
    // for high-priority requests: the reserved slot stays idle, so busy
    // never equals alive, but the pool is effectively saturated for
    // low-priority (indexing) work.
    bool saturated = (alive > 0 && busy == alive && has_queued) ||
                     (low_busy_count() >= effective_low_limit() && !low_queue.empty());

    if(saturated) {
        saturated_cycles += 1;
        idle_cycles = 0;
    } else if(busy == 0 && !has_queued) {
        idle_cycles += 1;
        saturated_cycles = 0;
    } else {
        saturated_cycles = 0;
        idle_cycles = 0;
    }

    if(saturated_cycles >= scale_up_ticks && available_ratio > 0.30) {
        if(scale_up_worker())
            saturated_cycles = 0;
    }

    if(idle_cycles >= scale_down_ticks) {
        retire_idle_worker();
        idle_cycles = 0;
    }
}

void WorkerPool::apply_crash_backoff() {
    auto new_limit = std::max<std::size_t>(1, low_limit * 3 / 4);
    if(new_limit < low_limit) {
        if(w_max == 0 || low_limit > w_max)
            w_max = low_limit;
        low_limit = new_limit;
        LOG_WARN("low_limit -> {} (worker crash AIMD backoff)", low_limit);
    }
    backoff_cooldown = 3;
}

bool WorkerPool::scale_up_worker() {
    // The ceiling bounds live processes, not schedulable slots: a retiring
    // worker still holds its process until the monitor reaps it, and a
    // replacement spawned beside it would push the pool past max_stateless
    // during exactly the memory pressure that triggered the retirement.
    if(stateless_footprint() >= options.max_stateless)
        return false;

    // A Dead slot is capacity already allocated, just waiting out its
    // revival cooldown; under scale-up pressure revive it now instead of
    // appending a fresh slot beside it — the cooldown revival would later
    // fire too and grow the pool past what saturation asked for. The
    // pending revive_slot task no-ops once the state is no longer Dead.
    auto dead = std::ranges::find(stateless_workers, SlotState::Dead, &WorkerProcess::state);
    if(dead != stateless_workers.end()) {
        auto index = static_cast<std::size_t>(dead - stateless_workers.begin());
        auto& w = *dead;
        w.crash_streak = 0;
        w.state = SlotState::Respawning;
        if(!respawn_worker(index, false)) {
            // Back to Dead with a fresh cooldown revival armed, so a failed
            // early revive does not orphan the slot.
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

    // The new worker raises the ceiling; grow the low allowance by exactly
    // the added capacity instead of resetting whatever pressure or crash
    // backoff state accumulated.
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

        // Signal the sender first, then take the slot out of rotation and
        // kill the process — the kill is what actually frees the memory.
        // The kill fails the in-flight request, and the sender classifies
        // that failure as preemption by consulting the source. Today the
        // runtime only queues waiters on cancel/close (never resumes them
        // inline), so either order behaves the same; cancelling first
        // keeps the classification correct without depending on that.
        auto source = w.preempt_source;
        w.preempted = true;
        // The preempt respawn skips crash accounting, so credit a healthy
        // run here the same way a crash would — the slot must not carry a
        // stale streak past the healthy interval that already cleared it.
        reset_streak_if_healthy(w);
        if(source)
            source->cancel();
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
