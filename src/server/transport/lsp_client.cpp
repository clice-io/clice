#include "server/transport/lsp_client.h"

#include <algorithm>
#include <chrono>
#include <format>
#include <string>
#include <type_traits>
#include <variant>

#include "version.h"
#include "command/argument_parser.h"
#include "semantic/symbol_kind.h"
#include "server/compiler/context_resolver.h"
#include "server/protocol/extension.h"
#include "server/protocol/serialize.h"
#include "server/service/format.h"
#include "server/state/file_tracker.h"
#include "server/transport/master_server.h"
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

/// Error response for feature requests on files with no open session.
static kota::ipc::Error document_not_open() {
    return kota::ipc::Error{kota::ipc::protocol::ErrorCode::InvalidParams, "Document not open"};
}

/// True once the server has begun shutting down; document-sync
/// notifications arriving past this point are dropped.
static bool past_shutdown(ServerLifecycle lifecycle) {
    return lifecycle == ServerLifecycle::ShuttingDown || lifecycle == ServerLifecycle::Exited;
}

LSPClient::LSPClient(MasterServer& server, kota::ipc::JsonPeer& peer) : server(server), peer(peer) {
    output_conn = server.compiler.on_output.connect(
        [this](const std::shared_ptr<Session>& session) { push_output(*session); });
    progress_conn =
        server.indexer.on_progress_changed.connect([this]() { report_index_progress(); });

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

LSPClient::ResolvedDoc LSPClient::resolve_uri(const std::string& uri) {
    auto path = uri_to_path(uri);
    auto path_id = this->server.workspace.path_pool.intern(path);
    return ResolvedDoc{std::move(path), path_id, this->server.find_session(path_id)};
}

void LSPClient::register_lifecycle() {
    using StringVec = std::vector<std::string>;

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
        // the handshake marks the session dirty, and pushing the pre-edit
        // output would pair stale inactive regions (whose notification
        // carries no version) with the new text — the next compile pushes
        // fresh results instead.
        srv.sessions.for_each(
            [this]([[maybe_unused]] std::uint32_t path_id, const Session& session) {
                if(session.output.has_value() && !session.ast_dirty &&
                   session.output->version == session.version) {
                    this->push_output(session);
                }
                return true;
            });
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
}

void LSPClient::register_document_sync() {
    peer.on_notification([this](const protocol::DidOpenTextDocumentParams& params) {
        auto& srv = this->server;
        if(past_shutdown(srv.lifecycle))
            return;

        auto [path, path_id, session] = resolve_uri(params.text_document.uri);

        // A didOpen racing ahead of the initialize handshake is a client
        // protocol violation, but sessions are plain state with no worker
        // dependency — accept the buffer so the session is in place once
        // the server is ready. A compile attempted before then degrades to
        // an operational worker-unavailable error and leaves the session
        // dirty, so the first post-ready request recompiles it.
        if(srv.lifecycle != ServerLifecycle::Ready) {
            LOG_WARN("didOpen before the server is ready, accepting: {}", path);
        }

        session = srv.open_session(path_id);
        srv.sessions.apply_open(*session, params.text_document.text, params.text_document.version);

        // A context choice persisted from an earlier session stays
        // authoritative only if it still holds.
        srv.contexts.validate_saved_context(*session);

        srv.dispatch(FileEvent::buffer_opened(path_id));

        LOG_DEBUG("didOpen: {} (v{})", path, params.text_document.version);
    });

    peer.on_notification([this](const protocol::DidChangeTextDocumentParams& params) {
        auto& srv = this->server;
        if(past_shutdown(srv.lifecycle))
            return;

        auto [path, path_id, session] = resolve_uri(params.text_document.uri);
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

        srv.sessions.apply_change(*session, params.content_changes, params.text_document.version);

        // The edit just made any in-flight compile stale. Abandon it now
        // instead of waiting for the next AST-backed request to observe
        // the supersede: with no follow-up request the stale parse (or its
        // dependency prep) would run to completion and hold up its waiters.
        srv.compiler.abandon_superseded(*session);

        srv.dispatch(FileEvent::buffer_edited(path_id));

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
        auto [path, path_id, session] = resolve_uri(params.text_document.uri);
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

        auto [path, path_id, session] = resolve_uri(params.text_document.uri);
        srv.dispatch(FileEvent::buffer_saved(path_id));

        LOG_DEBUG("didSave: {}", path);
    });
}

void LSPClient::register_language_features() {
    peer.on_request([this](RequestContext& ctx, const protocol::HoverParams& params) -> RawResult {
        auto& srv = this->server;
        auto [path, path_id, session] =
            resolve_uri(params.text_document_position_params.text_document.uri);
        if(!session)
            co_return kota::outcome_error(document_not_open());
        co_return co_await srv.features.hover(session,
                                              params.text_document_position_params.position,
                                              ctx.cancellation);
    });

    peer.on_request(
        [this](RequestContext& ctx, const protocol::SemanticTokensParams& params) -> RawResult {
            auto& srv = this->server;
            auto [path, path_id, session] = resolve_uri(params.text_document.uri);
            if(!session)
                co_return kota::outcome_error(document_not_open());
            co_return co_await srv.features.semantic_tokens(session, ctx.cancellation);
        });

    peer.on_request(
        [this](RequestContext& ctx, const protocol::InlayHintParams& params) -> RawResult {
            auto& srv = this->server;
            auto [path, path_id, session] = resolve_uri(params.text_document.uri);
            if(!session)
                co_return kota::outcome_error(document_not_open());
            co_return co_await srv.features.inlay_hints(session, params.range, ctx.cancellation);
        });

    peer.on_request(
        [this](RequestContext& ctx, const protocol::FoldingRangeParams& params) -> RawResult {
            auto& srv = this->server;
            auto [path, path_id, session] = resolve_uri(params.text_document.uri);
            if(!session)
                co_return kota::outcome_error(document_not_open());
            co_return co_await srv.features.folding_range(session, ctx.cancellation);
        });

    peer.on_request(
        [this](RequestContext& ctx, const protocol::DocumentSymbolParams& params) -> RawResult {
            auto& srv = this->server;
            auto [path, path_id, session] = resolve_uri(params.text_document.uri);
            if(!session)
                co_return kota::outcome_error(document_not_open());
            co_return co_await srv.features.document_symbol(session, ctx.cancellation);
        });

    peer.on_request(
        [this](RequestContext& ctx, const protocol::DocumentLinkParams& params) -> RawResult {
            auto [path, path_id, session] = resolve_uri(params.text_document.uri);
            if(!session)
                co_return kota::outcome_error(document_not_open());
            auto links = co_await this->server.features.document_links(session, ctx.cancellation);
            if(!links.has_value())
                co_return kota::outcome_error(std::move(links.error()));
            co_return to_raw(links.value());
        });

    peer.on_request(
        [this](RequestContext& ctx, const protocol::CodeActionParams& params) -> RawResult {
            auto& srv = this->server;
            auto [path, path_id, session] = resolve_uri(params.text_document.uri);
            if(!session)
                co_return kota::outcome_error(document_not_open());
            co_return co_await srv.features.code_action(session, ctx.cancellation);
        });

    peer.on_request([this](RequestContext& ctx,
                           const protocol::DefinitionParams& params) -> RawResult {
        auto& uri = params.text_document_position_params.text_document.uri;
        auto& pos = params.text_document_position_params.position;
        auto [path, path_id, session] = resolve_uri(uri);
        co_return co_await this->server.features.definition(session, path, pos, ctx.cancellation);
    });

    // The navigation handlers below are index-only: closed documents are
    // fully serveable from the index, and an empty result is a real answer,
    // returned as [] — never an error.
    peer.on_request(
        [this](RequestContext& ctx, const protocol::ReferenceParams& params) -> RawResult {
            auto& uri = params.text_document_position_params.text_document.uri;
            auto& pos = params.text_document_position_params.position;
            auto [path, path_id, session] = resolve_uri(uri);
            co_return co_await this->server.features.references(session,
                                                                path,
                                                                pos,
                                                                params.context.include_declaration);
        });

    peer.on_request(
        [this](RequestContext& ctx, const protocol::TypeDefinitionParams& params) -> RawResult {
            auto& uri = params.text_document_position_params.text_document.uri;
            auto& pos = params.text_document_position_params.position;
            auto [path, path_id, session] = resolve_uri(uri);
            co_return co_await this->server.features.type_definition(session, path, pos);
        });

    peer.on_request(
        [this](RequestContext& ctx, const protocol::ImplementationParams& params) -> RawResult {
            auto& uri = params.text_document_position_params.text_document.uri;
            auto& pos = params.text_document_position_params.position;
            auto [path, path_id, session] = resolve_uri(uri);
            co_return co_await this->server.features.implementation(session, path, pos);
        });

    peer.on_request(
        [this](RequestContext& ctx, const protocol::DeclarationParams& params) -> RawResult {
            auto& uri = params.text_document_position_params.text_document.uri;
            auto& pos = params.text_document_position_params.position;
            auto [path, path_id, session] = resolve_uri(uri);
            co_return co_await this->server.features.declaration(session, path, pos);
        });

    peer.on_request([this](RequestContext& ctx,
                           const protocol::CompletionParams& params) -> RawResult {
        auto& srv = this->server;
        auto [path, path_id, session] =
            resolve_uri(params.text_document_position_params.text_document.uri);
        if(!session)
            co_return kota::outcome_error(document_not_open());
        llvm::StringRef trigger;
        if(params.context && params.context->trigger_character) {
            trigger = *params.context->trigger_character;
        }
        co_return co_await srv.features.completion(session,
                                                   params.text_document_position_params.position,
                                                   trigger,
                                                   ctx.cancellation);
    });

    peer.on_request(
        [this](RequestContext& ctx, const protocol::SignatureHelpParams& params) -> RawResult {
            auto& srv = this->server;
            auto [path, path_id, session] =
                resolve_uri(params.text_document_position_params.text_document.uri);
            if(!session)
                co_return kota::outcome_error(document_not_open());
            co_return co_await srv.features.signature_help(
                session,
                params.text_document_position_params.position,
                ctx.cancellation);
        });

    peer.on_request(
        [this](RequestContext& ctx, const protocol::DocumentFormattingParams& params) -> RawResult {
            auto& srv = this->server;
            auto [path, path_id, session] = resolve_uri(params.text_document.uri);
            if(!session)
                co_return kota::outcome_error(document_not_open());
            co_return co_await srv.features.formatting(session, ctx.cancellation);
        });

    peer.on_request([this](RequestContext& ctx,
                           const protocol::DocumentRangeFormattingParams& params) -> RawResult {
        auto& srv = this->server;
        auto [path, path_id, session] = resolve_uri(params.text_document.uri);
        if(!session)
            co_return kota::outcome_error(document_not_open());
        co_return co_await srv.features.range_formatting(session, params.range, ctx.cancellation);
    });

    peer.on_request([this](RequestContext& ctx,
                           const protocol::CallHierarchyPrepareParams& params) -> RawResult {
        auto& uri = params.text_document_position_params.text_document.uri;
        auto& pos = params.text_document_position_params.position;
        auto [path, path_id, session] = resolve_uri(uri);
        co_return co_await this->server.features.call_hierarchy_prepare(session, uri, path, pos);
    });

    peer.on_request([this](RequestContext& ctx,
                           const protocol::CallHierarchyIncomingCallsParams& params) -> RawResult {
        auto [path, path_id, session] = resolve_uri(params.item.uri);
        co_return co_await this->server.features.call_hierarchy_incoming(session,
                                                                         path,
                                                                         params.item);
    });

    peer.on_request([this](RequestContext& ctx,
                           const protocol::CallHierarchyOutgoingCallsParams& params) -> RawResult {
        auto [path, path_id, session] = resolve_uri(params.item.uri);
        co_return co_await this->server.features.call_hierarchy_outgoing(session,
                                                                         path,
                                                                         params.item);
    });

    peer.on_request([this](RequestContext& ctx,
                           const protocol::TypeHierarchyPrepareParams& params) -> RawResult {
        auto& uri = params.text_document_position_params.text_document.uri;
        auto& pos = params.text_document_position_params.position;
        auto [path, path_id, session] = resolve_uri(uri);
        co_return co_await this->server.features.type_hierarchy_prepare(session, uri, path, pos);
    });

    peer.on_request([this](RequestContext& ctx,
                           const protocol::TypeHierarchySupertypesParams& params) -> RawResult {
        auto [path, path_id, session] = resolve_uri(params.item.uri);
        co_return co_await this->server.features.type_hierarchy_supertypes(session,
                                                                           path,
                                                                           params.item);
    });

    peer.on_request([this](RequestContext& ctx,
                           const protocol::TypeHierarchySubtypesParams& params) -> RawResult {
        auto [path, path_id, session] = resolve_uri(params.item.uri);
        co_return co_await this->server.features.type_hierarchy_subtypes(session,
                                                                         path,
                                                                         params.item);
    });

    peer.on_request(
        [this](RequestContext& ctx, const protocol::WorkspaceSymbolParams& params) -> RawResult {
            co_return co_await this->server.features.workspace_symbol(params.query);
        });
}

void LSPClient::register_extensions() {
    // ── Compilation context helpers ─────────────────────────────────

    peer.on_request(
        "clice/queryContext",
        [this](RequestContext& ctx, const ext::QueryContextParams& params) -> RawResult {
            auto [path, path_id, session] = resolve_uri(params.uri);
            co_return to_raw(this->server.contexts.query_contexts(path, path_id, params));
        });

    peer.on_request(
        "clice/currentContext",
        [this](RequestContext& ctx, const ext::CurrentContextParams& params) -> RawResult {
            auto [path, path_id, session] = resolve_uri(params.uri);
            co_return to_raw(this->server.contexts.current_context(path, session.get(), params));
        });

    peer.on_request(
        "clice/switchContext",
        [this](RequestContext& ctx, const ext::SwitchContextParams& params) -> RawResult {
            auto [path, path_id, session] = resolve_uri(params.uri);
            auto [context_path, context_path_id, context_session] = resolve_uri(params.context_uri);
            // The session reset lives inside switch_context (single owner,
            // synchronous, no cross-file cascade — exempt from the event
            // pipeline; see the Invalidator charter).
            auto result = this->server.contexts.switch_context(path,
                                                               path_id,
                                                               session.get(),
                                                               context_path,
                                                               context_path_id,
                                                               params);
            co_return to_raw(result);
        });

    // ── Test hook ───────────────────────────────────────────────────

    // Runs one file-tracker tick synchronously (see ext::PollParams).
    // Test-only and not a stable API; the CDB tick runs with force=true,
    // so the polling loop's two-tick settling debounce does not apply here.
    peer.on_request(
        "clice/internal/poll",
        [this](RequestContext& ctx, const ext::PollParams& params) -> RawResult {
            auto& srv = this->server;
            if(!srv.tracker) {
                co_return kota::outcome_error(kota::ipc::Error{protocol::ErrorCode::InvalidRequest,
                                                               "No workspace is loaded"});
            }

            llvm::SmallVector<FileEvent> events;
            if(params.loop == "cdb") {
                events = srv.tracker->tick_cdb(/*force=*/true);
            } else if(params.loop == "workspace") {
                events = co_await srv.tracker->tick_workspace();
            } else {
                co_return kota::outcome_error(
                    kota::ipc::Error{protocol::ErrorCode::InvalidParams,
                                     R"(loop must be "cdb" or "workspace")"});
            }

            if(!events.empty()) {
                srv.dispatch(events);
            }
            co_return to_raw(ext::PollResult{static_cast<std::uint32_t>(events.size())});
        });

    peer.on_request("clice/internal/logFlood",
                    [this](RequestContext& ctx, const ext::LogFloodParams& params) -> RawResult {
                        // Load-generating hook: a stray client must not be able to
                        // bloat the file log, so it only exists when the harness asked
                        // for it at initialize time.
                        if(!this->server.workspace.config.project.test_hooks.value_or(false)) {
                            co_return kota::outcome_error(
                                kota::ipc::Error{protocol::ErrorCode::InvalidRequest,
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
            auto& srv = this->server;
            ext::StatsResult stats;

            for(auto& entry: srv.workspace.pch_cache) {
                auto& st = entry.second;
                if(st.state) {
                    stats.pch_loaded_states += 1;
                    stats.pch_state_bytes += st.state->size();
                }
            }
            stats.pch_cache_entries = static_cast<std::uint32_t>(srv.workspace.pch_cache.size());

            for(auto& [path_id, shard]: srv.workspace.merged_indices) {
                if(shard.need_rewrite()) {
                    stats.index_inmemory_shards += 1;
                    stats.index_shard_content_bytes += shard.content().size();
                }
            }
            stats.last_save_shards = static_cast<std::uint32_t>(srv.indexer.last_save_shards());

            if(srv.workspace.store) {
                stats.pending_tmp_files =
                    static_cast<std::uint32_t>(srv.workspace.store->pending_tmp_files());
            }

            stats.header_contexts = static_cast<std::uint32_t>(srv.contexts.header_contexts.size());
            srv.sessions.for_each([&](std::uint32_t, const Session&) -> bool {
                stats.sessions += 1;
                return true;
            });

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
    // Held back until the handshake completes (the LSP spec forbids
    // publishDiagnostics before the initialize response); the output stays
    // materialized on the session and the initialized handler replays it.
    if(!client_ready) {
        return;
    }
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
    const auto& p = server.indexer.progress();
    using Stage = Indexer::Progress::Stage;
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
