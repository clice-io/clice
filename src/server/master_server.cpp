#include "server/master_server.h"

#include <algorithm>
#include <format>
#include <string>
#include <type_traits>
#include <variant>

#include "eventide/ipc/lsp/position.h"
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
    loop(loop), peer(peer), pool(loop), compiler(loop, peer, workspace, pool, sessions),
    indexer(loop,
            workspace,
            sessions,
            pool,
            compiler,
            [this](uint32_t proj_path_id) {
                // Bridge project-level path_id to server-level path_id.
                // The two PathPools may assign different IDs to the same path.
                auto path = workspace.project_index.path_pool.path(proj_path_id);
                auto server_id = workspace.path_pool.intern(path);
                return sessions.contains(server_id);
            }),
    self_path(std::move(self_path)) {}

MasterServer::~MasterServer() = default;

et::task<> MasterServer::load_workspace() {
    if(workspace_root.empty())
        co_return;

    if(!workspace.config.cache_dir.empty()) {
        auto ec = llvm::sys::fs::create_directories(workspace.config.cache_dir);
        if(ec) {
            LOG_WARN("Failed to create cache directory {}: {}",
                     workspace.config.cache_dir,
                     ec.message());
        } else {
            LOG_INFO("Cache directory: {}", workspace.config.cache_dir);
        }

        for(auto* subdir: {"cache/pch", "cache/pcm"}) {
            auto dir = path::join(workspace.config.cache_dir, subdir);
            auto ec2 = llvm::sys::fs::create_directories(dir);
            if(ec2) {
                LOG_WARN("Failed to create {}: {}", dir, ec2.message());
            }
        }

        // Clean up stale files first, then load — load_cache() only restores
        // entries still listed in cache.json, so cleanup won't delete live files.
        workspace.cleanup_cache();
        workspace.load_cache();
    }

    std::string cdb_path;
    if(!workspace.config.compile_commands_path.empty()) {
        if(llvm::sys::fs::exists(workspace.config.compile_commands_path)) {
            cdb_path = workspace.config.compile_commands_path;
        } else {
            LOG_WARN("Configured compile_commands_path not found: {}",
                     workspace.config.compile_commands_path);
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

    auto count = workspace.cdb.load(cdb_path);
    LOG_INFO("Loaded CDB from {} with {} entries", cdb_path, count);

    auto report = scan_dependency_graph(workspace.cdb, workspace.path_pool, workspace.dep_graph);
    workspace.dep_graph.build_reverse_map();

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

    workspace.build_module_map();
    indexer.load(workspace.config.index_dir);

    if(workspace.config.enable_indexing) {
        for(auto& entry: workspace.cdb.get_entries()) {
            auto file = workspace.cdb.resolve_path(entry.file);
            auto server_id = workspace.path_pool.intern(file);
            indexer.enqueue(server_id);
        }
        indexer.schedule();
    }

    compiler.init_compile_graph();
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
            workspace_root = uri_to_path(*init.root_uri);
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
        workspace.config = CliceConfig::load_from_workspace(workspace_root);

        if(!workspace.config.logging_dir.empty()) {
            auto now = std::chrono::system_clock::now();
            auto pid = llvm::sys::Process::getProcessId();
            auto session_dir = path::join(workspace.config.logging_dir,
                                          std::format("{:%Y-%m-%d_%H-%M-%S}_{}", now, pid));
            logging::file_logger("master", session_dir, logging::options);
            session_log_dir = session_dir;
        }

        LOG_INFO("Server ready (stateful={}, stateless={}, idle={}ms)",
                 workspace.config.stateful_worker_count,
                 workspace.config.stateless_worker_count,
                 workspace.config.idle_timeout_ms);

        WorkerPoolOptions pool_opts;
        pool_opts.self_path = self_path;
        pool_opts.stateful_count = workspace.config.stateful_worker_count;
        pool_opts.stateless_count = workspace.config.stateless_worker_count;
        pool_opts.worker_memory_limit = workspace.config.worker_memory_limit;
        pool_opts.log_dir = session_log_dir;
        if(!pool.start(pool_opts)) {
            LOG_ERROR("Failed to start worker pool");
            return;
        }

        lifecycle = ServerLifecycle::Ready;

        compiler.on_indexing_needed = [this]() {
            indexer.schedule();
        };

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

        indexer.save(workspace.config.index_dir);
        workspace.save_cache();

        loop.schedule([this]() -> et::task<> {
            co_await pool.stop();
            loop.stop();
        }());
    });

    /// Document lifecycle — handled directly by MasterServer.

    peer.on_notification([this](const protocol::DidOpenTextDocumentParams& params) {
        if(lifecycle != ServerLifecycle::Ready)
            return;

        auto path = uri_to_path(params.text_document.uri);
        auto path_id = workspace.path_pool.intern(path);

        auto [it, _] = sessions.try_emplace(path_id);
        auto& session = it->second;
        session.path_id = path_id;
        session.version = params.text_document.version;
        session.text = params.text_document.text;
        session.generation++;

        LOG_DEBUG("didOpen: {} (v{})", path, params.text_document.version);
    });

    peer.on_notification([this](const protocol::DidChangeTextDocumentParams& params) {
        if(lifecycle != ServerLifecycle::Ready)
            return;

        auto path = uri_to_path(params.text_document.uri);
        auto path_id = workspace.path_pool.intern(path);

        auto it = sessions.find(path_id);
        if(it == sessions.end())
            return;

        auto& session = it->second;
        session.version = params.text_document.version;

        for(auto& change: params.content_changes) {
            std::visit(
                [&](auto& c) {
                    using T = std::remove_cvref_t<decltype(c)>;
                    if constexpr(std::is_same_v<T,
                                                protocol::TextDocumentContentChangeWholeDocument>) {
                        session.text = c.text;
                    } else {
                        auto& range = c.range;
                        lsp::PositionMapper mapper(session.text, lsp::PositionEncoding::UTF16);
                        auto start = mapper.to_offset(range.start);
                        auto end = mapper.to_offset(range.end);
                        if(start && end && *start <= *end) {
                            session.text.replace(*start, *end - *start, c.text);
                        }
                    }
                },
                change);
        }

        session.generation++;
        session.ast_dirty = true;

        LOG_DEBUG("didChange: path={} version={} gen={}",
                  path,
                  session.version,
                  session.generation);

        worker::DocumentUpdateParams update;
        update.path = path;
        update.version = session.version;
        pool.notify_stateful(path_id, update);
    });

    peer.on_notification([this](const protocol::DidCloseTextDocumentParams& params) {
        if(lifecycle != ServerLifecycle::Ready)
            return;

        auto path = uri_to_path(params.text_document.uri);
        auto path_id = workspace.path_pool.intern(path);

        workspace.on_file_closed(path_id);
        pool.notify_stateful(path_id, worker::EvictParams{path});

        // Clear diagnostics for the closed file.
        protocol::PublishDiagnosticsParams diag_params;
        diag_params.uri = params.text_document.uri;
        peer.send_notification(diag_params);

        sessions.erase(path_id);

        indexer.enqueue(path_id);
        indexer.schedule();

        LOG_DEBUG("didClose: {}", path);
    });

    peer.on_notification([this](const protocol::DidSaveTextDocumentParams& params) {
        if(lifecycle != ServerLifecycle::Ready)
            return;

        auto path = uri_to_path(params.text_document.uri);
        auto path_id = workspace.path_pool.intern(path);

        auto dirtied = workspace.on_file_saved(path_id);
        for(auto dirty_id: dirtied) {
            if(auto sit = sessions.find(dirty_id); sit != sessions.end()) {
                sit->second.ast_dirty = true;
            } else {
                indexer.enqueue(dirty_id);
            }
        }

        // Invalidate header contexts for sessions whose host is this file.
        for(auto& [hdr_id, session]: sessions) {
            if(session.header_context && session.header_context->host_path_id == path_id) {
                session.header_context.reset();
                session.ast_dirty = true;
            }
        }

        indexer.schedule();

        LOG_DEBUG("didSave: {}", path);
    });

    /// Feature requests — stateful forwarding.

    peer.on_request([this](RequestContext& ctx, const protocol::HoverParams& params) -> RawResult {
        auto path = uri_to_path(params.text_document_position_params.text_document.uri);
        auto path_id = workspace.path_pool.intern(path);
        auto sit = sessions.find(path_id);
        if(sit == sessions.end())
            co_return serde_raw{"null"};
        co_return co_await compiler.forward_query(worker::QueryKind::Hover,
                                                  params.text_document_position_params.position,
                                                  sit->second);
    });

    peer.on_request([this](RequestContext& ctx,
                           const protocol::SemanticTokensParams& params) -> RawResult {
        auto path = uri_to_path(params.text_document.uri);
        auto path_id = workspace.path_pool.intern(path);
        auto sit = sessions.find(path_id);
        if(sit == sessions.end())
            co_return serde_raw{"null"};
        co_return co_await compiler.forward_query(worker::QueryKind::SemanticTokens, sit->second);
    });

    peer.on_request(
        [this](RequestContext& ctx, const protocol::InlayHintParams& params) -> RawResult {
            auto path = uri_to_path(params.text_document.uri);
            auto path_id = workspace.path_pool.intern(path);
            auto sit = sessions.find(path_id);
            if(sit == sessions.end())
                co_return serde_raw{"null"};
            co_return co_await compiler.forward_query(worker::QueryKind::InlayHints, sit->second);
        });

    peer.on_request(
        [this](RequestContext& ctx, const protocol::FoldingRangeParams& params) -> RawResult {
            auto path = uri_to_path(params.text_document.uri);
            auto path_id = workspace.path_pool.intern(path);
            auto sit = sessions.find(path_id);
            if(sit == sessions.end())
                co_return serde_raw{"null"};
            co_return co_await compiler.forward_query(worker::QueryKind::FoldingRange, sit->second);
        });

    peer.on_request([this](RequestContext& ctx,
                           const protocol::DocumentSymbolParams& params) -> RawResult {
        auto path = uri_to_path(params.text_document.uri);
        auto path_id = workspace.path_pool.intern(path);
        auto sit = sessions.find(path_id);
        if(sit == sessions.end())
            co_return serde_raw{"null"};
        co_return co_await compiler.forward_query(worker::QueryKind::DocumentSymbol, sit->second);
    });

    peer.on_request(
        [this](RequestContext& ctx, const protocol::DocumentLinkParams& params) -> RawResult {
            auto path = uri_to_path(params.text_document.uri);
            auto path_id = workspace.path_pool.intern(path);
            auto sit = sessions.find(path_id);
            if(sit == sessions.end())
                co_return serde_raw{"null"};
            co_return co_await compiler.forward_query(worker::QueryKind::DocumentLink, sit->second);
        });

    peer.on_request(
        [this](RequestContext& ctx, const protocol::CodeActionParams& params) -> RawResult {
            auto path = uri_to_path(params.text_document.uri);
            auto path_id = workspace.path_pool.intern(path);
            auto sit = sessions.find(path_id);
            if(sit == sessions.end())
                co_return serde_raw{"null"};
            co_return co_await compiler.forward_query(worker::QueryKind::CodeAction, sit->second);
        });

    /// Helper: resolve URI to path, path_id, and Session pointer.
    auto resolve_uri = [this](const std::string& uri) {
        struct Result {
            std::string path;
            std::uint32_t path_id;
            Session* session;
        };
        auto path = uri_to_path(uri);
        auto path_id = workspace.path_pool.intern(path);
        auto sit = sessions.find(path_id);
        Session* session = (sit != sessions.end()) ? &sit->second : nullptr;
        return Result{std::move(path), path_id, session};
    };

    auto lookup_at = [this, resolve_uri](const std::string& uri, const protocol::Position& pos) {
        auto [path, path_id, session] = resolve_uri(uri);
        return indexer.lookup_symbol(uri, path, pos, session);
    };

    auto query_at = [this, resolve_uri](const std::string& uri,
                                        const protocol::Position& pos,
                                        RelationKind kind) -> std::vector<protocol::Location> {
        auto [path, path_id, session] = resolve_uri(uri);
        return indexer.query_relations(path, pos, kind, session);
    };

    auto resolve_item =
        [this,
         resolve_uri](const std::string& uri,
                      const protocol::Range& range,
                      const std::optional<protocol::LSPAny>& data) -> std::optional<SymbolInfo> {
        auto [path, path_id, session] = resolve_uri(uri);
        return indexer.resolve_hierarchy_item(uri, path, range, data, session);
    };

    /// Feature requests — index-based with AST fallback.

    peer.on_request([this, query_at](RequestContext& ctx,
                                     const protocol::DefinitionParams& params) -> RawResult {
        auto& uri = params.text_document_position_params.text_document.uri;
        auto& pos = params.text_document_position_params.position;

        auto result = query_at(uri, pos, RelationKind::Definition);
        if(!result.empty()) {
            co_return to_raw(result);
        }

        auto path = uri_to_path(uri);
        auto path_id = workspace.path_pool.intern(path);
        auto sit = sessions.find(path_id);
        if(sit == sessions.end())
            co_return serde_raw{"null"};
        co_return co_await compiler.forward_query(worker::QueryKind::GoToDefinition,
                                                  pos,
                                                  sit->second);
    });

    peer.on_request([this, query_at](RequestContext& ctx,
                                     const protocol::ReferenceParams& params) -> RawResult {
        auto& uri = params.text_document_position_params.text_document.uri;
        auto& pos = params.text_document_position_params.position;

        auto locations = query_at(uri, pos, RelationKind::Reference);

        if(params.context.include_declaration) {
            auto defs = query_at(uri, pos, RelationKind::Definition);
            locations.insert(locations.end(),
                             std::make_move_iterator(defs.begin()),
                             std::make_move_iterator(defs.end()));
        }

        if(locations.empty())
            co_return serde_raw{"null"};
        co_return to_raw(locations);
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

    peer.on_request([this](RequestContext& ctx,
                           const protocol::CompletionParams& params) -> RawResult {
        auto path = uri_to_path(params.text_document_position_params.text_document.uri);
        auto path_id = workspace.path_pool.intern(path);
        auto sit = sessions.find(path_id);
        if(sit == sessions.end())
            co_return serde_raw{"null"};
        co_return co_await compiler.handle_completion(params.text_document_position_params.position,
                                                      sit->second);
    });

    peer.on_request(
        [this](RequestContext& ctx, const protocol::SignatureHelpParams& params) -> RawResult {
            auto path = uri_to_path(params.text_document_position_params.text_document.uri);
            auto path_id = workspace.path_pool.intern(path);
            auto sit = sessions.find(path_id);
            if(sit == sessions.end())
                co_return serde_raw{"null"};
            co_return co_await compiler.forward_build(worker::BuildKind::SignatureHelp,
                                                      params.text_document_position_params.position,
                                                      sit->second);
        });

    /// Hierarchy queries — index-based.

    peer.on_request(
        [this, lookup_at](RequestContext& ctx,
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

    peer.on_request([this, resolve_item](
                        RequestContext& ctx,
                        const protocol::CallHierarchyIncomingCallsParams& params) -> RawResult {
        auto info = resolve_item(params.item.uri, params.item.range, params.item.data);
        if(!info)
            co_return serde_raw{"null"};
        auto results = indexer.find_incoming_calls(info->hash);
        if(results.empty())
            co_return serde_raw{"null"};
        co_return to_raw(results);
    });

    peer.on_request([this, resolve_item](
                        RequestContext& ctx,
                        const protocol::CallHierarchyOutgoingCallsParams& params) -> RawResult {
        auto info = resolve_item(params.item.uri, params.item.range, params.item.data);
        if(!info)
            co_return serde_raw{"null"};
        auto results = indexer.find_outgoing_calls(info->hash);
        if(results.empty())
            co_return serde_raw{"null"};
        co_return to_raw(results);
    });

    peer.on_request(
        [this, lookup_at](RequestContext& ctx,
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

    peer.on_request(
        [this, resolve_item](RequestContext& ctx,
                             const protocol::TypeHierarchySupertypesParams& params) -> RawResult {
            auto info = resolve_item(params.item.uri, params.item.range, params.item.data);
            if(!info)
                co_return serde_raw{"null"};
            auto results = indexer.find_supertypes(info->hash);
            if(results.empty())
                co_return serde_raw{"null"};
            co_return to_raw(results);
        });

    peer.on_request(
        [this, resolve_item](RequestContext& ctx,
                             const protocol::TypeHierarchySubtypesParams& params) -> RawResult {
            auto info = resolve_item(params.item.uri, params.item.range, params.item.data);
            if(!info)
                co_return serde_raw{"null"};
            auto results = indexer.find_subtypes(info->hash);
            if(results.empty())
                co_return serde_raw{"null"};
            co_return to_raw(results);
        });

    peer.on_request(
        [this](RequestContext& ctx, const protocol::WorkspaceSymbolParams& params) -> RawResult {
            auto results = indexer.search_symbols(params.query);
            if(results.empty())
                co_return serde_raw{"null"};
            co_return to_raw(results);
        });

    /// clice/ extension commands.

    peer.on_request(
        "clice/queryContext",
        [this](RequestContext& ctx, const ext::QueryContextParams& params) -> RawResult {
            auto path = uri_to_path(params.uri);
            auto path_id = workspace.path_pool.intern(path);
            int offset_val = std::max(0, params.offset.value_or(0));
            constexpr int page_size = 10;

            ext::QueryContextResult result;
            std::vector<ext::ContextItem> all_items;

            auto hosts = workspace.dep_graph.find_host_sources(path_id);
            for(auto host_id: hosts) {
                auto host_path = workspace.path_pool.resolve(host_id);
                auto host_cdb = workspace.cdb.lookup(host_path, {.suppress_logging = true});
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
                auto entries = workspace.cdb.lookup(path, {.suppress_logging = true});
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
            auto path = uri_to_path(params.uri);
            auto path_id = workspace.path_pool.intern(path);

            ext::CurrentContextResult result;
            auto sit = sessions.find(path_id);
            if(sit != sessions.end() && sit->second.active_context) {
                auto ctx_path = workspace.path_pool.resolve(*sit->second.active_context);
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
            auto path = uri_to_path(params.uri);
            auto path_id = workspace.path_pool.intern(path);
            auto context_path = uri_to_path(params.context_uri);
            auto context_path_id = workspace.path_pool.intern(context_path);

            ext::SwitchContextResult result;

            auto context_cdb = workspace.cdb.lookup(context_path, {.suppress_logging = true});
            if(context_cdb.empty()) {
                result.success = false;
                co_return to_raw(result);
            }

            auto sit = sessions.find(path_id);
            if(sit == sessions.end()) {
                result.success = false;
                co_return to_raw(result);
            }

            sit->second.active_context = context_path_id;
            sit->second.header_context.reset();
            sit->second.pch_ref.reset();
            sit->second.ast_deps.reset();
            sit->second.ast_dirty = true;

            result.success = true;
            co_return to_raw(result);
        });
}

}  // namespace clice
