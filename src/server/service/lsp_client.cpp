#include "server/service/lsp_client.h"

#include <algorithm>
#include <format>
#include <string>
#include <type_traits>
#include <variant>

#include "semantic/symbol_kind.h"
#include "server/protocol/extension.h"
#include "server/protocol/worker.h"
#include "server/service/master_server.h"
#include "support/filesystem.h"
#include "support/logging.h"

#include "kota/codec/json/json.h"
#include "kota/ipc/lsp/position.h"
#include "kota/ipc/lsp/protocol.h"
#include "kota/ipc/lsp/uri.h"
#include "kota/meta/enum.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/Process.h"

namespace clice {

namespace protocol = kota::ipc::protocol;
namespace lsp = kota::ipc::lsp;
namespace refl = kota::meta;
using kota::ipc::RequestResult;
using RequestContext = kota::ipc::JsonPeer::RequestContext;
using serde_raw = kota::codec::RawValue;

template <typename T>
static serde_raw to_raw(const T& value) {
    auto json = kota::codec::json::to_json<kota::ipc::lsp_config>(value);
    return serde_raw{json ? std::move(*json) : "null"};
}

LSPClient::LSPClient(MasterServer& server, kota::ipc::JsonPeer& peer) : server(server), peer(peer) {
    server.compiler.set_peer(&peer);
    server.indexer.set_peer(&peer);

    using StringVec = std::vector<std::string>;

    peer.on_request([this](RequestContext& ctx, const protocol::InitializeParams& params)
                        -> RequestResult<protocol::InitializeParams> {
        if(this->server.lifecycle != ServerLifecycle::Uninitialized) {
            co_return kota::outcome_error(protocol::Error{"Server already initialized"});
        }

        auto& init = params.lsp__initialize_params;
        if(init.root_uri.has_value()) {
            this->server.workspace_root = uri_to_path(*init.root_uri);
        }

        if(init.initialization_options.has_value()) {
            auto json =
                kota::codec::json::to_json<kota::ipc::lsp_config>(*init.initialization_options);
            if(json)
                this->server.init_options_json = std::move(*json);
        }

        this->server.lifecycle = ServerLifecycle::Initialized;
        LOG_INFO("Initialized with workspace: {}", this->server.workspace_root);

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
        auto& srv = this->server;

        srv.workspace.config = Config::load_from_workspace(srv.workspace_root);
        if(!srv.init_options_json.empty()) {
            if(auto ov = kota::codec::json::parse(srv.init_options_json, srv.workspace.config);
               !ov) {
                LOG_WARN("Failed to apply initializationOptions: {}", ov.error().to_string());
            } else {
                srv.workspace.config.apply_defaults(srv.workspace_root);
                LOG_INFO("Applied initializationOptions overlay");
            }
            srv.init_options_json.clear();
        }

        auto& cfg = srv.workspace.config.project;

        if(!cfg.logging_dir.empty()) {
            auto now = std::chrono::system_clock::now();
            auto pid = llvm::sys::Process::getProcessId();
            auto session_dir =
                path::join(cfg.logging_dir, std::format("{:%Y-%m-%d_%H-%M-%S}_{}", now, pid));
            logging::file_logger("master", session_dir, logging::options);
            srv.session_log_dir = session_dir;
        }

        LOG_INFO("Server ready (stateful={}, stateless={}, idle={}ms)",
                 cfg.stateful_worker_count.value,
                 cfg.stateless_worker_count.value,
                 *cfg.idle_timeout_ms);

        WorkerPoolOptions pool_opts;
        pool_opts.self_path = srv.self_path;
        pool_opts.stateful_count = cfg.stateful_worker_count;
        pool_opts.stateless_count = cfg.stateless_worker_count;
        pool_opts.worker_memory_limit = cfg.worker_memory_limit;
        pool_opts.log_dir = srv.session_log_dir;
        if(!srv.pool.start(pool_opts)) {
            LOG_ERROR("Failed to start worker pool");
            return;
        }

        srv.lifecycle = ServerLifecycle::Ready;

        srv.compiler.on_indexing_needed = [&srv]() {
            srv.indexer.schedule();
        };

        srv.indexer.set_peer(&this->peer);
        srv.indexer.set_max_concurrency(cfg.stateless_worker_count.value);

        srv.load_workspace();
    });

    peer.on_request(
        [this](RequestContext& ctx,
               const protocol::ShutdownParams& params) -> RequestResult<protocol::ShutdownParams> {
            this->server.lifecycle = ServerLifecycle::ShuttingDown;
            LOG_INFO("Shutdown requested");
            co_return nullptr;
        });

    peer.on_notification([srv = &this->server](const protocol::ExitParams& params) {
        srv->lifecycle = ServerLifecycle::Exited;
        LOG_INFO("Exit notification received");

        srv->indexer.save(srv->workspace.config.project.index_dir);
        srv->workspace.save_cache();

        srv->loop.schedule([](MasterServer* s) -> kota::task<> {
            co_await s->pool.stop();
            s->loop.stop();
        }(srv));
    });

    peer.on_notification([this](const protocol::DidOpenTextDocumentParams& params) {
        auto& srv = this->server;
        if(srv.lifecycle != ServerLifecycle::Ready)
            return;

        auto path = uri_to_path(params.text_document.uri);
        auto path_id = srv.workspace.path_pool.intern(path);

        auto [it, inserted] = srv.sessions.try_emplace(path_id);
        auto& session = it->second;
        if(!inserted) {
            session = Session{};
        }
        session.path_id = path_id;
        session.version = params.text_document.version;
        session.text = params.text_document.text;
        session.generation++;

        LOG_DEBUG("didOpen: {} (v{})", path, params.text_document.version);
    });

    peer.on_notification([this](const protocol::DidChangeTextDocumentParams& params) {
        auto& srv = this->server;
        if(srv.lifecycle != ServerLifecycle::Ready)
            return;

        auto path = uri_to_path(params.text_document.uri);
        auto path_id = srv.workspace.path_pool.intern(path);

        auto it = srv.sessions.find(path_id);
        if(it == srv.sessions.end())
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
        srv.pool.notify_stateful(path_id, update);
    });

    peer.on_notification([this](const protocol::DidCloseTextDocumentParams& params) {
        auto& srv = this->server;
        if(srv.lifecycle != ServerLifecycle::Ready)
            return;

        auto path = uri_to_path(params.text_document.uri);
        auto path_id = srv.workspace.path_pool.intern(path);

        srv.workspace.on_file_closed(path_id);
        srv.pool.notify_stateful(path_id, worker::EvictParams{path});

        protocol::PublishDiagnosticsParams diag_params;
        diag_params.uri = params.text_document.uri;
        this->peer.send_notification(diag_params);

        srv.sessions.erase(path_id);

        srv.indexer.enqueue(path_id);
        srv.indexer.schedule();

        LOG_DEBUG("didClose: {}", path);
    });

    peer.on_notification([this](const protocol::DidSaveTextDocumentParams& params) {
        auto& srv = this->server;
        if(srv.lifecycle != ServerLifecycle::Ready)
            return;

        auto path = uri_to_path(params.text_document.uri);
        auto path_id = srv.workspace.path_pool.intern(path);

        auto dirtied = srv.workspace.on_file_saved(path_id);
        for(auto dirty_id: dirtied) {
            if(auto sit = srv.sessions.find(dirty_id); sit != srv.sessions.end()) {
                sit->second.ast_dirty = true;
            } else {
                srv.indexer.enqueue(dirty_id);
            }
        }

        for(auto& [hdr_id, session]: srv.sessions) {
            if(session.header_context && session.header_context->host_path_id == path_id) {
                session.header_context.reset();
                session.ast_dirty = true;
            }
        }

        srv.indexer.schedule();

        LOG_DEBUG("didSave: {}", path);
    });

    peer.on_request([this](RequestContext& ctx, const protocol::HoverParams& params) -> RawResult {
        auto& srv = this->server;
        auto path = uri_to_path(params.text_document_position_params.text_document.uri);
        auto path_id = srv.workspace.path_pool.intern(path);
        auto sit = srv.sessions.find(path_id);
        if(sit == srv.sessions.end())
            co_return serde_raw{"null"};
        co_return co_await srv.compiler.forward_query(
            worker::QueryKind::Hover,
            sit->second,
            params.text_document_position_params.position);
    });

    peer.on_request(
        [this](RequestContext& ctx, const protocol::SemanticTokensParams& params) -> RawResult {
            auto& srv = this->server;
            auto path = uri_to_path(params.text_document.uri);
            auto path_id = srv.workspace.path_pool.intern(path);
            auto sit = srv.sessions.find(path_id);
            if(sit == srv.sessions.end())
                co_return serde_raw{"null"};
            co_return co_await srv.compiler.forward_query(worker::QueryKind::SemanticTokens,
                                                          sit->second);
        });

    peer.on_request(
        [this](RequestContext& ctx, const protocol::InlayHintParams& params) -> RawResult {
            auto& srv = this->server;
            auto path = uri_to_path(params.text_document.uri);
            auto path_id = srv.workspace.path_pool.intern(path);
            auto sit = srv.sessions.find(path_id);
            if(sit == srv.sessions.end())
                co_return serde_raw{"null"};
            co_return co_await srv.compiler.forward_query(worker::QueryKind::InlayHints,
                                                          sit->second,
                                                          {},
                                                          params.range);
        });

    peer.on_request([this](RequestContext& ctx,
                           const protocol::FoldingRangeParams& params) -> RawResult {
        auto& srv = this->server;
        auto path = uri_to_path(params.text_document.uri);
        auto path_id = srv.workspace.path_pool.intern(path);
        auto sit = srv.sessions.find(path_id);
        if(sit == srv.sessions.end())
            co_return serde_raw{"null"};
        co_return co_await srv.compiler.forward_query(worker::QueryKind::FoldingRange, sit->second);
    });

    peer.on_request(
        [this](RequestContext& ctx, const protocol::DocumentSymbolParams& params) -> RawResult {
            auto& srv = this->server;
            auto path = uri_to_path(params.text_document.uri);
            auto path_id = srv.workspace.path_pool.intern(path);
            auto sit = srv.sessions.find(path_id);
            if(sit == srv.sessions.end())
                co_return serde_raw{"null"};
            co_return co_await srv.compiler.forward_query(worker::QueryKind::DocumentSymbol,
                                                          sit->second);
        });

    peer.on_request([this](RequestContext& ctx,
                           const protocol::DocumentLinkParams& params) -> RawResult {
        auto& srv = this->server;
        auto path = uri_to_path(params.text_document.uri);
        auto path_id = srv.workspace.path_pool.intern(path);
        auto sit = srv.sessions.find(path_id);
        if(sit == srv.sessions.end())
            co_return serde_raw{"null"};
        auto& session = sit->second;
        auto result = co_await srv.compiler.forward_query(worker::QueryKind::DocumentLink, session);
        if(!result.has_value())
            co_return serde_raw{"null"};
        auto& links = result.value();
        auto sit2 = srv.sessions.find(path_id);
        if(sit2 != srv.sessions.end() && sit2->second.pch_ref) {
            auto pch_it = srv.workspace.pch_cache.find(sit2->second.pch_ref->path_id);
            if(pch_it != srv.workspace.pch_cache.end() &&
               !pch_it->second.document_links_json.empty()) {
                auto& pch_json = pch_it->second.document_links_json;
                if(!links.data.empty() && links.data != "null" && links.data.size() > 2) {
                    links.data.pop_back();
                    links.data += ',';
                    links.data.append(pch_json.begin() + 1, pch_json.end());
                } else {
                    links.data = pch_json;
                }
            }
        }
        co_return std::move(links);
    });

    peer.on_request([this](RequestContext& ctx,
                           const protocol::CodeActionParams& params) -> RawResult {
        auto& srv = this->server;
        auto path = uri_to_path(params.text_document.uri);
        auto path_id = srv.workspace.path_pool.intern(path);
        auto sit = srv.sessions.find(path_id);
        if(sit == srv.sessions.end())
            co_return serde_raw{"null"};
        co_return co_await srv.compiler.forward_query(worker::QueryKind::CodeAction, sit->second);
    });

    auto resolve_uri = [this](const std::string& uri) {
        struct Result {
            std::string path;
            std::uint32_t path_id;
            Session* session;
        };
        auto path = uri_to_path(uri);
        auto path_id = this->server.workspace.path_pool.intern(path);
        auto sit = this->server.sessions.find(path_id);
        Session* session = (sit != this->server.sessions.end()) ? &sit->second : nullptr;
        return Result{std::move(path), path_id, session};
    };

    auto lookup_at = [this, resolve_uri](const std::string& uri, const protocol::Position& pos) {
        auto [path, path_id, session] = resolve_uri(uri);
        return this->server.indexer.lookup_symbol(uri, path, pos, session);
    };

    auto query_at = [this, resolve_uri](const std::string& uri,
                                        const protocol::Position& pos,
                                        RelationKind kind) -> std::vector<protocol::Location> {
        auto [path, path_id, session] = resolve_uri(uri);
        return this->server.indexer.query_relations(path, pos, kind, session);
    };

    auto resolve_item =
        [this,
         resolve_uri](const std::string& uri,
                      const protocol::Range& range,
                      const std::optional<protocol::LSPAny>& data) -> std::optional<SymbolInfo> {
        auto [path, path_id, session] = resolve_uri(uri);
        return this->server.indexer.resolve_hierarchy_item(uri, path, range, data, session);
    };

    peer.on_request([this, query_at](RequestContext& ctx,
                                     const protocol::DefinitionParams& params) -> RawResult {
        auto& uri = params.text_document_position_params.text_document.uri;
        auto& pos = params.text_document_position_params.position;

        auto result = query_at(uri, pos, RelationKind::Definition);
        if(!result.empty()) {
            co_return to_raw(result);
        }

        auto& srv = this->server;
        auto path = uri_to_path(uri);
        auto path_id = srv.workspace.path_pool.intern(path);
        auto sit = srv.sessions.find(path_id);
        if(sit == srv.sessions.end())
            co_return serde_raw{"null"};
        co_return co_await srv.compiler.forward_query(worker::QueryKind::GoToDefinition,
                                                      sit->second,
                                                      pos);
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

    peer.on_request([this](RequestContext& ctx,
                           const protocol::CompletionParams& params) -> RawResult {
        auto& srv = this->server;
        auto path = uri_to_path(params.text_document_position_params.text_document.uri);
        auto path_id = srv.workspace.path_pool.intern(path);
        auto sit = srv.sessions.find(path_id);
        if(sit == srv.sessions.end())
            co_return serde_raw{"null"};
        auto pause = srv.indexer.scoped_pause();
        auto result =
            co_await srv.compiler.handle_completion(params.text_document_position_params.position,
                                                    sit->second);
        co_return std::move(result);
    });

    peer.on_request(
        [this](RequestContext& ctx, const protocol::SignatureHelpParams& params) -> RawResult {
            auto& srv = this->server;
            auto path = uri_to_path(params.text_document_position_params.text_document.uri);
            auto path_id = srv.workspace.path_pool.intern(path);
            auto sit = srv.sessions.find(path_id);
            if(sit == srv.sessions.end())
                co_return serde_raw{"null"};
            auto pause = srv.indexer.scoped_pause();
            auto result =
                co_await srv.compiler.forward_build(worker::BuildKind::SignatureHelp,
                                                    params.text_document_position_params.position,
                                                    sit->second);
            co_return std::move(result);
        });

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
        auto results = this->server.indexer.find_incoming_calls(info->hash);
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
        auto results = this->server.indexer.find_outgoing_calls(info->hash);
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
            auto results = this->server.indexer.find_supertypes(info->hash);
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
            auto results = this->server.indexer.find_subtypes(info->hash);
            if(results.empty())
                co_return serde_raw{"null"};
            co_return to_raw(results);
        });

    peer.on_request(
        [this](RequestContext& ctx, const protocol::WorkspaceSymbolParams& params) -> RawResult {
            auto results = this->server.indexer.search_symbols(params.query);
            if(results.empty())
                co_return serde_raw{"null"};
            co_return to_raw(results);
        });

    peer.on_request(
        "clice/queryContext",
        [this](RequestContext& ctx, const ext::QueryContextParams& params) -> RawResult {
            auto& srv = this->server;
            auto path = uri_to_path(params.uri);
            auto path_id = srv.workspace.path_pool.intern(path);
            int offset_val = std::max(0, params.offset.value_or(0));
            constexpr int page_size = 10;

            ext::QueryContextResult result;
            std::vector<ext::ContextItem> all_items;

            auto hosts = srv.workspace.dep_graph.find_host_sources(path_id);
            for(auto host_id: hosts) {
                auto host_path = srv.workspace.path_pool.resolve(host_id);
                auto host_cdb = srv.workspace.cdb.lookup(host_path, {.suppress_logging = true});
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
                auto entries = srv.workspace.cdb.lookup(path, {.suppress_logging = true});
                for(std::size_t i = 0; i < entries.size(); ++i) {
                    auto& cmd = entries[i];
                    auto argv = cmd.to_argv();
                    std::string desc;
                    for(std::size_t j = 0; j < argv.size(); ++j) {
                        llvm::StringRef a(argv[j]);
                        if(a.starts_with("-D") || a.starts_with("-O") || a.starts_with("-std=") ||
                           a.starts_with("-g")) {
                            if(!desc.empty())
                                desc += ' ';
                            desc += argv[j];
                            if((a == "-D" || a == "-O") && j + 1 < argv.size()) {
                                desc += argv[++j];
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
                    item.description = cmd.resolved.directory.str();
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
            auto& srv = this->server;
            auto path = uri_to_path(params.uri);
            auto path_id = srv.workspace.path_pool.intern(path);

            ext::CurrentContextResult result;
            auto sit = srv.sessions.find(path_id);
            if(sit != srv.sessions.end() && sit->second.active_context) {
                auto ctx_path = srv.workspace.path_pool.resolve(*sit->second.active_context);
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
            auto& srv = this->server;
            auto path = uri_to_path(params.uri);
            auto path_id = srv.workspace.path_pool.intern(path);
            auto context_path = uri_to_path(params.context_uri);
            auto context_path_id = srv.workspace.path_pool.intern(context_path);

            ext::SwitchContextResult result;

            auto context_cdb = srv.workspace.cdb.lookup(context_path, {.suppress_logging = true});
            if(context_cdb.empty()) {
                result.success = false;
                co_return to_raw(result);
            }

            auto sit = srv.sessions.find(path_id);
            if(sit == srv.sessions.end()) {
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

LSPClient::~LSPClient() {
    server.compiler.set_peer(nullptr);
    server.indexer.set_peer(nullptr);
}

}  // namespace clice
