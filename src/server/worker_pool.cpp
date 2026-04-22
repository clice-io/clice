#include "server/worker_pool.h"

#include <csignal>
#include <string>

#include "support/logging.h"

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
    if(stateful) {
        opts.args = {self_path,
                     "--mode",
                     "stateful-worker",
                     "--worker-memory-limit",
                     std::to_string(memory_limit)};
    } else {
        opts.args = {self_path, "--mode", "stateless-worker"};
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

    // Schedule stderr log collection
    std::string prefix = "[" + worker_name + "]";
    loop.schedule(drain_stderr(std::move(spawn.stderr_pipe), prefix));

    workers.push_back(WorkerProcess{
        .proc = std::move(spawn.proc),
        .peer = std::move(peer),
        .owned_documents = 0,
    });

    auto& w = workers.back();
    w.alive = true;
    ++alive_count_;
    loop.schedule(w.peer->run());

    return true;
}

bool WorkerPool::start(const WorkerPoolOptions& options) {
    options_ = options;
    log_dir_ = options.log_dir;

    for(std::uint32_t i = 0; i < options.stateless_count; ++i) {
        if(!spawn_worker(options.self_path, false, 0)) {
            return false;
        }
        loop.schedule(monitor_worker(stateless_workers.size() - 1, false));
    }

    for(std::uint32_t i = 0; i < options.stateful_count; ++i) {
        if(!spawn_worker(options.self_path, true, options.worker_memory_limit)) {
            return false;
        }
        loop.schedule(monitor_worker(stateful_workers.size() - 1, true));
    }

    // Register evicted notification handler for each stateful worker
    for(std::size_t i = 0; i < stateful_workers.size(); ++i) {
        stateful_workers[i].peer->on_notification([this](const worker::EvictedParams& params) {
            if(on_evicted) {
                on_evicted(params.path);
            }
        });
    }

    LOG_INFO("WorkerPool started: {} stateless, {} stateful workers",
             stateless_workers.size(),
             stateful_workers.size());
    return true;
}

kota::task<> WorkerPool::stop() {
    LOG_INFO("WorkerPool stopping...");
    shutting_down_ = true;

    // Close output pipes to signal workers to exit gracefully.
    for(auto& w: stateless_workers)
        w.peer->close_output();
    for(auto& w: stateful_workers)
        w.peer->close_output();

    // Send SIGTERM.  monitor_worker coroutines handle the wait.
    for(auto& w: stateless_workers)
        w.proc.kill(SIGTERM);
    for(auto& w: stateful_workers)
        w.proc.kill(SIGTERM);

    // Wait until all monitor_worker coroutines have finished.
    if(alive_count_ > 0) {
        all_exited_.reset();
        co_await all_exited_.wait();
    }

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
        if(stateful_workers[i].owned_documents < stateful_workers[best].owned_documents) {
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
    auto& w = workers[index];
    auto name = std::string(stateful ? "SF-" : "SL-") + std::to_string(index);

    auto result = co_await w.proc.wait();
    w.alive = false;
    --alive_count_;

    if(shutting_down_) {
        if(alive_count_ == 0)
            all_exited_.set();
        co_return;
    }

    if(result.has_value()) {
        auto& exit = result.value();
        if(exit.term_signal != 0) {
            LOG_ERROR("Worker {} killed by signal {} (restarts: {})",
                      name,
                      exit.term_signal,
                      w.restart_count);
        } else {
            LOG_ERROR("Worker {} exited with code {} (restarts: {})",
                      name,
                      exit.status,
                      w.restart_count);
        }
    } else {
        LOG_ERROR("Worker {} lost: {} (restarts: {})",
                  name,
                  result.error().message(),
                  w.restart_count);
    }

    if(stateful)
        clear_owner(index);

    constexpr unsigned max_restarts = 5;
    if(w.restart_count >= max_restarts) {
        LOG_ERROR("Worker {} exceeded max restarts ({}), giving up", name, max_restarts);
        co_return;
    }

    if(!respawn_worker(index, stateful)) {
        LOG_ERROR("Worker {} respawn failed", name);
    }
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
    if(stateful) {
        opts.args = {options_.self_path,
                     "--mode",
                     "stateful-worker",
                     "--worker-memory-limit",
                     std::to_string(options_.worker_memory_limit)};
    } else {
        opts.args = {options_.self_path, "--mode", "stateless-worker"};
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
    loop.schedule(drain_stderr(std::move(spawn.stderr_pipe), prefix));

    workers[index] = WorkerProcess{
        .proc = std::move(spawn.proc),
        .peer = std::move(peer),
        .owned_documents = 0,
        .alive = true,
        .restart_count = old_restart_count,
    };

    auto& w = workers[index];
    ++alive_count_;
    loop.schedule(w.peer->run());

    if(stateful) {
        w.peer->on_notification([this](const worker::EvictedParams& params) {
            if(on_evicted)
                on_evicted(params.path);
        });
    }

    loop.schedule(monitor_worker(index, stateful));

    LOG_INFO("Worker {} restarted (attempt {})", worker_name, old_restart_count);
    return true;
}

}  // namespace clice
