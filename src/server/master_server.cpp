#include "server/master_server.h"

#include <algorithm>
#include <format>
#include <string>

#include "eventide/ipc/lsp/protocol.h"
#include "eventide/ipc/lsp/uri.h"
#include "eventide/reflection/enum.h"
#include "eventide/serde/json/json.h"
#include "semantic/symbol_kind.h"
#include "server/protocol.h"
#include "support/filesystem.h"
#include "support/logging.h"

#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/Process.h"

namespace clice {

namespace protocol = eventide::ipc::protocol;
namespace lsp = eventide::ipc::lsp;
namespace refl = eventide::refl;
using et::ipc::RequestResult;
using RequestContext = et::ipc::JsonPeer::RequestContext;
using serde_raw = et::serde::RawValue;

/// Serialize a value to a JSON RawValue using LSP config.
template <typename T>
static serde_raw to_raw(const T& value) {
    auto json = et::serde::json::to_json<et::ipc::lsp_config>(value);
    return serde_raw{json ? std::move(*json) : "null"};
}

MasterServer::MasterServer(et::event_loop& loop, et::ipc::JsonPeer& peer, std::string self_path) :
    loop(loop),
    peer(peer),
    pool(loop),
    indexer(path_pool),
    compiler(loop, peer, path_pool, pool, indexer, config, cdb, dependency_graph),
    self_path(std::move(self_path)) {}

MasterServer::~MasterServer() = default;

et::task<> MasterServer::load_workspace() {
    if(workspace_root.empty())
        co_return;

    if(!config.cache_dir.empty()) {
        auto ec = llvm::sys::fs::create_directories(config.cache_dir);
        if(ec) {
            LOG_WARN("Failed to create cache directory {}: {}", config.cache_dir, ec.message());
        } else {
            LOG_INFO("Cache directory: {}", config.cache_dir);
        }

        for(auto* subdir: {"cache/pch", "cache/pcm"}) {
            auto dir = path::join(config.cache_dir, subdir);
            auto ec2 = llvm::sys::fs::create_directories(dir);
            if(ec2) {
                LOG_WARN("Failed to create {}: {}", dir, ec2.message());
            }
        }

        // Clean up stale files first, then load — load_cache() only restores
        // entries still listed in cache.json, so cleanup won't delete live files.
        compiler.cleanup_cache();
        compiler.load_cache();
    }

    std::string cdb_path;
    if(!config.compile_commands_path.empty()) {
        if(llvm::sys::fs::exists(config.compile_commands_path)) {
            cdb_path = config.compile_commands_path;
        } else {
            LOG_WARN("Configured compile_commands_path not found: {}",
                     config.compile_commands_path);
        }
    }

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
    dependency_graph.build_reverse_map();

    auto unresolved = report.includes_found - report.includes_resolved;
    double accuracy =
        report.includes_found > 0
            ? 100.0 * static_cast<double>(report.includes_resolved) / report.includes_found
            : 100.0;
    LOG_INFO(
        "Dependency scan: {}ms, {} files ({} source + {} header), "
        "{} edges, {}/{} resolved ({:.1f}%), {} waves",
        report.elapsed_ms, report.total_files, report.source_files, report.header_files,
        report.total_edges, report.includes_resolved, report.includes_found, accuracy,
        report.waves);
    if(unresolved > 0) {
        LOG_WARN("{} unresolved includes", unresolved);
    }

    compiler.build_module_map();
    indexer.load(config.index_dir);

    if(config.enable_indexing) {
        for(auto& entry: cdb.get_entries()) {
            auto file = cdb.resolve_path(entry.file);
            auto server_id = path_pool.intern(file);
            index_queue.push_back(server_id);
        }
        if(!index_queue.empty()) {
            LOG_INFO("Queued {} files for background indexing", index_queue.size());
            schedule_indexing();
        }
    }

    compiler.init_compile_graph();
}

void MasterServer::schedule_indexing() {
    if(!config.enable_indexing || indexing_active || indexing_scheduled)
        return;
    indexing_scheduled = true;

    if(!index_idle_timer) {
        index_idle_timer = std::make_shared<et::timer>(et::timer::create(loop));
    }
    index_idle_timer->start(std::chrono::milliseconds(config.idle_timeout_ms));
    loop.schedule(run_background_indexing());
}

et::task<> MasterServer::run_background_indexing() {
    if(index_idle_timer) {
        co_await index_idle_timer->wait();
    }
    indexing_scheduled = false;

    if(index_queue_pos >= index_queue.size()) {
        LOG_DEBUG("Background indexing: queue exhausted");
        co_return;
    }

    indexing_active = true;
    std::size_t processed = 0;

    while(index_queue_pos < index_queue.size()) {
        auto server_path_id = index_queue[index_queue_pos];
        index_queue_pos++;

        auto file_path = std::string(path_pool.resolve(server_path_id));

        /// Skip open files — their index comes from the stateful worker.
        if(compiler.is_file_open(server_path_id)) {
            continue;
        }

        if(!indexer.need_update(file_path))
            continue;

        worker::IndexParams params;
        params.file = file_path;
        if(!compiler.fill_compile_args(file_path, params.directory, params.arguments))
            continue;

        compiler.fill_pcm_deps(params.pcms);

        LOG_INFO("Background indexing: {}", file_path);

        auto result = co_await pool.send_stateless(params);
        if(result.has_value() && result.value().success && !result.value().tu_index_data.empty()) {
            LOG_INFO("Background indexing got TUIndex for {}: {} bytes",
                     file_path,
                     result.value().tu_index_data.size());
            indexer.merge(result.value().tu_index_data.data(),
                          result.value().tu_index_data.size());
            ++processed;
        } else if(result.has_value() && !result.value().success) {
            LOG_WARN("Background index failed for {}: {}", file_path, result.value().error);
        } else if(result.has_value() && result.value().tu_index_data.empty()) {
            LOG_WARN("Background index returned empty TUIndex for {}", file_path);
        } else {
            LOG_WARN("Background index IPC error for {}: {}", file_path, result.error().message);
        }
    }

    indexing_active = false;
    LOG_INFO("Background indexing complete: {} files processed", processed);
    indexer.save(config.index_dir);
}

void MasterServer::register_handlers() {
    using StringVec = std::vector<std::string>;

    peer.on_request([this](RequestContext& ctx, const protocol::InitializeParams& params)
                        -> RequestResult<protocol::InitializeParams> {
        if(lifecycle != ServerLifecycle::Uninitialized) {
            co_return et::outcome_error(protocol::Error{"Server already initialized"});
        }

        auto& init = params.lsp__initialize_params;
        if(init.root_uri.has_value()) {
            workspace_root = Compiler::uri_to_path(*init.root_uri);
        }

        lifecycle = ServerLifecycle::Initialized;
        LOG_INFO("Initialized with workspace: {}", workspace_root);

        protocol::InitializeResult result;
        auto& caps = result.capabilities;

        caps.text_document_sync = protocol::TextDocumentSyncOptions{
            .open_close = true,
            .change = protocol::TextDocumentSyncKind::Incremental,
            .save = protocol::variant<protocol::boolean, protocol::SaveOptions>{true},
        };
        caps.workspace = protocol::WorkspaceOptions{};
        caps.workspace->workspace_folders = protocol::WorkspaceFoldersServerCapabilities{
            .supported = true,
            .change_notifications = true,
        };

        caps.hover_provider = true;
        caps.completion_provider = protocol::CompletionOptions{
            .trigger_characters = StringVec{".", "<", ">", ":", "\"", "/", "*"},
        };
        caps.signature_help_provider = protocol::SignatureHelpOptions{
            .trigger_characters = StringVec{"(", ")", "{", "}", "<", ">", ","},
        };
        /// FIXME: In the future, we would support work done progress.
        caps.declaration_provider = protocol::DeclarationOptions{
            .work_done_progress = false,
        };
        caps.definition_provider = protocol::DefinitionOptions{
            .work_done_progress = false,
        };
        caps.implementation_provider = protocol::ImplementationOptions{
            .work_done_progress = false,
        };
        caps.type_definition_provider = protocol::TypeDefinitionOptions{
            .work_done_progress = false,
        };
        caps.references_provider = protocol::ReferenceOptions{
            .work_done_progress = false,
        };
        caps.document_symbol_provider = true;
        caps.document_link_provider = protocol::DocumentLinkOptions{};
        caps.code_action_provider = true;
        caps.folding_range_provider = true;
        caps.inlay_hint_provider = true;
        caps.call_hierarchy_provider = true;
        caps.type_hierarchy_provider = true;
        caps.workspace_symbol_provider = true;

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

        protocol::ServerInfo info;
        info.name = "clice";
        info.version = "0.1.0";
        result.server_info = std::move(info);

        co_return result;
    });

    peer.on_notification([this](const protocol::InitializedParams& params) {
        config = CliceConfig::load_from_workspace(workspace_root);

        if(!config.logging_dir.empty()) {
            auto now = std::chrono::system_clock::now();
            auto pid = llvm::sys::Process::getProcessId();
            auto session_dir =
                path::join(config.logging_dir, std::format("{:%Y-%m-%d_%H-%M-%S}_{}", now, pid));
            logging::file_logger("master", session_dir, logging::options);
            session_log_dir = session_dir;
        }

        LOG_INFO("Server ready (stateful={}, stateless={}, idle={}ms)",
                 config.stateful_worker_count,
                 config.stateless_worker_count,
                 config.idle_timeout_ms);

        WorkerPoolOptions pool_opts;
        pool_opts.self_path = self_path;
        pool_opts.stateful_count = config.stateful_worker_count;
        pool_opts.stateless_count = config.stateless_worker_count;
        pool_opts.worker_memory_limit = config.worker_memory_limit;
        pool_opts.log_dir = session_log_dir;
        if(!pool.start(pool_opts)) {
            LOG_ERROR("Failed to start worker pool");
            return;
        }

        lifecycle = ServerLifecycle::Ready;

        compiler.on_indexing_needed = [this]() { schedule_indexing(); };

        loop.schedule(load_workspace());
    });

    peer.on_request(
        [this](RequestContext& ctx,
               const protocol::ShutdownParams& params) -> RequestResult<protocol::ShutdownParams> {
            lifecycle = ServerLifecycle::ShuttingDown;
            LOG_INFO("Shutdown requested");
            co_return nullptr;
        });

    peer.on_notification([this](const protocol::ExitParams& params) {
        lifecycle = ServerLifecycle::Exited;
        LOG_INFO("Exit notification received");

        indexer.save(config.index_dir);
        compiler.save_cache();

        loop.schedule([this]() -> et::task<> {
            co_await pool.stop();
            loop.stop();
        }());
    });

    /// Document lifecycle — delegate to Compiler.
    peer.on_notification([this](const protocol::DidOpenTextDocumentParams& params) {
        if(lifecycle != ServerLifecycle::Ready)
            return;
        compiler.open_document(params.text_document.uri, params.text_document.text,
                               params.text_document.version);
    });

    peer.on_notification([this](const protocol::DidChangeTextDocumentParams& params) {
        if(lifecycle != ServerLifecycle::Ready)
            return;
        compiler.apply_changes(params);
    });

    peer.on_notification([this](const protocol::DidCloseTextDocumentParams& params) {
        if(lifecycle != ServerLifecycle::Ready)
            return;
        auto path_id = compiler.close_document(params.text_document.uri);
        index_queue.push_back(path_id);
        schedule_indexing();
    });

    peer.on_notification([this](const protocol::DidSaveTextDocumentParams& params) {
        if(lifecycle != ServerLifecycle::Ready)
            return;
        auto to_index = compiler.on_save(params.text_document.uri);
        for(auto id: to_index)
            index_queue.push_back(id);
        schedule_indexing();
    });

    /// Feature requests — stateful forwarding.
    peer.on_request([this](RequestContext& ctx, const protocol::HoverParams& params) -> RawResult {
        co_return co_await compiler.forward_stateful<worker::HoverParams>(
            params.text_document_position_params.text_document.uri,
            params.text_document_position_params.position);
    });

    peer.on_request([this](RequestContext& ctx,
                           const protocol::SemanticTokensParams& params) -> RawResult {
        co_return co_await compiler.forward_stateful<worker::SemanticTokensParams>(
            params.text_document.uri);
    });

    peer.on_request(
        [this](RequestContext& ctx, const protocol::InlayHintParams& params) -> RawResult {
            co_return co_await compiler.forward_stateful<worker::InlayHintsParams>(
                params.text_document.uri);
        });

    peer.on_request([this](RequestContext& ctx,
                           const protocol::FoldingRangeParams& params) -> RawResult {
        co_return co_await compiler.forward_stateful<worker::FoldingRangeParams>(
            params.text_document.uri);
    });

    peer.on_request([this](RequestContext& ctx,
                           const protocol::DocumentSymbolParams& params) -> RawResult {
        co_return co_await compiler.forward_stateful<worker::DocumentSymbolParams>(
            params.text_document.uri);
    });

    peer.on_request([this](RequestContext& ctx,
                           const protocol::DocumentLinkParams& params) -> RawResult {
        co_return co_await compiler.forward_stateful<worker::DocumentLinkParams>(
            params.text_document.uri);
    });

    peer.on_request(
        [this](RequestContext& ctx, const protocol::CodeActionParams& params) -> RawResult {
            co_return co_await compiler.forward_stateful<worker::CodeActionParams>(
                params.text_document.uri);
        });

    /// Helpers for index-based queries: resolve URI → path → path_id → doc_text.
    auto lookup_at = [this](const std::string& uri, const protocol::Position& pos) {
        auto path = Compiler::uri_to_path(uri);
        auto path_id = path_pool.intern(path);
        auto* doc = compiler.get_document(path_id);
        const std::string* doc_text = doc ? &doc->text : nullptr;
        return indexer.lookup_symbol(uri, path, path_id, pos, doc_text);
    };

    auto query_at = [this](const std::string& uri, const protocol::Position& pos,
                           RelationKind kind) {
        auto path = Compiler::uri_to_path(uri);
        auto path_id = path_pool.intern(path);
        auto* doc = compiler.get_document(path_id);
        const std::string* doc_text = doc ? &doc->text : nullptr;
        return indexer.query_relations(path, path_id, pos, kind, doc_text);
    };

    auto resolve_item = [this](const std::string& uri, const protocol::Range& range,
                               const std::optional<protocol::LSPAny>& data)
                             -> std::optional<SymbolInfo> {
        auto path = Compiler::uri_to_path(uri);
        auto path_id = path_pool.intern(path);
        auto* doc = compiler.get_document(path_id);
        const std::string* doc_text = doc ? &doc->text : nullptr;
        return indexer.resolve_hierarchy_item(uri, path, path_id, range, data, doc_text);
    };

    /// Feature requests — index-based with AST fallback.
    peer.on_request(
        [this, query_at](RequestContext& ctx, const protocol::DefinitionParams& params) -> RawResult {
            auto& uri = params.text_document_position_params.text_document.uri;
            auto& pos = params.text_document_position_params.position;

            auto result = query_at(uri, pos, RelationKind::Definition);
            if(!result.empty() && result.data != "null") {
                co_return std::move(result);
            }

            co_return co_await compiler.forward_stateful<worker::GoToDefinitionParams>(uri, pos);
        });

    peer.on_request(
        [this, query_at](RequestContext& ctx, const protocol::ReferenceParams& params) -> RawResult {
            auto& uri = params.text_document_position_params.text_document.uri;
            auto& pos = params.text_document_position_params.position;

            auto refs = query_at(uri, pos, RelationKind::Reference);

            if(params.context.include_declaration) {
                auto defs = query_at(uri, pos, RelationKind::Definition);
                if(!defs.empty() && defs.data != "null") {
                    if(refs.empty() || refs.data == "null") {
                        co_return std::move(defs);
                    }
                    if(refs.data.size() > 2 && defs.data.size() > 2) {
                        std::string merged = refs.data.substr(0, refs.data.size() - 1);
                        merged += ',';
                        merged += defs.data.substr(1);
                        co_return serde_raw{std::move(merged)};
                    }
                }
            }

            co_return std::move(refs);
        });

    peer.on_request(
        [this](RequestContext& ctx, const protocol::TypeDefinitionParams& params) -> RawResult {
            co_return serde_raw{"null"};
        });

    peer.on_request(
        [this](RequestContext& ctx, const protocol::ImplementationParams& params) -> RawResult {
            co_return serde_raw{"null"};
        });

    peer.on_request(
        [this](RequestContext& ctx, const protocol::DeclarationParams& params) -> RawResult {
            co_return serde_raw{"null"};
        });

    /// Feature requests — stateless forwarding.
    peer.on_request(
        [this](RequestContext& ctx, const protocol::CompletionParams& params) -> RawResult {
            co_return co_await compiler.handle_completion(
                params.text_document_position_params.text_document.uri,
                params.text_document_position_params.position);
        });

    peer.on_request(
        [this](RequestContext& ctx, const protocol::SignatureHelpParams& params) -> RawResult {
            co_return co_await compiler.forward_stateless<worker::SignatureHelpParams>(
                params.text_document_position_params.text_document.uri,
                params.text_document_position_params.position);
        });

    /// Hierarchy queries — index-based.
    peer.on_request([this, lookup_at](RequestContext& ctx,
                           const protocol::CallHierarchyPrepareParams& params) -> RawResult {
        auto& uri = params.text_document_position_params.text_document.uri;
        auto& pos = params.text_document_position_params.position;

        auto info = lookup_at(uri, pos);
        if(!info)
            co_return serde_raw{"null"};
        if(!(info->kind == SymbolKind::Function || info->kind == SymbolKind::Method))
            co_return serde_raw{"null"};

        std::vector<protocol::CallHierarchyItem> items;
        items.push_back(Indexer::build_call_hierarchy_item(*info));
        co_return to_raw(items);
    });

    peer.on_request([this, resolve_item](RequestContext& ctx,
                           const protocol::CallHierarchyIncomingCallsParams& params) -> RawResult {
        auto info = resolve_item(params.item.uri, params.item.range, params.item.data);
        if(!info)
            co_return serde_raw{"null"};
        co_return indexer.find_incoming_calls(info->hash);
    });

    peer.on_request([this, resolve_item](RequestContext& ctx,
                           const protocol::CallHierarchyOutgoingCallsParams& params) -> RawResult {
        auto info = resolve_item(params.item.uri, params.item.range, params.item.data);
        if(!info)
            co_return serde_raw{"null"};
        co_return indexer.find_outgoing_calls(info->hash);
    });

    peer.on_request([this, lookup_at](RequestContext& ctx,
                           const protocol::TypeHierarchyPrepareParams& params) -> RawResult {
        auto& uri = params.text_document_position_params.text_document.uri;
        auto& pos = params.text_document_position_params.position;

        auto info = lookup_at(uri, pos);
        if(!info)
            co_return serde_raw{"null"};
        if(!(info->kind == SymbolKind::Class || info->kind == SymbolKind::Struct ||
             info->kind == SymbolKind::Enum || info->kind == SymbolKind::Union))
            co_return serde_raw{"null"};

        std::vector<protocol::TypeHierarchyItem> items;
        items.push_back(Indexer::build_type_hierarchy_item(*info));
        co_return to_raw(items);
    });

    peer.on_request([this, resolve_item](RequestContext& ctx,
                           const protocol::TypeHierarchySupertypesParams& params) -> RawResult {
        auto info = resolve_item(params.item.uri, params.item.range, params.item.data);
        if(!info)
            co_return serde_raw{"null"};
        co_return indexer.find_supertypes(info->hash);
    });

    peer.on_request([this, resolve_item](RequestContext& ctx,
                           const protocol::TypeHierarchySubtypesParams& params) -> RawResult {
        auto info = resolve_item(params.item.uri, params.item.range, params.item.data);
        if(!info)
            co_return serde_raw{"null"};
        co_return indexer.find_subtypes(info->hash);
    });

    peer.on_request([this](RequestContext& ctx,
                           const protocol::WorkspaceSymbolParams& params) -> RawResult {
        co_return indexer.search_symbols(params.query);
    });

    /// clice/ extension commands.
    peer.on_request(
        "clice/queryContext",
        [this](RequestContext& ctx, const ext::QueryContextParams& params) -> RawResult {
            auto path = Compiler::uri_to_path(params.uri);
            auto path_id = path_pool.intern(path);
            int offset_val = std::max(0, params.offset.value_or(0));
            constexpr int page_size = 10;

            ext::QueryContextResult result;
            std::vector<ext::ContextItem> all_items;

            auto hosts = dependency_graph.find_host_sources(path_id);
            for(auto host_id: hosts) {
                auto host_path = path_pool.resolve(host_id);
                auto host_cdb = cdb.lookup(host_path, {.suppress_logging = true});
                if(host_cdb.empty())
                    continue;
                auto host_uri_opt = lsp::URI::from_file_path(std::string(host_path));
                if(!host_uri_opt)
                    continue;
                ext::ContextItem item;
                item.label = llvm::sys::path::filename(host_path).str();
                item.description = std::string(host_path);
                item.uri = host_uri_opt->str();
                all_items.push_back(std::move(item));
            }

            if(hosts.empty()) {
                auto entries = cdb.lookup(path, {.suppress_logging = true});
                for(std::size_t i = 0; i < entries.size(); ++i) {
                    auto& entry = entries[i];
                    std::string desc;
                    for(std::size_t j = 0; j < entry.arguments.size(); ++j) {
                        llvm::StringRef a(entry.arguments[j]);
                        if(a.starts_with("-D") || a.starts_with("-O") || a.starts_with("-std=") ||
                           a.starts_with("-g")) {
                            if(!desc.empty())
                                desc += ' ';
                            desc += entry.arguments[j];
                            if((a == "-D" || a == "-O") && j + 1 < entry.arguments.size()) {
                                desc += entry.arguments[++j];
                            }
                        }
                    }
                    if(desc.empty())
                        desc = std::format("config #{}", i);

                    auto uri_opt = lsp::URI::from_file_path(std::string(path));
                    if(!uri_opt)
                        continue;
                    ext::ContextItem item;
                    item.label = desc;
                    item.description = entry.directory.str();
                    item.uri = uri_opt->str();
                    all_items.push_back(std::move(item));
                }
            }

            result.total = static_cast<int>(all_items.size());
            int end = std::min(offset_val + page_size, static_cast<int>(all_items.size()));
            for(int i = offset_val; i < end; ++i) {
                result.contexts.push_back(std::move(all_items[i]));
            }
            co_return to_raw(result);
        });

    peer.on_request(
        "clice/currentContext",
        [this](RequestContext& ctx, const ext::CurrentContextParams& params) -> RawResult {
            auto path = Compiler::uri_to_path(params.uri);
            auto path_id = path_pool.intern(path);

            ext::CurrentContextResult result;
            auto active_ctx = compiler.get_active_context(path_id);
            if(active_ctx) {
                auto ctx_path = path_pool.resolve(*active_ctx);
                auto ctx_uri_opt = lsp::URI::from_file_path(std::string(ctx_path));
                if(ctx_uri_opt) {
                    ext::ContextItem item;
                    item.label = llvm::sys::path::filename(ctx_path).str();
                    item.description = std::string(ctx_path);
                    item.uri = ctx_uri_opt->str();
                    result.context = std::move(item);
                }
            }
            co_return to_raw(result);
        });

    peer.on_request(
        "clice/switchContext",
        [this](RequestContext& ctx, const ext::SwitchContextParams& params) -> RawResult {
            auto path = Compiler::uri_to_path(params.uri);
            auto path_id = path_pool.intern(path);
            auto context_path = Compiler::uri_to_path(params.context_uri);
            auto context_path_id = path_pool.intern(context_path);

            ext::SwitchContextResult result;

            auto context_cdb = cdb.lookup(context_path, {.suppress_logging = true});
            if(context_cdb.empty()) {
                result.success = false;
                co_return to_raw(result);
            }

            compiler.switch_context(path_id, context_path_id);
            result.success = true;
            co_return to_raw(result);
        });
}

}  // namespace clice
