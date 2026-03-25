#include "server/master_server.h"

#include <string>
#include <type_traits>
#include <variant>
#include <vector>

#include "eventide/ipc/lsp/position.h"
#include "eventide/ipc/lsp/uri.h"
#include "eventide/reflection/enum.h"
#include "eventide/serde/json/json.h"
#include "eventide/serde/serde/raw_value.h"
#include "semantic/symbol_kind.h"
#include "server/protocol.h"
#include "support/filesystem.h"
#include "support/logging.h"
#include "syntax/dependency_graph.h"

namespace clice {

namespace protocol = eventide::ipc::protocol;
namespace lsp = eventide::ipc::lsp;
namespace refl = eventide::refl;
using et::ipc::RequestResult;
using RequestContext = et::ipc::JsonPeer::RequestContext;

MasterServer::MasterServer(et::event_loop& loop, et::ipc::JsonPeer& peer, std::string self_path) :
    loop(loop), peer(peer), pool(loop), self_path(std::move(self_path)) {}

std::string MasterServer::uri_to_path(const std::string& uri) {
    auto parsed = lsp::URI::parse(uri);
    if(parsed.has_value()) {
        auto path = parsed->file_path();
        if(path.has_value()) {
            return std::move(*path);
        }
    }
    return uri;
}

void MasterServer::publish_diagnostics(const std::string& uri,
                                       int version,
                                       const et::serde::RawValue& diagnostics_json) {
    std::vector<protocol::Diagnostic> diagnostics;
    if(!diagnostics_json.empty()) {
        auto status = et::serde::json::from_json(diagnostics_json.data, diagnostics);
        if(!status) {
            LOG_WARN("Failed to deserialize diagnostics JSON for {}", uri);
        }
    }
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

void MasterServer::schedule_build(std::uint32_t path_id, const std::string& uri) {
    auto it = documents.find(path_id);
    if(it == documents.end())
        return;

    auto& doc = it->second;

    if(doc.build_running) {
        doc.build_requested = true;
        return;
    }

    // Create or reset debounce timer
    auto& timer_ptr = debounce_timers[path_id];
    if(!timer_ptr) {
        timer_ptr = std::make_unique<et::timer>(et::timer::create(loop));
    }
    timer_ptr->start(std::chrono::milliseconds(config.debounce_ms));

    if(!doc.drain_scheduled) {
        doc.drain_scheduled = true;
        loop.schedule(run_build_drain(path_id, uri));
    }
}

et::task<> MasterServer::run_build_drain(std::uint32_t path_id, std::string uri) {
    // Wait for debounce timer
    auto timer_it = debounce_timers.find(path_id);
    if(timer_it != debounce_timers.end() && timer_it->second) {
        co_await timer_it->second->wait();
    }

    while(true) {
        auto doc_it = documents.find(path_id);
        if(doc_it == documents.end())
            co_return;

        doc_it->second.build_running = true;
        doc_it->second.build_requested = false;
        auto gen = doc_it->second.generation;

        // Send compile request to stateful worker
        worker::CompileParams params;
        params.path = std::string(path_pool.resolve(path_id));
        params.version = doc_it->second.version;
        params.text = doc_it->second.text;
        fill_compile_args(path_pool.resolve(path_id), params.directory, params.arguments);

        LOG_DEBUG("Sending compile: path={}, args={}, gen={}",
                  params.path,
                  params.arguments.size(),
                  gen);

        auto result = co_await pool.send_stateful(path_id, params);

        // Re-lookup document (may have been closed during compile)
        doc_it = documents.find(path_id);
        if(doc_it == documents.end())
            co_return;

        auto& doc2 = doc_it->second;

        if(result.has_value()) {
            // Only publish diagnostics if the generation hasn't changed
            if(doc2.generation == gen) {
                publish_diagnostics(uri, doc2.version, result.value().diagnostics);
            } else {
                LOG_DEBUG("Generation mismatch ({} vs {}), dropping diagnostics for {}",
                          doc2.generation,
                          gen,
                          uri);
            }
        } else {
            LOG_WARN("Compile failed for {}: {}", uri, result.error().message);
            // Publish empty diagnostics so stale errors don't linger
            clear_diagnostics(uri);
        }

        // Check if more builds were requested while compiling
        if(!doc2.build_requested) {
            doc2.build_running = false;
            doc2.drain_scheduled = false;
            co_return;
        }
        // Loop continues for the next build
    }
}

et::task<> MasterServer::load_workspace() {
    if(workspace_root.empty())
        co_return;

    // Create cache directory if configured
    if(!config.cache_dir.empty()) {
        auto ec = llvm::sys::fs::create_directories(config.cache_dir);
        if(ec) {
            LOG_WARN("Failed to create cache directory {}: {}", config.cache_dir, ec.message());
        } else {
            LOG_INFO("Cache directory: {}", config.cache_dir);
        }
    }

    // Search for compile_commands.json
    std::string cdb_path;

    // If the config specifies a CDB path, use it
    if(!config.compile_commands_path.empty()) {
        if(llvm::sys::fs::exists(config.compile_commands_path)) {
            cdb_path = config.compile_commands_path;
        } else {
            LOG_WARN("Configured compile_commands_path not found: {}",
                     config.compile_commands_path);
        }
    }

    // Otherwise auto-detect in common locations
    if(cdb_path.empty()) {
        for(auto* subdir: {"build", "cmake-build-debug", "cmake-build-release", "out", "."}) {
            auto candidate = path::join(workspace_root, subdir, "compile_commands.json");
            if(llvm::sys::fs::exists(candidate)) {
                cdb_path = std::move(candidate);
                break;
            }
        }
    }

    if(cdb_path.empty()) {
        LOG_WARN("No compile_commands.json found in workspace {}", workspace_root);
        co_return;
    }

    auto count = cdb.load(cdb_path);
    LOG_INFO("Loaded CDB from {} with {} entries", cdb_path, count);

    // Build dependency graph via wavefront BFS scan.
    scan_dependency_graph(cdb, path_pool, dependency_graph);
}

void MasterServer::fill_compile_args(llvm::StringRef path,
                                     std::string& directory,
                                     std::vector<std::string>& arguments) {
    auto results = cdb.lookup(path, {.query_toolchain = true});
    auto& ctx = results.front();
    directory = ctx.directory.str();
    arguments.clear();
    for(auto* arg: ctx.arguments) {
        arguments.emplace_back(arg);
    }
}

et::task<bool> MasterServer::ensure_compiled(std::uint32_t path_id, const std::string& uri) {
    auto doc_it = documents.find(path_id);
    if(doc_it == documents.end())
        co_return false;

    // If the document has never been compiled, schedule a build and wait
    // For now, just return true - the worker may already have an AST
    // from a previous compile, or the feature request will return empty results.
    co_return true;
}

// =========================================================================
// Forwarding helpers
// =========================================================================

using serde_raw = et::serde::RawValue;

template <typename WorkerParams>
MasterServer::RawResult MasterServer::forward_stateful(const std::string& uri) {
    auto path = uri_to_path(uri);
    auto path_id = path_pool.intern(path);

    if(!co_await ensure_compiled(path_id, uri))
        co_return serde_raw{"null"};

    WorkerParams wp;
    wp.path = path;

    auto result = co_await pool.send_stateful(path_id, wp);
    if(!result.has_value())
        co_return serde_raw{};
    co_return std::move(result.value());
}

template <typename WorkerParams>
MasterServer::RawResult MasterServer::forward_stateful(const std::string& uri,
                                                       const protocol::Position& position) {
    auto path = uri_to_path(uri);
    auto path_id = path_pool.intern(path);

    if(!co_await ensure_compiled(path_id, uri))
        co_return serde_raw{"null"};

    WorkerParams wp;
    wp.path = path;

    auto doc_it = documents.find(path_id);
    if(doc_it != documents.end()) {
        lsp::PositionMapper mapper(doc_it->second.text, lsp::PositionEncoding::UTF16);
        wp.offset = mapper.to_offset(position);
    }

    auto result = co_await pool.send_stateful(path_id, wp);
    if(!result.has_value())
        co_return serde_raw{};
    co_return std::move(result.value());
}

template <typename WorkerParams>
MasterServer::RawResult MasterServer::forward_stateless(const std::string& uri,
                                                        const protocol::Position& position) {
    auto path = uri_to_path(uri);
    auto path_id = path_pool.intern(path);

    auto doc_it = documents.find(path_id);
    if(doc_it == documents.end())
        co_return serde_raw{};

    auto& doc = doc_it->second;

    lsp::PositionMapper mapper(doc.text, lsp::PositionEncoding::UTF16);

    WorkerParams wp;
    wp.path = path;
    wp.version = doc.version;
    wp.text = doc.text;
    fill_compile_args(path, wp.directory, wp.arguments);
    wp.offset = mapper.to_offset(position);

    auto result = co_await pool.send_stateless(wp);
    if(!result.has_value())
        co_return serde_raw{};
    co_return std::move(result.value());
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

        LOG_INFO("Initialized with workspace: {}", workspace_root);

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
        {
            auto lower_first = [](std::string_view name) -> std::string {
                std::string s(name);
                if(!s.empty()) {
                    s[0] = static_cast<char>(std::tolower(static_cast<unsigned char>(s[0])));
                }
                return s;
            };

            auto to_names = [&](auto names) {
                return std::ranges::to<std::vector>(names | std::views::transform(lower_first));
            };

            sem_opts.legend = protocol::SemanticTokensLegend{
                to_names(refl::reflection<SymbolKind::Kind>::member_names),
                to_names(refl::reflection<SymbolModifiers::Kind>::member_names),
            };
        }
        sem_opts.full = true;
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
        // Load configuration from workspace
        config = CliceConfig::load_from_workspace(workspace_root);

        LOG_INFO("Server ready (stateful={}, stateless={}, debounce={}ms, idle={}ms)",
                 config.stateful_worker_count,
                 config.stateless_worker_count,
                 config.debounce_ms,
                 config.idle_timeout_ms);

        // Start worker pool
        WorkerPoolOptions pool_opts;
        pool_opts.self_path = self_path;
        pool_opts.stateful_count = config.stateful_worker_count;
        pool_opts.stateless_count = config.stateless_worker_count;
        pool_opts.worker_memory_limit = config.worker_memory_limit;
        if(!pool.start(pool_opts)) {
            LOG_ERROR("Failed to start worker pool");
            return;
        }

        lifecycle = ServerLifecycle::Ready;

        // Load CDB in background
        loop.schedule(load_workspace());
    });

    // === shutdown ===
    peer.on_request(
        [this](RequestContext& ctx,
               const protocol::ShutdownParams& params) -> RequestResult<protocol::ShutdownParams> {
            lifecycle = ServerLifecycle::ShuttingDown;
            LOG_INFO("Shutdown requested");
            co_return nullptr;
        });

    // === exit ===
    peer.on_notification([this](const protocol::ExitParams& params) {
        lifecycle = ServerLifecycle::Exited;
        LOG_INFO("Exit notification received");

        // Graceful shutdown: cancel compilations, stop workers, then stop loop
        loop.schedule([this]() -> et::task<> {
            co_await pool.stop();
            loop.stop();
        }());
    });

    // === textDocument/didOpen ===
    peer.on_notification([this](const protocol::DidOpenTextDocumentParams& params) {
        if(lifecycle != ServerLifecycle::Ready)
            return;

        auto& td = params.text_document;
        auto path = uri_to_path(td.uri);
        auto path_id = path_pool.intern(path);

        auto& doc = documents[path_id];
        doc.version = td.version;
        doc.text = td.text;
        doc.generation++;

        LOG_DEBUG("didOpen: {} (v{})", path, td.version);

        schedule_build(path_id, td.uri);
    });

    // === textDocument/didChange ===
    peer.on_notification([this](const protocol::DidChangeTextDocumentParams& params) {
        if(lifecycle != ServerLifecycle::Ready)
            return;

        auto path = uri_to_path(params.text_document.uri);
        auto path_id = path_pool.intern(path);

        auto it = documents.find(path_id);
        if(it == documents.end())
            return;

        auto& doc = it->second;
        doc.version = params.text_document.version;

        // Apply incremental changes
        for(auto& change: params.content_changes) {
            std::visit(
                [&](auto& c) {
                    using T = std::remove_cvref_t<decltype(c)>;
                    if constexpr(std::is_same_v<T,
                                                protocol::TextDocumentContentChangeWholeDocument>) {
                        doc.text = c.text;
                    } else {
                        // Incremental change: replace range
                        auto& range = c.range;

                        lsp::PositionMapper mapper(doc.text, lsp::PositionEncoding::UTF16);
                        auto start = mapper.to_offset(range.start);
                        auto end = mapper.to_offset(range.end);
                        if(start <= doc.text.size() && end <= doc.text.size() && start <= end) {
                            doc.text.replace(start, end - start, c.text);
                        }
                    }
                },
                change);
        }

        doc.generation++;

        // Notify the owning stateful worker so it marks the document dirty
        worker::DocumentUpdateParams update;
        update.path = path;
        update.version = doc.version;
        update.text = doc.text;
        pool.notify_stateful(path_id, update);

        schedule_build(path_id, params.text_document.uri);
    });

    // === textDocument/didClose ===
    peer.on_notification([this](const protocol::DidCloseTextDocumentParams& params) {
        if(lifecycle != ServerLifecycle::Ready)
            return;

        auto path = uri_to_path(params.text_document.uri);
        auto path_id = path_pool.intern(path);

        documents.erase(path_id);
        debounce_timers.erase(path_id);

        // Clear diagnostics for closed file
        clear_diagnostics(params.text_document.uri);

        LOG_DEBUG("didClose: {}", path);
    });

    // === textDocument/didSave ===
    peer.on_notification([this](const protocol::DidSaveTextDocumentParams& params) {
        if(lifecycle != ServerLifecycle::Ready)
            return;

        // TODO: Trigger dependent file rebuilds
        LOG_DEBUG("didSave: {}", params.text_document.uri);
    });

    // =========================================================================
    // Feature requests routed to stateful workers (RawValue passthrough)
    // =========================================================================

    // --- textDocument/hover ---
    peer.on_request([this](RequestContext& ctx, const protocol::HoverParams& params) -> RawResult {
        co_return co_await forward_stateful<worker::HoverParams>(
            params.text_document_position_params.text_document.uri,
            params.text_document_position_params.position);
    });

    // --- textDocument/semanticTokens/full ---
    peer.on_request([this](RequestContext& ctx,
                           const protocol::SemanticTokensParams& params) -> RawResult {
        co_return co_await forward_stateful<worker::SemanticTokensParams>(params.text_document.uri);
    });

    // --- textDocument/inlayHint ---
    peer.on_request(
        [this](RequestContext& ctx, const protocol::InlayHintParams& params) -> RawResult {
            co_return co_await forward_stateful<worker::InlayHintsParams>(params.text_document.uri);
        });

    // --- textDocument/foldingRange ---
    peer.on_request([this](RequestContext& ctx,
                           const protocol::FoldingRangeParams& params) -> RawResult {
        co_return co_await forward_stateful<worker::FoldingRangeParams>(params.text_document.uri);
    });

    // --- textDocument/documentSymbol ---
    peer.on_request([this](RequestContext& ctx,
                           const protocol::DocumentSymbolParams& params) -> RawResult {
        co_return co_await forward_stateful<worker::DocumentSymbolParams>(params.text_document.uri);
    });

    // --- textDocument/documentLink ---
    peer.on_request([this](RequestContext& ctx,
                           const protocol::DocumentLinkParams& params) -> RawResult {
        co_return co_await forward_stateful<worker::DocumentLinkParams>(params.text_document.uri);
    });

    // --- textDocument/codeAction ---
    peer.on_request(
        [this](RequestContext& ctx, const protocol::CodeActionParams& params) -> RawResult {
            co_return co_await forward_stateful<worker::CodeActionParams>(params.text_document.uri);
        });

    // --- textDocument/definition ---
    peer.on_request(
        [this](RequestContext& ctx, const protocol::DefinitionParams& params) -> RawResult {
            co_return co_await forward_stateful<worker::GoToDefinitionParams>(
                params.text_document_position_params.text_document.uri,
                params.text_document_position_params.position);
        });

    // =========================================================================
    // Feature requests routed to stateless workers
    // =========================================================================

    // --- textDocument/completion ---
    peer.on_request(
        [this](RequestContext& ctx, const protocol::CompletionParams& params) -> RawResult {
            co_return co_await forward_stateless<worker::CompletionParams>(
                params.text_document_position_params.text_document.uri,
                params.text_document_position_params.position);
        });

    // --- textDocument/signatureHelp ---
    peer.on_request(
        [this](RequestContext& ctx, const protocol::SignatureHelpParams& params) -> RawResult {
            co_return co_await forward_stateless<worker::SignatureHelpParams>(
                params.text_document_position_params.text_document.uri,
                params.text_document_position_params.position);
        });
}

}  // namespace clice
