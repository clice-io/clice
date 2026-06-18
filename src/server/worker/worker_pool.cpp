#include "server/worker/worker_pool.h"

#include <algorithm>
#include <csignal>
#include <string>

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

    if(!log_dir_.empty()) {
        opts.args.push_back("--log-dir");
        opts.args.push_back(log_dir_);
    }

    opts.streams = {
        kota::process::stdio::pipe(true, false),  // stdin: child reads
        kota::process::stdio::pipe(false, true),  // stdout: child writes
        kota::process::stdio::pipe(false, true),  // stderr: child writes
    };

    auto result = kota::process::spawn(opts, loop);
    if(!result) {
        LOG_ERROR("Failed to spawn {} worker: {}",
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
        .owned_documents = 0,
    });

    auto& w = workers.back();
    w.alive = true;
    if(!stateful)
        ++alive_stateless_count;
    io_group.spawn(w.peer->run());

    return true;
}

bool WorkerPool::start(const WorkerPoolOptions& options) {
    options_ = options;
    log_dir_ = options.log_dir;

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

    max_low_limit = alive_stateless_count > 1 ? alive_stateless_count - 1 : alive_stateless_count;
    low_limit = max_low_limit;

    monitor_group.spawn(monitor_memory());

    LOG_INFO("WorkerPool started: {} stateless, {} stateful workers",
             stateless_workers.size(),
             stateful_workers.size());
    return true;
}

kota::task<> WorkerPool::stop() {
    LOG_INFO("WorkerPool stopping...");
    shutting_down_ = true;

    for(auto& w: stateless_workers)
        w.peer->close_output();
    for(auto& w: stateful_workers)
        w.peer->close_output();

    for(auto& w: stateless_workers)
        w.proc.kill(SIGTERM);
    for(auto& w: stateful_workers)
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
    stateful_workers[selected].owned_documents++;
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
    stateful_workers[worker_idx].owned_documents--;
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

    if(shutting_down_)
        co_return;

    int exit_code = 0, exit_signal = 0;
    if(result.has_value()) {
        exit_code = result.value().status;
        exit_signal = result.value().term_signal;
    } else {
        auto name = std::string(stateful ? "SF-" : "SL-") + std::to_string(index);
        LOG_ERROR("Worker {} lost: {}", name, result.error().message());
        exit_signal = 9;
    }

    if(process_crash(index, stateful, exit_code, exit_signal)) {
        if(!respawn_worker(index, stateful)) {
            auto name = std::string(stateful ? "SF-" : "SL-") + std::to_string(index);
            LOG_ERROR("Worker {} respawn failed", name);
        }
    }
}

bool WorkerPool::process_crash(std::size_t index, bool stateful, int exit_code, int exit_signal) {
    auto& workers = stateful ? stateful_workers : stateless_workers;
    auto& w = workers[index];
    w.alive = false;

    auto name = std::string(stateful ? "SF-" : "SL-") + std::to_string(index);
    if(exit_signal != 0) {
        LOG_ERROR("Worker {} killed by signal {} (restarts: {})",
                  name,
                  exit_signal,
                  w.restart_count);
    } else {
        LOG_ERROR("Worker {} exited with code {} (restarts: {})", name, exit_code, w.restart_count);
    }

    WorkerCrashInfo info;
    info.worker_index = index;
    info.stateful = stateful;
    info.exit_code = exit_code;
    info.exit_signal = exit_signal;
    info.restart_count = w.restart_count;

    if(stateful) {
        for(auto& [path_id, widx]: owner) {
            if(widx == index)
                info.lost_documents.push_back(path_id);
        }
        clear_owner(index);
    } else {
        --alive_stateless_count;
        if(w.busy) {
            w.busy = false;
            --stateless_busy_count;
        }
        apply_crash_backoff();
        try_dispatch_pending();
    }

    info.will_restart = w.restart_count < options_.max_restarts;
    if(!info.will_restart) {
        LOG_ERROR("Worker {} exceeded max restarts ({}), giving up", name, options_.max_restarts);
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
    opts.file = options_.self_path;
    opts.args = {options_.self_path, "worker"};
    if(stateful) {
        opts.args.push_back("--stateful");
        opts.args.push_back("--memory-limit");
        opts.args.push_back(std::to_string(options_.worker_memory_limit));
    }
    opts.args.push_back("--worker-name");
    opts.args.push_back(worker_name);
    if(!log_dir_.empty()) {
        opts.args.push_back("--log-dir");
        opts.args.push_back(log_dir_);
    }
    opts.streams = {
        kota::process::stdio::pipe(true, false),
        kota::process::stdio::pipe(false, true),
        kota::process::stdio::pipe(false, true),
    };

    auto result = kota::process::spawn(opts, loop);
    if(!result) {
        LOG_ERROR("Failed to respawn worker {}: {}", worker_name, result.error().message());
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
        .owned_documents = 0,
        .alive = true,
        .busy = false,
        .restart_count = old_restart_count,
    };

    if(!stateful)
        ++alive_stateless_count;

    auto& w = workers[index];
    io_group.spawn(w.peer->run());

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

kota::task<std::size_t> WorkerPool::acquire_stateless_slot(worker::Priority priority) {
    using P = worker::Priority;
    auto can_proceed = [&]() {
        auto idle = alive_stateless_count - stateless_busy_count;
        if(idle == 0)
            return false;
        if(priority == P::High)
            return true;
        return stateless_busy_count < low_limit;
    };

    if(!can_proceed()) {
        PendingStateless pending(priority);
        if(priority == P::High)
            high_queue.push_back(&pending);
        else
            low_queue.push_back(&pending);
        co_await pending.ready.wait();
        co_return pending.assigned_worker;
    }

    auto idx = pick_idle_stateless();
    stateless_workers[idx].busy = true;
    ++stateless_busy_count;

    auto pid = stateless_workers[idx].proc.pid();
    if(pid > 0) {
        kota::sys::set_priority(priority == P::Low ? 10 : 0, pid);
    }

    co_return idx;
}

void WorkerPool::release_stateless_slot(std::size_t worker_index) {
    if(!stateless_workers[worker_index].busy)
        return;
    stateless_workers[worker_index].busy = false;
    --stateless_busy_count;
    try_dispatch_pending();
}

void WorkerPool::try_dispatch_pending() {
    auto idle = alive_stateless_count - stateless_busy_count;

    while(!high_queue.empty() && idle > 0) {
        auto* next = high_queue.front();
        high_queue.pop_front();
        auto idx = pick_idle_stateless();
        stateless_workers[idx].busy = true;
        ++stateless_busy_count;
        --idle;
        auto pid = stateless_workers[idx].proc.pid();
        if(pid > 0)
            kota::sys::set_priority(0, pid);
        next->assigned_worker = idx;
        next->ready.set();
    }

    while(!low_queue.empty() && idle > 0 && stateless_busy_count < low_limit) {
        auto* next = low_queue.front();
        low_queue.pop_front();
        auto idx = pick_idle_stateless();
        stateless_workers[idx].busy = true;
        ++stateless_busy_count;
        --idle;
        auto pid = stateless_workers[idx].proc.pid();
        if(pid > 0)
            kota::sys::set_priority(10, pid);
        next->assigned_worker = idx;
        next->ready.set();
    }
}

std::size_t WorkerPool::pick_idle_stateless() {
    for(std::size_t i = 0; i < stateless_workers.size(); ++i) {
        if(stateless_workers[i].alive && !stateless_workers[i].busy)
            return i;
    }
    llvm_unreachable("pick_idle_stateless called with no idle workers");
}

kota::task<> WorkerPool::monitor_memory() {
    while(true) {
        co_await kota::sleep(std::chrono::milliseconds(3000));
        if(shutting_down_)
            co_return;

        auto mem = kota::sys::memory();
        if(mem.total == 0)
            continue;

        auto effective_total =
            (mem.constrained > 0 && mem.constrained < mem.total) ? mem.constrained : mem.total;
        auto ratio = static_cast<double>(mem.available) / static_cast<double>(effective_total);

        if(ratio < 0.20 && low_limit > 1) {
            --low_limit;
            LOG_INFO("Stateless low_limit -> {} (memory pressure: {:.0f}% available)",
                     low_limit,
                     ratio * 100);
        } else if(ratio > 0.40 && low_limit < max_low_limit) {
            ++low_limit;
            LOG_DEBUG("Stateless low_limit -> {} (memory OK: {:.0f}% available)",
                      low_limit,
                      ratio * 100);
        }
    }
}

void WorkerPool::apply_crash_backoff() {
    auto new_limit = std::max<std::size_t>(1, low_limit * 3 / 4);
    if(new_limit < low_limit) {
        low_limit = new_limit;
        LOG_WARN("Stateless low_limit -> {} (worker crash AIMD backoff)", low_limit);
    }
}

}  // namespace clice
