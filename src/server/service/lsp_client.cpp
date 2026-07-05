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
#include "server/feature/format.h"
#include "server/protocol/extension.h"
#include "server/protocol/serialize.h"
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

/// Error response for feature requests on files with no open session.
static kota::ipc::Error document_not_open() {
    return kota::ipc::Error{kota::ipc::protocol::ErrorCode::InvalidParams, "Document not open"};
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

    register_lifecycle();
    register_document_sync();
    register_language_features();
    register_extensions();
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
}

void LSPClient::register_document_sync() {
    peer.on_notification([this](const protocol::DidOpenTextDocumentParams& params) {
        auto& srv = this->server;
        if(srv.lifecycle != ServerLifecycle::Ready)
            return;

        auto [path, path_id, session] = resolve_uri(params.text_document.uri);
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
        if(srv.lifecycle != ServerLifecycle::Ready)
            return;

        auto [path, path_id, session] = resolve_uri(params.text_document.uri);
        if(!session)
            return;

        srv.sessions.apply_change(*session, params.content_changes, params.text_document.version);

        srv.dispatch(FileEvent::buffer_edited(path_id));

        LOG_DEBUG("didChange: path={} version={} gen={}",
                  path,
                  session->version,
                  session->generation);
    });

    peer.on_notification([this](const protocol::DidCloseTextDocumentParams& params) {
        auto& srv = this->server;
        if(srv.lifecycle != ServerLifecycle::Ready)
            return;

        auto [path, path_id, session] = resolve_uri(params.text_document.uri);
        srv.close_session(path_id, this->peer);
    });

    peer.on_notification([this](const protocol::DidSaveTextDocumentParams& params) {
        auto& srv = this->server;
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
                                              params.text_document_position_params.position);
    });

    peer.on_request(
        [this](RequestContext& ctx, const protocol::SemanticTokensParams& params) -> RawResult {
            auto& srv = this->server;
            auto [path, path_id, session] = resolve_uri(params.text_document.uri);
            if(!session)
                co_return kota::outcome_error(document_not_open());
            co_return co_await srv.features.semantic_tokens(session);
        });

    peer.on_request(
        [this](RequestContext& ctx, const protocol::InlayHintParams& params) -> RawResult {
            auto& srv = this->server;
            auto [path, path_id, session] = resolve_uri(params.text_document.uri);
            if(!session)
                co_return kota::outcome_error(document_not_open());
            co_return co_await srv.features.inlay_hints(session, params.range);
        });

    peer.on_request(
        [this](RequestContext& ctx, const protocol::FoldingRangeParams& params) -> RawResult {
            auto& srv = this->server;
            auto [path, path_id, session] = resolve_uri(params.text_document.uri);
            if(!session)
                co_return kota::outcome_error(document_not_open());
            co_return co_await srv.features.folding_range(session);
        });

    peer.on_request(
        [this](RequestContext& ctx, const protocol::DocumentSymbolParams& params) -> RawResult {
            auto& srv = this->server;
            auto [path, path_id, session] = resolve_uri(params.text_document.uri);
            if(!session)
                co_return kota::outcome_error(document_not_open());
            co_return co_await srv.features.document_symbol(session);
        });

    peer.on_request(
        [this](RequestContext& ctx, const protocol::DocumentLinkParams& params) -> RawResult {
            auto [path, path_id, session] = resolve_uri(params.text_document.uri);
            if(!session)
                co_return kota::outcome_error(document_not_open());
            auto links = co_await this->server.features.document_links(session);
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
            co_return co_await srv.features.code_action(session);
        });

    peer.on_request(
        [this](RequestContext& ctx, const protocol::DefinitionParams& params) -> RawResult {
            auto& uri = params.text_document_position_params.text_document.uri;
            auto& pos = params.text_document_position_params.position;
            auto [path, path_id, session] = resolve_uri(uri);
            co_return co_await this->server.features.definition(session, path, pos);
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
        co_return co_await srv.features.completion(session,
                                                   params.text_document_position_params.position);
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
                params.text_document_position_params.position);
        });

    peer.on_request(
        [this](RequestContext& ctx, const protocol::DocumentFormattingParams& params) -> RawResult {
            auto& srv = this->server;
            auto [path, path_id, session] = resolve_uri(params.text_document.uri);
            if(!session)
                co_return kota::outcome_error(document_not_open());
            co_return co_await srv.features.formatting(session);
        });

    peer.on_request([this](RequestContext& ctx,
                           const protocol::DocumentRangeFormattingParams& params) -> RawResult {
        auto& srv = this->server;
        auto [path, path_id, session] = resolve_uri(params.text_document.uri);
        if(!session)
            co_return kota::outcome_error(document_not_open());
        co_return co_await srv.features.range_formatting(session, params.range);
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
            auto result = this->server.contexts.switch_context(path,
                                                               path_id,
                                                               session.get(),
                                                               context_path,
                                                               context_path_id,
                                                               params);
            this->server.dispatch(FileEvent::context_changed(path_id));
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
    auto& st = *index_progress;
    switch(p.stage) {
        case Stage::Begin: {
            st.round_active = true;
            st.total = static_cast<std::uint32_t>(p.total);
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
            // TODO: an in-flight create() still races connection teardown —
            // the reporter references the peer while awaiting. This matches
            // the pre-refactor risk (the reporter captured the peer inside
            // the indexer coroutine); a real fix needs a cancellation handle
            // tied to the peer's lifetime.
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
    logging::set_notify_hook(nullptr);
    // A progress handshake may still be awaiting the client; it holds this
    // state alive and checks the flag before touching the peer.
    index_progress->abandoned = true;
}

}  // namespace clice
