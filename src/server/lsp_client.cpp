#include "server/lsp_client.h"

#include <algorithm>
#include <chrono>
#include <format>
#include <map>
#include <string>
#include <type_traits>
#include <variant>

#include "version.h"
#include "command/argument_parser.h"
#include "semantic/symbol.h"
#include "server/editor_context.h"
#include "server/extension.h"
#include "server/file_tracker.h"
#include "server/format.h"
#include "server/master_server.h"
#include "server/uri.h"
#include "support/anomaly.h"
#include "support/filesystem.h"
#include "support/logging.h"
#include "syntax/preamble_synthesis.h"
#include "worker/serialize.h"

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

/// Error response for feature requests on files with no open session.
static kota::ipc::Error document_not_open() {
    return kota::ipc::Error{kota::ipc::protocol::ErrorCode::InvalidParams, "Document not open"};
}

/// True once the server has begun shutting down; document-sync
/// notifications arriving past this point are dropped.
static bool past_shutdown(ServerLifecycle lifecycle) {
    return lifecycle == ServerLifecycle::ShuttingDown || lifecycle == ServerLifecycle::Exited;
}

/// Fire a workspace refresh request without awaiting the reply. Peer
/// lifetime discipline as in the progress handshake: the peer outlives the
/// client, and teardown fails pending requests before run() returns. A
/// captureless coroutine lambda invoked immediately: parameters are copied
/// into the frame, captures would dangle.
template <typename Params>
static void fire_refresh(kota::event_loop& loop, kota::ipc::JsonPeer& peer, Params params) {
    loop.schedule([](kota::ipc::JsonPeer* peer, Params request) -> kota::task<> {
        co_await peer->send_request(request, {.timeout = std::chrono::milliseconds(3000)});
    }(&peer, std::move(params)));
}

LSPClient::LSPClient(MasterServer& server, kota::ipc::JsonPeer& peer) : server(server), peer(peer) {
    output_conn = server.on_output.connect(
        [this](ProjectServer& project, const std::shared_ptr<Session>& session) {
            push_output(project, *session);
        });
    projects_conn = server.on_projects_changed.connect([this]() {
        if(client_ready) {
            publish_config_diagnostics();
        }
    });
    progress_conn = server.on_index_progress.connect([this]() { report_index_progress(); });
    serving_conn = server.on_serving_rows_changed.connect([this]() { refresh_index_served(); });

    // Guidance/anomaly messages travel as window/logMessage, which the LSP
    // spec allows before the initialize handshake — drain what a headless
    // server accumulated, then drain again on every wake-up.
    forward_notify_messages();
    notify_conn = server.on_notify.connect([this]() { forward_notify_messages(); });

    register_lifecycle();
    register_document_sync();
    register_language_features();
    register_extensions();
}

void LSPClient::forward_notify_messages() {
    auto first_seq = server.notify_seq - server.notify_log.size();
    for(auto seq = std::max(notify_cursor, first_seq); seq < server.notify_seq; ++seq) {
        auto& message = server.notify_log[static_cast<std::size_t>(seq - first_seq)];
        peer.send_notification(protocol::LogMessageParams{
            static_cast<protocol::MessageType>(message.level),
            message.text,
        });
    }
    notify_cursor = server.notify_seq;
}

/// Fold versioned document changes into the plain `changes` map for a
/// client without documentChanges support.
static void unversion(protocol::WorkspaceEdit& edit) {
    if(!edit.document_changes) {
        return;
    }
    std::map<protocol::DocumentUri, std::vector<protocol::TextEdit>> changes;
    for(auto& change: *edit.document_changes) {
        auto* document = std::get_if<protocol::TextDocumentEdit>(&change);
        if(!document) {
            continue;
        }
        auto& edits = changes[document->text_document.uri];
        for(auto& text_edit: document->edits) {
            if(auto* plain = std::get_if<protocol::TextEdit>(&text_edit)) {
                edits.push_back(std::move(*plain));
            }
        }
    }
    edit.changes = std::move(changes);
    edit.document_changes.reset();
}

LSPClient::ResolvedDoc LSPClient::resolve_uri(const std::string& uri) {
    auto path = uri_to_path(uri);
    auto path_id = this->server.files.intern(path);
    return ResolvedDoc{std::move(path),
                       path_id,
                       this->server.find_session(path_id),
                       this->server.owner_of(path_id).shared_from_this()};
}

void LSPClient::register_lifecycle() {
    using StringVec = std::vector<std::string>;

    peer.on_request([this](RequestContext& ctx, const protocol::InitializeParams& params)
                        -> RequestResult<protocol::InitializeParams> {
        this->server.pool.foreground_pulse();
        auto& srv = this->server;
        if(srv.lifecycle != ServerLifecycle::Uninitialized) {
            co_return kota::outcome_error(protocol::Error{"Server already initialized"});
        }

        // Every workspace folder is a project; a client without folder
        // support names its one root through rootUri.
        auto& init = params.lsp__initialize_params;
        auto& folders = params.workspace_folders_initialize_params.workspace_folders;
        std::vector<std::string> roots;
        if(folders.has_value() && folders->has_value() && !(*folders)->empty()) {
            for(auto& folder: **folders) {
                roots.push_back(uri_to_path(folder.uri));
            }
        } else if(init.root_uri.has_value()) {
            roots.push_back(uri_to_path(*init.root_uri));
        }
        srv.change_folders({}, std::move(roots));

        if(init.capabilities.workspace.has_value()) {
            auto& ws_caps = *init.capabilities.workspace;
            semantic_tokens_refresh =
                ws_caps.semantic_tokens.has_value() && ws_caps.semantic_tokens->refresh_support;
            inlay_hint_refresh =
                ws_caps.inlay_hint.has_value() && ws_caps.inlay_hint->refresh_support;
            folding_range_refresh =
                ws_caps.folding_range.has_value() && ws_caps.folding_range->refresh_support;
            versioned_edits =
                ws_caps.workspace_edit.has_value() && ws_caps.workspace_edit->document_changes;
        }

        if(init.initialization_options.has_value()) {
            auto json =
                kota::codec::json::to_string<kota::ipc::lsp_config>(*init.initialization_options);
            if(json)
                srv.init_options_json = std::move(*json);
        }

        srv.lifecycle = ServerLifecycle::Initialized;
        LOG_INFO("Initialized with folders: {}", llvm::join(srv.workspace_roots, ", "));

        protocol::InitializeResult result;
        auto& caps = result.capabilities;

        caps.text_document_sync = protocol::TextDocumentSyncOptions{
            .open_close = true,
            .change = protocol::TextDocumentSyncKind::Incremental,
            .save = protocol::variant<protocol::boolean, protocol::SaveOptions>{true},
        };

        caps.hover_provider = true;
        caps.completion_provider = protocol::CompletionOptions{
            .trigger_characters = StringVec{".", "<", ">", ":", "\"", "/", "*", " "},
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
        caps.code_action_provider = protocol::CodeActionOptions{
            .code_action_kinds =
                std::vector<protocol::CodeActionKind>(feature::code_action_kinds.begin(),
                                                      feature::code_action_kinds.end()),
        };

        protocol::WorkspaceFoldersServerCapabilities folder_caps;
        folder_caps.supported = true;
        folder_caps.change_notifications = true;
        caps.workspace = protocol::WorkspaceOptions{.workspace_folders = std::move(folder_caps)};

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
        info.version = std::string(clice::version);
        result.server_info = std::move(info);

        co_return result;
    });

    peer.on_notification([this]([[maybe_unused]] const protocol::InitializedParams& params) {
        auto& srv = this->server;
        // A server pre-initialized from the command line (serve
        // --workspace) rejects the LSP initialize request but the client
        // still completes its handshake; re-running initialize() here
        // would spawn a second worker pool.
        if(srv.lifecycle == ServerLifecycle::Initialized) {
            srv.initialize();
        }

        this->client_ready = true;
        this->publish_config_diagnostics();

        // Replay compile outputs that materialized while no ready client
        // was attached (documents opened and compiled before the
        // handshake, or while the server ran headless). Only outputs that
        // still describe the current buffer are replayed: an edit during
        // the handshake marks the session dirty, and the next compile
        // pushes fresh results instead.
        for(auto& project: srv.projects) {
            project->sessions.for_each([&](Fid path_id, const Session& session) {
                auto& projections = project->ast.projections;
                auto projection = projections.projection(path_id);
                if(projection && projection->output.has_value() && projections.current(path_id) &&
                   projection->output->version == session.version) {
                    this->push_output(*project, session);
                }
                return true;
            });
        }
    });

    // Each folder is a project: an added one starts serving, a removed
    // one hands its open documents to the projects routing picks now.
    peer.on_notification([this](const protocol::DidChangeWorkspaceFoldersParams& params) {
        auto& srv = this->server;
        if(past_shutdown(srv.lifecycle)) {
            return;
        }
        std::vector<std::string> removed;
        for(auto& folder: params.event.removed) {
            removed.push_back(uri_to_path(folder.uri));
        }
        std::vector<std::string> added;
        for(auto& folder: params.event.added) {
            added.push_back(uri_to_path(folder.uri));
        }
        srv.change_folders(std::move(removed), std::move(added));
    });

    peer.on_request(
        [this](RequestContext& ctx,
               const protocol::ShutdownParams& params) -> RequestResult<protocol::ShutdownParams> {
            this->server.pool.foreground_pulse();
            this->server.lifecycle = ServerLifecycle::ShuttingDown;
            LOG_INFO("Shutdown requested");
            co_return nullptr;
        });

    peer.on_notification([this]([[maybe_unused]] const protocol::ExitParams& params) {
        LOG_INFO("Exit notification received");
        this->server.schedule_shutdown();
    });
}

void LSPClient::register_document_sync() {
    peer.on_notification([this](const protocol::DidOpenTextDocumentParams& params) {
        auto& srv = this->server;
        if(past_shutdown(srv.lifecycle))
            return;
        srv.pool.foreground_pulse();

        auto path = uri_to_path(params.text_document.uri);

        // A didOpen racing ahead of the initialize handshake is a client
        // protocol violation, but sessions are plain state with no worker
        // dependency — accept the buffer so the session is in place once
        // the server is ready. A compile attempted before then degrades to
        // an operational worker-unavailable error and leaves the session
        // dirty, so the first post-ready request recompiles it.
        if(srv.lifecycle != ServerLifecycle::Ready) {
            LOG_WARN("didOpen before the server is ready, accepting: {}", path);
        }

        srv.open_session(srv.files.intern(path),
                         params.text_document.text,
                         params.text_document.version);

        LOG_DEBUG("didOpen: {} (v{})", path, params.text_document.version);
    });

    peer.on_notification([this](const protocol::DidChangeTextDocumentParams& params) {
        auto& srv = this->server;
        if(past_shutdown(srv.lifecycle))
            return;
        srv.pool.foreground_pulse();

        auto [path, path_id, session, project] = resolve_uri(params.text_document.uri);
        if(!session) {
            // Dropping is the only safe move: without the didOpen baseline
            // there is no buffer to fold the edits into.
            LOG_ERROR("didChange for a document with no open session, dropping: {}", path);
            return;
        }

        if(params.text_document.version <= session->version) {
            LOG_WARN("didChange version did not increase for {}: {} -> {}",
                     path,
                     session->version,
                     params.text_document.version);
        }

        project->sessions.apply_change(*session,
                                       params.content_changes,
                                       params.text_document.version);

        // The edit just made any in-flight compile stale. Supersede it now
        // instead of waiting for the next AST-backed request to observe
        // it: the round's advisory token releases its dependency waits,
        // and the CancelCompile interrupt keeps a stale parse from
        // holding up its waiters.
        project->ast.supersede(path_id);

        // Editing is the canonical escalation trigger: from here on the
        // session invests in PCH/AST.
        project->ast.escalate(*session);

        project->dispatch(FileEvent::buffer_edited(path_id));

        LOG_DEBUG("didChange: path={} version={} gen={}",
                  path,
                  session->version,
                  session->generation);
    });

    peer.on_notification([this](const protocol::DidCloseTextDocumentParams& params) {
        auto& srv = this->server;
        if(past_shutdown(srv.lifecycle))
            return;

        // Accepted before the server is ready for the same reason didOpen
        // is: a session opened during the handshake window must not stay
        // open forever when the editor already closed it. The diagnostics
        // clear is suppressed until the handshake completes — nothing was
        // pushed, and publishDiagnostics may not flow yet (push_output
        // drops the clear while !client_ready).
        auto path_id = srv.files.intern(uri_to_path(params.text_document.uri));
        // LSP versions are scoped to an open document: a reopen restarts
        // them, so a stale entry would misread the fresh document's first
        // compile as an unchanged-text recompile.
        published_versions.erase(path_id);
        srv.close_session(path_id);
    });

    peer.on_notification([this](const protocol::DidSaveTextDocumentParams& params) {
        auto& srv = this->server;
        // Unlike didOpen/didChange, a save arriving before the server is
        // ready needs no special handling: BufferSaved only invalidates
        // derived state, none of which exists yet — the workspace load at
        // ready reads the saved disk content anyway.
        if(srv.lifecycle != ServerLifecycle::Ready)
            return;
        srv.pool.foreground_pulse();

        auto path = uri_to_path(params.text_document.uri);
        srv.saved(srv.files.intern(path));

        LOG_DEBUG("didSave: {}", path);
    });
}

void LSPClient::register_language_features() {
    peer.on_request([this](RequestContext& ctx, const protocol::HoverParams& params) -> RawResult {
        this->server.pool.foreground_pulse();
        auto [path, path_id, session, project] =
            resolve_uri(params.text_document_position_params.text_document.uri);
        if(!session)
            co_return kota::outcome_error(document_not_open());
        co_return co_await project->features.hover(session,
                                                   params.text_document_position_params.position,
                                                   ctx.cancellation);
    });

    peer.on_request(
        [this](RequestContext& ctx, const protocol::SemanticTokensParams& params) -> RawResult {
            this->server.pool.foreground_pulse();
            auto [path, path_id, session, project] = resolve_uri(params.text_document.uri);
            if(!session)
                co_return kota::outcome_error(document_not_open());
            co_return co_await project->features.semantic_tokens(session, ctx.cancellation);
        });

    peer.on_request([this](RequestContext& ctx,
                           const protocol::InlayHintParams& params) -> RawResult {
        this->server.pool.foreground_pulse();
        auto [path, path_id, session, project] = resolve_uri(params.text_document.uri);
        if(!session)
            co_return kota::outcome_error(document_not_open());
        co_return co_await project->features.inlay_hints(session, params.range, ctx.cancellation);
    });

    peer.on_request(
        [this](RequestContext& ctx, const protocol::FoldingRangeParams& params) -> RawResult {
            this->server.pool.foreground_pulse();
            auto [path, path_id, session, project] = resolve_uri(params.text_document.uri);
            if(!session)
                co_return kota::outcome_error(document_not_open());
            co_return co_await project->features.folding_range(session, ctx.cancellation);
        });

    peer.on_request(
        [this](RequestContext& ctx, const protocol::DocumentSymbolParams& params) -> RawResult {
            this->server.pool.foreground_pulse();
            auto [path, path_id, session, project] = resolve_uri(params.text_document.uri);
            if(!session)
                co_return kota::outcome_error(document_not_open());
            co_return co_await project->features.document_symbol(session, ctx.cancellation);
        });

    peer.on_request(
        [this](RequestContext& ctx, const protocol::DocumentLinkParams& params) -> RawResult {
            this->server.pool.foreground_pulse();
            auto [path, path_id, session, project] = resolve_uri(params.text_document.uri);
            if(!session)
                co_return kota::outcome_error(document_not_open());
            auto links = co_await project->features.document_links(session, ctx.cancellation);
            if(!links.has_value())
                co_return kota::outcome_error(std::move(links.error()));
            co_return to_raw(links.value());
        });

    peer.on_request(
        [this](RequestContext& ctx, const protocol::CodeActionParams& params) -> RawResult {
            this->server.pool.foreground_pulse();
            auto [path, path_id, session, project] = resolve_uri(params.text_document.uri);
            if(!session)
                co_return kota::outcome_error(document_not_open());
            auto actions = co_await project->features.code_action(
                session,
                params.range,
                params.context.only.value_or(std::vector<protocol::CodeActionKind>{}),
                ctx.cancellation);
            if(!actions.has_value())
                co_return kota::outcome_error(std::move(actions.error()));
            if(!versioned_edits) {
                for(auto& action: actions.value()) {
                    unversion(*action.edit);
                }
            }
            co_return to_raw(actions.value());
        });

    peer.on_request([this](RequestContext& ctx,
                           const protocol::DefinitionParams& params) -> RawResult {
        this->server.pool.foreground_pulse();
        auto& uri = params.text_document_position_params.text_document.uri;
        auto& pos = params.text_document_position_params.position;
        auto [path, path_id, session, project] = resolve_uri(uri);
        co_return co_await project->features.definition(session, path_id, pos, ctx.cancellation);
    });

    // The navigation handlers below are index-only: closed documents are
    // fully serveable from the index, and an empty result is a real answer,
    // returned as [] — never an error.
    peer.on_request(
        [this](RequestContext& ctx, const protocol::ReferenceParams& params) -> RawResult {
            this->server.pool.foreground_pulse();
            auto& uri = params.text_document_position_params.text_document.uri;
            auto& pos = params.text_document_position_params.position;
            auto [path, path_id, session, project] = resolve_uri(uri);
            co_return co_await project->features.references(session,
                                                            path_id,
                                                            pos,
                                                            params.context.include_declaration);
        });

    peer.on_request(
        [this](RequestContext& ctx, const protocol::TypeDefinitionParams& params) -> RawResult {
            this->server.pool.foreground_pulse();
            auto& uri = params.text_document_position_params.text_document.uri;
            auto& pos = params.text_document_position_params.position;
            auto [path, path_id, session, project] = resolve_uri(uri);
            co_return co_await project->features.type_definition(session, path_id, pos);
        });

    peer.on_request(
        [this](RequestContext& ctx, const protocol::ImplementationParams& params) -> RawResult {
            this->server.pool.foreground_pulse();
            auto& uri = params.text_document_position_params.text_document.uri;
            auto& pos = params.text_document_position_params.position;
            auto [path, path_id, session, project] = resolve_uri(uri);
            co_return co_await project->features.implementation(session, path_id, pos);
        });

    peer.on_request(
        [this](RequestContext& ctx, const protocol::DeclarationParams& params) -> RawResult {
            this->server.pool.foreground_pulse();
            auto& uri = params.text_document_position_params.text_document.uri;
            auto& pos = params.text_document_position_params.position;
            auto [path, path_id, session, project] = resolve_uri(uri);
            co_return co_await project->features.declaration(session, path_id, pos);
        });

    peer.on_request(
        [this](RequestContext& ctx, const protocol::CompletionParams& params) -> RawResult {
            this->server.pool.foreground_pulse();
            auto [path, path_id, session, project] =
                resolve_uri(params.text_document_position_params.text_document.uri);
            if(!session)
                co_return kota::outcome_error(document_not_open());
            llvm::StringRef trigger;
            if(params.context && params.context->trigger_character) {
                trigger = *params.context->trigger_character;
            }
            co_return co_await project->features.completion(
                session,
                params.text_document_position_params.position,
                trigger,
                ctx.cancellation);
        });

    peer.on_request(
        [this](RequestContext& ctx, const protocol::SignatureHelpParams& params) -> RawResult {
            this->server.pool.foreground_pulse();
            auto [path, path_id, session, project] =
                resolve_uri(params.text_document_position_params.text_document.uri);
            if(!session)
                co_return kota::outcome_error(document_not_open());
            co_return co_await project->features.signature_help(
                session,
                params.text_document_position_params.position,
                ctx.cancellation);
        });

    peer.on_request(
        [this](RequestContext& ctx, const protocol::DocumentFormattingParams& params) -> RawResult {
            this->server.pool.foreground_pulse();
            auto [path, path_id, session, project] = resolve_uri(params.text_document.uri);
            if(!session)
                co_return kota::outcome_error(document_not_open());
            co_return co_await project->features.formatting(session, ctx.cancellation);
        });

    peer.on_request([this](RequestContext& ctx,
                           const protocol::DocumentRangeFormattingParams& params) -> RawResult {
        this->server.pool.foreground_pulse();
        auto [path, path_id, session, project] = resolve_uri(params.text_document.uri);
        if(!session)
            co_return kota::outcome_error(document_not_open());
        co_return co_await project->features.range_formatting(session,
                                                              params.range,
                                                              ctx.cancellation);
    });

    peer.on_request([this](RequestContext& ctx,
                           const protocol::CallHierarchyPrepareParams& params) -> RawResult {
        this->server.pool.foreground_pulse();
        auto& uri = params.text_document_position_params.text_document.uri;
        auto& pos = params.text_document_position_params.position;
        auto [path, path_id, session, project] = resolve_uri(uri);
        co_return co_await project->features.call_hierarchy_prepare(session, path_id, pos);
    });

    peer.on_request([this](RequestContext& ctx,
                           const protocol::CallHierarchyIncomingCallsParams& params) -> RawResult {
        this->server.pool.foreground_pulse();
        auto [path, path_id, session, project] = resolve_uri(params.item.uri);
        co_return co_await project->features.call_hierarchy_incoming(path_id, params.item);
    });

    peer.on_request([this](RequestContext& ctx,
                           const protocol::CallHierarchyOutgoingCallsParams& params) -> RawResult {
        this->server.pool.foreground_pulse();
        auto [path, path_id, session, project] = resolve_uri(params.item.uri);
        co_return co_await project->features.call_hierarchy_outgoing(path_id, params.item);
    });

    peer.on_request([this](RequestContext& ctx,
                           const protocol::TypeHierarchyPrepareParams& params) -> RawResult {
        this->server.pool.foreground_pulse();
        auto& uri = params.text_document_position_params.text_document.uri;
        auto& pos = params.text_document_position_params.position;
        auto [path, path_id, session, project] = resolve_uri(uri);
        co_return co_await project->features.type_hierarchy_prepare(session, path_id, pos);
    });

    peer.on_request([this](RequestContext& ctx,
                           const protocol::TypeHierarchySupertypesParams& params) -> RawResult {
        this->server.pool.foreground_pulse();
        auto [path, path_id, session, project] = resolve_uri(params.item.uri);
        co_return co_await project->features.type_hierarchy_supertypes(path_id, params.item);
    });

    peer.on_request([this](RequestContext& ctx,
                           const protocol::TypeHierarchySubtypesParams& params) -> RawResult {
        this->server.pool.foreground_pulse();
        auto [path, path_id, session, project] = resolve_uri(params.item.uri);
        co_return co_await project->features.type_hierarchy_subtypes(path_id, params.item);
    });

    peer.on_request(
        [this](RequestContext& ctx, const protocol::WorkspaceSymbolParams& params) -> RawResult {
            this->server.pool.foreground_pulse();
            co_return to_raw(this->server.workspace_symbol(params.query));
        });
}

void LSPClient::register_extensions() {
    // ── Compilation context helpers ─────────────────────────────────

    peer.on_request(
        "clice/queryContext",
        [this](RequestContext& ctx, const ext::QueryContextParams& params) -> RawResult {
            this->server.pool.foreground_pulse();
            auto [path, path_id, session, project] = resolve_uri(params.uri);
            co_return to_raw(this->server.query_contexts(path, path_id, params));
        });

    peer.on_request(
        "clice/currentContext",
        [this](RequestContext& ctx, const ext::CurrentContextParams& params) -> RawResult {
            this->server.pool.foreground_pulse();
            auto [path, path_id, session, project] = resolve_uri(params.uri);
            co_return to_raw(project->context_service.current_context(path, session.get(), params));
        });

    peer.on_request(
        "clice/switchContext",
        [this](RequestContext& ctx, const ext::SwitchContextParams& params) -> RawResult {
            this->server.pool.foreground_pulse();
            auto path = uri_to_path(params.uri);
            auto path_id = this->server.files.intern(path);
            auto context_path = uri_to_path(params.context_uri);
            auto context_path_id = this->server.files.intern(context_path);
            // The session reset lives inside switch_context (single owner,
            // synchronous, no cross-file cascade — exempt from the event
            // pipeline; see the Invalidator charter).
            co_return to_raw(co_await this->server.switch_context(path,
                                                                  path_id,
                                                                  context_path,
                                                                  context_path_id,
                                                                  params));
        });

    // The project serving the named document; without one the first
    // project over a folder, the first one being rootless in a server
    // without folders.
    auto configured = [this](const std::optional<std::string>& uri) {
        if(uri) {
            return resolve_uri(*uri).project;
        }
        auto& projects = this->server.projects;
        auto it = llvm::find_if(projects, [](auto& project) { return !project->root.empty(); });
        return it != projects.end() ? *it : projects.front();
    };

    peer.on_request("clice/listConfigurations",
                    [configured](RequestContext& ctx,
                                 const ext::ListConfigurationsParams& params) -> RawResult {
                        co_return to_raw(
                            configured(params.uri)->context_service.list_configurations());
                    });

    peer.on_request("clice/switchConfiguration",
                    [this, configured](RequestContext& ctx,
                                       const ext::SwitchConfigurationParams& params) -> RawResult {
                        co_return to_raw(configured(params.uri)
                                             ->context_service.switch_configuration(
                                                 params.name,
                                                 this->server.requested_configuration));
                    });

    // ── Test hook ───────────────────────────────────────────────────

    // Runs one file-tracker tick synchronously (see ext::PollParams).
    // Test-only and not a stable API.
    peer.on_request("clice/internal/poll",
                    [this](RequestContext& ctx, const ext::PollParams& params) -> RawResult {
                        auto& srv = this->server;
                        if(params.loop != "cdb" && params.loop != "workspace") {
                            co_return kota::outcome_error(
                                kota::ipc::Error{protocol::ErrorCode::InvalidParams,
                                                 R"(loop must be "cdb" or "workspace")"});
                        }
                        // Every project ticks; the reply counts the events of all.
                        std::uint32_t count = 0;
                        bool loaded = false;
                        for(std::size_t i = 0; i < srv.projects.size(); i += 1) {
                            auto project = srv.projects[i];
                            if(!project->tracker) {
                                continue;
                            }
                            loaded = true;
                            llvm::SmallVector<FileEvent> events;
                            if(params.loop == "cdb") {
                                events = project->tracker->tick_cdb(params.force.value_or(true));
                            } else {
                                events = co_await project->tracker->tick_workspace();
                            }
                            count += static_cast<std::uint32_t>(events.size());
                            if(!events.empty()) {
                                project->dispatch(events);
                            }
                        }
                        if(!loaded) {
                            co_return kota::outcome_error(
                                kota::ipc::Error{protocol::ErrorCode::InvalidRequest,
                                                 "No workspace is loaded"});
                        }
                        co_return to_raw(ext::PollResult{count});
                    });

    peer.on_request(
        "clice/internal/logFlood",
        [this](RequestContext& ctx, const ext::LogFloodParams& params) -> RawResult {
            // Load-generating hook: a stray client must not be able to
            // bloat the file log, so it only exists when the harness asked
            // for it at initialize time.
            if(!this->server.projects.front()->project.config.project.test_hooks.value) {
                co_return kota::outcome_error(kota::ipc::Error{protocol::ErrorCode::InvalidRequest,
                                                               "test hooks are not enabled"});
            }
            auto count = std::min<std::uint32_t>(params.count, 100'000);
            auto size = std::clamp<std::uint32_t>(params.size, 16, 4096);
            std::string padding(size, 'f');
            for(std::uint32_t i = 0; i < count; ++i) {
                LOG_INFO("[stderr-flood {}] {}", i, padding);
            }
            co_return to_raw(ext::LogFloodResult{count});
        });

    // Ownership gauges for memory-lifecycle tests (see ext::StatsParams).
    // Read-only and synchronous: every counter is computed from live
    // master state on the event loop, so an assertion made after a settled
    // operation observes exactly the state that operation left behind.
    peer.on_request(
        "clice/internal/stats",
        [this](RequestContext& ctx, const ext::StatsParams&) -> RawResult {
            ext::StatsResult stats;
            for(auto& served: this->server.projects) {
                auto& project = served->project;
                for(auto& entry: project.pch_cache) {
                    auto& st = entry.second;
                    if(st.state) {
                        stats.pch_loaded_states += 1;
                        stats.pch_state_bytes += st.state->bytes().size();
                    }
                }
                stats.pch_cache_entries += static_cast<std::uint32_t>(project.pch_cache.size());

                auto& store = served->sched.store;
                stats.index_inmemory_shards +=
                    static_cast<std::uint32_t>(store.pending_shard_writes());
                for(auto& [path_id, shard]: project.project_index.shards) {
                    stats.index_shard_content_bytes += shard.bytes().size();
                }
                stats.last_save_shards += static_cast<std::uint32_t>(store.last_save_shards());

                if(project.store) {
                    stats.pending_tmp_files +=
                        static_cast<std::uint32_t>(project.store->pending_tmp_files());
                }

                stats.header_contexts +=
                    static_cast<std::uint32_t>(served->contexts.header_contexts.size());
                stats.sessions += static_cast<std::uint32_t>(served->sessions.sessions.size());
            }
            co_return to_raw(stats);
        });
}

/// Publish clice.toml load problems as diagnostics, each on its own file's
/// URI (multiple files can contribute issues when the first config candidate
/// is malformed and the next one loads). The files are usually not open in
/// the editor — publishing to a closed file is fine, the client shows it in
/// the problems panel. The loaded file always gets a publish, so a clean
/// load clears diagnostics from a previous (broken) state.
void LSPClient::publish_config_diagnostics() {
    llvm::StringMap<std::vector<protocol::Diagnostic>> by_file;
    for(auto& file: published_configs) {
        by_file.try_emplace(file.getKey());
    }
    published_configs.clear();
    for(auto& project: server.projects) {
        if(project->config_path.empty()) {
            continue;
        }
        // The loaded file always gets a publish (even with zero issues), so
        // a clean load clears diagnostics from a previous broken state.
        by_file.try_emplace(project->config_path);
        for(auto& issue: project->config_issues) {
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
            published_configs.insert(issue.file);
        }
        published_configs.insert(project->config_path);
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

void LSPClient::push_output(ProjectServer& project, const Session& session) {
    // Held back until the handshake completes (the LSP spec forbids
    // publishDiagnostics before the initialize response); the output stays
    // materialized in the projection and the initialized handler replays it.
    if(!client_ready) {
        return;
    }
    auto projection = project.ast.projections.projection(session.path_id);
    if(!projection || !projection->output.has_value()) {
        return;
    }
    auto& output = *projection->output;

    auto file_path = std::string(server.files.resolve(session.path_id));
    auto uri = lsp::URI::from_file_path(file_path);
    std::string uri_str = uri.has_value() ? uri->str() : file_path;

    protocol::PublishDiagnosticsParams params;
    params.uri = uri_str;
    params.version = output.version;
    params.diagnostics = format_diagnostics(output);
    peer.send_notification(params);

    // Two cases make the client re-pull whole-document results it already
    // holds: index projections served while this compile was pending
    // (their answers just got better), and a compile that landed for
    // unchanged text — a context switch or a changed dependency reshapes
    // tokens (including their inactive regions), hints and folds with no
    // didChange to trigger the client's own re-pull. Edit-driven compiles
    // need neither: the client re-pulls on didChange and that pull awaits
    // the fresh AST.
    if(output.version.has_value()) {
        auto [it, inserted] = published_versions.try_emplace(session.path_id, *output.version);
        bool same_text = !inserted && it->second == *output.version;
        it->second = *output.version;
        if(session.index_served || same_text) {
            if(semantic_tokens_refresh) {
                fire_refresh(server.loop, peer, protocol::SemanticTokensRefreshParams{});
            }
            if(inlay_hint_refresh) {
                fire_refresh(server.loop, peer, protocol::InlayHintRefreshParams{});
            }
            if(folding_range_refresh) {
                fire_refresh(server.loop, peer, protocol::FoldingRangeRefreshParams{});
            }
        }
    }
}

void LSPClient::refresh_index_served() {
    if(!client_ready) {
        return;
    }
    // Inlay hints stay out: index projections never produce them, so a
    // background merge cannot have changed what the client holds.
    if(semantic_tokens_refresh) {
        fire_refresh(server.loop, peer, protocol::SemanticTokensRefreshParams{});
    }
    if(folding_range_refresh) {
        fire_refresh(server.loop, peer, protocol::FoldingRangeRefreshParams{});
    }
}

void LSPClient::report_index_progress() {
    auto& p = server.index_progress;
    using Stage = IndexPump::Progress::Stage;
    auto& st = *index_progress;
    switch(p.stage) {
        case Stage::Begin: {
            st.round_active = true;
            st.total = static_cast<std::uint32_t>(p.total);
            // Progress-token creation is a server→client request, forbidden
            // until the initialize response; a round that begins before the
            // handshake stays unannounced (the next round announces).
            if(!client_ready) {
                break;
            }
            // Register a fresh work-done token; once the client acknowledges
            // it, announce the round. This is the create()+begin() handshake
            // the indexer used to run inline, now driven from the transport so
            // the indexer no longer needs a peer. The dispatch it once gated on
            // proceeds independently; reports before the token is announced are
            // dropped, so the first sub-second of a round may report fewer
            // increments than before. If a previous round's handshake is still
            // in flight, the token is reused: its continuation reconciles
            // against the current round below, so at most one create() is ever
            // outstanding and the reporter is never replaced mid-await.
            if(st.create_inflight || st.token_active) {
                break;
            }
            st.reporter.emplace(peer,
                                protocol::ProgressToken(std::string("clice/backgroundIndex")));
            st.create_inflight = true;
            // Captureless lambda with the state as a parameter: parameters
            // are copied into the coroutine frame, while captures would live
            // in the closure temporary that dies with this statement.
            //
            // Connection teardown mid-handshake needs no cancellation
            // handle. The only suspension point touching the peer is
            // create()'s send_request, and the peer fails every pending
            // outgoing request before its run() returns — that resumption
            // reads only the request's shared completion state, never the
            // peer. Both composition sites destroy the LSPClient before the
            // peer, so once the peer can be gone `abandoned` is already
            // set and the continuation below drops the token without
            // another peer access. The timeout path does reach back into
            // peer internals, but it can only fire while the peer is alive:
            // teardown completes the wait through the failure path first,
            // which also stops the timeout timer.
            server.loop.schedule([](std::shared_ptr<IndexProgressState> state) -> kota::task<> {
                // Timeout prevents the handshake from hanging when the client
                // never responds.
                auto create_result =
                    co_await state->reporter->create({.timeout = std::chrono::milliseconds(3000)});
                state->create_inflight = false;
                // The round may have ended — or the whole connection may have
                // been torn down — while the handshake was in flight; drop the
                // token without announcing it.
                if(state->abandoned || create_result.has_error() || !state->round_active) {
                    state->reporter.reset();
                    co_return;
                }
                state->reporter->begin("Indexing", std::format("0/{} files", state->total), 0);
                state->token_active = true;
            }(index_progress));
            break;
        }
        case Stage::Report: {
            if(st.token_active) {
                auto pct =
                    p.total > 0 ? static_cast<std::uint32_t>(p.completed * 100 / p.total) : 100;
                st.reporter->report(std::format("{}/{} files", p.completed, p.total), pct);
            }
            break;
        }
        case Stage::End: {
            st.round_active = false;
            if(st.token_active) {
                st.reporter->end(std::format("Indexed {} files", p.dispatched));
                st.reporter.reset();
                st.token_active = false;
            }
            // With a handshake still in flight, its continuation sees the
            // round is gone and drops the token.
            break;
        }
    }
}

LSPClient::~LSPClient() {
    // A progress handshake may still be awaiting the client; it holds this
    // state alive and checks the flag before touching the peer.
    index_progress->abandoned = true;
}

}  // namespace clice
