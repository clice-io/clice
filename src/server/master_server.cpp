#include "server/master_server.h"

#include "spdlog/spdlog.h"

#include <cstddef>
#include <string>
#include <type_traits>
#include <variant>
#include <vector>

namespace clice {

using et::ipc::RequestResult;
using RequestContext = et::ipc::JsonPeer::RequestContext;

MasterServer::MasterServer(et::event_loop& loop, et::ipc::JsonPeer& peer)
    : loop(loop), peer(peer), pool(loop) {}

std::string MasterServer::uri_to_path(const std::string& uri) {
    if(uri.starts_with("file://")) {
        return uri.substr(7);
    }
    return uri;
}

void MasterServer::publish_diagnostics(const std::string& uri, int version,
                                        std::vector<protocol::Diagnostic> diagnostics) {
    protocol::PublishDiagnosticsParams params;
    params.uri = uri;
    params.version = version;
    params.diagnostics = std::move(diagnostics);
    peer.send_notification(params);
}

void MasterServer::clear_diagnostics(const std::string& uri) {
    protocol::PublishDiagnosticsParams params;
    params.uri = uri;
    params.diagnostics = {};
    peer.send_notification(params);
}

void MasterServer::register_handlers() {
    // === initialize ===
    peer.on_request([this](RequestContext& ctx, const protocol::InitializeParams& params)
                        -> RequestResult<protocol::InitializeParams> {
        if(lifecycle != ServerLifecycle::Uninitialized) {
            co_return et::outcome_error(protocol::Error{"Server already initialized"});
        }

        // Extract workspace root
        auto& init = params.lsp__initialize_params;
        if(init.root_uri.has_value()) {
            workspace_root = uri_to_path(*init.root_uri);
        }

        lifecycle = ServerLifecycle::Initialized;

        spdlog::info("Initialized with workspace: {}", workspace_root);

        // Build capabilities
        protocol::InitializeResult result;

        // Text document sync: incremental
        protocol::TextDocumentSyncOptions sync_opts;
        sync_opts.open_close = true;
        sync_opts.change = protocol::TextDocumentSyncKind::Incremental;
        sync_opts.save = protocol::variant<protocol::boolean, protocol::SaveOptions>{true};
        result.capabilities.text_document_sync = std::move(sync_opts);

        // Feature capabilities
        result.capabilities.hover_provider = true;
        result.capabilities.completion_provider = protocol::CompletionOptions{};
        result.capabilities.signature_help_provider = protocol::SignatureHelpOptions{};
        result.capabilities.definition_provider = true;
        result.capabilities.document_symbol_provider = true;
        result.capabilities.document_link_provider = protocol::DocumentLinkOptions{};
        result.capabilities.code_action_provider = true;
        result.capabilities.folding_range_provider = true;
        result.capabilities.inlay_hint_provider = true;

        // Semantic tokens
        protocol::SemanticTokensOptions sem_opts;
        sem_opts.legend = protocol::SemanticTokensLegend{{}, {}};
        sem_opts.full = protocol::variant<protocol::boolean, protocol::SemanticTokensFullDelta>{true};
        result.capabilities.semantic_tokens_provider = std::move(sem_opts);

        // Server info
        protocol::ServerInfo info;
        info.name = "clice";
        info.version = "0.1.0";
        result.server_info = std::move(info);

        co_return result;
    });

    // === initialized ===
    peer.on_notification([this](const protocol::InitializedParams& params) {
        lifecycle = ServerLifecycle::Ready;
        spdlog::info("Server ready");

        // TODO: Start WorkerPool, load CDB, start FuzzyGraph scan
    });

    // === shutdown ===
    peer.on_request([this](RequestContext& ctx, const protocol::ShutdownParams& params)
                        -> RequestResult<protocol::ShutdownParams> {
        lifecycle = ServerLifecycle::ShuttingDown;
        spdlog::info("Shutdown requested");
        co_return nullptr;
    });

    // === exit ===
    peer.on_notification([this](const protocol::ExitParams& params) {
        lifecycle = ServerLifecycle::Exited;
        spdlog::info("Exit notification received");

        // TODO: Graceful shutdown (stop WorkerPool, save index, etc.)
        loop.stop();
    });

    // === textDocument/didOpen ===
    peer.on_notification([this](const protocol::DidOpenTextDocumentParams& params) {
        if(lifecycle != ServerLifecycle::Ready) return;

        auto& td = params.text_document;
        auto path = uri_to_path(td.uri);
        auto path_id = path_pool.intern(path);

        auto& doc = documents[path_id];
        doc.version = td.version;
        doc.text = td.text;
        doc.generation++;

        spdlog::debug("didOpen: {} (v{})", path, td.version);

        // TODO: Query CDB, register compile unit, schedule_build
    });

    // === textDocument/didChange ===
    peer.on_notification([this](const protocol::DidChangeTextDocumentParams& params) {
        if(lifecycle != ServerLifecycle::Ready) return;

        auto path = uri_to_path(params.text_document.uri);
        auto path_id = path_pool.intern(path);

        auto it = documents.find(path_id);
        if(it == documents.end()) return;

        auto& doc = it->second;
        doc.version = params.text_document.version;

        // Apply incremental changes
        for(auto& change : params.content_changes) {
            std::visit([&](auto& c) {
                using T = std::remove_cvref_t<decltype(c)>;
                if constexpr(std::is_same_v<T, protocol::TextDocumentContentChangeWholeDocument>) {
                    doc.text = c.text;
                } else {
                    // Incremental change: replace range
                    auto& range = c.range;

                    // Convert Position (line/character) to byte offset
                    auto pos_to_offset = [&](protocol::Position pos) -> std::size_t {
                        std::size_t offset = 0;
                        int line = 0;
                        for(std::size_t i = 0; i < doc.text.size(); i++) {
                            if(line == static_cast<int>(pos.line)) {
                                return i + pos.character;
                            }
                            if(doc.text[i] == '\n') line++;
                        }
                        return doc.text.size();
                    };

                    auto start = pos_to_offset(range.start);
                    auto end = pos_to_offset(range.end);
                    if(start <= doc.text.size() && end <= doc.text.size() && start <= end) {
                        doc.text.replace(start, end - start, c.text);
                    }
                }
            }, change);
        }

        doc.generation++;

        // TODO: compile_graph.update(), notify worker, schedule_build
    });

    // === textDocument/didClose ===
    peer.on_notification([this](const protocol::DidCloseTextDocumentParams& params) {
        if(lifecycle != ServerLifecycle::Ready) return;

        auto path = uri_to_path(params.text_document.uri);
        auto path_id = path_pool.intern(path);

        documents.erase(path_id);

        // Clear diagnostics for closed file
        clear_diagnostics(params.text_document.uri);

        spdlog::debug("didClose: {}", path);
    });

    // === textDocument/didSave ===
    peer.on_notification([this](const protocol::DidSaveTextDocumentParams& params) {
        if(lifecycle != ServerLifecycle::Ready) return;

        // TODO: Trigger dependent file rebuilds
        spdlog::debug("didSave: {}", params.text_document.uri);
    });
}

}  // namespace clice
