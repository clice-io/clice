#include "server/master_server.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

#include "compile/preamble.h"
#include "eventide/language/protocol.h"
#include "eventide/language/uri.h"
#include "server/config.h"
#include "server/protocol.h"
#include "server/worker_pool.h"
#include "support/filesystem.h"
#include "support/logging.h"

namespace clice::server {

namespace {

namespace language = eventide::language;
namespace fs_std = std::filesystem;

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

auto normalized_path(fs_std::path path) -> std::string {
    std::error_code ec;
    auto canonical = fs_std::weakly_canonical(path, ec);
    if(!ec) {
        return canonical.string();
    }
    return path.lexically_normal().string();
}

auto resolve_workspace_path(std::string_view uri_or_path)
    -> std::expected<std::string, std::string> {
    if(uri_or_path.empty()) {
        return std::unexpected("workspace path is empty");
    }

    if(auto parsed = language::URI::parse(uri_or_path)) {
        if(!parsed->is_file()) {
            return std::unexpected("workspace URI must use file:// scheme");
        }

        auto file_path = parsed->file_path();
        if(!file_path) {
            return std::unexpected(file_path.error());
        }
        return normalized_path(*file_path);
    }

    fs_std::path candidate(uri_or_path);
    if(candidate.is_absolute()) {
        return normalized_path(std::move(candidate));
    }

    return std::unexpected("workspace path must be absolute");
}

auto workspace_from_initialize_params(const rpc::InitializeParams& params)
    -> std::expected<std::string, std::string> {
    const auto& workspace_folders = params.workspace_folders_initialize_params.workspace_folders;
    if(workspace_folders && *workspace_folders && !(*workspace_folders)->empty()) {
        return resolve_workspace_path((*workspace_folders)->front().uri);
    }

    const auto& root_uri = params.lsp__initialize_params.root_uri;
    if(root_uri) {
        return resolve_workspace_path(*root_uri);
    }

    return std::unexpected("initialize request is missing workspaceFolders and rootUri");
}

}  // namespace

struct MasterServer::Impl {
    struct DocumentState {
        int version = 0;
        std::string text;
        std::uint64_t generation = 0;
        bool build_running = false;
        bool build_requested = false;
    };

    struct HoverRequestSnapshot {
        std::string uri;
        int version = 0;
        std::uint64_t generation = 0;
        std::string text;
        int line = 0;
        int character = 0;
    };

    using CompletionRequestSnapshot = HoverRequestSnapshot;
    using SignatureHelpRequestSnapshot = HoverRequestSnapshot;

    struct StatelessPCHState {
        std::string output_path;
        std::string preamble;
        std::uint32_t preamble_bound = 0;
    };

    struct StatelessPCHBinding {
        std::string path;
        std::uint32_t preamble_bound = 0;
    };

    Impl(et::event_loop& loop, jsonrpc::Peer& peer, const Options& options) :
        loop(loop), peer(peer), workers(loop, options) {
        register_callbacks();
    }

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

    auto on_initialize(jsonrpc::RequestContext&, const rpc::InitializeParams& params)
        -> jsonrpc::RequestResult<rpc::InitializeParams> {
        if(initialize_request) {
            co_return std::unexpected("initialize can only be requested once");
        }
        if(shutdown_request) {
            co_return std::unexpected("server is shutting down");
        }

        auto workspace = workspace_from_initialize_params(params);
        if(!workspace) {
            LOG_ERROR("initialize failed: {}", workspace.error());
            co_return std::unexpected(std::move(workspace.error()));
        }

        config = ServerConfig{};
        auto parsed = config.parse(*workspace);
        if(parsed) {
            LOG_INFO("config initialized from workspace: {}", config.workspace);
        } else {
            LOG_WARN("failed to parse config for workspace {}: {}", *workspace, parsed.error());
            LOG_INFO("using default config for workspace: {}", config.workspace);
        }

        workers.set_compile_commands_paths(config.project.compile_commands_paths);

        if(!config.project.logging_dir.empty()) {
            if(auto error = fs::create_directories(config.project.logging_dir)) {
                LOG_WARN("failed to create logging dir {}: {}",
                         config.project.logging_dir,
                         error.message());
            } else {
                logging::file_loggger("clice", config.project.logging_dir, logging::options);
                LOG_INFO("server logging redirected to {}", config.project.logging_dir);
            }
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
        co_return nullptr;
    }

    auto on_hover(jsonrpc::RequestContext&, const rpc::HoverParams& params)
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

        auto snapshot = HoverRequestSnapshot{
            .uri = std::string(uri),
            .version = doc_iter->second.version,
            .generation = doc_iter->second.generation,
            .text = doc_iter->second.text,
            .line = line,
            .character = character,
        };

        co_return co_await run_hover(std::move(snapshot));
    }

    auto on_completion(jsonrpc::RequestContext&, const rpc::CompletionParams& params)
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

        auto snapshot = CompletionRequestSnapshot{
            .uri = std::string(uri),
            .version = doc_iter->second.version,
            .generation = doc_iter->second.generation,
            .text = doc_iter->second.text,
            .line = line,
            .character = character,
        };

        co_return co_await run_completion(std::move(snapshot));
    }

    auto on_signature_help(jsonrpc::RequestContext&, const rpc::SignatureHelpParams& params)
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

        auto snapshot = SignatureHelpRequestSnapshot{
            .uri = std::string(uri),
            .version = doc_iter->second.version,
            .generation = doc_iter->second.generation,
            .text = doc_iter->second.text,
            .line = line,
            .character = character,
        };

        co_return co_await run_signature_help(std::move(snapshot));
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
        documents.erase(uri);
        stateless_pch.erase(uri);
        workers.release_document(uri);

        auto status =
            peer.send_notification(make_publish_diagnostics(std::move(uri), std::nullopt));
        (void)status;
    }

    auto run_hover(HoverRequestSnapshot snapshot) -> jsonrpc::RequestResult<rpc::HoverParams> {
        WorkerHoverParams params{
            .uri = snapshot.uri,
            .version = snapshot.version,
            .text = snapshot.text,
            .line = snapshot.line,
            .character = snapshot.character,
        };

        auto hover_result = co_await workers.hover(std::move(params));
        if(!hover_result) {
            co_return std::nullopt;
        }

        auto latest_iter = documents.find(snapshot.uri);
        if(latest_iter == documents.end() ||
           latest_iter->second.generation != snapshot.generation) {
            co_return std::nullopt;
        }

        co_return std::move(hover_result->result);
    }

    auto ensure_stateless_pch(const HoverRequestSnapshot& snapshot)
        -> et::task<std::optional<StatelessPCHBinding>> {
        auto preamble_bound = compute_preamble_bound(snapshot.text);
        if(preamble_bound == 0) {
            stateless_pch.erase(snapshot.uri);
            co_return std::nullopt;
        }

        auto preamble = snapshot.text.substr(0, preamble_bound);
        auto& cache = stateless_pch[snapshot.uri];
        const bool need_rebuild = cache.output_path.empty() ||
                                  cache.preamble_bound != preamble_bound ||
                                  cache.preamble != preamble;

        if(need_rebuild) {
            WorkerBuildPCHParams build_params{
                .uri = snapshot.uri,
                .text = snapshot.text,
                .output_path = cache.output_path,
            };
            auto built = co_await workers.build_pch(std::move(build_params));
            if(!built || !built->built || built->output_path.empty()) {
                stateless_pch.erase(snapshot.uri);
                co_return std::nullopt;
            }

            cache.output_path = std::move(built->output_path);
            cache.preamble = std::move(preamble);
            cache.preamble_bound = preamble_bound;
        }

        co_return StatelessPCHBinding{
            .path = cache.output_path,
            .preamble_bound = cache.preamble_bound,
        };
    }

    auto run_completion(CompletionRequestSnapshot snapshot)
        -> jsonrpc::RequestResult<rpc::CompletionParams> {
        auto pch = co_await ensure_stateless_pch(snapshot);

        auto latest_iter = documents.find(snapshot.uri);
        if(latest_iter == documents.end() ||
           latest_iter->second.generation != snapshot.generation) {
            co_return nullptr;
        }

        WorkerCompletionParams params{
            .uri = snapshot.uri,
            .version = snapshot.version,
            .text = snapshot.text,
            .line = snapshot.line,
            .character = snapshot.character,
        };
        if(pch) {
            params.pch_path = std::move(pch->path);
            params.pch_preamble_bound = pch->preamble_bound;
        }

        auto completion_result = co_await workers.completion(std::move(params));
        if(!completion_result) {
            co_return std::unexpected(std::move(completion_result.error()));
        }

        latest_iter = documents.find(snapshot.uri);
        if(latest_iter == documents.end() ||
           latest_iter->second.generation != snapshot.generation) {
            co_return nullptr;
        }

        co_return std::move(completion_result->result);
    }

    auto run_signature_help(SignatureHelpRequestSnapshot snapshot)
        -> jsonrpc::RequestResult<rpc::SignatureHelpParams> {
        auto pch = co_await ensure_stateless_pch(snapshot);

        auto latest_iter = documents.find(snapshot.uri);
        if(latest_iter == documents.end() ||
           latest_iter->second.generation != snapshot.generation) {
            co_return std::nullopt;
        }

        WorkerSignatureHelpParams params{
            .uri = snapshot.uri,
            .version = snapshot.version,
            .text = snapshot.text,
            .line = snapshot.line,
            .character = snapshot.character,
        };
        if(pch) {
            params.pch_path = std::move(pch->path);
            params.pch_preamble_bound = pch->preamble_bound;
        }

        auto signature_help_result = co_await workers.signature_help(std::move(params));
        if(!signature_help_result) {
            co_return std::unexpected(std::move(signature_help_result.error()));
        }

        latest_iter = documents.find(snapshot.uri);
        if(latest_iter == documents.end() ||
           latest_iter->second.generation != snapshot.generation) {
            co_return std::nullopt;
        }

        co_return std::move(signature_help_result->result);
    }

    [[nodiscard]] auto accept_document_notifications() const -> bool {
        return initialize_request && !shutdown_request;
    }

    void schedule_build(std::string uri) {
        auto doc_iter = documents.find(uri);
        if(doc_iter == documents.end()) {
            return;
        }

        auto& document = doc_iter->second;
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
            WorkerCompileParams params{
                .uri = uri,
                .version = document.version,
                .text = document.text,
            };

            auto compile_result = co_await workers.compile(std::move(params));
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

        co_await workers.shutdown();
        loop.stop();
    }

    auto start() -> std::expected<void, std::string> {
        return workers.start();
    }

    [[nodiscard]] auto exit_code() const -> int {
        return requested_exit_code;
    }

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
    std::unordered_map<std::string, StatelessPCHState> stateless_pch;
    ServerConfig config;
};

MasterServer::MasterServer(et::event_loop& loop, jsonrpc::Peer& peer, const Options& options) :
    impl(std::make_unique<Impl>(loop, peer, options)) {}

MasterServer::~MasterServer() = default;

MasterServer::MasterServer(MasterServer&&) noexcept = default;

MasterServer& MasterServer::operator=(MasterServer&&) noexcept = default;

auto MasterServer::start() -> std::expected<void, std::string> {
    return impl->start();
}

auto MasterServer::exit_code() const -> int {
    return impl->exit_code();
}

}  // namespace clice::server
