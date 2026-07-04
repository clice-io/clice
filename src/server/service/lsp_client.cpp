#include "server/service/lsp_client.h"

#include <algorithm>
#include <chrono>
#include <format>
#include <string>
#include <type_traits>
#include <variant>

#include "command/argument_parser.h"
#include "semantic/symbol_kind.h"
#include "server/context/context_resolver.h"
#include "server/protocol/extension.h"
#include "server/protocol/serialize.h"
#include "server/protocol/worker.h"
#include "server/service/master_server.h"
#include "support/anomaly.h"
#include "support/filesystem.h"
#include "support/logging.h"
#include "syntax/preamble_synthesis.h"

#include "kota/codec/json/json.h"
#include "kota/ipc/lsp/position.h"
#include "kota/ipc/lsp/protocol.h"
#include "kota/ipc/lsp/uri.h"
#include "kota/meta/enum.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Process.h"

namespace clice {

namespace protocol = kota::ipc::protocol;
namespace lsp = kota::ipc::lsp;
namespace refl = kota::meta;
using kota::ipc::RequestResult;
using RequestContext = kota::ipc::JsonPeer::RequestContext;
using serde_raw = kota::codec::RawValue;

/// Error response for feature requests on files with no open session.
static kota::ipc::Error document_not_open() {
    return kota::ipc::Error{kota::ipc::protocol::ErrorCode::InvalidParams, "Document not open"};
}

/// Error response when a call/type hierarchy item cannot be resolved back to
/// an indexed symbol.
static kota::ipc::Error item_not_resolved(llvm::StringRef kind) {
    return kota::ipc::Error{kota::ipc::protocol::ErrorCode::InvalidParams,
                            std::format("Failed to resolve {} item", kind)};
}

LSPClient::LSPClient(MasterServer& server, kota::ipc::JsonPeer& peer) : server(server), peer(peer) {
    output_conn = server.compiler.on_output.connect(
        [this](const std::shared_ptr<Session>& session) { push_output(*session); });
    progress_conn = server.background_indexer.on_progress_changed.connect(
        [this]() { report_index_progress(); });

    // The notify hook is process-wide and forwards anomaly/guidance messages
    // as window/logMessage notifications. It captures the peer, so it must
    // live exactly as long as this LSPClient (cleared in the destructor).
    // TODO: materialize these messages into server state and deliver them
    // through a typed signal instead of a process-wide hook.
    logging::set_notify_hook([&peer](logging::NotifyLevel level, std::string_view message) {
        peer.send_notification(protocol::LogMessageParams{
            static_cast<protocol::MessageType>(level),
            std::string(message),
        });
    });

    using StringVec = std::vector<std::string>;

    // Shared front half of every document-addressed handler: URI → path →
    // interned path_id → open session (null when the document is not open).
    auto resolve_uri = [this](const std::string& uri) {
        struct Result {
            std::string path;
            std::uint32_t path_id;
            std::shared_ptr<Session> session;
        };
        auto path = uri_to_path(uri);
        auto path_id = this->server.workspace.path_pool.intern(path);
        return Result{std::move(path), path_id, this->server.find_session(path_id)};
    };

    peer.on_request([this](RequestContext& ctx, const protocol::InitializeParams& params)
                        -> RequestResult<protocol::InitializeParams> {
        auto& srv = this->server;
        if(srv.lifecycle != ServerLifecycle::Uninitialized) {
            co_return kota::outcome_error(protocol::Error{"Server already initialized"});
        }

        auto& init = params.lsp__initialize_params;
        if(init.root_uri.has_value()) {
            srv.workspace_root = uri_to_path(*init.root_uri);
        }

        if(init.initialization_options.has_value()) {
            auto json =
                kota::codec::json::to_json<kota::ipc::lsp_config>(*init.initialization_options);
            if(json)
                srv.init_options_json = std::move(*json);
        }

        srv.lifecycle = ServerLifecycle::Initialized;
        LOG_INFO("Initialized with workspace: {}", srv.workspace_root);

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
        caps.folding_range_provider = true;
        caps.inlay_hint_provider = true;
        caps.call_hierarchy_provider = true;
        caps.type_hierarchy_provider = true;
        caps.workspace_symbol_provider = true;
        caps.document_formatting_provider = true;
        caps.document_range_formatting_provider = true;

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

    peer.on_notification([this]([[maybe_unused]] const protocol::InitializedParams& params) {
        this->server.initialize();
        this->publish_config_diagnostics();
    });

    peer.on_request(
        [this](RequestContext& ctx,
               const protocol::ShutdownParams& params) -> RequestResult<protocol::ShutdownParams> {
            this->server.lifecycle = ServerLifecycle::ShuttingDown;
            LOG_INFO("Shutdown requested");
            co_return nullptr;
        });

    peer.on_notification([this]([[maybe_unused]] const protocol::ExitParams& params) {
        LOG_INFO("Exit notification received");
        this->server.schedule_shutdown();
    });

    peer.on_notification([this, resolve_uri](const protocol::DidOpenTextDocumentParams& params) {
        auto& srv = this->server;
        if(srv.lifecycle != ServerLifecycle::Ready)
            return;

        auto [path, path_id, session] = resolve_uri(params.text_document.uri);
        session = srv.open_session(path_id);
        srv.sessions.apply_open(*session, params.text_document.text, params.text_document.version);

        // Restore a context choice persisted from an earlier session.
        srv.contexts.restore_saved_context(*session);

        LOG_DEBUG("didOpen: {} (v{})", path, params.text_document.version);
    });

    peer.on_notification([this, resolve_uri](const protocol::DidChangeTextDocumentParams& params) {
        auto& srv = this->server;
        if(srv.lifecycle != ServerLifecycle::Ready)
            return;

        auto [path, path_id, session] = resolve_uri(params.text_document.uri);
        if(!session)
            return;

        srv.sessions.apply_change(*session, params.content_changes, params.text_document.version);

        LOG_DEBUG("didChange: path={} version={} gen={}",
                  path,
                  session->version,
                  session->generation);
    });

    peer.on_notification([this, resolve_uri](const protocol::DidCloseTextDocumentParams& params) {
        auto& srv = this->server;
        if(srv.lifecycle != ServerLifecycle::Ready)
            return;

        auto [path, path_id, session] = resolve_uri(params.text_document.uri);
        srv.close_session(path_id, this->peer);
    });

    peer.on_notification([this, resolve_uri](const protocol::DidSaveTextDocumentParams& params) {
        auto& srv = this->server;
        if(srv.lifecycle != ServerLifecycle::Ready)
            return;

        auto [path, path_id, session] = resolve_uri(params.text_document.uri);
        srv.on_file_saved(path_id);

        LOG_DEBUG("didSave: {}", path);
    });

    peer.on_request(
        [this, resolve_uri](RequestContext& ctx, const protocol::HoverParams& params) -> RawResult {
            auto& srv = this->server;
            auto [path, path_id, session] =
                resolve_uri(params.text_document_position_params.text_document.uri);
            if(!session)
                co_return kota::outcome_error(document_not_open());
            co_return co_await srv.compiler.forward_query(
                worker::QueryKind::Hover,
                session,
                params.text_document_position_params.position);
        });

    peer.on_request([this, resolve_uri](RequestContext& ctx,
                                        const protocol::SemanticTokensParams& params) -> RawResult {
        auto& srv = this->server;
        auto [path, path_id, session] = resolve_uri(params.text_document.uri);
        if(!session)
            co_return kota::outcome_error(document_not_open());
        co_return co_await srv.compiler.forward_query(worker::QueryKind::SemanticTokens, session);
    });

    peer.on_request([this, resolve_uri](RequestContext& ctx,
                                        const protocol::InlayHintParams& params) -> RawResult {
        auto& srv = this->server;
        auto [path, path_id, session] = resolve_uri(params.text_document.uri);
        if(!session)
            co_return kota::outcome_error(document_not_open());
        co_return co_await srv.compiler.forward_query(worker::QueryKind::InlayHints,
                                                      session,
                                                      {},
                                                      params.range);
    });

    peer.on_request([this, resolve_uri](RequestContext& ctx,
                                        const protocol::FoldingRangeParams& params) -> RawResult {
        auto& srv = this->server;
        auto [path, path_id, session] = resolve_uri(params.text_document.uri);
        if(!session)
            co_return kota::outcome_error(document_not_open());
        co_return co_await srv.compiler.forward_query(worker::QueryKind::FoldingRange, session);
    });

    peer.on_request([this, resolve_uri](RequestContext& ctx,
                                        const protocol::DocumentSymbolParams& params) -> RawResult {
        auto& srv = this->server;
        auto [path, path_id, session] = resolve_uri(params.text_document.uri);
        if(!session)
            co_return kota::outcome_error(document_not_open());
        co_return co_await srv.compiler.forward_query(worker::QueryKind::DocumentSymbol, session);
    });

    peer.on_request([this, resolve_uri](RequestContext& ctx,
                                        const protocol::DocumentLinkParams& params) -> RawResult {
        auto& srv = this->server;
        auto [path, path_id, session] = resolve_uri(params.text_document.uri);
        if(!session)
            co_return kota::outcome_error(document_not_open());
        auto result = co_await srv.compiler.forward_document_links(session);
        if(!result.has_value())
            co_return kota::outcome_error(std::move(result.error()));

        // The preamble is compiled into the PCH, so the worker's AST only
        // covers the rest of the file — merge the preamble's links in front.
        std::vector<protocol::DocumentLink> links;
        auto append = [&](const feature::DocumentLink& link) {
            protocol::DocumentLink out{.range = link.range};
            out.target = link.target;
            links.push_back(std::move(out));
        };
        // Skipped while dirty: a failed or superseded compile leaves
        // the cached links describing the pre-edit preamble.
        if(!session->ast_dirty) {
            if(auto* pch_links = srv.find_preamble_links(*session)) {
                std::ranges::for_each(*pch_links, append);
            }
        }
        std::ranges::for_each(result.value(), append);
        co_return to_raw(links);
    });

    peer.on_request([this, resolve_uri](RequestContext& ctx,
                                        const protocol::CodeActionParams& params) -> RawResult {
        auto& srv = this->server;
        auto [path, path_id, session] = resolve_uri(params.text_document.uri);
        if(!session)
            co_return kota::outcome_error(document_not_open());
        co_return co_await srv.compiler.forward_query(worker::QueryKind::CodeAction, session);
    });

    auto lookup_at = [this, resolve_uri](const std::string& uri, const protocol::Position& pos) {
        auto [path, path_id, session] = resolve_uri(uri);
        return this->server.index_query.lookup_symbol(uri, path, pos, session.get());
    };

    auto query_at = [this, resolve_uri](const std::string& uri,
                                        const protocol::Position& pos,
                                        RelationKind kind) -> std::vector<protocol::Location> {
        auto [path, path_id, session] = resolve_uri(uri);
        return this->server.index_query.query_relations(path, pos, kind, session.get());
    };

    auto query_targets_at = [this,
                             resolve_uri](const std::string& uri,
                                          const protocol::Position& pos,
                                          RelationKind kind) -> std::vector<protocol::Location> {
        auto [path, path_id, session] = resolve_uri(uri);
        return this->server.index_query.query_symbol_targets(path, pos, kind, session.get());
    };

    auto resolve_item =
        [this,
         resolve_uri](const std::string& uri,
                      const protocol::Range& range,
                      const std::optional<protocol::LSPAny>& data) -> std::optional<SymbolInfo> {
        auto [path, path_id, session] = resolve_uri(uri);
        return this->server.index_query.resolve_hierarchy_item(uri,
                                                               path,
                                                               range,
                                                               data,
                                                               session.get());
    };

    peer.on_request([this, resolve_uri, query_at](
                        RequestContext& ctx,
                        const protocol::DefinitionParams& params) -> RawResult {
        auto& uri = params.text_document_position_params.text_document.uri;
        auto& pos = params.text_document_position_params.position;

        auto& srv = this->server;
        auto [path, path_id, session] = resolve_uri(uri);

        // Preamble include lines first: they have no symbol occurrence in
        // the index and are invisible to the worker's AST. Dirty sessions
        // skip this — the cached links may describe the pre-edit preamble —
        // and retry below once the worker compile refreshed the PCH.
        if(session && !session->ast_dirty) {
            if(auto directive = srv.resolve_directive_definition(*session, pos);
               !directive.empty()) {
                co_return to_raw(directive);
            }
        }

        // Dirty sessions also skip the eager index query: resolve_cursor
        // would fall back to the stale merged shard and could return a
        // non-empty hit for pre-edit content, bypassing the compile below.
        if(!session || !session->ast_dirty) {
            auto result = query_at(uri, pos, RelationKind::Definition);
            if(!result.empty()) {
                co_return to_raw(result);
            }
        }

        if(!session)
            co_return kota::outcome_error(document_not_open());
        auto raw =
            co_await srv.compiler.forward_query(worker::QueryKind::GoToDefinition, session, pos);
        if(raw.has_value() && raw.value().data != "[]" && raw.value().data != "null") {
            co_return std::move(raw.value());
        }

        // The forward compiled a dirty buffer: retry against the refreshed
        // session index and preamble links, but only when the compile
        // actually completed — a failed or superseded compile leaves
        // ast_dirty set and the caches stale.
        if(!session->ast_dirty) {
            if(auto retry = query_at(uri, pos, RelationKind::Definition); !retry.empty()) {
                co_return to_raw(retry);
            }
            if(auto directive = srv.resolve_directive_definition(*session, pos);
               !directive.empty()) {
                co_return to_raw(directive);
            }
        }
        co_return std::move(raw);
    });

    // The navigation handlers below are index-only: closed documents are
    // fully serveable from the index, and an empty result is a real answer,
    // returned as [] — never an error.
    peer.on_request(
        [query_at](RequestContext& ctx, const protocol::ReferenceParams& params) -> RawResult {
            auto& uri = params.text_document_position_params.text_document.uri;
            auto& pos = params.text_document_position_params.position;

            auto locations = query_at(uri, pos, RelationKind::Reference);

            if(params.context.include_declaration) {
                for(auto kind: {RelationKind::Declaration, RelationKind::Definition}) {
                    auto extra = query_at(uri, pos, kind);
                    locations.insert(locations.end(),
                                     std::make_move_iterator(extra.begin()),
                                     std::make_move_iterator(extra.end()));
                }
            }

            co_return to_raw(locations);
        });

    peer.on_request([query_targets_at](RequestContext& ctx,
                                       const protocol::TypeDefinitionParams& params) -> RawResult {
        auto& uri = params.text_document_position_params.text_document.uri;
        auto& pos = params.text_document_position_params.position;
        co_return to_raw(query_targets_at(uri, pos, RelationKind::TypeDefinition));
    });

    peer.on_request([query_targets_at](RequestContext& ctx,
                                       const protocol::ImplementationParams& params) -> RawResult {
        auto& uri = params.text_document_position_params.text_document.uri;
        auto& pos = params.text_document_position_params.position;
        co_return to_raw(query_targets_at(uri, pos, RelationKind::Implementation));
    });

    // Declarations plus the definition: symbols defined inline have no
    // separate Declaration relation, and navigating to the definition is
    // what every client expects in that case.
    peer.on_request(
        [query_at](RequestContext& ctx, const protocol::DeclarationParams& params) -> RawResult {
            auto& uri = params.text_document_position_params.text_document.uri;
            auto& pos = params.text_document_position_params.position;
            auto locations = query_at(uri, pos, RelationKind::Declaration);
            auto defs = query_at(uri, pos, RelationKind::Definition);
            locations.insert(locations.end(),
                             std::make_move_iterator(defs.begin()),
                             std::make_move_iterator(defs.end()));
            co_return to_raw(locations);
        });

    peer.on_request([this, resolve_uri](RequestContext& ctx,
                                        const protocol::CompletionParams& params) -> RawResult {
        auto& srv = this->server;
        auto [path, path_id, session] =
            resolve_uri(params.text_document_position_params.text_document.uri);
        if(!session)
            co_return kota::outcome_error(document_not_open());
        auto pause = srv.background_indexer.scoped_pause();
        auto result =
            co_await srv.compiler.handle_completion(params.text_document_position_params.position,
                                                    session);
        co_return std::move(result);
    });

    peer.on_request([this, resolve_uri](RequestContext& ctx,
                                        const protocol::SignatureHelpParams& params) -> RawResult {
        auto& srv = this->server;
        auto [path, path_id, session] =
            resolve_uri(params.text_document_position_params.text_document.uri);
        if(!session)
            co_return kota::outcome_error(document_not_open());
        auto pause = srv.background_indexer.scoped_pause();
        auto result =
            co_await srv.compiler.forward_build(worker::BuildKind::SignatureHelp,
                                                params.text_document_position_params.position,
                                                session);
        co_return std::move(result);
    });

    peer.on_request(
        [this, resolve_uri](RequestContext& ctx,
                            const protocol::DocumentFormattingParams& params) -> RawResult {
            auto& srv = this->server;
            auto [path, path_id, session] = resolve_uri(params.text_document.uri);
            if(!session)
                co_return kota::outcome_error(document_not_open());
            auto pause = srv.background_indexer.scoped_pause();
            co_return co_await srv.compiler.forward_format(session);
        });

    peer.on_request(
        [this, resolve_uri](RequestContext& ctx,
                            const protocol::DocumentRangeFormattingParams& params) -> RawResult {
            auto& srv = this->server;
            auto [path, path_id, session] = resolve_uri(params.text_document.uri);
            if(!session)
                co_return kota::outcome_error(document_not_open());
            auto pause = srv.background_indexer.scoped_pause();
            co_return co_await srv.compiler.forward_format(session, params.range);
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
            items.push_back(IndexQuery::build_call_hierarchy_item(*info));
            co_return to_raw(items);
        });

    peer.on_request([this, resolve_item](
                        RequestContext& ctx,
                        const protocol::CallHierarchyIncomingCallsParams& params) -> RawResult {
        auto info = resolve_item(params.item.uri, params.item.range, params.item.data);
        if(!info)
            co_return kota::outcome_error(item_not_resolved("call hierarchy"));
        auto results = this->server.index_query.find_incoming_calls(info->hash);
        co_return to_raw(results);
    });

    peer.on_request([this, resolve_item](
                        RequestContext& ctx,
                        const protocol::CallHierarchyOutgoingCallsParams& params) -> RawResult {
        auto info = resolve_item(params.item.uri, params.item.range, params.item.data);
        if(!info)
            co_return kota::outcome_error(item_not_resolved("call hierarchy"));
        auto results = this->server.index_query.find_outgoing_calls(info->hash);
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
            items.push_back(IndexQuery::build_type_hierarchy_item(*info));
            co_return to_raw(items);
        });

    peer.on_request(
        [this, resolve_item](RequestContext& ctx,
                             const protocol::TypeHierarchySupertypesParams& params) -> RawResult {
            auto info = resolve_item(params.item.uri, params.item.range, params.item.data);
            if(!info)
                co_return kota::outcome_error(item_not_resolved("type hierarchy"));
            auto results = this->server.index_query.find_supertypes(info->hash);
            co_return to_raw(results);
        });

    peer.on_request(
        [this, resolve_item](RequestContext& ctx,
                             const protocol::TypeHierarchySubtypesParams& params) -> RawResult {
            auto info = resolve_item(params.item.uri, params.item.range, params.item.data);
            if(!info)
                co_return kota::outcome_error(item_not_resolved("type hierarchy"));
            auto results = this->server.index_query.find_subtypes(info->hash);
            co_return to_raw(results);
        });

    peer.on_request(
        [this](RequestContext& ctx, const protocol::WorkspaceSymbolParams& params) -> RawResult {
            auto results = this->server.index_query.search_symbols(params.query);
            co_return to_raw(results);
        });

    // ── Compilation context helpers ─────────────────────────────────

    peer.on_request("clice/queryContext",
                    [this, resolve_uri](RequestContext& ctx,
                                        const ext::QueryContextParams& params) -> RawResult {
                        auto [path, path_id, session] = resolve_uri(params.uri);
                        co_return to_raw(
                            this->server.contexts.query_contexts(path, path_id, params));
                    });

    peer.on_request("clice/currentContext",
                    [this, resolve_uri](RequestContext& ctx,
                                        const ext::CurrentContextParams& params) -> RawResult {
                        auto [path, path_id, session] = resolve_uri(params.uri);
                        co_return to_raw(
                            this->server.contexts.current_context(path, session.get(), params));
                    });

    peer.on_request("clice/switchContext",
                    [this, resolve_uri](RequestContext& ctx,
                                        const ext::SwitchContextParams& params) -> RawResult {
                        auto [path, path_id, session] = resolve_uri(params.uri);
                        auto [context_path, context_path_id, context_session] =
                            resolve_uri(params.context_uri);
                        co_return to_raw(this->server.contexts.switch_context(path,
                                                                              path_id,
                                                                              session.get(),
                                                                              context_path,
                                                                              context_path_id,
                                                                              params));
                    });
}

/// Publish clice.toml load problems as diagnostics, each on its own file's
/// URI (multiple files can contribute issues when the first config candidate
/// is malformed and the next one loads). The files are usually not open in
/// the editor — publishing to a closed file is fine, the client shows it in
/// the problems panel. The loaded file always gets a publish, so a clean
/// load clears diagnostics from a previous (broken) state.
void LSPClient::publish_config_diagnostics() {
    if(server.config_path.empty())
        return;

    llvm::StringMap<std::vector<protocol::Diagnostic>> by_file;
    // The loaded file always gets a publish (even with zero issues), so a
    // clean load clears diagnostics from a previous broken state.
    by_file.try_emplace(server.config_path);
    for(auto& issue: server.config_issues) {
        // rich_error positions are 1-based; LSP wants 0-based. An unknown
        // position (0) maps to the file top. The range spans a single
        // character — clients render it as the whole token anyway.
        auto line = issue.line > 0 ? issue.line - 1 : 0;
        auto character = issue.column > 0 ? issue.column - 1 : 0;

        protocol::Diagnostic diagnostic;
        diagnostic.range = protocol::Range{
            .start = protocol::Position{.line = line, .character = character    },
            .end = protocol::Position{.line = line, .character = character + 1},
        };
        diagnostic.severity = issue.severity == ConfigIssue::Severity::Error
                                  ? protocol::DiagnosticSeverity::Error
                                  : protocol::DiagnosticSeverity::Warning;
        diagnostic.source = "clice";
        diagnostic.message = issue.message;
        by_file[issue.file].push_back(std::move(diagnostic));

        LOG_GUIDANCE("Configuration problem in {}: {}", issue.file, issue.message);
    }

    for(auto& [file, diagnostics]: by_file) {
        auto uri = lsp::URI::from_file_path(file.str());
        if(!uri) {
            LOG_WARN("Cannot build URI for config file {}", file.str());
            continue;
        }
        protocol::PublishDiagnosticsParams params;
        params.uri = uri->str();
        params.diagnostics = std::move(diagnostics);
        peer.send_notification(params);
    }
}

void LSPClient::push_output(const Session& session) {
    if(!session.output.has_value()) {
        return;
    }
    auto& output = *session.output;

    auto file_path = std::string(server.workspace.path_pool.resolve(session.path_id));
    auto uri = lsp::URI::from_file_path(file_path);
    std::string uri_str = uri.has_value() ? uri->str() : file_path;

    protocol::PublishDiagnosticsParams params;
    params.uri = uri_str;
    params.version = output.version;
    params.diagnostics = format_diagnostics(output);
    peer.send_notification(params);

    // The clear path carries no inactive-regions update; a successful
    // compile always pushes the current regions (even when empty) so a
    // context switch immediately re-dims the regions selected away by
    // the new preprocessor state.
    if(output.inactive_regions.has_value()) {
        ext::InactiveRegionsParams regions;
        regions.uri = uri_str;
        regions.regions = format_inactive_regions(session, output);
        peer.send_notification("clice/inactiveRegions", regions);
    }
}

void LSPClient::report_index_progress() {
    const auto& p = server.background_indexer.progress();
    using Stage = BackgroundIndexer::Progress::Stage;
    switch(p.stage) {
        case Stage::Begin: {
            progress_round_active = true;
            progress_total = static_cast<std::uint32_t>(p.total);
            // Register a fresh work-done token; once the client acknowledges
            // it, announce the round. This is the create()+begin() handshake
            // the indexer used to run inline, now driven from the transport so
            // the indexer no longer needs a peer. The dispatch it once gated on
            // proceeds independently; reports before the token is announced are
            // dropped, so the first sub-second of a round may report fewer
            // increments than before. If a previous round's handshake is still
            // in flight, the token is reused: its continuation reconciles
            // against the current round below, so at most one create() is ever
            // outstanding and index_progress is never replaced mid-await.
            if(progress_create_inflight || progress_token_active) {
                break;
            }
            index_progress.emplace(peer,
                                   protocol::ProgressToken(std::string("clice/backgroundIndex")));
            progress_create_inflight = true;
            server.loop.schedule([this]() -> kota::task<> {
                // Timeout prevents the handshake from hanging when the client
                // never responds.
                auto create_result =
                    co_await index_progress->create({.timeout = std::chrono::milliseconds(3000)});
                progress_create_inflight = false;
                // The round may have ended while the handshake was in flight —
                // drop the token without announcing it.
                if(create_result.has_error() || !progress_round_active) {
                    index_progress.reset();
                    co_return;
                }
                index_progress->begin("Indexing", std::format("0/{} files", progress_total), 0);
                progress_token_active = true;
            }());
            break;
        }
        case Stage::Report: {
            if(progress_token_active) {
                auto pct =
                    p.total > 0 ? static_cast<std::uint32_t>(p.completed * 100 / p.total) : 100;
                index_progress->report(std::format("{}/{} files", p.completed, p.total), pct);
            }
            break;
        }
        case Stage::End: {
            progress_round_active = false;
            if(progress_token_active) {
                index_progress->end(std::format("Indexed {} files", p.dispatched));
                index_progress.reset();
                progress_token_active = false;
            }
            // With a handshake still in flight, its continuation sees the
            // round is gone and drops the token.
            break;
        }
    }
}

LSPClient::~LSPClient() {
    logging::set_notify_hook(nullptr);
}

}  // namespace clice
