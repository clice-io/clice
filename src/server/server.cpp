#include <csignal>
#include <cstdio>
#include <expected>
#include <list>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#include "eventide/async/cancellation.h"
#include "eventide/async/loop.h"
#include "eventide/async/process.h"
#include "eventide/async/stream.h"
#include "eventide/jsonrpc/peer.h"
#include "eventide/jsonrpc/transport.h"
#include "eventide/language/protocol.h"
#include "server/protocol.h"
#include "server/runtime.h"

namespace clice::server {

namespace {

auto print_error_message(std::string_view prefix, std::string_view message) -> void {
    std::fprintf(stderr,
                 "%.*s%.*s\n",
                 static_cast<int>(prefix.size()),
                 prefix.data(),
                 static_cast<int>(message.size()),
                 message.data());
}

auto request_id_key(const rpc::RequestID& id) -> std::string {
    return std::visit(
        [](const auto& value) -> std::string {
            using value_t = std::remove_cvref_t<decltype(value)>;
            if constexpr(std::is_same_v<value_t, rpc::integer>) {
                return "i:" + std::to_string(value);
            } else {
                return "s:" + value;
            }
        },
        id);
}

auto make_initialize_result() -> rpc::InitializeResult {
    rpc::InitializeResult result;

    rpc::TextDocumentSyncOptions sync;
    sync.open_close = true;
    sync.change = rpc::TextDocumentSyncKind::Full;
    sync.save = true;

    rpc::CompletionOptions completion;
    completion.resolve_provider = false;

    rpc::SignatureHelpOptions signature_help;
    signature_help.trigger_characters = std::vector<rpc::string>{"(", ","};

    result.capabilities.text_document_sync = std::move(sync);
    result.capabilities.hover_provider = true;
    result.capabilities.completion_provider = std::move(completion);
    result.capabilities.signature_help_provider = std::move(signature_help);
    result.server_info = rpc::ServerInfo{
        .name = "clice",
        .version = std::string("0.1.0"),
    };

    return result;
}

auto make_publish_diagnostics(std::string uri,
                              std::optional<int> version,
                              std::vector<rpc::Diagnostic> diagnostics = {})
    -> rpc::PublishDiagnosticsParams {
    rpc::PublishDiagnosticsParams payload{
        .uri = std::move(uri),
        .diagnostics = std::move(diagnostics),
    };
    if(version) {
        payload.version = static_cast<rpc::integer>(*version);
    }
    return payload;
}

class WorkerPool {
public:
    WorkerPool(et::event_loop& loop, const Options& options) : loop(loop), options(options) {}

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

        workers.reserve(options.worker_count);
        for(std::size_t index = 0; index < options.worker_count; ++index) {
            auto spawned = spawn_worker();
            if(!spawned) {
                return std::unexpected(std::move(spawned.error()));
            }
            workers.push_back(std::move(*spawned));
        }

        return {};
    }

    auto compile(WorkerCompileParams params) -> et::task<jsonrpc::Result<WorkerCompileResult>> {
        if(workers.empty()) {
            co_return std::unexpected("worker pool is empty");
        }

        const auto worker_index = assign_worker(params.uri);
        auto response = co_await workers[worker_index]->send_request(params);
        if(response) {
            co_return response;
        }

        auto restarted = restart_worker(worker_index);
        if(!restarted) {
            co_return std::unexpected("worker request failed: " + response.error() +
                                      "; worker restart failed: " + restarted.error());
        }

        co_return co_await workers[worker_index]->send_request(params);
    }

    auto hover(WorkerHoverParams params) -> et::task<jsonrpc::Result<WorkerHoverResult>> {
        if(workers.empty()) {
            co_return std::unexpected("worker pool is empty");
        }

        const auto worker_index = assign_worker(params.uri);
        auto response = co_await workers[worker_index]->send_request(params);
        if(response) {
            co_return response;
        }

        auto restarted = restart_worker(worker_index);
        if(!restarted) {
            co_return std::unexpected("worker request failed: " + response.error() +
                                      "; worker restart failed: " + restarted.error());
        }

        co_return co_await workers[worker_index]->send_request(params);
    }

    auto completion(WorkerCompletionParams params)
        -> et::task<jsonrpc::Result<WorkerCompletionResult>> {
        if(workers.empty()) {
            co_return std::unexpected("worker pool is empty");
        }

        const auto worker_index = assign_worker(params.uri);
        auto response = co_await workers[worker_index]->send_request(params);
        if(response) {
            co_return response;
        }

        auto restarted = restart_worker(worker_index);
        if(!restarted) {
            co_return std::unexpected("worker request failed: " + response.error() +
                                      "; worker restart failed: " + restarted.error());
        }

        co_return co_await workers[worker_index]->send_request(params);
    }

    auto signature_help(WorkerSignatureHelpParams params)
        -> et::task<jsonrpc::Result<WorkerSignatureHelpResult>> {
        if(workers.empty()) {
            co_return std::unexpected("worker pool is empty");
        }

        const auto worker_index = assign_worker(params.uri);
        auto response = co_await workers[worker_index]->send_request(params);
        if(response) {
            co_return response;
        }

        auto restarted = restart_worker(worker_index);
        if(!restarted) {
            co_return std::unexpected("worker request failed: " + response.error() +
                                      "; worker restart failed: " + restarted.error());
        }

        co_return co_await workers[worker_index]->send_request(params);
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
            auto waited = co_await worker.process.wait();
            if(waited) {
                continue;
            }

            auto kill_status = worker.process.kill(SIGTERM);
            (void)kill_status;
            auto waited_after_kill = co_await worker.process.wait();
            (void)waited_after_kill;
        }

        workers.clear();
        owner.clear();
        owner_lru.clear();
        owner_lru_index.clear();
    }

private:
    struct WorkerClient {
        et::process process;
        std::shared_ptr<jsonrpc::Peer> peer;
        std::size_t owned_documents = 0;

        auto operator->() -> jsonrpc::Peer* {
            return peer.get();
        }
    };

    static auto run_worker_peer(std::shared_ptr<jsonrpc::Peer> peer) -> et::task<> {
        co_await peer->run();
    }

    auto spawn_worker() -> std::expected<WorkerClient, std::string> {
        et::process::options process_options;
        process_options.file = options.self_path;
        process_options.args = {
            options.self_path,
            std::string(k_worker_mode),
            "--worker-doc-capacity=" + std::to_string(options.worker_document_capacity),
        };
        process_options.streams = {
            et::process::stdio::pipe(true, false),
            et::process::stdio::pipe(false, true),
            et::process::stdio::inherit(),
        };

        auto spawned = et::process::spawn(process_options, loop);
        if(!spawned) {
            return std::unexpected(std::string(spawned.error().message()));
        }

        auto transport = std::make_unique<jsonrpc::StreamTransport>(std::move(spawned->stdout_pipe),
                                                                    std::move(spawned->stdin_pipe));
        auto peer = std::make_shared<jsonrpc::Peer>(loop, std::move(transport));
        loop.schedule(run_worker_peer(peer));

        WorkerClient client;
        client.process = std::move(spawned->proc);
        client.peer = std::move(peer);
        return client;
    }

    auto restart_worker(std::size_t worker_index) -> std::expected<void, std::string> {
        if(worker_index >= workers.size()) {
            return std::unexpected("worker index out of range");
        }

        auto replacement = spawn_worker();
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
            loop.schedule(reap_worker_process(std::move(old_process)));
        }

        workers[worker_index].process = std::move(replacement->process);
        workers[worker_index].peer = std::move(replacement->peer);
        return {};
    }

    auto reap_worker_process(et::process process) -> et::task<> {
        auto waited = co_await process.wait();
        (void)waited;
    }

    auto assign_worker(std::string_view uri) -> std::size_t {
        auto key = std::string(uri);
        auto owner_iter = owner.find(key);
        if(owner_iter != owner.end()) {
            touch_owner_lru(key);
            return owner_iter->second;
        }

        shrink_owner();
        const auto selected = pick_worker();
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

    auto pick_worker() const -> std::size_t {
        if(workers.empty()) {
            return 0;
        }

        std::size_t selected = 0;
        for(std::size_t index = 1; index < workers.size(); ++index) {
            if(workers[index].owned_documents < workers[selected].owned_documents) {
                selected = index;
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

private:
    et::event_loop& loop;
    const Options& options;
    bool started = false;

    std::vector<WorkerClient> workers;
    std::unordered_map<std::string, std::size_t> owner;
    std::list<std::string> owner_lru;
    std::unordered_map<std::string, std::list<std::string>::iterator> owner_lru_index;
};

class MasterServer {
public:
    MasterServer(et::event_loop& loop, jsonrpc::Peer& peer, const Options& options) :
        loop(loop), peer(peer), workers(loop, options) {
        register_callbacks();
    }

    auto start() -> std::expected<void, std::string> {
        return workers.start();
    }

    [[nodiscard]] auto exit_code() const -> int {
        return requested_exit_code;
    }

private:
    struct DocumentState {
        int version = 0;
        std::string text;
        std::uint64_t generation = 0;
        et::cancellation_source cancel_source;
        bool build_running = false;
        bool build_requested = false;
    };

    struct RequestState {
        et::cancellation_source cancel_source;
    };

    struct HoverRequestSnapshot {
        std::string request_key;
        std::string uri;
        int version = 0;
        std::uint64_t generation = 0;
        std::string text;
        int line = 0;
        int character = 0;
        et::cancellation_token request_token;
        et::cancellation_token document_token;
    };

    using CompletionRequestSnapshot = HoverRequestSnapshot;
    using SignatureHelpRequestSnapshot = HoverRequestSnapshot;

    void register_callbacks() {
        peer.on_request(
            [this](jsonrpc::RequestContext& context, const rpc::InitializeParams& params)
                -> jsonrpc::RequestResult<rpc::InitializeParams> {
                return on_initialize(context, params);
            });

        peer.on_request([this](jsonrpc::RequestContext& context, const rpc::ShutdownParams& params)
                            -> jsonrpc::RequestResult<rpc::ShutdownParams> {
            return on_shutdown(context, params);
        });

        peer.on_request(
            [this](jsonrpc::RequestContext& context,
                   const rpc::HoverParams& params) -> jsonrpc::RequestResult<rpc::HoverParams> {
                return on_hover(context, params);
            });

        peer.on_request(
            [this](jsonrpc::RequestContext& context, const rpc::CompletionParams& params)
                -> jsonrpc::RequestResult<rpc::CompletionParams> {
                return on_completion(context, params);
            });

        peer.on_request(
            [this](jsonrpc::RequestContext& context, const rpc::SignatureHelpParams& params)
                -> jsonrpc::RequestResult<rpc::SignatureHelpParams> {
                return on_signature_help(context, params);
            });

        peer.on_notification([this](const rpc::InitializedParams&) {
            if(!initialize_request || shutdown_request) {
                return;
            }
            initialized_notification = true;
        });

        peer.on_notification(
            [this](const rpc::CancelParams& params) { on_cancel_request(params); });

        peer.on_notification([this](const rpc::ExitParams&) {
            if(exiting) {
                return;
            }
            exiting = true;
            requested_exit_code = shutdown_request ? 0 : 1;
            loop.schedule(stop());
        });

        peer.on_notification(
            [this](const rpc::DidOpenTextDocumentParams& params) { on_did_open(params); });

        peer.on_notification(
            [this](const rpc::DidChangeTextDocumentParams& params) { on_did_change(params); });

        peer.on_notification(
            [this](const rpc::DidSaveTextDocumentParams& params) { on_did_save(params); });

        peer.on_notification(
            [this](const rpc::DidCloseTextDocumentParams& params) { on_did_close(params); });
    }

    auto on_initialize(jsonrpc::RequestContext&, const rpc::InitializeParams&)
        -> jsonrpc::RequestResult<rpc::InitializeParams> {
        if(initialize_request) {
            co_return std::unexpected("initialize can only be requested once");
        }
        if(shutdown_request) {
            co_return std::unexpected("server is shutting down");
        }

        initialize_request = true;
        co_return make_initialize_result();
    }

    auto on_shutdown(jsonrpc::RequestContext&, const rpc::ShutdownParams&)
        -> jsonrpc::RequestResult<rpc::ShutdownParams> {
        if(!initialize_request) {
            co_return std::unexpected("server is not initialized");
        }
        if(shutdown_request) {
            co_return std::unexpected("shutdown has already been requested");
        }

        shutdown_request = true;
        cancel_all_requests();
        cancel_all_builds();
        co_return nullptr;
    }

    auto on_hover(jsonrpc::RequestContext& context, const rpc::HoverParams& params)
        -> jsonrpc::RequestResult<rpc::HoverParams> {
        if(!initialize_request) {
            co_return std::unexpected("server is not initialized");
        }
        if(shutdown_request) {
            co_return std::unexpected("server is shutting down");
        }

        const auto& tdpp = params.text_document_position_params;
        const auto& uri = tdpp.text_document.uri;
        auto line = static_cast<int>(tdpp.position.line);
        auto character = static_cast<int>(tdpp.position.character);

        auto doc_iter = documents.find(uri);
        if(doc_iter == documents.end()) {
            co_return std::nullopt;
        }

        auto request_key = request_id_key(context.id);
        auto& request = active_requests[request_key];
        request.cancel_source.cancel();
        request.cancel_source = et::cancellation_source{};

        auto snapshot = HoverRequestSnapshot{
            .request_key = std::move(request_key),
            .uri = std::string(uri),
            .version = doc_iter->second.version,
            .generation = doc_iter->second.generation,
            .text = doc_iter->second.text,
            .line = line,
            .character = character,
            .request_token = request.cancel_source.token(),
            .document_token = doc_iter->second.cancel_source.token(),
        };

        co_return co_await run_hover(std::move(snapshot));
    }

    auto on_completion(jsonrpc::RequestContext& context, const rpc::CompletionParams& params)
        -> jsonrpc::RequestResult<rpc::CompletionParams> {
        if(!initialize_request) {
            co_return std::unexpected("server is not initialized");
        }
        if(shutdown_request) {
            co_return std::unexpected("server is shutting down");
        }

        const auto& tdpp = params.text_document_position_params;
        const auto& uri = tdpp.text_document.uri;
        auto line = static_cast<int>(tdpp.position.line);
        auto character = static_cast<int>(tdpp.position.character);

        auto doc_iter = documents.find(uri);
        if(doc_iter == documents.end()) {
            co_return nullptr;
        }

        auto request_key = request_id_key(context.id);
        auto& request = active_requests[request_key];
        request.cancel_source.cancel();
        request.cancel_source = et::cancellation_source{};

        auto snapshot = CompletionRequestSnapshot{
            .request_key = std::move(request_key),
            .uri = std::string(uri),
            .version = doc_iter->second.version,
            .generation = doc_iter->second.generation,
            .text = doc_iter->second.text,
            .line = line,
            .character = character,
            .request_token = request.cancel_source.token(),
            .document_token = doc_iter->second.cancel_source.token(),
        };

        co_return co_await run_completion(std::move(snapshot));
    }

    auto on_signature_help(jsonrpc::RequestContext& context, const rpc::SignatureHelpParams& params)
        -> jsonrpc::RequestResult<rpc::SignatureHelpParams> {
        if(!initialize_request) {
            co_return std::unexpected("server is not initialized");
        }
        if(shutdown_request) {
            co_return std::unexpected("server is shutting down");
        }

        const auto& tdpp = params.text_document_position_params;
        const auto& uri = tdpp.text_document.uri;
        auto line = static_cast<int>(tdpp.position.line);
        auto character = static_cast<int>(tdpp.position.character);

        auto doc_iter = documents.find(uri);
        if(doc_iter == documents.end()) {
            co_return std::nullopt;
        }

        auto request_key = request_id_key(context.id);
        auto& request = active_requests[request_key];
        request.cancel_source.cancel();
        request.cancel_source = et::cancellation_source{};

        auto snapshot = SignatureHelpRequestSnapshot{
            .request_key = std::move(request_key),
            .uri = std::string(uri),
            .version = doc_iter->second.version,
            .generation = doc_iter->second.generation,
            .text = doc_iter->second.text,
            .line = line,
            .character = character,
            .request_token = request.cancel_source.token(),
            .document_token = doc_iter->second.cancel_source.token(),
        };

        co_return co_await run_signature_help(std::move(snapshot));
    }

    void on_cancel_request(const rpc::CancelParams& params) {
        cancel_request(params.id);
    }

    void on_did_open(const rpc::DidOpenTextDocumentParams& params) {
        if(!accept_document_notifications()) {
            return;
        }

        auto& document = documents[params.text_document.uri];
        document.version = static_cast<int>(params.text_document.version);
        document.text = params.text_document.text;
        document.generation += 1;

        schedule_build(params.text_document.uri);
    }

    void on_did_change(const rpc::DidChangeTextDocumentParams& params) {
        if(!accept_document_notifications()) {
            return;
        }

        std::optional<std::string> latest_text;
        for(const auto& change: params.content_changes) {
            if(auto whole = std::get_if<rpc::TextDocumentContentChangeWholeDocument>(&change)) {
                latest_text = whole->text;
                continue;
            }
            if(auto partial = std::get_if<rpc::TextDocumentContentChangePartial>(&change)) {
                latest_text = partial->text;
            }
        }
        if(!latest_text) {
            return;
        }

        auto& document = documents[params.text_document.uri];
        document.version = static_cast<int>(params.text_document.version);
        document.text = std::move(*latest_text);
        document.generation += 1;

        schedule_build(params.text_document.uri);
    }

    void on_did_save(const rpc::DidSaveTextDocumentParams& params) {
        if(!accept_document_notifications()) {
            return;
        }

        auto doc_iter = documents.find(params.text_document.uri);
        if(doc_iter == documents.end()) {
            return;
        }

        auto& document = doc_iter->second;
        if(params.text) {
            document.text = *params.text;
        }
        document.generation += 1;
        schedule_build(params.text_document.uri);
    }

    void on_did_close(const rpc::DidCloseTextDocumentParams& params) {
        if(!accept_document_notifications()) {
            return;
        }

        auto uri = std::string(params.text_document.uri);
        auto doc_iter = documents.find(uri);
        if(doc_iter != documents.end()) {
            doc_iter->second.cancel_source.cancel();
            documents.erase(doc_iter);
        }
        workers.release_document(uri);

        auto status =
            peer.send_notification(make_publish_diagnostics(std::move(uri), std::nullopt));
        (void)status;
    }

    auto run_hover(HoverRequestSnapshot snapshot) -> jsonrpc::RequestResult<rpc::HoverParams> {
        auto finish = [this, request_key = snapshot.request_key]() {
            finish_request(request_key);
        };

        WorkerHoverParams params{
            .uri = snapshot.uri,
            .version = snapshot.version,
            .text = snapshot.text,
            .line = snapshot.line,
            .character = snapshot.character,
        };

        auto guarded_by_document =
            et::with_token(snapshot.document_token, workers.hover(std::move(params)));
        auto guarded =
            co_await et::with_token(snapshot.request_token, std::move(guarded_by_document));
        if(!guarded.has_value()) {
            finish();
            co_return std::nullopt;
        }

        auto hover_result = std::move(*guarded);
        if(!hover_result) {
            finish();
            co_return std::nullopt;
        }

        auto latest_iter = documents.find(snapshot.uri);
        if(latest_iter == documents.end() ||
           latest_iter->second.generation != snapshot.generation) {
            finish();
            co_return std::nullopt;
        }

        finish();
        co_return std::move(hover_result->result);
    }

    auto run_completion(CompletionRequestSnapshot snapshot)
        -> jsonrpc::RequestResult<rpc::CompletionParams> {
        auto finish = [this, request_key = snapshot.request_key]() {
            finish_request(request_key);
        };

        WorkerCompletionParams params{
            .uri = snapshot.uri,
            .version = snapshot.version,
            .text = snapshot.text,
            .line = snapshot.line,
            .character = snapshot.character,
        };

        auto guarded_by_document =
            et::with_token(snapshot.document_token, workers.completion(std::move(params)));
        auto guarded =
            co_await et::with_token(snapshot.request_token, std::move(guarded_by_document));
        if(!guarded.has_value()) {
            finish();
            co_return nullptr;
        }

        auto completion_result = std::move(*guarded);
        if(!completion_result) {
            finish();
            co_return nullptr;
        }

        auto latest_iter = documents.find(snapshot.uri);
        if(latest_iter == documents.end() ||
           latest_iter->second.generation != snapshot.generation) {
            finish();
            co_return nullptr;
        }

        finish();
        co_return std::move(completion_result->result);
    }

    auto run_signature_help(SignatureHelpRequestSnapshot snapshot)
        -> jsonrpc::RequestResult<rpc::SignatureHelpParams> {
        auto finish = [this, request_key = snapshot.request_key]() {
            finish_request(request_key);
        };

        WorkerSignatureHelpParams params{
            .uri = snapshot.uri,
            .version = snapshot.version,
            .text = snapshot.text,
            .line = snapshot.line,
            .character = snapshot.character,
        };

        auto guarded_by_document =
            et::with_token(snapshot.document_token, workers.signature_help(std::move(params)));
        auto guarded =
            co_await et::with_token(snapshot.request_token, std::move(guarded_by_document));
        if(!guarded.has_value()) {
            finish();
            co_return std::nullopt;
        }

        auto signature_help_result = std::move(*guarded);
        if(!signature_help_result) {
            finish();
            co_return std::nullopt;
        }

        auto latest_iter = documents.find(snapshot.uri);
        if(latest_iter == documents.end() ||
           latest_iter->second.generation != snapshot.generation) {
            finish();
            co_return std::nullopt;
        }

        finish();
        co_return std::move(signature_help_result->result);
    }

    [[nodiscard]] auto accept_document_notifications() const -> bool {
        return initialize_request && !shutdown_request;
    }

    void cancel_request(const rpc::RequestID& request_id) {
        auto key = request_id_key(request_id);
        auto request_iter = active_requests.find(key);
        if(request_iter == active_requests.end()) {
            return;
        }

        request_iter->second.cancel_source.cancel();
    }

    void finish_request(std::string_view request_key) {
        active_requests.erase(std::string(request_key));
    }

    void schedule_build(std::string uri) {
        auto doc_iter = documents.find(uri);
        if(doc_iter == documents.end()) {
            return;
        }

        auto& document = doc_iter->second;
        document.cancel_source.cancel();
        document.cancel_source = et::cancellation_source{};
        document.build_requested = true;

        if(document.build_running) {
            return;
        }
        document.build_running = true;

        loop.schedule(run_build_drain(std::move(uri)));
    }

    auto run_build_drain(std::string uri) -> et::task<> {
        while(true) {
            auto doc_iter = documents.find(uri);
            if(doc_iter == documents.end()) {
                co_return;
            }

            auto& document = doc_iter->second;
            if(!document.build_requested) {
                document.build_running = false;
                co_return;
            }

            document.build_requested = false;
            const auto generation = document.generation;
            const auto token = document.cancel_source.token();
            WorkerCompileParams params{
                .uri = uri,
                .version = document.version,
                .text = document.text,
            };

            auto guarded = co_await et::with_token(token, workers.compile(std::move(params)));
            if(!guarded.has_value()) {
                continue;
            }

            auto compile_result = std::move(*guarded);
            if(!compile_result) {
                continue;
            }

            auto latest_iter = documents.find(uri);
            if(latest_iter == documents.end()) {
                co_return;
            }
            if(latest_iter->second.generation != generation) {
                continue;
            }

            auto status = peer.send_notification(
                make_publish_diagnostics(compile_result->uri,
                                         compile_result->version,
                                         std::move(compile_result->diagnostics)));
            (void)status;
        }
    }

    auto stop() -> et::task<> {
        if(stopping) {
            co_return;
        }
        stopping = true;

        cancel_all_requests();
        cancel_all_builds();
        co_await workers.shutdown();
        loop.stop();
    }

    void cancel_all_requests() {
        for(auto& [_, request]: active_requests) {
            request.cancel_source.cancel();
        }
        active_requests.clear();
    }

    void cancel_all_builds() {
        for(auto& [_, doc]: documents) {
            doc.cancel_source.cancel();
        }
    }

private:
    et::event_loop& loop;
    jsonrpc::Peer& peer;
    WorkerPool workers;

    bool initialize_request = false;
    bool initialized_notification = false;
    bool shutdown_request = false;
    bool exiting = false;
    bool stopping = false;
    int requested_exit_code = 0;

    std::unordered_map<std::string, DocumentState> documents;
    std::unordered_map<std::string, RequestState> active_requests;
};

auto run_master_session(et::event_loop& loop,
                        std::unique_ptr<jsonrpc::Transport> transport,
                        const Options& options) -> int {
    jsonrpc::Peer peer(loop, std::move(transport));
    MasterServer server(loop, peer, options);

    auto started = server.start();
    if(!started) {
        std::fprintf(stderr, "failed to start worker pool: %s\n", started.error().c_str());
        return 1;
    }

    loop.schedule(peer.run());
    auto loop_status = loop.run();
    if(loop_status != 0) {
        return loop_status;
    }
    return server.exit_code();
}

}  // namespace

auto run_pipe_mode(const Options& options) -> int {
    et::event_loop loop;
    auto stdio = jsonrpc::StreamTransport::open_stdio(loop);
    if(!stdio) {
        std::fprintf(stderr, "failed to open stdio transport: %s\n", stdio.error().c_str());
        return 1;
    }

    std::unique_ptr<jsonrpc::Transport> transport = std::move(*stdio);
    return run_master_session(loop, std::move(transport), options);
}

auto run_socket_mode(const Options& options) -> int {
    et::event_loop loop;

    auto listener_result = et::tcp_socket::listen(options.host, options.port, {}, loop);
    if(!listener_result) {
        print_error_message("failed to listen: ", listener_result.error().message());
        return 1;
    }

    auto listener = std::move(*listener_result);
    auto accept_task = listener.accept();
    loop.schedule(accept_task);
    auto loop_status = loop.run();
    if(loop_status != 0) {
        return loop_status;
    }

    auto accepted = accept_task.value();
    listener = {};
    if(!accepted || !accepted->has_value()) {
        if(accepted && !accepted->has_value()) {
            print_error_message("failed to accept connection: ", accepted->error().message());
        } else {
            std::fprintf(stderr, "failed to accept connection: unknown error\n");
        }
        return 1;
    }

    auto socket = std::move(**accepted);
    auto stream = et::stream(std::move(socket));
    auto transport = std::make_unique<jsonrpc::StreamTransport>(std::move(stream));
    return run_master_session(loop, std::move(transport), options);
}

}  // namespace clice::server
