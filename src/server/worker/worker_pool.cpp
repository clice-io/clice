#include "server/worker/worker_pool.h"

#include <algorithm>
#include <csignal>
#include <string>

#include "support/anomaly.h"
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

}  // namespace

bool WorkerPool::spawn_worker(const std::string& self_path,
                              bool stateful,
                              std::uint64_t memory_limit) {
    auto& workers = stateful ? stateful_workers : stateless_workers;
    auto worker_index = workers.size();
    std::string worker_name = std::string(stateful ? "SF-" : "SL-") + std::to_string(worker_index);

    kota::process::options opts;
    opts.file = self_path;
    opts.args = {self_path, "worker"};
    if(stateful) {
        opts.args.push_back("--stateful");
        opts.args.push_back("--memory-limit");
        opts.args.push_back(std::to_string(memory_limit));
    }

    opts.args.push_back("--worker-name");
    opts.args.push_back(worker_name);

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
                    "Failed to spawn {} worker: {}",
                    stateful ? "stateful" : "stateless",
                    result.error().message());
        return false;
    }

    auto& spawn = *result;

    // StreamTransport: input = child's stdout (parent reads), output = child's stdin (parent
    // writes)
    auto transport = std::make_unique<kota::ipc::StreamTransport>(std::move(spawn.stdout_pipe),
                                                                  std::move(spawn.stdin_pipe));
    auto peer = std::make_unique<kota::ipc::BincodePeer>(loop, std::move(transport));

    std::string prefix = "[" + worker_name + "]";
    io_group.spawn(drain_stderr(std::move(spawn.stderr_pipe), prefix));

    workers.push_back(WorkerProcess{
        .proc = std::move(spawn.proc),
        .peer = std::move(peer),
        .name = worker_name,
        .owned_documents = 0,
    });

    auto& w = workers.back();
    w.alive = true;
    if(!stateful)
        alive_stateless_count += 1;
    io_group.spawn(w.peer->run());

    return true;
}

bool WorkerPool::start(const WorkerPoolOptions& opts) {
    options = opts;
    log_dir = opts.log_dir;

    stateless_workers.reserve(options.stateless_count);
    stateful_workers.reserve(options.stateful_count);

    for(std::uint32_t i = 0; i < options.stateless_count; ++i) {
        if(!spawn_worker(options.self_path, false, 0)) {
            return false;
        }
        monitor_group.spawn(monitor_worker(stateless_workers.size() - 1, false));
    }

    for(std::uint32_t i = 0; i < options.stateful_count; ++i) {
        if(!spawn_worker(options.self_path, true, options.worker_memory_limit)) {
            return false;
        }
        monitor_group.spawn(monitor_worker(stateful_workers.size() - 1, true));
    }

    // Register evicted notification handler for each stateful worker
    for(std::size_t i = 0; i < stateful_workers.size(); ++i) {
        stateful_workers[i].peer->on_notification([this](const worker::EvictedParams& params) {
            if(on_evicted) {
                on_evicted(params.path);
            }
        });
    }

    // Resolve auto max_stateless (0 = CPU cores).
    if(options.max_stateless == 0)
        options.max_stateless = kota::sys::parallelism();
    if(options.min_stateless == 0)
        options.min_stateless = 1;
    options.min_stateless = std::min(options.min_stateless, options.stateless_count);
    options.max_stateless = std::max(options.max_stateless, options.stateless_count);

    // Reserve one worker for high-priority requests by capping low-priority
    // concurrency to N-1. With a single worker this collapses to 1: both
    // priorities share it, and high wins only via queue ordering.
    max_low_limit = alive_stateless_count > 1 ? alive_stateless_count - 1 : alive_stateless_count;
    low_limit = max_low_limit;

    slot_cancel_sources.resize(stateless_workers.size());

    monitor_group.spawn(monitor_memory());

    LOG_INFO("WorkerPool started: {} stateless, {} stateful workers",
             stateless_workers.size(),
             stateful_workers.size());
    return true;
}

kota::task<> WorkerPool::stop() {
    LOG_INFO("WorkerPool stopping...");
    shutting_down = true;
    fail_pending_requests();

    // Retired workers (scale-down) have their peer moved to retired_peers
    // and their process already exited — skip them to avoid null deref / SEGV.
    for(auto& w: stateless_workers)
        if(w.peer)
            w.peer->close_output();
    for(auto& w: stateful_workers)
        if(w.peer)
            w.peer->close_output();

    for(auto& w: stateless_workers)
        if(w.alive)
            w.proc.kill(SIGTERM);
    for(auto& w: stateful_workers)
        if(w.alive)
            w.proc.kill(SIGTERM);

    co_await kota::when_all(monitor_group.join(), io_group.join());

    LOG_INFO("WorkerPool stopped");
}

std::size_t WorkerPool::assign_worker(std::uint32_t path_id) {
    auto it = owner.find(path_id);
    if(it != owner.end()) {
        // Already assigned; touch LRU
        auto lru_it = owner_lru_index.find(path_id);
        if(lru_it != owner_lru_index.end()) {
            owner_lru.erase(lru_it->second);
        }
        owner_lru.push_front(path_id);
        owner_lru_index[path_id] = owner_lru.begin();
        return it->second;
    }

    // New assignment: pick the least-loaded worker
    auto selected = pick_least_loaded();
    owner[path_id] = selected;
    stateful_workers[selected].owned_documents += 1;
    owner_lru.push_front(path_id);
    owner_lru_index[path_id] = owner_lru.begin();
    return selected;
}

std::size_t WorkerPool::pick_least_loaded() {
    std::size_t best = 0;
    for(std::size_t i = 1; i < stateful_workers.size(); ++i) {
        if(!stateful_workers[i].alive)
            continue;
        if(!stateful_workers[best].alive ||
           stateful_workers[i].owned_documents < stateful_workers[best].owned_documents) {
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

    auto lru_it = owner_lru_index.find(path_id);
    if(lru_it != owner_lru_index.end()) {
        owner_lru.erase(lru_it->second);
        owner_lru_index.erase(lru_it);
    }
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

kota::task<> WorkerPool::monitor_worker(std::size_t index, bool stateful) {
    auto& workers = stateful ? stateful_workers : stateless_workers;

    auto result = co_await workers[index].proc.wait();

    if(shutting_down)
        co_return;

    // Intentional retirement (scale-down): skip crash processing and respawn.
    if(!stateful && workers[index].retiring) {
        LOG_INFO("Worker {} retired gracefully", workers[index].name);
        workers[index].alive = false;
        alive_stateless_count -= 1;
        if(workers[index].busy) {
            if(workers[index].low_priority)
                low_busy_count -= 1;
            workers[index].busy = false;
            workers[index].low_priority = false;
            stateless_busy_count -= 1;
        }
        if(workers[index].peer) {
            workers[index].peer->close();
            retired_peers.push_back(std::move(workers[index].peer));
        }
        max_low_limit =
            alive_stateless_count > 1 ? alive_stateless_count - 1 : alive_stateless_count;
        if(low_limit > max_low_limit)
            low_limit = max_low_limit;
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

    if(process_crash(index, stateful, exit_code, exit_signal)) {
        if(!respawn_worker(index, stateful)) {
            LOG_ERROR("Worker {} respawn failed", workers[index].name);
            if(!stateful) {
                // Slot is effectively permanently dead — shrink ceiling
                // so monitor_memory() can't raise low_limit above capacity.
                max_low_limit =
                    alive_stateless_count > 1 ? alive_stateless_count - 1 : alive_stateless_count;
                if(low_limit > max_low_limit)
                    low_limit = max_low_limit;
                if(alive_stateless_count == 0)
                    fail_pending_requests();
            }
        }
    }
}

bool WorkerPool::process_crash(std::size_t index, bool stateful, int exit_code, int exit_signal) {
    auto& workers = stateful ? stateful_workers : stateless_workers;
    auto& w = workers[index];
    w.alive = false;

    // POSIX SIGHUP == 1 by value: Windows' <csignal> does not define the
    // macro, and a worker can only receive it on POSIX anyway.
    constexpr int sighup = 1;
    if(exit_signal == SIGTERM || exit_signal == SIGINT || exit_signal == sighup) {
        // Termination requested from outside — e.g. the editor tearing the
        // whole process group down on a hard restart. The worker did not
        // crash; don't alarm the user through the anomaly channel.
        LOG_WARN("Worker {} terminated by signal {} (restarts: {})",
                 w.name,
                 exit_signal,
                 w.restart_count);
    } else if(exit_signal != 0) {
        LOG_ANOMALY(WorkerCrash,
                    "Worker {} killed by signal {} (restarts: {})",
                    w.name,
                    exit_signal,
                    w.restart_count);
    } else {
        LOG_ANOMALY(WorkerCrash,
                    "Worker {} exited with code {} (restarts: {})",
                    w.name,
                    exit_code,
                    w.restart_count);
    }

    WorkerCrashInfo info;
    info.worker_index = index;
    info.stateful = stateful;
    info.exit_code = exit_code;
    info.exit_signal = exit_signal;
    info.restart_count = w.restart_count;
    info.will_restart = w.restart_count < options.max_restarts;

    if(!info.will_restart) {
        LOG_ERROR("Worker {} exceeded max restarts ({}), giving up", w.name, options.max_restarts);
    }

    if(stateful) {
        // Collect documents owned by this worker so the caller (on_crash)
        // can mark them dirty for recompilation on the next request.
        for(auto& [path_id, widx]: owner) {
            if(widx == index)
                info.lost_documents.push_back(path_id);
        }
        clear_owner(index);
    } else {
        alive_stateless_count -= 1;
        if(w.busy) {
            if(w.low_priority)
                low_busy_count -= 1;
            w.busy = false;
            w.low_priority = false;
            stateless_busy_count -= 1;
        }
        apply_crash_backoff();

        // Permanently lost slot: shrink the ceiling so monitor_memory()
        // can't raise low_limit above the surviving worker count.
        if(!info.will_restart) {
            max_low_limit =
                alive_stateless_count > 1 ? alive_stateless_count - 1 : alive_stateless_count;
            if(low_limit > max_low_limit)
                low_limit = max_low_limit;
        }

        // If no workers remain and none will restart, wake all waiters
        // with SIZE_MAX so they can return an error instead of hanging.
        if(alive_stateless_count == 0 && !info.will_restart)
            fail_pending_requests();
        else
            try_dispatch_pending();
    }

    if(on_crash)
        on_crash(info);

    return info.will_restart;
}

bool WorkerPool::respawn_worker(std::size_t index, bool stateful) {
    auto& workers = stateful ? stateful_workers : stateless_workers;
    auto old_restart_count = workers[index].restart_count + 1;
    auto worker_name = std::string(stateful ? "SF-" : "SL-") + std::to_string(index);

    // Close the old peer and retire it so its coroutines (run/write_loop)
    // can finish naturally before the object is destroyed.
    if(workers[index].peer) {
        workers[index].peer->close();
        retired_peers.push_back(std::move(workers[index].peer));
    }

    kota::process::options opts;
    opts.file = options.self_path;
    opts.args = {options.self_path, "worker"};
    if(stateful) {
        opts.args.push_back("--stateful");
        opts.args.push_back("--memory-limit");
        opts.args.push_back(std::to_string(options.worker_memory_limit));
    }
    opts.args.push_back("--worker-name");
    opts.args.push_back(worker_name);
    if(!log_dir.empty()) {
        opts.args.push_back("--log-dir");
        opts.args.push_back(log_dir);
    }
    opts.streams = {
        kota::process::stdio::pipe(true, false),
        kota::process::stdio::pipe(false, true),
        kota::process::stdio::pipe(false, true),
    };

    auto result = kota::process::spawn(opts, loop);
    if(!result) {
        LOG_ANOMALY(WorkerSpawnFail,
                    "Failed to respawn worker {}: {}",
                    worker_name,
                    result.error().message());
        return false;
    }

    auto& spawn = *result;
    auto transport = std::make_unique<kota::ipc::StreamTransport>(std::move(spawn.stdout_pipe),
                                                                  std::move(spawn.stdin_pipe));
    auto peer = std::make_unique<kota::ipc::BincodePeer>(loop, std::move(transport));

    std::string prefix = "[" + worker_name + "]";
    io_group.spawn(drain_stderr(std::move(spawn.stderr_pipe), prefix));

    workers[index] = WorkerProcess{
        .proc = std::move(spawn.proc),
        .peer = std::move(peer),
        .name = worker_name,
        .owned_documents = 0,
        .alive = true,
        .busy = false,
        .low_priority = false,
        .restart_count = old_restart_count,
    };

    if(!stateful)
        alive_stateless_count += 1;

    auto& w = workers[index];
    io_group.spawn(w.peer->run());

    // Dispatch pending requests now that a fresh worker is available.
    if(!stateful)
        try_dispatch_pending();

    if(stateful) {
        w.peer->on_notification([this](const worker::EvictedParams& params) {
            if(on_evicted)
                on_evicted(params.path);
        });
    }

    monitor_group.spawn(monitor_worker(index, stateful));

    LOG_INFO("Worker {} restarted (attempt {})", worker_name, old_restart_count);
    return true;
}

kota::task<std::size_t> WorkerPool::acquire_stateless_slot(worker::Priority priority,
                                                           std::size_t exclude) {
    using P = worker::Priority;
    auto can_proceed = [&]() {
        auto idle = alive_stateless_count - stateless_busy_count;
        // Don't count the excluded worker (failed peer whose crash
        // hasn't been processed by monitor_worker yet).
        if(exclude < stateless_workers.size() && stateless_workers[exclude].alive &&
           !stateless_workers[exclude].busy)
            idle -= 1;
        // Don't count retiring workers — they are alive but won't accept work.
        for(auto& w: stateless_workers)
            if(w.retiring && w.alive && !w.busy)
                idle -= 1;
        if(idle == 0)
            return false;
        if(priority == P::High)
            return true;
        return low_busy_count < low_limit;
    };

    while(true) {
        if(alive_stateless_count == 0)
            co_return SIZE_MAX;

        if(!can_proceed()) {
            // Enqueue and suspend until try_dispatch_pending() wakes us.
            // PendingStateless destructor handles cleanup on cancellation:
            //   - still queued → removes from queue
            //   - dispatched but cancelled before StatelessSlot → releases slot
            PendingStateless pending(priority);
            pending.pool = this;
            if(priority == P::High) {
                high_queue.push_back(&pending);
                pending.queue = &high_queue;
            } else {
                low_queue.push_back(&pending);
                pending.queue = &low_queue;
            }
            co_await pending.ready.wait();
            pending.pool = nullptr;  // claimed — StatelessSlot will handle release

            if(pending.assigned_worker == SIZE_MAX)
                co_return SIZE_MAX;

            // If the assigned worker crashed and was respawned between
            // dispatch and resume, the slot is stale — loop to re-acquire.
            auto idx = pending.assigned_worker;
            if(stateless_workers[idx].restart_count != pending.assigned_gen)
                continue;

            co_return idx;
        }

        auto idx = pick_idle_stateless();
        stateless_workers[idx].busy = true;
        stateless_workers[idx].low_priority = (priority == P::Low);
        stateless_busy_count += 1;
        if(priority == P::Low)
            low_busy_count += 1;

        co_return idx;
    }
}

void WorkerPool::release_stateless_slot(std::size_t worker_index) {
    if(worker_index >= stateless_workers.size() || !stateless_workers[worker_index].busy)
        return;
    if(worker_index < slot_cancel_sources.size())
        slot_cancel_sources[worker_index].reset();
    if(stateless_workers[worker_index].low_priority)
        low_busy_count -= 1;
    stateless_workers[worker_index].busy = false;
    stateless_workers[worker_index].low_priority = false;
    stateless_busy_count -= 1;
    LOG_DEBUG("Release {} (busy={}, low_busy={})",
              stateless_workers[worker_index].name,
              stateless_busy_count,
              low_busy_count);
    try_dispatch_pending();
}

void WorkerPool::try_dispatch_pending() {
    // Subtract retiring workers from idle count — they won't accept new work.
    std::size_t retiring_idle = 0;
    for(auto& w: stateless_workers)
        if(w.retiring && w.alive && !w.busy)
            retiring_idle += 1;
    auto idle = alive_stateless_count - stateless_busy_count - retiring_idle;

    auto dispatch = [&](std::deque<PendingStateless*>& queue, bool is_low) {
        while(!queue.empty() && idle > 0 && (!is_low || low_busy_count < low_limit)) {
            auto* next = queue.front();
            queue.pop_front();
            next->queue = nullptr;
            auto idx = pick_idle_stateless();
            stateless_workers[idx].busy = true;
            stateless_workers[idx].low_priority = is_low;
            stateless_busy_count += 1;
            if(is_low)
                low_busy_count += 1;
            idle -= 1;
            next->assigned_worker = idx;
            next->assigned_gen = stateless_workers[idx].restart_count;
            LOG_DEBUG("Dispatch {} -> {} (busy={}, low_busy={}, idle={})",
                      is_low ? "low" : "high",
                      stateless_workers[idx].name,
                      stateless_busy_count,
                      low_busy_count,
                      idle);
            next->ready.set();
        }
    };

    dispatch(high_queue, false);
    dispatch(low_queue, true);
}

void WorkerPool::fail_pending_requests() {
    // SIZE_MAX signals to send_stateless() that no worker was assigned.
    auto drain = [](std::deque<PendingStateless*>& queue) {
        while(!queue.empty()) {
            auto* next = queue.front();
            queue.pop_front();
            next->queue = nullptr;
            next->assigned_worker = SIZE_MAX;
            next->ready.set();
        }
    };
    drain(high_queue);
    drain(low_queue);
}

std::size_t WorkerPool::pick_idle_stateless(std::size_t exclude) {
    for(std::size_t i = 0; i < stateless_workers.size(); ++i) {
        if(i != exclude && stateless_workers[i].alive && !stateless_workers[i].busy &&
           !stateless_workers[i].retiring)
            return i;
    }
    llvm_unreachable("pick_idle_stateless called with no idle workers");
}

kota::task<> WorkerPool::monitor_memory() {
    while(true) {
        co_await kota::sleep(std::chrono::milliseconds(3000), loop);
        if(shutting_down)
            co_return;

        auto mem = kota::sys::memory();
        if(mem.total == 0)
            continue;

        auto effective_total =
            (mem.constrained > 0 && mem.constrained < mem.total) ? mem.constrained : mem.total;
        auto ratio = static_cast<double>(mem.available) / static_cast<double>(effective_total);

        LOG_DEBUG(
            "Memory: {:.0f}% available, low_limit={}/{}, busy={} (low={}), " "queued=hi:{}/lo:{}, alive={}, sat={}/idle={}",
            ratio * 100,
            low_limit,
            max_low_limit,
            stateless_busy_count,
            low_busy_count,
            high_queue.size(),
            low_queue.size(),
            alive_stateless_count,
            saturated_cycles,
            idle_cycles);

        // Severe pressure: preempt low-priority in-flight requests.
        // FIXME: cancelled Index requests are not requeued by the indexer,
        // so those files will be skipped until the next indexing cycle.
        if(ratio < 0.10 && low_busy_count > 0) {
            cancel_low_priority_requests(low_busy_count);
        }

        // Inner loop: adjust low_limit with CUBIC-style fast recovery.
        if(ratio < 0.20 && low_limit > 1) {
            if(backoff_cooldown > 0) {
                backoff_cooldown -= 1;
            } else {
                if(w_max == 0 || low_limit > w_max)
                    w_max = low_limit;
                low_limit -= 1;
                LOG_INFO("low_limit -> {} (memory pressure: {:.0f}% available)",
                         low_limit,
                         ratio * 100);
            }
        } else if(ratio > 0.40 && low_limit < max_low_limit) {
            backoff_cooldown = 0;
            if(w_max > 0 && low_limit < w_max) {
                auto gap = w_max - low_limit;
                auto increment = std::max<std::size_t>(1, gap / 2);
                low_limit = std::min(low_limit + increment, max_low_limit);
            } else {
                low_limit += 1;
                if(low_limit >= max_low_limit)
                    w_max = 0;
            }
            LOG_DEBUG("low_limit -> {} (memory OK: {:.0f}% available, w_max={})",
                      low_limit,
                      ratio * 100,
                      w_max);
            try_dispatch_pending();
        }

        // Outer loop: dynamic scaling.
        check_scaling();

        // Severe pressure: also scale down immediately.
        if(ratio < 0.10 && alive_stateless_count > options.min_stateless) {
            retire_idle_worker();
        }
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
    if(shutting_down)
        return false;
    if(alive_stateless_count >= options.max_stateless)
        return false;

    auto new_index = stateless_workers.size();
    if(!spawn_worker(options.self_path, false, 0)) {
        LOG_WARN("scale_up: spawn_worker failed");
        return false;
    }

    slot_cancel_sources.push_back(nullptr);
    monitor_group.spawn(monitor_worker(new_index, false));

    max_low_limit = alive_stateless_count > 1 ? alive_stateless_count - 1 : alive_stateless_count;
    low_limit = max_low_limit;
    w_max = 0;
    try_dispatch_pending();

    LOG_INFO("Scaled up: spawned {} (alive={})",
             stateless_workers.back().name,
             alive_stateless_count);
    return true;
}

void WorkerPool::retire_idle_worker() {
    if(shutting_down)
        return;
    // Count workers already marked retiring (exit not yet observed by
    // monitor_worker) to avoid retiring below min_stateless.
    std::size_t pending_retires = 0;
    for(auto& w: stateless_workers)
        if(w.retiring && w.alive)
            pending_retires += 1;
    if(alive_stateless_count - pending_retires <= options.min_stateless)
        return;

    std::size_t target = SIZE_MAX;
    for(std::size_t i = stateless_workers.size(); i-- > 0;) {
        auto& w = stateless_workers[i];
        if(w.alive && !w.busy && !w.retiring) {
            target = i;
            break;
        }
    }

    if(target == SIZE_MAX)
        return;

    auto& w = stateless_workers[target];
    w.retiring = true;
    w.peer->close_output();
    w.proc.kill(SIGTERM);

    LOG_INFO("Retiring worker {} (alive={})", w.name, alive_stateless_count);
}

void WorkerPool::check_scaling() {
    if(shutting_down)
        return;
    bool has_queued = !high_queue.empty() || !low_queue.empty();

    // The pool is saturated when all workers are busy with queued work, OR
    // when low-priority slots are at capacity with low-priority work queued.
    // The second condition handles the case where low_limit reserves a worker
    // for high-priority requests: the reserved slot stays idle, so busy_count
    // never equals alive_count, but the pool is effectively saturated for
    // low-priority (indexing) work.
    bool saturated = (stateless_busy_count == alive_stateless_count && has_queued) ||
                     (low_busy_count >= low_limit && !low_queue.empty());

    if(saturated) {
        saturated_cycles += 1;
        idle_cycles = 0;
    } else if(stateless_busy_count == 0 && !has_queued) {
        idle_cycles += 1;
        saturated_cycles = 0;
    } else {
        saturated_cycles = 0;
        idle_cycles = 0;
    }

    if(saturated_cycles >= scale_up_ticks) {
        auto mem = kota::sys::memory();
        auto effective =
            (mem.constrained > 0 && mem.constrained < mem.total) ? mem.constrained : mem.total;
        auto ratio = effective > 0 ? static_cast<double>(mem.available) / effective : 1.0;
        if(ratio > 0.30) {
            if(scale_up_worker())
                saturated_cycles = 0;
        }
    }

    if(idle_cycles >= scale_down_ticks) {
        retire_idle_worker();
        idle_cycles = 0;
    }
}

void WorkerPool::cancel_low_priority_requests(std::size_t count) {
    std::size_t cancelled = 0;
    for(std::size_t i = 0; i < stateless_workers.size() && cancelled < count; ++i) {
        auto& w = stateless_workers[i];
        if(w.alive && w.busy && w.low_priority) {
            if(i < slot_cancel_sources.size() && slot_cancel_sources[i]) {
                slot_cancel_sources[i]->cancel();
                ++cancelled;
            }
        }
    }
    if(cancelled > 0)
        LOG_WARN("Cancelled {} low-priority requests (memory pressure)", cancelled);
}

}  // namespace clice
