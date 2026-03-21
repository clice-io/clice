#include "server/master_server.h"
#include "server/protocol.h"
#include "support/filesystem.h"

#include "eventide/serde/serde/raw_value.h"

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
    : loop(loop), peer(peer), pool(loop), compile_graph(pool) {}

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

void MasterServer::schedule_build(std::uint32_t path_id, const std::string& uri) {
    auto it = documents.find(path_id);
    if(it == documents.end()) return;

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
    timer_ptr->start(std::chrono::milliseconds(200));

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
        if(doc_it == documents.end()) co_return;

        auto& doc = doc_it->second;
        doc.build_running = true;
        doc.build_requested = false;
        auto gen = doc.generation;

        // Ensure PCM/PCH dependencies are ready
        co_await compile_graph.compile_deps(path_id, loop);

        // Send compile request to stateful worker
        worker::CompileParams params;
        params.uri = uri;
        params.version = doc.version;
        params.text = doc.text;
        fill_compile_args(path_pool.resolve(path_id), params.directory, params.arguments);

        auto result = co_await pool.send_stateful<worker::CompileParams>(path_id, params);

        // Re-lookup document (may have been closed during compile)
        doc_it = documents.find(path_id);
        if(doc_it == documents.end()) co_return;

        auto& doc2 = doc_it->second;

        if(result.has_value()) {
            // Only publish diagnostics if the generation hasn't changed
            if(doc2.generation == gen) {
                publish_diagnostics(uri, doc2.version,
                                    std::move(result.value().diagnostics));
            }
        } else {
            spdlog::warn("Compile failed for {}: {}", uri, result.error().message);
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
    if(workspace_root.empty()) co_return;

    // Search for compile_commands.json in common locations
    std::string cdb_path;
    for(auto* subdir : {"build", "cmake-build-debug", "cmake-build-release", "out", "."}) {
        auto candidate = path::join(workspace_root, subdir, "compile_commands.json");
        if(llvm::sys::fs::exists(candidate)) {
            cdb_path = std::move(candidate);
            break;
        }
    }

    if(cdb_path.empty()) {
        spdlog::warn("No compile_commands.json found in workspace {}", workspace_root);
        co_return;
    }

    auto updates = cdb.load_compile_database(cdb_path);
    spdlog::info("Loaded CDB from {} with {} entries", cdb_path, updates.size());

    // Scan all source files from CDB to build include graph
    auto all_files = cdb.files();
    for(auto* file : all_files) {
        auto path_id = path_pool.intern(file);
        scan_file(path_id, file);
    }

    spdlog::info("Initial scan complete, {} files in include graph", include_forward.size());
}

void MasterServer::scan_file(std::uint32_t path_id, llvm::StringRef path) {
    auto ctx = cdb.lookup(path);
    if(ctx.arguments.empty()) return;

    auto results = scan_fuzzy(ctx.arguments, ctx.directory,
                              /*arguments_from_database=*/true,
                              /*content=*/{}, &scan_cache);

    // Clear old forward edges for this file
    auto old_it = include_forward.find(path_id);
    if(old_it != include_forward.end()) {
        for(auto dep_id : old_it->second) {
            auto back_it = include_backward.find(dep_id);
            if(back_it != include_backward.end()) {
                back_it->second.erase(path_id);
            }
        }
        old_it->second.clear();
    }

    // Build new forward/backward edges from scan results
    for(auto& [included_path, scan_result] : results) {
        for(auto& inc : scan_result.includes) {
            if(inc.not_found) continue;
            auto included_id = path_pool.intern(inc.path);
            include_forward[path_id].insert(included_id);
            include_backward[included_id].insert(path_id);
        }
    }
}

void MasterServer::fill_compile_args(llvm::StringRef path, std::string& directory,
                                      std::vector<std::string>& arguments) {
    auto ctx = cdb.lookup(path);
    directory = ctx.directory.str();
    arguments.clear();
    for(auto* arg : ctx.arguments) {
        arguments.emplace_back(arg);
    }
}

et::task<bool> MasterServer::ensure_compiled(std::uint32_t path_id, const std::string& uri) {
    auto doc_it = documents.find(path_id);
    if(doc_it == documents.end()) co_return false;

    // If the document has never been compiled, schedule a build and wait
    // For now, just return true - the worker may already have an AST
    // from a previous compile, or the feature request will return empty results.
    co_return true;
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

        // Load CDB and build include graph in background
        loop.schedule(load_workspace());
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

        // Scan includes and register compile unit
        scan_file(path_id, path);

        // Get dependency path_ids from the forward graph
        llvm::SmallVector<std::uint32_t> deps;
        auto fwd_it = include_forward.find(path_id);
        if(fwd_it != include_forward.end()) {
            for(auto dep_id : fwd_it->second) {
                deps.push_back(dep_id);
            }
        }
        compile_graph.register_unit(path_id, deps);

        schedule_build(path_id, td.uri);
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

        // Rescan the file to update include graph
        scan_file(path_id, path);

        compile_graph.update(path_id);
        schedule_build(path_id, params.text_document.uri);
    });

    // === textDocument/didClose ===
    peer.on_notification([this](const protocol::DidCloseTextDocumentParams& params) {
        if(lifecycle != ServerLifecycle::Ready) return;

        auto path = uri_to_path(params.text_document.uri);
        auto path_id = path_pool.intern(path);

        documents.erase(path_id);
        debounce_timers.erase(path_id);

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

    // =========================================================================
    // Feature requests routed to stateful workers (RawValue passthrough)
    // =========================================================================

    // We use the method-string overload of on_request so the handler can return
    // task<serde::RawValue, Error> instead of the LSP-typed result.  The JsonPeer
    // serialises RawValue as inline JSON, giving us zero-copy forwarding of the
    // worker's already-serialised response back to the LSP client.

    using serde_raw = eventide::serde::RawValue;
    using RawResult = et::task<serde_raw, et::ipc::Error>;

    // --- textDocument/hover ---
    peer.on_request("textDocument/hover",
        [this](RequestContext& ctx, const protocol::HoverParams& params) -> RawResult {
            if(lifecycle != ServerLifecycle::Ready)
                co_return et::outcome_error(et::ipc::Error{"Server not ready"});

            auto path = uri_to_path(
                params.text_document_position_params.text_document.uri);
            auto path_id = path_pool.intern(path);

            co_await ensure_compiled(path_id,
                params.text_document_position_params.text_document.uri);

            worker::HoverParams wp;
            wp.uri = params.text_document_position_params.text_document.uri;
            wp.line = params.text_document_position_params.position.line;
            wp.character = params.text_document_position_params.position.character;

            auto result = co_await pool.send_stateful<worker::HoverParams>(path_id, wp);
            if(!result.has_value())
                co_return serde_raw{};  // null
            co_return std::move(result.value());
        });

    // --- textDocument/semanticTokens/full ---
    peer.on_request("textDocument/semanticTokens/full",
        [this](RequestContext& ctx, const protocol::SemanticTokensParams& params) -> RawResult {
            if(lifecycle != ServerLifecycle::Ready)
                co_return et::outcome_error(et::ipc::Error{"Server not ready"});

            auto path = uri_to_path(params.text_document.uri);
            auto path_id = path_pool.intern(path);

            co_await ensure_compiled(path_id, params.text_document.uri);

            worker::SemanticTokensParams wp;
            wp.uri = params.text_document.uri;

            auto result = co_await pool.send_stateful<worker::SemanticTokensParams>(
                path_id, wp);
            if(!result.has_value())
                co_return serde_raw{};
            co_return std::move(result.value());
        });

    // --- textDocument/inlayHint ---
    peer.on_request("textDocument/inlayHint",
        [this](RequestContext& ctx, const protocol::InlayHintParams& params) -> RawResult {
            if(lifecycle != ServerLifecycle::Ready)
                co_return et::outcome_error(et::ipc::Error{"Server not ready"});

            auto path = uri_to_path(params.text_document.uri);
            auto path_id = path_pool.intern(path);

            co_await ensure_compiled(path_id, params.text_document.uri);

            worker::InlayHintsParams wp;
            wp.uri = params.text_document.uri;
            wp.range = params.range;

            auto result = co_await pool.send_stateful<worker::InlayHintsParams>(
                path_id, wp);
            if(!result.has_value())
                co_return serde_raw{};
            co_return std::move(result.value());
        });

    // --- textDocument/foldingRange ---
    peer.on_request("textDocument/foldingRange",
        [this](RequestContext& ctx, const protocol::FoldingRangeParams& params) -> RawResult {
            if(lifecycle != ServerLifecycle::Ready)
                co_return et::outcome_error(et::ipc::Error{"Server not ready"});

            auto path = uri_to_path(params.text_document.uri);
            auto path_id = path_pool.intern(path);

            co_await ensure_compiled(path_id, params.text_document.uri);

            worker::FoldingRangeParams wp;
            wp.uri = params.text_document.uri;

            auto result = co_await pool.send_stateful<worker::FoldingRangeParams>(
                path_id, wp);
            if(!result.has_value())
                co_return serde_raw{};
            co_return std::move(result.value());
        });

    // --- textDocument/documentSymbol ---
    peer.on_request("textDocument/documentSymbol",
        [this](RequestContext& ctx, const protocol::DocumentSymbolParams& params) -> RawResult {
            if(lifecycle != ServerLifecycle::Ready)
                co_return et::outcome_error(et::ipc::Error{"Server not ready"});

            auto path = uri_to_path(params.text_document.uri);
            auto path_id = path_pool.intern(path);

            co_await ensure_compiled(path_id, params.text_document.uri);

            worker::DocumentSymbolParams wp;
            wp.uri = params.text_document.uri;

            auto result = co_await pool.send_stateful<worker::DocumentSymbolParams>(
                path_id, wp);
            if(!result.has_value())
                co_return serde_raw{};
            co_return std::move(result.value());
        });

    // --- textDocument/documentLink ---
    peer.on_request("textDocument/documentLink",
        [this](RequestContext& ctx, const protocol::DocumentLinkParams& params) -> RawResult {
            if(lifecycle != ServerLifecycle::Ready)
                co_return et::outcome_error(et::ipc::Error{"Server not ready"});

            auto path = uri_to_path(params.text_document.uri);
            auto path_id = path_pool.intern(path);

            co_await ensure_compiled(path_id, params.text_document.uri);

            worker::DocumentLinkParams wp;
            wp.uri = params.text_document.uri;

            auto result = co_await pool.send_stateful<worker::DocumentLinkParams>(
                path_id, wp);
            if(!result.has_value())
                co_return serde_raw{};
            co_return std::move(result.value());
        });

    // --- textDocument/codeAction ---
    peer.on_request("textDocument/codeAction",
        [this](RequestContext& ctx, const protocol::CodeActionParams& params) -> RawResult {
            if(lifecycle != ServerLifecycle::Ready)
                co_return et::outcome_error(et::ipc::Error{"Server not ready"});

            auto path = uri_to_path(params.text_document.uri);
            auto path_id = path_pool.intern(path);

            co_await ensure_compiled(path_id, params.text_document.uri);

            worker::CodeActionParams wp;
            wp.uri = params.text_document.uri;
            wp.range = params.range;
            wp.context = params.context;

            auto result = co_await pool.send_stateful<worker::CodeActionParams>(
                path_id, wp);
            if(!result.has_value())
                co_return serde_raw{};
            co_return std::move(result.value());
        });

    // --- textDocument/definition ---
    peer.on_request("textDocument/definition",
        [this](RequestContext& ctx, const protocol::DefinitionParams& params) -> RawResult {
            if(lifecycle != ServerLifecycle::Ready)
                co_return et::outcome_error(et::ipc::Error{"Server not ready"});

            auto path = uri_to_path(
                params.text_document_position_params.text_document.uri);
            auto path_id = path_pool.intern(path);

            co_await ensure_compiled(path_id,
                params.text_document_position_params.text_document.uri);

            worker::GoToDefinitionParams wp;
            wp.uri = params.text_document_position_params.text_document.uri;
            wp.line = params.text_document_position_params.position.line;
            wp.character = params.text_document_position_params.position.character;

            auto result = co_await pool.send_stateful<worker::GoToDefinitionParams>(
                path_id, wp);
            if(!result.has_value())
                co_return serde_raw{};
            co_return std::move(result.value());
        });

    // =========================================================================
    // Feature requests routed to stateless workers
    // =========================================================================

    // --- textDocument/completion ---
    peer.on_request("textDocument/completion",
        [this](RequestContext& ctx, const protocol::CompletionParams& params) -> RawResult {
            if(lifecycle != ServerLifecycle::Ready)
                co_return et::outcome_error(et::ipc::Error{"Server not ready"});

            auto& uri = params.text_document_position_params.text_document.uri;
            auto path = uri_to_path(uri);
            auto path_id = path_pool.intern(path);

            auto doc_it = documents.find(path_id);
            if(doc_it == documents.end())
                co_return serde_raw{};  // null

            auto& doc = doc_it->second;

            worker::CompletionParams wp;
            wp.uri = uri;
            wp.version = doc.version;
            wp.text = doc.text;
            fill_compile_args(path, wp.directory, wp.arguments);
            wp.line = params.text_document_position_params.position.line;
            wp.character = params.text_document_position_params.position.character;

            auto result = co_await pool.send_stateless<worker::CompletionParams>(wp);
            if(!result.has_value())
                co_return serde_raw{};
            co_return std::move(result.value());
        });

    // --- textDocument/signatureHelp ---
    peer.on_request("textDocument/signatureHelp",
        [this](RequestContext& ctx, const protocol::SignatureHelpParams& params) -> RawResult {
            if(lifecycle != ServerLifecycle::Ready)
                co_return et::outcome_error(et::ipc::Error{"Server not ready"});

            auto& uri = params.text_document_position_params.text_document.uri;
            auto path = uri_to_path(uri);
            auto path_id = path_pool.intern(path);

            auto doc_it = documents.find(path_id);
            if(doc_it == documents.end())
                co_return serde_raw{};

            auto& doc = doc_it->second;

            worker::SignatureHelpParams wp;
            wp.uri = uri;
            wp.version = doc.version;
            wp.text = doc.text;
            fill_compile_args(path, wp.directory, wp.arguments);
            wp.line = params.text_document_position_params.position.line;
            wp.character = params.text_document_position_params.position.character;

            auto result = co_await pool.send_stateless<worker::SignatureHelpParams>(wp);
            if(!result.has_value())
                co_return serde_raw{};
            co_return std::move(result.value());
        });
}

}  // namespace clice
