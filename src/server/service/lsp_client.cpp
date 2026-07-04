#include "server/service/lsp_client.h"

#include <algorithm>
#include <format>
#include <string>
#include <type_traits>
#include <variant>

#include "command/argument_parser.h"
#include "semantic/symbol_kind.h"
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
#include "llvm/ADT/StringSet.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
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
    server.compiler.set_peer(&peer);
    server.indexer.set_peer(&peer);

    // The notify hook is process-wide and forwards anomaly/guidance messages
    // as window/logMessage notifications. It captures the peer, so it must
    // live exactly as long as this LSPClient (cleared in the destructor).
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
        session->version = params.text_document.version;
        session->text = params.text_document.text;
        session->line_starts = lsp::build_line_starts(session->text);

        // Restore a context choice persisted from an earlier session. The
        // CDB or include graph may have changed while the server was down —
        // validate before adopting, since a stale active_context suppresses
        // automatic host resolution and strands the file on the fallback
        // command.
        if(auto it = srv.workspace.saved_contexts.find(path_id);
           it != srv.workspace.saved_contexts.end()) {
            auto& ws = srv.workspace;
            auto& saved = it->second;
            auto entry_has_hash = [&ws](llvm::StringRef entry_path, llvm::StringRef hash) {
                std::vector<std::string> rule_append, rule_remove;
                ws.config.match_rules(entry_path, rule_append, rule_remove);
                for(auto& cmd:
                    ws.cdb.lookup(entry_path, {.remove = rule_remove, .append = rule_append})) {
                    if(canonical_command_hash(cmd.to_string_argv(), cmd.resolved.directory) ==
                       hash) {
                        return true;
                    }
                }
                return false;
            };

            bool valid = false;
            if(saved.host_path_id != no_path_id) {
                auto host_path = ws.path_pool.resolve(saved.host_path_id);
                valid =
                    ws.cdb.has_entry(host_path) &&
                    !ws.dep_graph.find_include_chain(saved.host_path_id, path_id).empty() &&
                    (saved.command_hash.empty() || entry_has_hash(host_path, saved.command_hash));
                if(valid) {
                    session->active_context = Session::ActiveContext{saved.host_path_id,
                                                                     saved.occurrence,
                                                                     saved.command_hash};
                }
            } else if(!saved.command_hash.empty()) {
                valid = ws.cdb.has_entry(path) && entry_has_hash(path, saved.command_hash);
                if(valid) {
                    session->active_command = saved.command_hash;
                }
            }
            if(!valid) {
                LOG_INFO("didOpen: dropping stale saved context for {}", path);
                srv.workspace.saved_contexts.erase(it);
            }
        }

        session->generation++;

        LOG_DEBUG("didOpen: {} (v{})", path, params.text_document.version);
    });

    peer.on_notification([this, resolve_uri](const protocol::DidChangeTextDocumentParams& params) {
        auto& srv = this->server;
        if(srv.lifecycle != ServerLifecycle::Ready)
            return;

        auto [path, path_id, session] = resolve_uri(params.text_document.uri);
        if(!session)
            return;

        session->version = params.text_document.version;

        for(auto& change: params.content_changes) {
            std::visit(
                [&](auto& c) {
                    using T = std::remove_cvref_t<decltype(c)>;
                    if constexpr(std::is_same_v<T,
                                                protocol::TextDocumentContentChangeWholeDocument>) {
                        session->text = c.text;
                    } else {
                        auto& range = c.range;
                        auto map = session->line_map();
                        auto start = map.to_offset(range.start);
                        auto end = map.to_offset(range.end);
                        if(start && end && *start <= *end) {
                            session->text.replace(*start, *end - *start, c.text);
                        }
                    }
                    session->line_starts = lsp::build_line_starts(session->text);
                },
                change);
        }

        session->generation++;
        session->ast_dirty = true;

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
        return this->server.indexer.lookup_symbol(uri, path, pos, session.get());
    };

    auto query_at = [this, resolve_uri](const std::string& uri,
                                        const protocol::Position& pos,
                                        RelationKind kind) -> std::vector<protocol::Location> {
        auto [path, path_id, session] = resolve_uri(uri);
        return this->server.indexer.query_relations(path, pos, kind, session.get());
    };

    auto query_targets_at = [this,
                             resolve_uri](const std::string& uri,
                                          const protocol::Position& pos,
                                          RelationKind kind) -> std::vector<protocol::Location> {
        auto [path, path_id, session] = resolve_uri(uri);
        return this->server.indexer.query_symbol_targets(path, pos, kind, session.get());
    };

    auto resolve_item =
        [this,
         resolve_uri](const std::string& uri,
                      const protocol::Range& range,
                      const std::optional<protocol::LSPAny>& data) -> std::optional<SymbolInfo> {
        auto [path, path_id, session] = resolve_uri(uri);
        return this->server.indexer.resolve_hierarchy_item(uri, path, range, data, session.get());
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
        auto pause = srv.indexer.scoped_pause();
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
        auto pause = srv.indexer.scoped_pause();
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
            auto pause = srv.indexer.scoped_pause();
            co_return co_await srv.compiler.forward_format(session);
        });

    peer.on_request(
        [this, resolve_uri](RequestContext& ctx,
                            const protocol::DocumentRangeFormattingParams& params) -> RawResult {
            auto& srv = this->server;
            auto [path, path_id, session] = resolve_uri(params.text_document.uri);
            if(!session)
                co_return kota::outcome_error(document_not_open());
            auto pause = srv.indexer.scoped_pause();
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
            items.push_back(Indexer::build_call_hierarchy_item(*info));
            co_return to_raw(items);
        });

    peer.on_request([this, resolve_item](
                        RequestContext& ctx,
                        const protocol::CallHierarchyIncomingCallsParams& params) -> RawResult {
        auto info = resolve_item(params.item.uri, params.item.range, params.item.data);
        if(!info)
            co_return kota::outcome_error(item_not_resolved("call hierarchy"));
        auto results = this->server.indexer.find_incoming_calls(info->hash);
        co_return to_raw(results);
    });

    peer.on_request([this, resolve_item](
                        RequestContext& ctx,
                        const protocol::CallHierarchyOutgoingCallsParams& params) -> RawResult {
        auto info = resolve_item(params.item.uri, params.item.range, params.item.data);
        if(!info)
            co_return kota::outcome_error(item_not_resolved("call hierarchy"));
        auto results = this->server.indexer.find_outgoing_calls(info->hash);
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
                co_return kota::outcome_error(item_not_resolved("type hierarchy"));
            auto results = this->server.indexer.find_supertypes(info->hash);
            co_return to_raw(results);
        });

    peer.on_request(
        [this, resolve_item](RequestContext& ctx,
                             const protocol::TypeHierarchySubtypesParams& params) -> RawResult {
            auto info = resolve_item(params.item.uri, params.item.range, params.item.data);
            if(!info)
                co_return kota::outcome_error(item_not_resolved("type hierarchy"));
            auto results = this->server.indexer.find_subtypes(info->hash);
            co_return to_raw(results);
        });

    peer.on_request(
        [this](RequestContext& ctx, const protocol::WorkspaceSymbolParams& params) -> RawResult {
            auto results = this->server.indexer.search_symbols(params.query);
            co_return to_raw(results);
        });

    // ── Compilation context helpers ─────────────────────────────────

    // Human-readable summary of the distinguishing flags of a command.
    auto flags_label = [](const CompileCommand& cmd) {
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
        return desc;
    };

    // How many includes of `target_id` the direct includer along the
    auto count_occurrences = [this](std::uint32_t host_id, std::uint32_t target_id) {
        return this->server.workspace.count_occurrences(host_id, target_id);
    };

    peer.on_request(
        "clice/queryContext",
        [this, resolve_uri, flags_label, count_occurrences](
            RequestContext& ctx,
            const ext::QueryContextParams& params) -> RawResult {
            auto& srv = this->server;
            auto& ws = srv.workspace;
            auto [path, path_id, session] = resolve_uri(params.uri);
            int offset_val = std::max(0, params.offset.value_or(0));
            constexpr int page_size = 10;

            ext::QueryContextResult result;
            std::vector<ext::ContextItem> all_items;

            // Contexts that would produce identical compilation results are
            // collapsed: identical canonical flags mean an identical compile
            // — but only for headers CONFIRMED self-contained. A header that
            // needs includer context gets a different synthesized prefix per
            // host, and an un-trialed header may turn out the same way, so
            // every host stays a distinct context for both.
            llvm::StringSet<> seen_configs;
            bool dedup_hosts = ws.header_mode(path, path_id) == HeaderMode::SelfContained;

            auto hosts = ws.dep_graph.find_host_sources(path_id);
            for(auto host_id: ws.rank_hosts(path_id, hosts)) {
                auto host_path = ws.path_pool.resolve(host_id);
                if(!ws.cdb.has_entry(host_path))
                    continue;
                auto host_uri_opt = lsp::URI::from_file_path(std::string(host_path));
                if(!host_uri_opt)
                    continue;

                // A multi-configuration host contributes one context per
                // CDB entry: each configuration compiles the header under
                // different preprocessor state.
                std::vector<std::string> host_append, host_remove;
                ws.config.match_rules(host_path, host_append, host_remove);
                auto cmds =
                    ws.cdb.lookup(host_path, {.remove = host_remove, .append = host_append});
                auto occurrences = count_occurrences(host_id, path_id);

                for(auto& cmd: cmds) {
                    auto hash =
                        canonical_command_hash(cmd.to_string_argv(), cmd.resolved.directory);
                    if(dedup_hosts && !seen_configs.insert(hash).second)
                        continue;

                    ext::ContextItem item;
                    item.label = llvm::sys::path::filename(host_path).str();
                    if(cmds.size() > 1) {
                        auto desc = flags_label(cmd);
                        if(!desc.empty()) {
                            item.label = std::format("{} [{}]", item.label, desc);
                        }
                        item.command_hash = hash;
                    }
                    item.description = std::string(host_path);
                    item.uri = host_uri_opt->str();

                    // A guard-less header can be included several times by
                    // one host — each occurrence is a distinct context.
                    if(occurrences > 1) {
                        for(std::uint32_t n = 0; n < occurrences; ++n) {
                            auto occ_item = item;
                            occ_item.label = std::format("{} (#{})", item.label, n + 1);
                            occ_item.occurrence = n;
                            all_items.push_back(std::move(occ_item));
                        }
                    } else {
                        all_items.push_back(std::move(item));
                    }
                }
            }

            // Real entries only: lookup() would synthesize a default command
            // even for unknown files, offering a bogus context that
            // switchContext would then reject. Offered even when hosts
            // exist, so a host override can be switched back to the file's
            // own command.
            if(ws.cdb.has_entry(path)) {
                std::vector<std::string> rule_append, rule_remove;
                ws.config.match_rules(path, rule_append, rule_remove);
                auto entries = ws.cdb.lookup(path, {.remove = rule_remove, .append = rule_append});
                auto uri_opt = lsp::URI::from_file_path(std::string(path));
                for(std::size_t i = 0; uri_opt && i < entries.size(); ++i) {
                    auto& cmd = entries[i];
                    auto hash =
                        canonical_command_hash(cmd.to_string_argv(), cmd.resolved.directory);
                    if(!seen_configs.insert(hash).second)
                        continue;

                    auto desc = flags_label(cmd);
                    ext::ContextItem item;
                    item.label = desc.empty() ? std::format("config #{}", i) : desc;
                    item.description = cmd.resolved.directory.str();
                    item.uri = uri_opt->str();
                    item.command_hash = std::move(hash);
                    all_items.push_back(std::move(item));
                }
            }

            result.epoch = ws.context_epoch;
            result.total = static_cast<int>(all_items.size());
            int end = std::min(offset_val + page_size, static_cast<int>(all_items.size()));
            for(int i = offset_val; i < end; ++i) {
                result.contexts.push_back(std::move(all_items[i]));
            }
            co_return to_raw(result);
        });

    peer.on_request(
        "clice/currentContext",
        [this, resolve_uri, flags_label](RequestContext& ctx,
                                         const ext::CurrentContextParams& params) -> RawResult {
            auto& srv = this->server;
            auto [path, path_id, session] = resolve_uri(params.uri);

            ext::CurrentContextResult result;
            if(session && session->active_context) {
                auto& active = *session->active_context;
                auto ctx_path = srv.workspace.path_pool.resolve(active.host_path_id);
                auto ctx_uri_opt = lsp::URI::from_file_path(std::string(ctx_path));
                if(ctx_uri_opt) {
                    ext::ContextItem item;
                    item.label = llvm::sys::path::filename(ctx_path).str();
                    if(active.occurrence.value_or(0) > 0) {
                        item.label = std::format("{} (#{})", item.label, *active.occurrence + 1);
                    }
                    item.description = std::string(ctx_path);
                    item.uri = ctx_uri_opt->str();
                    item.occurrence = active.occurrence;
                    if(!active.command_hash.empty()) {
                        item.command_hash = active.command_hash;
                    }
                    result.context = std::move(item);
                }
            } else if(session && session->active_command) {
                auto& ws = srv.workspace;
                ext::ContextItem item;
                item.uri = params.uri;
                item.command_hash = *session->active_command;
                item.label = std::format("config {}", session->active_command->substr(0, 8));
                if(ws.cdb.has_entry(path)) {
                    std::vector<std::string> rule_append, rule_remove;
                    ws.config.match_rules(path, rule_append, rule_remove);
                    for(auto& cmd:
                        ws.cdb.lookup(path, {.remove = rule_remove, .append = rule_append})) {
                        if(canonical_command_hash(cmd.to_string_argv(), cmd.resolved.directory) ==
                           *session->active_command) {
                            auto desc = flags_label(cmd);
                            if(!desc.empty()) {
                                item.label = std::move(desc);
                            }
                            item.description = cmd.resolved.directory.str();
                            break;
                        }
                    }
                }
                result.context = std::move(item);
            }
            co_return to_raw(result);
        });

    peer.on_request(
        "clice/switchContext",
        [this, resolve_uri, count_occurrences](
            RequestContext& ctx,
            const ext::SwitchContextParams& params) -> RawResult {
            auto& srv = this->server;
            auto& ws = srv.workspace;
            auto [path, path_id, session] = resolve_uri(params.uri);
            auto [context_path, context_path_id, context_session] = resolve_uri(params.context_uri);

            ext::SwitchContextResult result;

            // A choice made against an outdated listing may reference
            // contexts that no longer exist — make the client re-query.
            if(params.epoch.has_value() && *params.epoch != ws.context_epoch) {
                result.stale = true;
                co_return to_raw(result);
            }

            if(!session) {
                co_return to_raw(result);
            }

            // Validate that `hash` names a real CDB entry of `entry_path`.
            auto has_command = [&](llvm::StringRef entry_path, llvm::StringRef hash) {
                if(!ws.cdb.has_entry(entry_path)) {
                    return false;
                }
                std::vector<std::string> rule_append, rule_remove;
                ws.config.match_rules(entry_path, rule_append, rule_remove);
                for(auto& cmd:
                    ws.cdb.lookup(entry_path, {.remove = rule_remove, .append = rule_append})) {
                    if(canonical_command_hash(cmd.to_string_argv(), cmd.resolved.directory) ==
                       hash) {
                        return true;
                    }
                }
                return false;
            };

            if(context_path_id == path_id && params.command_hash.has_value()) {
                // Pin one of the file's own CDB entries.
                if(!has_command(path, *params.command_hash)) {
                    co_return to_raw(result);
                }
                session->active_command = *params.command_hash;
                session->active_context.reset();
            } else {
                // Pin a host source for a header: it must have a real CDB
                // entry, actually (transitively) include this header, and —
                // for multi-configuration hosts — own the pinned entry.
                if(!ws.cdb.has_entry(context_path)) {
                    co_return to_raw(result);
                }
                if(ws.dep_graph.find_include_chain(context_path_id, path_id).empty()) {
                    co_return to_raw(result);
                }
                if(params.command_hash.has_value() &&
                   !has_command(context_path, *params.command_hash)) {
                    co_return to_raw(result);
                }
                if(params.occurrence.has_value() && *params.occurrence > 0) {
                    auto count = count_occurrences(context_path_id, path_id);
                    if(count > 0 && *params.occurrence >= count) {
                        co_return to_raw(result);
                    }
                }
                session->active_context = Session::ActiveContext{context_path_id,
                                                                 params.occurrence,
                                                                 params.command_hash.value_or("")};
                session->active_command.reset();
            }

            session->header_context.reset();
            session->pch_ref.reset();
            session->ast_deps.reset();
            session->ast_dirty = true;
            // The new context needs its own self-containment trial — a
            // different host can change the macro environment.
            session->trial_done = false;
            ws.forget_self_contained(path_id);
            // Invalidate any in-flight compile: without the bump it would
            // clobber ast_dirty on completion and publish results for the
            // old context, with nothing left for is_stale() to detect.
            session->generation++;

            // Persist the choice across sessions.
            SavedContext saved;
            if(session->active_context) {
                saved.host_path_id = session->active_context->host_path_id;
                saved.occurrence = session->active_context->occurrence;
                saved.command_hash = session->active_context->command_hash;
            } else if(session->active_command) {
                saved.command_hash = *session->active_command;
            }
            ws.saved_contexts[path_id] = std::move(saved);
            ws.save_cache();

            result.success = true;
            co_return to_raw(result);
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

LSPClient::~LSPClient() {
    logging::set_notify_hook(nullptr);
    server.compiler.set_peer(nullptr);
    server.indexer.set_peer(nullptr);
}

}  // namespace clice
