#include "server/worker_pool.h"

#include <csignal>
#include <list>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

#include "eventide/async/process.h"
#include "eventide/async/stream.h"
#include "eventide/jsonrpc/transport.h"
#include "server/compilation_service.h"
#include "support/logging.h"

namespace clice::server {

struct WorkerPool::Impl {
    struct WorkerClient {
        et::process process;
        std::shared_ptr<jsonrpc::Peer> peer;
        WorkerRole role = WorkerRole::Stateful;
        int pid = -1;
        std::size_t owned_documents = 0;
        std::size_t inflight_requests = 0;

        auto operator->() -> jsonrpc::Peer* {
            return peer.get();
        }
    };

    Impl(et::event_loop& loop, const Options& options) : loop(loop), options(options) {}

    static auto run_worker_peer(std::shared_ptr<jsonrpc::Peer> peer) -> et::task<> {
        co_await peer->run();
    }

    static auto worker_role_name(WorkerRole role) -> std::string_view {
        switch(role) {
            case WorkerRole::Stateful: return "stateful";
            case WorkerRole::Stateless: return "stateless";
        }
        return "unknown";
    }

    static void print_worker_stderr_line(WorkerRole role, int pid, std::string_view line) {
        auto role_name = worker_role_name(role);
        LOG_INFO("[worker:{} pid={}] {}", role_name, pid, line);
    }

    static auto pump_worker_stderr(et::pipe pipe, WorkerRole role, int pid) -> et::task<> {
        std::string buffered;
        while(true) {
            auto chunk = co_await pipe.read();
            if(!chunk) {
                if(chunk.error() == et::error::end_of_file ||
                   chunk.error() == et::error::operation_aborted) {
                    break;
                }

                auto role_name = worker_role_name(role);
                auto message = chunk.error().message();
                LOG_WARN("[worker:{} pid={}] stderr read failed: {}", role_name, pid, message);
                break;
            }

            buffered += *chunk;
            while(true) {
                auto pos = buffered.find('\n');
                if(pos == std::string::npos) {
                    break;
                }

                auto line = std::string_view(buffered).substr(0, pos);
                if(!line.empty() && line.back() == '\r') {
                    line.remove_suffix(1);
                }
                print_worker_stderr_line(role, pid, line);
                buffered.erase(0, pos + 1);
            }
        }

        if(!buffered.empty()) {
            auto line = std::string_view(buffered);
            if(!line.empty() && line.back() == '\r') {
                line.remove_suffix(1);
            }
            print_worker_stderr_line(role, pid, line);
        }
    }

    void register_worker_callbacks(jsonrpc::Peer& peer) {
        peer.on_request(
            [this](jsonrpc::RequestContext& context, const WorkerGetCompileRecipeParams& params)
                -> jsonrpc::RequestResult<WorkerGetCompileRecipeParams> {
                return on_get_compile_recipe(context, params);
            });
    }

    auto on_get_compile_recipe(jsonrpc::RequestContext& context,
                               const WorkerGetCompileRecipeParams& params)
        -> jsonrpc::RequestResult<WorkerGetCompileRecipeParams> {
        (void)context;

        auto recipe = compilation_service.resolve_recipe(params);
        if(!recipe) {
            co_return std::unexpected(std::move(recipe.error()));
        }
        co_return std::move(*recipe);
    }

    static auto worker_role_argument(WorkerRole role) -> std::string_view {
        switch(role) {
            case WorkerRole::Stateful: return "stateful";
            case WorkerRole::Stateless: return "stateless";
        }
        return "stateful";
    }

    auto spawn_worker(WorkerRole role) -> std::expected<WorkerClient, std::string> {
        et::process::options process_options;
        process_options.file = options.self_path;
        process_options.args = {
            options.self_path,
            std::string(k_worker_mode),
            "--worker-role=" + std::string(worker_role_argument(role)),
            "--worker-doc-capacity=" + std::to_string(options.worker_document_capacity),
        };
        process_options.streams = {
            et::process::stdio::pipe(true, false),
            et::process::stdio::pipe(false, true),
            et::process::stdio::pipe(false, true),
        };

        auto spawned = et::process::spawn(process_options, loop);
        if(!spawned) {
            return std::unexpected(std::string(spawned.error().message()));
        }

        auto pid = spawned->proc.pid();
        auto transport = std::make_unique<jsonrpc::StreamTransport>(std::move(spawned->stdout_pipe),
                                                                    std::move(spawned->stdin_pipe));
        auto stderr_pipe = std::move(spawned->stderr_pipe);
        auto peer = std::make_shared<jsonrpc::Peer>(loop, std::move(transport));
        register_worker_callbacks(*peer);
        loop.schedule(run_worker_peer(peer));
        loop.schedule(pump_worker_stderr(std::move(stderr_pipe), role, pid));

        WorkerClient client;
        client.process = std::move(spawned->proc);
        client.peer = std::move(peer);
        client.role = role;
        client.pid = client.process.pid();
        return client;
    }

    auto spawn_worker_into_pool(WorkerRole role) -> std::expected<std::size_t, std::string> {
        auto spawned = spawn_worker(role);
        if(!spawned) {
            return std::unexpected(std::move(spawned.error()));
        }

        auto index = workers.size();
        workers.push_back(std::move(*spawned));
        if(role == WorkerRole::Stateful) {
            stateful_workers.push_back(index);
        } else {
            stateless_workers.push_back(index);
        }
        return index;
    }

    auto restart_worker(std::size_t worker_index) -> std::expected<void, std::string> {
        if(worker_index >= workers.size()) {
            return std::unexpected("worker index out of range");
        }

        auto old_role = workers[worker_index].role;
        auto old_pid = workers[worker_index].pid;
        auto replacement = spawn_worker(workers[worker_index].role);
        if(!replacement) {
            return std::unexpected(std::move(replacement.error()));
        }

        auto old_process = std::move(workers[worker_index].process);
        auto old_peer = std::move(workers[worker_index].peer);

        if(old_peer) {
            auto status = old_peer->close_output();
            (void)status;
        }

        if(old_process.pid() > 0) {
            auto kill_status = old_process.kill(SIGTERM);
            (void)kill_status;
            loop.schedule(
                reap_worker_process(std::move(old_process), old_role, old_pid, "restart"));
        }

        workers[worker_index].process = std::move(replacement->process);
        workers[worker_index].peer = std::move(replacement->peer);
        workers[worker_index].pid = workers[worker_index].process.pid();
        return {};
    }

    auto reap_worker_process(et::process process, WorkerRole role, int pid, std::string_view reason)
        -> et::task<> {
        auto waited = co_await process.wait();
        auto role_name = worker_role_name(role);
        if(!waited) {
            auto message = waited.error().message();
            LOG_WARN("[worker:{} pid={}] wait failed ({}): {}", role_name, pid, reason, message);
            co_return;
        }

        LOG_INFO("[worker:{} pid={}] exited ({}): status={} signal={}",
                 role_name,
                 pid,
                 reason,
                 static_cast<long long>(waited->status),
                 waited->term_signal);
    }

    auto ensure_stateful_worker() -> std::expected<void, std::string> {
        if(!stateful_workers.empty()) {
            return {};
        }

        auto spawned = spawn_worker_into_pool(WorkerRole::Stateful);
        if(!spawned) {
            return std::unexpected(std::move(spawned.error()));
        }
        return {};
    }

    auto ensure_stateless_worker() -> std::expected<std::size_t, std::string> {
        if(auto selected = select_stateless_worker()) {
            return *selected;
        }

        auto spawned = spawn_worker_into_pool(WorkerRole::Stateless);
        if(!spawned) {
            return std::unexpected(std::move(spawned.error()));
        }
        return *spawned;
    }

    template <typename Params>
    auto dispatch_request(std::size_t worker_index, Params params)
        -> et::task<jsonrpc::Result<typename rpc::RequestTraits<Params>::Result>> {
        auto response = co_await send_request(worker_index, params);
        if(response) {
            co_return response;
        }

        auto restarted = restart_worker(worker_index);
        if(!restarted) {
            co_return std::unexpected("worker request failed: " + response.error() +
                                      "; worker restart failed: " + restarted.error());
        }

        co_return co_await send_request(worker_index, std::move(params));
    }

    template <typename Params>
    auto send_request(std::size_t worker_index, Params params)
        -> et::task<jsonrpc::Result<typename rpc::RequestTraits<Params>::Result>> {
        if(worker_index >= workers.size()) {
            co_return std::unexpected("worker index out of range");
        }

        auto& worker = workers[worker_index];
        worker.inflight_requests += 1;
        auto response = co_await worker->send_request(std::move(params));
        if(worker.inflight_requests > 0) {
            worker.inflight_requests -= 1;
        }
        if(!response) {
            auto role_name = worker_role_name(worker.role);
            LOG_WARN("[worker:{} pid={} idx={}] request failed: {}",
                     role_name,
                     worker.pid,
                     worker_index,
                     response.error());
        }
        co_return response;
    }

    auto assign_stateful_worker(std::string_view uri) -> std::size_t {
        auto key = std::string(uri);
        auto owner_iter = owner.find(key);
        if(owner_iter != owner.end()) {
            touch_owner_lru(key);
            return owner_iter->second;
        }

        shrink_owner();
        const auto selected = pick_stateful_worker();
        owner.emplace(key, selected);
        workers[selected].owned_documents += 1;
        touch_owner_lru(key);
        return selected;
    }

    void touch_owner_lru(const std::string& key) {
        auto lru_iter = owner_lru_index.find(key);
        if(lru_iter != owner_lru_index.end()) {
            owner_lru.splice(owner_lru.begin(), owner_lru, lru_iter->second);
            lru_iter->second = owner_lru.begin();
            return;
        }

        owner_lru.push_front(key);
        owner_lru_index.emplace(key, owner_lru.begin());
    }

    void shrink_owner() {
        while(owner.size() >= options.master_document_capacity && !owner_lru.empty()) {
            auto victim = std::move(owner_lru.back());
            owner_lru.pop_back();
            owner_lru_index.erase(victim);

            auto owner_iter = owner.find(victim);
            if(owner_iter == owner.end()) {
                continue;
            }

            auto& worker = workers[owner_iter->second];
            auto worker_id = owner_iter->second;
            if(worker.owned_documents > 0) {
                worker.owned_documents -= 1;
            }
            evict_from_worker(worker_id, victim);
            owner.erase(owner_iter);
        }
    }

    auto pick_stateful_worker() const -> std::size_t {
        if(stateful_workers.empty()) {
            return 0;
        }

        auto selected = stateful_workers.front();
        for(auto candidate: stateful_workers) {
            const auto& lhs = workers[candidate];
            const auto& rhs = workers[selected];
            if(lhs.owned_documents < rhs.owned_documents) {
                selected = candidate;
                continue;
            }
            if(lhs.owned_documents == rhs.owned_documents &&
               lhs.inflight_requests < rhs.inflight_requests) {
                selected = candidate;
            }
        }
        return selected;
    }

    auto select_stateless_worker() const -> std::optional<std::size_t> {
        if(stateless_workers.empty()) {
            return std::nullopt;
        }

        auto selected = stateless_workers.front();
        for(auto candidate: stateless_workers) {
            if(workers[candidate].inflight_requests < workers[selected].inflight_requests) {
                selected = candidate;
            }
        }
        return selected;
    }

    void evict_from_worker(std::size_t worker_id, const std::string& uri) {
        if(worker_id >= workers.size()) {
            return;
        }

        auto status = workers[worker_id]->send_notification(WorkerEvictParams{
            .uri = uri,
        });
        (void)status;
    }

    auto start() -> std::expected<void, std::string> {
        if(started) {
            return {};
        }
        started = true;

        if(options.worker_count == 0) {
            return std::unexpected("worker_count cannot be 0");
        }
        if(options.self_path.empty()) {
            return std::unexpected("worker executable path is empty");
        }
        workers.reserve(options.worker_count + options.stateless_worker_count);

        for(std::size_t index = 0; index < options.worker_count; ++index) {
            auto spawned = spawn_worker_into_pool(WorkerRole::Stateful);
            if(!spawned) {
                return std::unexpected(std::move(spawned.error()));
            }
        }

        for(std::size_t index = 0; index < options.stateless_worker_count; ++index) {
            auto spawned = spawn_worker_into_pool(WorkerRole::Stateless);
            if(!spawned) {
                return std::unexpected(std::move(spawned.error()));
            }
        }

        return {};
    }

    auto compile(WorkerCompileParams params) -> et::task<jsonrpc::Result<WorkerCompileResult>> {
        if(auto ensured = ensure_stateful_worker(); !ensured) {
            co_return std::unexpected(std::move(ensured.error()));
        }

        const auto worker_index = assign_stateful_worker(params.uri);
        co_return co_await dispatch_request(worker_index, std::move(params));
    }

    auto hover(WorkerHoverParams params) -> et::task<jsonrpc::Result<WorkerHoverResult>> {
        if(auto ensured = ensure_stateful_worker(); !ensured) {
            co_return std::unexpected(std::move(ensured.error()));
        }

        const auto worker_index = assign_stateful_worker(params.uri);
        co_return co_await dispatch_request(worker_index, std::move(params));
    }

    auto completion(WorkerCompletionParams params)
        -> et::task<jsonrpc::Result<WorkerCompletionResult>> {
        auto selected = ensure_stateless_worker();
        if(!selected) {
            co_return std::unexpected(
                std::string("no stateless worker available for completion request: ") +
                selected.error());
        }

        co_return co_await dispatch_request(*selected, std::move(params));
    }

    auto signature_help(WorkerSignatureHelpParams params)
        -> et::task<jsonrpc::Result<WorkerSignatureHelpResult>> {
        auto selected = ensure_stateless_worker();
        if(!selected) {
            co_return std::unexpected(
                std::string("no stateless worker available for signature help request: ") +
                selected.error());
        }

        co_return co_await dispatch_request(*selected, std::move(params));
    }

    auto build_pch(WorkerBuildPCHParams params) -> et::task<jsonrpc::Result<WorkerBuildPCHResult>> {
        auto selected = ensure_stateless_worker();
        if(!selected) {
            co_return std::unexpected(
                std::string("no stateless worker available for pch build request: ") +
                selected.error());
        }

        co_return co_await dispatch_request(*selected, std::move(params));
    }

    auto build_pcm(WorkerBuildPCMParams params) -> et::task<jsonrpc::Result<WorkerBuildPCMResult>> {
        auto selected = ensure_stateless_worker();
        if(!selected) {
            co_return std::unexpected(
                std::string("no stateless worker available for pcm build request: ") +
                selected.error());
        }

        co_return co_await dispatch_request(*selected, std::move(params));
    }

    auto build_index(WorkerBuildIndexParams params)
        -> et::task<jsonrpc::Result<WorkerBuildIndexResult>> {
        auto selected = ensure_stateless_worker();
        if(!selected) {
            co_return std::unexpected(
                std::string("no stateless worker available for index build request: ") +
                selected.error());
        }

        co_return co_await dispatch_request(*selected, std::move(params));
    }

    auto shutdown() -> et::task<> {
        if(!started) {
            co_return;
        }
        started = false;

        for(auto& worker: workers) {
            if(worker.peer) {
                auto status = worker.peer->close_output();
                (void)status;
            }
        }

        for(auto& worker: workers) {
            auto role_name = worker_role_name(worker.role);
            auto waited = co_await worker.process.wait();
            if(waited) {
                LOG_INFO("[worker:{} pid={}] exited (shutdown): status={} signal={}",
                         role_name,
                         worker.pid,
                         static_cast<long long>(waited->status),
                         waited->term_signal);
                continue;
            }

            auto kill_status = worker.process.kill(SIGTERM);
            (void)kill_status;
            auto waited_after_kill = co_await worker.process.wait();
            if(waited_after_kill) {
                LOG_INFO("[worker:{} pid={}] exited (shutdown-kill): status={} signal={}",
                         role_name,
                         worker.pid,
                         static_cast<long long>(waited_after_kill->status),
                         waited_after_kill->term_signal);
            }
        }

        workers.clear();
        stateful_workers.clear();
        stateless_workers.clear();
        owner.clear();
        owner_lru.clear();
        owner_lru_index.clear();
    }

    void release_document(std::string_view uri) {
        auto key = std::string(uri);
        auto owner_iter = owner.find(key);
        if(owner_iter != owner.end()) {
            auto worker_id = owner_iter->second;
            auto& worker = workers[worker_id];
            if(worker.owned_documents > 0) {
                worker.owned_documents -= 1;
            }
            evict_from_worker(worker_id, key);
            owner.erase(owner_iter);
        }

        auto lru_iter = owner_lru_index.find(key);
        if(lru_iter != owner_lru_index.end()) {
            owner_lru.erase(lru_iter->second);
            owner_lru_index.erase(lru_iter);
        }
    }

    void set_compile_commands_paths(const std::vector<std::string>& paths) {
        compilation_service.set_compile_commands_paths(paths);
    }

    et::event_loop& loop;
    const Options& options;
    bool started = false;
    CompilationService compilation_service;

    std::vector<WorkerClient> workers;
    std::vector<std::size_t> stateful_workers;
    std::vector<std::size_t> stateless_workers;
    std::unordered_map<std::string, std::size_t> owner;
    std::list<std::string> owner_lru;
    std::unordered_map<std::string, std::list<std::string>::iterator> owner_lru_index;
};

WorkerPool::WorkerPool(et::event_loop& loop, const Options& options) :
    impl(std::make_unique<Impl>(loop, options)) {}

WorkerPool::~WorkerPool() = default;

WorkerPool::WorkerPool(WorkerPool&&) noexcept = default;

WorkerPool& WorkerPool::operator=(WorkerPool&&) noexcept = default;

void WorkerPool::set_compile_commands_paths(const std::vector<std::string>& paths) {
    impl->set_compile_commands_paths(paths);
}

auto WorkerPool::start() -> std::expected<void, std::string> {
    return impl->start();
}

auto WorkerPool::compile(WorkerCompileParams params)
    -> et::task<jsonrpc::Result<WorkerCompileResult>> {
    co_return co_await impl->compile(std::move(params));
}

auto WorkerPool::hover(WorkerHoverParams params) -> et::task<jsonrpc::Result<WorkerHoverResult>> {
    co_return co_await impl->hover(std::move(params));
}

auto WorkerPool::completion(WorkerCompletionParams params)
    -> et::task<jsonrpc::Result<WorkerCompletionResult>> {
    co_return co_await impl->completion(std::move(params));
}

auto WorkerPool::signature_help(WorkerSignatureHelpParams params)
    -> et::task<jsonrpc::Result<WorkerSignatureHelpResult>> {
    co_return co_await impl->signature_help(std::move(params));
}

auto WorkerPool::build_pch(WorkerBuildPCHParams params)
    -> et::task<jsonrpc::Result<WorkerBuildPCHResult>> {
    co_return co_await impl->build_pch(std::move(params));
}

auto WorkerPool::build_pcm(WorkerBuildPCMParams params)
    -> et::task<jsonrpc::Result<WorkerBuildPCMResult>> {
    co_return co_await impl->build_pcm(std::move(params));
}

auto WorkerPool::build_index(WorkerBuildIndexParams params)
    -> et::task<jsonrpc::Result<WorkerBuildIndexResult>> {
    co_return co_await impl->build_index(std::move(params));
}

auto WorkerPool::shutdown() -> et::task<> {
    co_await impl->shutdown();
}

void WorkerPool::release_document(std::string_view uri) {
    impl->release_document(uri);
}

}  // namespace clice::server
