#include "server/master_server.h"

#include <cstddef>
#include <string>
#include <type_traits>
#include <variant>
#include <vector>

#include "eventide/ipc/lsp/position.h"
#include "eventide/ipc/lsp/uri.h"
#include "eventide/serde/json/json.h"
#include "eventide/serde/serde/raw_value.h"
#include "server/protocol.h"
#include "support/filesystem.h"
#include "support/logging.h"

namespace clice {

namespace protocol = eventide::ipc::protocol;
using et::ipc::RequestResult;
using RequestContext = et::ipc::JsonPeer::RequestContext;

MasterServer::MasterServer(et::event_loop& loop, et::ipc::JsonPeer& peer, std::string self_path) :
    loop(loop), peer(peer), pool(loop), compile_graph(pool), self_path(std::move(self_path)) {}

std::string MasterServer::uri_to_path(const std::string& uri) {
    namespace lsp = eventide::ipc::lsp;
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

        // Ensure PCM/PCH dependencies are ready
        auto deps_ok = co_await compile_graph.compile_deps(path_id, loop);

        // Re-lookup after co_await (map may have changed)
        doc_it = documents.find(path_id);
        if(doc_it == documents.end())
            co_return;

        if(!deps_ok) {
            LOG_WARN("Dependency compilation failed for {}, skipping build", uri);
            clear_diagnostics(uri);
            if(!doc_it->second.build_requested) {
                doc_it->second.build_running = false;
                doc_it->second.drain_scheduled = false;
                co_return;
            }
            continue;
        }

        // Send compile request to stateful worker
        worker::CompileParams params;
        params.path = std::string(path_pool.resolve(path_id));
        params.version = doc_it->second.version;
        params.text = doc_it->second.text;
        fill_compile_args(path_pool.resolve(path_id), params.directory, params.arguments);

        auto result = co_await pool.send_stateful<worker::CompileParams>(path_id, params);

        // Re-lookup document (may have been closed during compile)
        doc_it = documents.find(path_id);
        if(doc_it == documents.end())
            co_return;

        auto& doc2 = doc_it->second;

        if(result.has_value()) {
            // Only publish diagnostics if the generation hasn't changed
            if(doc2.generation == gen) {
                publish_diagnostics(uri, doc2.version, result.value().diagnostics);
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

    auto updates = cdb.load_compile_database(cdb_path);
    LOG_INFO("Loaded CDB from {} with {} entries", cdb_path, updates.size());

    // Scan all source files from CDB to build include graph
    auto all_files = cdb.files();
    for(auto* file: all_files) {
        auto path_id = path_pool.intern(file);
        scan_file(path_id, file);
    }

    LOG_INFO("Initial scan complete, {} files in include graph", include_forward.size());

    // Populate the index queue with all CDB files for background indexing
    populate_index_queue();
    LOG_INFO("Index queue populated with {} files", index_queue.size());
}

void MasterServer::scan_file(std::uint32_t path_id, llvm::StringRef path) {
    auto ctx = cdb.lookup(path);
    if(ctx.arguments.empty())
        return;

    auto results = scan_fuzzy(ctx.arguments,
                              ctx.directory,
                              /*arguments_from_database=*/true,
                              /*content=*/{},
                              &scan_cache);

    // Clear old forward edges for this file
    auto old_it = include_forward.find(path_id);
    if(old_it != include_forward.end()) {
        for(auto dep_id: old_it->second) {
            auto back_it = include_backward.find(dep_id);
            if(back_it != include_backward.end()) {
                back_it->second.erase(path_id);
            }
        }
        old_it->second.clear();
    }

    // Build new forward/backward edges from scan results
    for(auto& [included_path, scan_result]: results) {
        for(auto& inc: scan_result.includes) {
            if(inc.not_found)
                continue;
            auto included_id = path_pool.intern(inc.path);
            include_forward[path_id].insert(included_id);
            include_backward[included_id].insert(path_id);
        }
    }
}

void MasterServer::fill_compile_args(llvm::StringRef path,
                                     std::string& directory,
                                     std::vector<std::string>& arguments) {
    auto ctx = cdb.lookup(path);
    directory = ctx.directory.str();
    arguments.clear();
    for(auto* arg: ctx.arguments) {
        arguments.emplace_back(arg);
    }
}

void MasterServer::reset_idle_timer() {
    if(!idle_timer) {
        idle_timer = std::make_unique<et::timer>(et::timer::create(loop));
    }
    // Restart the idle timer
    idle_timer->start(std::chrono::milliseconds(config.idle_timeout_ms));
}

void MasterServer::populate_index_queue() {
    auto all_files = cdb.files();
    index_queue.clear();
    for(auto* file: all_files) {
        auto path_id = path_pool.intern(file);
        index_queue.push_back(path_id);
    }
    index_total = static_cast<std::uint32_t>(index_queue.size());
    index_progress = 0;

    if(!index_queue.empty()) {
        index_event.set();
    }
}

et::task<> MasterServer::run_background_indexer() {
    while(lifecycle != ServerLifecycle::ShuttingDown && lifecycle != ServerLifecycle::Exited) {
        // Wait until there is work in the index queue
        if(index_queue.empty()) {
            index_event.reset();
            co_await index_event.wait();
            continue;
        }

        // Wait for idle (3 seconds of no user activity)
        if(!idle_timer) {
            idle_timer = std::make_unique<et::timer>(et::timer::create(loop));
            idle_timer->start(std::chrono::milliseconds(config.idle_timeout_ms));
        }
        co_await idle_timer->wait();

        if(index_queue.empty())
            continue;

        indexing_active = true;
        LOG_INFO("Background indexing started, {} files queued", index_queue.size());

        // Process files from the queue while idle
        while(!index_queue.empty() && lifecycle != ServerLifecycle::ShuttingDown) {
            auto path_id = index_queue.front();
            index_queue.pop_front();

            auto path = path_pool.resolve(path_id);
            LOG_DEBUG("Indexing: {}", path.str());

            // Ensure dependencies are compiled first
            auto deps_ok = co_await compile_graph.compile_deps(path_id, loop);
            if(!deps_ok) {
                LOG_WARN("Index skipped for {} (dependency compilation failed)", path.str());
                continue;
            }

            // Build IndexParams
            worker::IndexParams params;
            params.file = path.str();
            fill_compile_args(path, params.directory, params.arguments);
            // TODO: Fill params.pcms from CompileGraph's compiled PCM paths

            // Send to a stateless worker
            auto result = co_await pool.send_stateless<worker::IndexParams>(params);

            if(result.has_value()) {
                auto& idx_result = result.value();
                if(idx_result.success) {
                    index_progress++;
                    LOG_DEBUG("Indexed {}/{}: {}", index_progress, index_total, path.str());

                    // TODO (Phase 9.2): Deserialize tu_index_data (FlatBuffers)
                    //   into TUIndex, perform path mapping, and merge into
                    //   ProjectIndex/MergedIndex.
                    //
                    // Sketch:
                    //   auto tu_index = TUIndex::deserialize(idx_result.tu_index_data);
                    //   project_index.merge(tu_index);
                    //   merged_index.merge(...);
                } else {
                    LOG_WARN("Index failed for {}: {}", path.str(), idx_result.error);
                }
            } else {
                LOG_WARN("Index request failed for {}: {}", path.str(), result.error().message);
            }
        }

        indexing_active = false;
        if(index_queue.empty()) {
            LOG_INFO("Background indexing complete: {}/{} files indexed",
                     index_progress,
                     index_total);
        }
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
        sem_opts.legend = protocol::SemanticTokensLegend{{}, {}};
        sem_opts.full =
            protocol::variant<protocol::boolean, protocol::SemanticTokensFullDelta>{true};
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

        // Load CDB and build include graph in background
        loop.schedule(load_workspace());

        // Start background indexer coroutine if enabled
        if(config.enable_indexing) {
            loop.schedule(run_background_indexer());
        }
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
            compile_graph.cancel_all();
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

        // Reset idle timer on user activity
        reset_idle_timer();

        // Scan includes and register compile unit
        scan_file(path_id, path);

        // Get dependency path_ids from the forward graph
        llvm::SmallVector<std::uint32_t> deps;
        auto fwd_it = include_forward.find(path_id);
        if(fwd_it != include_forward.end()) {
            for(auto dep_id: fwd_it->second) {
                deps.push_back(dep_id);
            }
        }
        compile_graph.register_unit(path_id, deps);

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

                        using eventide::ipc::lsp::PositionMapper;
                        using eventide::ipc::lsp::PositionEncoding;
                        PositionMapper mapper(doc.text, PositionEncoding::UTF16);
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

        // Reset idle timer on user activity
        reset_idle_timer();

        // Rescan the file to update include graph
        scan_file(path_id, path);

        compile_graph.update(path_id);
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

    // We use the method-string overload of on_request so the handler can return
    // task<serde::RawValue, Error> instead of the LSP-typed result.  The JsonPeer
    // serialises RawValue as inline JSON, giving us zero-copy forwarding of the
    // worker's already-serialised response back to the LSP client.

    using serde_raw = eventide::serde::RawValue;
    using RawResult = et::task<serde_raw, et::ipc::Error>;

    // --- textDocument/hover ---
    peer.on_request(
        "textDocument/hover",
        [this](RequestContext& ctx, const protocol::HoverParams& params) -> RawResult {
            if(lifecycle != ServerLifecycle::Ready)
                co_return et::outcome_error(et::ipc::Error{"Server not ready"});

            auto path = uri_to_path(params.text_document_position_params.text_document.uri);
            auto path_id = path_pool.intern(path);

            co_await ensure_compiled(path_id,
                                     params.text_document_position_params.text_document.uri);

            worker::HoverParams wp;
            wp.path = path;

            auto doc_it = documents.find(path_id);
            if(doc_it != documents.end()) {
                using eventide::ipc::lsp::PositionMapper;
                using eventide::ipc::lsp::PositionEncoding;
                PositionMapper mapper(doc_it->second.text, PositionEncoding::UTF16);
                wp.offset = mapper.to_offset(params.text_document_position_params.position);
            }

            auto result = co_await pool.send_stateful<worker::HoverParams>(path_id, wp);
            if(!result.has_value())
                co_return serde_raw{};  // null
            co_return std::move(result.value());
        });

    // --- textDocument/semanticTokens/full ---
    peer.on_request(
        "textDocument/semanticTokens/full",
        [this](RequestContext& ctx, const protocol::SemanticTokensParams& params) -> RawResult {
            if(lifecycle != ServerLifecycle::Ready)
                co_return et::outcome_error(et::ipc::Error{"Server not ready"});

            auto path = uri_to_path(params.text_document.uri);
            auto path_id = path_pool.intern(path);

            co_await ensure_compiled(path_id, params.text_document.uri);

            worker::SemanticTokensParams wp;
            wp.path = path;

            auto result = co_await pool.send_stateful<worker::SemanticTokensParams>(path_id, wp);
            if(!result.has_value())
                co_return serde_raw{};
            co_return std::move(result.value());
        });

    // --- textDocument/inlayHint ---
    peer.on_request(
        "textDocument/inlayHint",
        [this](RequestContext& ctx, const protocol::InlayHintParams& params) -> RawResult {
            if(lifecycle != ServerLifecycle::Ready)
                co_return et::outcome_error(et::ipc::Error{"Server not ready"});

            auto path = uri_to_path(params.text_document.uri);
            auto path_id = path_pool.intern(path);

            co_await ensure_compiled(path_id, params.text_document.uri);

            worker::InlayHintsParams wp;
            wp.path = path;

            auto result = co_await pool.send_stateful<worker::InlayHintsParams>(path_id, wp);
            if(!result.has_value())
                co_return serde_raw{};
            co_return std::move(result.value());
        });

    // --- textDocument/foldingRange ---
    peer.on_request(
        "textDocument/foldingRange",
        [this](RequestContext& ctx, const protocol::FoldingRangeParams& params) -> RawResult {
            if(lifecycle != ServerLifecycle::Ready)
                co_return et::outcome_error(et::ipc::Error{"Server not ready"});

            auto path = uri_to_path(params.text_document.uri);
            auto path_id = path_pool.intern(path);

            co_await ensure_compiled(path_id, params.text_document.uri);

            worker::FoldingRangeParams wp;
            wp.path = path;

            auto result = co_await pool.send_stateful<worker::FoldingRangeParams>(path_id, wp);
            if(!result.has_value())
                co_return serde_raw{};
            co_return std::move(result.value());
        });

    // --- textDocument/documentSymbol ---
    peer.on_request(
        "textDocument/documentSymbol",
        [this](RequestContext& ctx, const protocol::DocumentSymbolParams& params) -> RawResult {
            if(lifecycle != ServerLifecycle::Ready)
                co_return et::outcome_error(et::ipc::Error{"Server not ready"});

            auto path = uri_to_path(params.text_document.uri);
            auto path_id = path_pool.intern(path);

            co_await ensure_compiled(path_id, params.text_document.uri);

            worker::DocumentSymbolParams wp;
            wp.path = path;

            auto result = co_await pool.send_stateful<worker::DocumentSymbolParams>(path_id, wp);
            if(!result.has_value())
                co_return serde_raw{};
            co_return std::move(result.value());
        });

    // --- textDocument/documentLink ---
    peer.on_request(
        "textDocument/documentLink",
        [this](RequestContext& ctx, const protocol::DocumentLinkParams& params) -> RawResult {
            if(lifecycle != ServerLifecycle::Ready)
                co_return et::outcome_error(et::ipc::Error{"Server not ready"});

            auto path = uri_to_path(params.text_document.uri);
            auto path_id = path_pool.intern(path);

            co_await ensure_compiled(path_id, params.text_document.uri);

            worker::DocumentLinkParams wp;
            wp.path = path;

            auto result = co_await pool.send_stateful<worker::DocumentLinkParams>(path_id, wp);
            if(!result.has_value())
                co_return serde_raw{};
            co_return std::move(result.value());
        });

    // --- textDocument/codeAction ---
    peer.on_request(
        "textDocument/codeAction",
        [this](RequestContext& ctx, const protocol::CodeActionParams& params) -> RawResult {
            if(lifecycle != ServerLifecycle::Ready)
                co_return et::outcome_error(et::ipc::Error{"Server not ready"});

            auto path = uri_to_path(params.text_document.uri);
            auto path_id = path_pool.intern(path);

            co_await ensure_compiled(path_id, params.text_document.uri);

            worker::CodeActionParams wp;
            wp.path = path;

            auto result = co_await pool.send_stateful<worker::CodeActionParams>(path_id, wp);
            if(!result.has_value())
                co_return serde_raw{};
            co_return std::move(result.value());
        });

    // --- textDocument/definition ---
    peer.on_request(
        "textDocument/definition",
        [this](RequestContext& ctx, const protocol::DefinitionParams& params) -> RawResult {
            if(lifecycle != ServerLifecycle::Ready)
                co_return et::outcome_error(et::ipc::Error{"Server not ready"});

            auto path = uri_to_path(params.text_document_position_params.text_document.uri);
            auto path_id = path_pool.intern(path);

            co_await ensure_compiled(path_id,
                                     params.text_document_position_params.text_document.uri);

            worker::GoToDefinitionParams wp;
            wp.path = path;

            auto doc_it = documents.find(path_id);
            if(doc_it != documents.end()) {
                using eventide::ipc::lsp::PositionMapper;
                using eventide::ipc::lsp::PositionEncoding;
                PositionMapper mapper(doc_it->second.text, PositionEncoding::UTF16);
                wp.offset = mapper.to_offset(params.text_document_position_params.position);
            }

            auto result = co_await pool.send_stateful<worker::GoToDefinitionParams>(path_id, wp);
            if(!result.has_value())
                co_return serde_raw{};
            co_return std::move(result.value());
        });

    // =========================================================================
    // Feature requests routed to stateless workers
    // =========================================================================

    // --- textDocument/completion ---
    peer.on_request(
        "textDocument/completion",
        [this](RequestContext& ctx, const protocol::CompletionParams& params) -> RawResult {
            if(lifecycle != ServerLifecycle::Ready)
                co_return et::outcome_error(et::ipc::Error{"Server not ready"});

            auto path = uri_to_path(params.text_document_position_params.text_document.uri);
            auto path_id = path_pool.intern(path);

            auto doc_it = documents.find(path_id);
            if(doc_it == documents.end())
                co_return serde_raw{};  // null

            auto& doc = doc_it->second;

            using eventide::ipc::lsp::PositionMapper;
            using eventide::ipc::lsp::PositionEncoding;
            PositionMapper mapper(doc.text, PositionEncoding::UTF16);

            worker::CompletionParams wp;
            wp.path = path;
            wp.version = doc.version;
            wp.text = doc.text;
            fill_compile_args(path, wp.directory, wp.arguments);
            wp.offset = mapper.to_offset(params.text_document_position_params.position);

            auto result = co_await pool.send_stateless<worker::CompletionParams>(wp);
            if(!result.has_value())
                co_return serde_raw{};
            co_return std::move(result.value());
        });

    // --- textDocument/signatureHelp ---
    peer.on_request(
        "textDocument/signatureHelp",
        [this](RequestContext& ctx, const protocol::SignatureHelpParams& params) -> RawResult {
            if(lifecycle != ServerLifecycle::Ready)
                co_return et::outcome_error(et::ipc::Error{"Server not ready"});

            auto path = uri_to_path(params.text_document_position_params.text_document.uri);
            auto path_id = path_pool.intern(path);

            auto doc_it = documents.find(path_id);
            if(doc_it == documents.end())
                co_return serde_raw{};

            auto& doc = doc_it->second;

            using eventide::ipc::lsp::PositionMapper;
            using eventide::ipc::lsp::PositionEncoding;
            PositionMapper mapper(doc.text, PositionEncoding::UTF16);

            worker::SignatureHelpParams wp;
            wp.path = path;
            wp.version = doc.version;
            wp.text = doc.text;
            fill_compile_args(path, wp.directory, wp.arguments);
            wp.offset = mapper.to_offset(params.text_document_position_params.position);

            auto result = co_await pool.send_stateless<worker::SignatureHelpParams>(wp);
            if(!result.has_value())
                co_return serde_raw{};
            co_return std::move(result.value());
        });
}

}  // namespace clice
