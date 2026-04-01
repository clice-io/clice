#include "server/master_server.h"

#include <optional>
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
#include "syntax/scan.h"

#include "llvm/Support/xxhash.h"

namespace clice {

namespace protocol = eventide::ipc::protocol;
namespace lsp = eventide::ipc::lsp;
namespace refl = eventide::refl;
using et::ipc::RequestResult;
using RequestContext = et::ipc::JsonPeer::RequestContext;

MasterServer::MasterServer(et::event_loop& loop, et::ipc::JsonPeer& peer, std::string self_path) :
    loop(loop), peer(peer), pool(loop), self_path(std::move(self_path)) {}

MasterServer::~MasterServer() {
    if(compile_graph) {
        compile_graph->cancel_all();
    }
}

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
        timer_ptr = std::make_shared<et::timer>(et::timer::create(loop));
    }
    timer_ptr->start(std::chrono::milliseconds(config.debounce_ms));

    if(!doc.drain_scheduled) {
        doc.drain_scheduled = true;
        loop.schedule(run_build_drain(path_id, uri));
    }
}

et::task<> MasterServer::run_build_drain(std::uint32_t path_id, std::string uri) {
    // Wait for debounce timer.  Hold a shared_ptr copy so the timer
    // stays alive even if didClose erases the map entry mid-wait.
    if(auto timer_it = debounce_timers.find(path_id);
       timer_it != debounce_timers.end() && timer_it->second) {
        auto timer = timer_it->second;
        co_await timer->wait();
    }

    while(true) {
        auto doc_it = documents.find(path_id);
        if(doc_it == documents.end())
            co_return;

        doc_it->second.build_running = true;
        doc_it->second.build_requested = false;
        auto gen = doc_it->second.generation;

        // Ensure module dependencies are compiled first.
        if(compile_graph) {
            auto file_path = path_pool.resolve(path_id);
            auto cdb_results =
                cdb.lookup(file_path, {.query_toolchain = true, .suppress_logging = true});
            bool deps_ok = true;
            if(!cdb_results.empty()) {
                auto scan_result = scan_precise(cdb_results[0].arguments, cdb_results[0].directory);
                for(auto& mod_name: scan_result.modules) {
                    auto mod_ids = dependency_graph.lookup_module(mod_name);
                    if(!mod_ids.empty()) {
                        auto r = co_await compile_graph->compile(mod_ids[0]);
                        if(!r) {
                            deps_ok = false;
                            break;
                        }
                    }
                }
                // Module implementation units need their interface PCM.
                if(deps_ok && !scan_result.module_name.empty() && !scan_result.is_interface_unit) {
                    auto mod_ids = dependency_graph.lookup_module(scan_result.module_name);
                    if(!mod_ids.empty()) {
                        auto r = co_await compile_graph->compile(mod_ids[0]);
                        if(!r) {
                            deps_ok = false;
                        }
                    }
                }
            }
            if(!deps_ok) {
                LOG_WARN("Module dependency build failed for {}, skipping compile", uri);
                doc_it = documents.find(path_id);
                if(doc_it != documents.end()) {
                    doc_it->second.build_running = false;
                    doc_it->second.drain_scheduled = false;
                }
                co_return;
            }
        }

        // Re-lookup document after co_awaits in compile_graph section.
        doc_it = documents.find(path_id);
        if(doc_it == documents.end())
            co_return;

        // Send compile request to stateful worker
        worker::CompileParams params;
        params.path = std::string(path_pool.resolve(path_id));
        params.version = doc_it->second.version;
        params.text = doc_it->second.text;
        if(!fill_compile_args(path_pool.resolve(path_id), params.directory, params.arguments)) {
            doc_it->second.build_running = false;
            doc_it->second.drain_scheduled = false;
            co_return;
        }

        // Fill all available PCM paths (clang needs transitive deps).
        // Skip the file's own PCM — a module interface must not receive its
        // own precompiled module, or clang reports "multiple module declarations".
        for(auto& [pid, pcm_path]: pcm_paths) {
            if(pid == path_id)
                continue;
            auto mod_it = path_to_module.find(pid);
            if(mod_it != path_to_module.end()) {
                params.pcms[mod_it->second] = pcm_path;
            }
        }

        // Build or reuse PCH for preamble acceleration.
        co_await ensure_pch(path_id, params.path, params.text, params.directory, params.arguments);

        // Populate PCH info if available.
        if(auto pch_it = pch_paths.find(path_id); pch_it != pch_paths.end()) {
            params.pch = {pch_it->second, pch_bounds[path_id]};
        }

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

    auto report = scan_dependency_graph(cdb, path_pool, dependency_graph);

    auto unresolved = report.includes_found - report.includes_resolved;
    double accuracy =
        report.includes_found > 0
            ? 100.0 * static_cast<double>(report.includes_resolved) / report.includes_found
            : 100.0;
    LOG_INFO(
        "Dependency scan: {}ms, {} files ({} source + {} header), " "{} edges, {}/{} resolved ({:.1f}%), {} waves",
        report.elapsed_ms,
        report.total_files,
        report.source_files,
        report.header_files,
        report.total_edges,
        report.includes_resolved,
        report.includes_found,
        accuracy,
        report.waves);
    if(unresolved > 0) {
        LOG_WARN("{} unresolved includes", unresolved);
    }

    // Build reverse mapping: path_id -> module name.
    for(auto& [module_name, path_ids]: dependency_graph.modules()) {
        for(auto path_id: path_ids) {
            path_to_module[path_id] = module_name.str();
        }
    }

    if(path_to_module.empty()) {
        LOG_INFO("No C++20 modules detected, skipping CompileGraph");
        co_return;
    }

    // Lazy dependency resolver: scans a module file on demand to discover imports.
    auto resolve = [this](std::uint32_t path_id) -> llvm::SmallVector<std::uint32_t> {
        auto file_path = path_pool.resolve(path_id);
        auto results = cdb.lookup(file_path, {.query_toolchain = true, .suppress_logging = true});
        if(results.empty()) {
            return {};
        }

        auto& ctx = results[0];
        auto scan_result = scan_precise(ctx.arguments, ctx.directory);

        llvm::SmallVector<std::uint32_t> deps;
        for(auto& mod_name: scan_result.modules) {
            auto mod_ids = dependency_graph.lookup_module(mod_name);
            if(!mod_ids.empty()) {
                deps.push_back(mod_ids[0]);
            }
        }
        return deps;
    };

    // Dispatch: sends BuildPCM request to a stateless worker.
    auto dispatch = [this](std::uint32_t path_id) -> et::task<bool> {
        auto mod_it = path_to_module.find(path_id);
        if(mod_it == path_to_module.end()) {
            co_return false;
        }

        auto file_path = std::string(path_pool.resolve(path_id));
        worker::BuildPCMParams pcm_params;
        pcm_params.file = file_path;
        if(!fill_compile_args(file_path, pcm_params.directory, pcm_params.arguments)) {
            co_return false;
        }
        pcm_params.module_name = mod_it->second;

        // Clang needs ALL transitive PCM deps, not just direct imports.
        for(auto& [pid, pcm_path]: pcm_paths) {
            auto dep_mod_it = path_to_module.find(pid);
            if(dep_mod_it != path_to_module.end()) {
                pcm_params.pcms[dep_mod_it->second] = pcm_path;
            }
        }

        auto result = co_await pool.send_stateless(pcm_params);
        if(!result.has_value() || !result.value().success) {
            LOG_WARN("BuildPCM failed for module {}: {}",
                     mod_it->second,
                     result.has_value() ? result.value().error : result.error().message);
            co_return false;
        }

        pcm_paths[path_id] = result.value().pcm_path;
        LOG_INFO("Built PCM for module {}: {}", mod_it->second, result.value().pcm_path);
        co_return true;
    };

    compile_graph = std::make_unique<CompileGraph>(std::move(dispatch), std::move(resolve));
    LOG_INFO("CompileGraph initialized with {} module(s)", path_to_module.size());
}

bool MasterServer::fill_compile_args(llvm::StringRef path,
                                     std::string& directory,
                                     std::vector<std::string>& arguments) {
    auto results = cdb.lookup(path, {.query_toolchain = true});
    if(results.empty()) {
        LOG_WARN("No CDB entry for {}", path);
        return false;
    }
    auto& ctx = results.front();
    directory = ctx.directory.str();
    arguments.clear();
    for(auto* arg: ctx.arguments) {
        arguments.emplace_back(arg);
    }
    return true;
}

et::task<bool> MasterServer::ensure_pch(std::uint32_t path_id,
                                        llvm::StringRef path,
                                        const std::string& text,
                                        const std::string& directory,
                                        const std::vector<std::string>& arguments) {
    auto bound = compute_preamble_bound(text);
    if(bound == 0) {
        // No preamble directives — PCH would be empty. Clear any stale entry.
        if(auto old_it = pch_paths.find(path_id); old_it != pch_paths.end()) {
            fs::remove(old_it->second);
        }
        pch_paths.erase(path_id);
        pch_bounds.erase(path_id);
        pch_hashes.erase(path_id);
        co_return true;
    }

    auto preamble_hash = llvm::xxh3_64bits(llvm::StringRef(text).substr(0, bound));

    // Reuse existing PCH if preamble content hasn't changed.
    if(auto it = pch_hashes.find(path_id); it != pch_hashes.end()) {
        if(it->second == preamble_hash && pch_paths.contains(path_id)) {
            pch_bounds[path_id] = bound;
            co_return true;
        }
    }

    // If another coroutine is already building PCH for this file, wait for it.
    if(auto it = pch_building.find(path_id); it != pch_building.end()) {
        co_await it->second->wait();
        co_return pch_paths.contains(path_id);
    }

    // Register in-flight build so concurrent requests wait on us.
    auto completion = std::make_shared<et::event>();
    pch_building[path_id] = completion;

    // Build a new PCH via stateless worker.
    worker::BuildPCHParams pch_params;
    pch_params.file = std::string(path);
    pch_params.directory = directory;
    pch_params.arguments = arguments;
    pch_params.content = text;
    pch_params.preamble_bound = bound;

    LOG_DEBUG("Building PCH for {}, bound={}", path, bound);

    auto result = co_await pool.send_stateless(pch_params);

    if(!result.has_value() || !result.value().success) {
        LOG_WARN("PCH build failed for {}: {}",
                 path,
                 result.has_value() ? result.value().error : result.error().message);
        pch_building.erase(path_id);
        completion->set();
        co_return false;
    }

    // Delete old PCH temp file before replacing.
    if(auto old_it = pch_paths.find(path_id); old_it != pch_paths.end()) {
        fs::remove(old_it->second);
    }

    pch_paths[path_id] = result.value().pch_path;
    pch_bounds[path_id] = bound;
    pch_hashes[path_id] = preamble_hash;

    LOG_INFO("PCH built for {}: {}", path, result.value().pch_path);

    // Signal waiters after state is fully updated, then remove in-flight entry.
    pch_building.erase(path_id);
    completion->set();
    co_return true;
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
        auto offset = mapper.to_offset(position);
        if(!offset)
            co_return serde_raw{"null"};
        wp.offset = *offset;
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

    WorkerParams wp;
    wp.path = path;
    wp.version = doc.version;
    wp.text = doc.text;
    if(!fill_compile_args(path, wp.directory, wp.arguments))
        co_return serde_raw{};

    // Ensure PCH is available for stateless compilation (completion/signatureHelp).
    co_await ensure_pch(path_id, path, wp.text, wp.directory, wp.arguments);
    if(auto pch_it = pch_paths.find(path_id); pch_it != pch_paths.end()) {
        wp.pch = {pch_it->second, pch_bounds[path_id]};
    }

    // Fill available PCM paths for module-aware completion.
    // Skip the file's own PCM to avoid "multiple module declarations" errors.
    for(auto& [pid, pcm_path]: pcm_paths) {
        if(pid == path_id)
            continue;
        auto mod_it = path_to_module.find(pid);
        if(mod_it != path_to_module.end()) {
            wp.pcms[mod_it->second] = pcm_path;
        }
    }

    lsp::PositionMapper mapper(wp.text, lsp::PositionEncoding::UTF16);
    auto offset = mapper.to_offset(position);
    if(!offset)
        co_return serde_raw{"null"};
    wp.offset = *offset;

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
                        if(start && end && *start <= *end) {
                            doc.text.replace(*start, *end - *start, c.text);
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

        // Cancel in-flight module compilations for this file.
        if(compile_graph && compile_graph->has_unit(path_id)) {
            compile_graph->update(path_id);
        }

        documents.erase(path_id);
        debounce_timers.erase(path_id);
        pch_paths.erase(path_id);
        pch_bounds.erase(path_id);
        pch_hashes.erase(path_id);

        // Clear diagnostics for closed file
        clear_diagnostics(params.text_document.uri);

        LOG_DEBUG("didClose: {}", path);
    });

    // === textDocument/didSave ===
    peer.on_notification([this](const protocol::DidSaveTextDocumentParams& params) {
        if(lifecycle != ServerLifecycle::Ready)
            return;

        auto path = uri_to_path(params.text_document.uri);
        auto path_id = path_pool.intern(path);

        // Invalidate this file and cascade to dependents in the compile graph.
        if(compile_graph) {
            auto dirtied = compile_graph->update(path_id);
            // Remove stale PCMs for all invalidated units.
            for(auto dirty_id: dirtied) {
                pcm_paths.erase(dirty_id);
            }
            // Schedule rebuilds for dirtied units that are currently open.
            for(auto dirty_id: dirtied) {
                if(dirty_id == path_id)
                    continue;  // The saved file itself is rebuilt by its own didChange.
                if(documents.contains(dirty_id)) {
                    auto dirty_path = path_pool.resolve(dirty_id);
                    auto uri = lsp::URI::from_file_path(dirty_path);
                    if(uri.has_value()) {
                        schedule_build(dirty_id, uri->str());
                    }
                }
            }
        }

        // Invalidate all cached PCH hashes — the saved file may be a header
        // included by other TUs, so we must force rebuild for all open documents.
        pch_hashes.clear();

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
