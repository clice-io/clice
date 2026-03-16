#include "server/worker_pool.h"

#include "support/logging.h"

#include "eventide/async/io/process.h"

namespace clice {

namespace et = eventide;

WorkerPool::WorkerPool(const Options& options, et::event_loop& loop)
    : options(options), loop(loop) {}

et::task<> WorkerPool::start() {
    for(std::size_t i = 0; i < options.stateless_worker_count; ++i) {
        auto label = "stateless-" + std::to_string(i);
        auto handle = co_await spawn_worker(Options::Mode::StatelessWorker, label);
        if(handle) {
            LOG_INFO("Spawned {}", label);
            stateless.workers.push_back(std::move(handle));
        } else {
            LOG_ERROR("Failed to spawn {}", label);
        }
    }

    for(std::size_t i = 0; i < options.stateful_worker_count; ++i) {
        auto label = "stateful-" + std::to_string(i);
        auto handle = co_await spawn_worker(Options::Mode::StatefulWorker, label);
        if(handle) {
            if(evicted_handler) {
                handle->peer->on_notification([this](const worker::EvictedParams& p) {
                    if(evicted_handler) {
                        evicted_handler(p.uri);
                    }
                });
            }
            LOG_INFO("Spawned {}", label);
            stateful.workers.push_back(std::move(handle));
        } else {
            LOG_ERROR("Failed to spawn {}", label);
        }
    }

    for(std::size_t i = 0; i < stateless.workers.size(); ++i) {
        loop.schedule(monitor_worker(i, false));
    }
    for(std::size_t i = 0; i < stateful.workers.size(); ++i) {
        loop.schedule(monitor_worker(i, true));
    }
}

et::task<> WorkerPool::stop() {
    for(auto& w : stateless.workers) {
        if(w && w->alive) {
            w->peer->close_output();
        }
    }
    for(auto& w : stateful.workers) {
        if(w && w->alive) {
            w->peer->close_output();
        }
    }
    co_return;
}

et::task<std::unique_ptr<WorkerHandle>>
WorkerPool::spawn_worker(Options::Mode mode, const std::string& label) {
    et::process::options opts;
    opts.file = options.self_path;
    opts.args = {opts.file};

    if(mode == Options::Mode::StatelessWorker) {
        opts.args.push_back("--mode=stateless-worker");
    } else {
        opts.args.push_back("--mode=stateful-worker");
        opts.args.push_back("--worker-memory-limit=" +
                            std::to_string(options.worker_memory_limit));
    }

    opts.streams = {
        et::process::stdio::pipe(true, false),
        et::process::stdio::pipe(false, true),
        et::process::stdio::pipe(false, true),
    };

    auto spawn_res = et::process::spawn(opts, loop);
    if(!spawn_res) {
        LOG_ERROR("Failed to spawn worker {}: {}", label, spawn_res.error().message());
        co_return nullptr;
    }

    auto transport = std::make_unique<et::ipc::StreamTransport>(
        et::stream(std::move(spawn_res->stdout_pipe)),
        et::stream(std::move(spawn_res->stdin_pipe)));

    auto peer = std::make_unique<et::ipc::BincodePeer>(loop, std::move(transport));

    loop.schedule(collect_stderr(std::move(spawn_res->stderr_pipe), label));
    loop.schedule(peer->run());

    auto handle = std::make_unique<WorkerHandle>();
    handle->proc = std::move(spawn_res->proc);
    handle->peer = std::move(peer);
    handle->alive = true;
    handle->label = label;

    co_return handle;
}

et::task<> WorkerPool::monitor_worker(std::size_t index, bool is_stateful) {
    auto& workers = is_stateful ? stateful.workers : stateless.workers;
    if(index >= workers.size())
        co_return;

    auto& w = workers[index];
    if(!w)
        co_return;

    auto status = co_await w->proc.wait();
    w->alive = false;

    if(status.has_value()) {
        LOG_WARN("Worker {} exited with status {} signal {}",
                 w->label, status->status, status->term_signal);
    } else {
        LOG_ERROR("Worker {} wait failed: {}", w->label, status.error().message());
    }

    if(is_stateful) {
        clear_owner(index);
    }

    co_await restart_worker(is_stateful, index);
}

et::task<> WorkerPool::collect_stderr(et::pipe stderr_pipe, std::string prefix) {
    et::stream stderr_stream(std::move(stderr_pipe));
    while(true) {
        auto chunk = co_await stderr_stream.read_chunk();
        if(!chunk.has_value())
            break;
        auto data = std::string_view(chunk->data(), chunk->size());
        while(!data.empty() && (data.back() == '\n' || data.back() == '\r')) {
            data.remove_suffix(1);
        }
        if(!data.empty()) {
            LOG_INFO("[{}] {}", prefix, data);
        }
        stderr_stream.consume(chunk->size());
    }
}

et::task<> WorkerPool::restart_worker(bool is_stateful, std::size_t index) {
    auto& workers = is_stateful ? stateful.workers : stateless.workers;
    auto mode = is_stateful ? Options::Mode::StatefulWorker : Options::Mode::StatelessWorker;
    auto& old = workers[index];
    auto label = old ? old->label : (is_stateful ? "stateful-" : "stateless-") + std::to_string(index);

    LOG_INFO("Restarting worker {}", label);

    auto handle = co_await spawn_worker(mode, label);
    if(handle) {
        if(is_stateful && evicted_handler) {
            handle->peer->on_notification([this](const worker::EvictedParams& p) {
                if(evicted_handler) {
                    evicted_handler(p.uri);
                }
            });
        }
        workers[index] = std::move(handle);
        loop.schedule(monitor_worker(index, is_stateful));
        LOG_INFO("Worker {} restarted", label);
    } else {
        LOG_ERROR("Failed to restart worker {}", label);
    }
}

std::size_t WorkerPool::assign_worker(std::uint32_t path_id) {
    auto it = stateful.owner.find(path_id);
    if(it != stateful.owner.end()) {
        auto lru_it = stateful.owner_lru_index.find(path_id);
        if(lru_it != stateful.owner_lru_index.end()) {
            stateful.owner_lru.erase(lru_it->second);
            stateful.owner_lru.push_front(path_id);
            lru_it->second = stateful.owner_lru.begin();
        }
        return it->second;
    }

    std::size_t min_count = SIZE_MAX;
    std::size_t best_idx = 0;
    for(std::size_t i = 0; i < stateful.workers.size(); ++i) {
        std::size_t count = 0;
        for(auto& [pid, wid] : stateful.owner) {
            if(wid == i)
                count++;
        }
        if(count < min_count) {
            min_count = count;
            best_idx = i;
        }
    }

    stateful.owner[path_id] = best_idx;
    stateful.owner_lru.push_front(path_id);
    stateful.owner_lru_index[path_id] = stateful.owner_lru.begin();

    return best_idx;
}

void WorkerPool::clear_owner(std::size_t worker_index) {
    std::vector<std::uint32_t> to_remove;
    for(auto& [path_id, wid] : stateful.owner) {
        if(wid == worker_index) {
            to_remove.push_back(path_id);
        }
    }
    for(auto pid : to_remove) {
        remove_owner(pid);
    }
}

void WorkerPool::remove_owner(std::uint32_t path_id) {
    stateful.owner.erase(path_id);
    auto lru_it = stateful.owner_lru_index.find(path_id);
    if(lru_it != stateful.owner_lru_index.end()) {
        stateful.owner_lru.erase(lru_it->second);
        stateful.owner_lru_index.erase(lru_it);
    }
}

void WorkerPool::register_evicted_handler(std::function<void(const std::string&)> handler) {
    evicted_handler = std::move(handler);
}

bool WorkerPool::has_workers() const {
    return !stateless.workers.empty() || !stateful.workers.empty();
}

}  // namespace clice
