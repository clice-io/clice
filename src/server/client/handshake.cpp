module;

#include "version.h"

module clice;

import :feature.code_actions;
import :feature.protocol;
import :feature.semantic_tokens;
import :server.client;
import :server.extension;
import :server.master;
import :server.served_project;
import :support.logging;
import :vfs.path;

namespace clice {

using ipc::RequestResult;

std::optional<Spelling> uri_to_path(llvm::StringRef uri) {
    auto parsed = lsp::URI::parse(uri.str());
    if(!parsed) {
        return std::nullopt;
    }
    auto path = parsed->file_path();
    if(!path || !path::is_absolute(*path)) {
        return std::nullopt;
    }
    return Spelling::absolute(*path);
}

LSPClient::LSPClient(MasterServer& server, ipc::JSONPeer& peer) : server(server), peer(peer) {
    held_conn = server.probe.on_held.connect([this](std::uint64_t id) {
        this->peer.send_notification("clice/internal/held", ext::HeldParams{id});
    });

    register_lifecycle();
    register_document_sync();
    register_document_requests();
    register_navigation();
    register_editing();
    register_extensions();
    register_test_hooks();
    server.client = this;
}

LSPClient::~LSPClient() {
    if(server.client == this) {
        server.client = nullptr;
    }
    // A progress handshake may still be awaiting the client; it holds this
    // state alive and checks the flag before touching the peer.
    index_progress->abandoned = true;
    for(auto& [path_id, state]: compile_progress) {
        state->abandoned = true;
    }
}

std::optional<ipc::Error> LSPClient::refused(bool shutdown) const {
    switch(server.lifecycle) {
        case ServerLifecycle::Ready: return std::nullopt;
        case ServerLifecycle::Initialized:
            if(shutdown) {
                return std::nullopt;
            }
            [[fallthrough]];
        case ServerLifecycle::Uninitialized:
            return ipc::Error{
                static_cast<protocol::integer>(protocol::ErrorCodes::ServerNotInitialized),
                "the server is not initialized"};
        case ServerLifecycle::ShuttingDown:
        case ServerLifecycle::Exited:
            return ipc::Error{protocol::ErrorCode::InvalidRequest, "the server is shutting down"};
    }
    std::unreachable();
}

/// What positions count in for a client offering `offered`: UTF-8, the
/// server's own, whenever offered; else the client's first choice; else
/// UTF-16, which every client takes.
static PositionEncoding choose_encoding(llvm::ArrayRef<protocol::PositionEncodingKind> offered) {
    if(ranges::any_of(offered, [](const std::string& kind) {
           return kind == protocol::PositionEncodingKind::UTF8;
       })) {
        return PositionEncoding::UTF8;
    }
    for(const auto& kind: offered) {
        if(kind == protocol::PositionEncodingKind::UTF16) {
            return PositionEncoding::UTF16;
        }
        if(kind == protocol::PositionEncodingKind::UTF32) {
            return PositionEncoding::UTF32;
        }
    }
    return PositionEncoding::UTF16;
}

static protocol::PositionEncodingKind encoding_kind(PositionEncoding encoding) {
    switch(encoding) {
        case PositionEncoding::UTF8: return protocol::PositionEncodingKind::UTF8;
        case PositionEncoding::UTF16: return protocol::PositionEncodingKind::UTF16;
        case PositionEncoding::UTF32: return protocol::PositionEncodingKind::UTF32;
    }
    std::unreachable();
}

void LSPClient::register_lifecycle() {
    using StringVec = std::vector<std::string>;

    peer.on_request([this](RequestContext& ctx, const protocol::InitializeParams& params)
                        -> RequestResult<protocol::InitializeParams> {
        // At dispatch: an initialized read right behind the request must find
        // the server initialized.
        this->server.pool.foreground_pulse();
        auto& srv = this->server;
        if(srv.lifecycle != ServerLifecycle::Uninitialized) {
            return kota::outcome_error(protocol::Error{"Server already initialized"});
        }

        if(auto& general = params.capabilities.general; general && general->position_encodings) {
            srv.position_encoding = choose_encoding(*general->position_encodings);
        }

        // Every workspace folder is a project; a client without folder
        // support names its one root through rootUri.
        auto& folders = params.workspace_folders;
        std::vector<Spelling> roots;
        if(folders.has_value() && folders->has_value() && !(*folders)->empty()) {
            for(auto& folder: **folders) {
                if(auto root = uri_to_path(folder.uri)) {
                    roots.push_back(std::move(*root));
                }
            }
        } else if(params.root_uri.has_value()) {
            if(auto root = uri_to_path(*params.root_uri)) {
                roots.push_back(std::move(*root));
            }
        }
        srv.change_folders({}, std::move(roots));
        if(params.process_id && *params.process_id > 0 && srv.client_on_host) {
            srv.watch_client(static_cast<std::uint32_t>(*params.process_id));
        }

        work_done_progress = params.capabilities.window.has_value() &&
                             params.capabilities.window->work_done_progress;

        if(params.capabilities.experimental.has_value() &&
           !params.capabilities.experimental->is_null()) {
            ext::ExperimentalCapabilities experimental;
            if(codec::dyn::from_dyn<ipc::lsp_config>(*params.capabilities.experimental,
                                                     experimental)) {
                restart_server = experimental.restart_server;
            }
        }

        if(params.capabilities.workspace.has_value()) {
            auto& ws_caps = *params.capabilities.workspace;
            semantic_tokens_refresh =
                ws_caps.semantic_tokens.has_value() && ws_caps.semantic_tokens->refresh_support;
            inlay_hint_refresh =
                ws_caps.inlay_hint.has_value() && ws_caps.inlay_hint->refresh_support;
            folding_range_refresh =
                ws_caps.folding_range.has_value() && ws_caps.folding_range->refresh_support;
            versioned_edits =
                ws_caps.workspace_edit.has_value() && ws_caps.workspace_edit->document_changes;
            diagnostic_refresh =
                ws_caps.diagnostics.has_value() && ws_caps.diagnostics->refresh_support;
        }

        pull_diagnostics = params.capabilities.text_document.has_value() &&
                           params.capabilities.text_document->diagnostic.has_value();

        if(auto& text_document = params.capabilities.text_document) {
            auto& pushed = text_document->publish_diagnostics;
            auto& pulled = text_document->diagnostic;
            related_information =
                (pushed && pushed->related_information) || (pulled && pulled->related_information);
        }

        label_parts = params.capabilities.text_document.has_value() &&
                      params.capabilities.text_document->inlay_hint.has_value();

        // The formats come in the client's order of preference.
        if(params.capabilities.text_document.has_value() &&
           params.capabilities.text_document->hover.has_value() &&
           params.capabilities.text_document->hover->content_format.has_value()) {
            auto& formats = *params.capabilities.text_document->hover->content_format;
            auto preferred = ranges::find_if(formats, [](const protocol::MarkupKind& kind) {
                return kind == protocol::MarkupKind::Markdown ||
                       kind == protocol::MarkupKind::PlainText;
            });
            if(preferred != formats.end()) {
                hover_markup = *preferred;
            }
        }

        if(params.capabilities.text_document.has_value() &&
           params.capabilities.text_document->folding_range.has_value()) {
            auto& folding = *params.capabilities.text_document->folding_range;
            folding_client = {
                .line_folding_only = folding.line_folding_only,
                .collapsed_text = folding.folding_range && folding.folding_range->collapsed_text,
            };
        }

        if(params.capabilities.text_document.has_value() &&
           params.capabilities.text_document->document_symbol.has_value()) {
            auto& symbols = *params.capabilities.text_document->document_symbol;
            symbol_client = {
                .hierarchical = symbols.hierarchical_document_symbol_support,
                .all_kinds = symbols.symbol_kind && symbols.symbol_kind->value_set,
            };
        }

        if(params.capabilities.text_document.has_value() &&
           params.capabilities.text_document->completion.has_value() &&
           params.capabilities.text_document->completion->completion_item.has_value()) {
            auto& item = *params.capabilities.text_document->completion->completion_item;
            completion_client = {
                .snippets = item.snippet_support,
                .insert_replace = item.insert_replace_support,
                .markdown = item.documentation_format &&
                            ranges::any_of(*item.documentation_format,
                                           [](const std::string& kind) {
                                               return kind == protocol::MarkupKind::Markdown;
                                           }),
            };
        }

        if(params.initialization_options.has_value() && !params.initialization_options->is_null()) {
            srv.init_options = *params.initialization_options;
        }

        srv.lifecycle = ServerLifecycle::Initialized;
        LOG_INFO("Initialized with folders: {}", llvm::join(srv.folders, ", "));

        protocol::InitializeResult result;
        auto& caps = result.capabilities;

        caps.position_encoding = encoding_kind(srv.position_encoding);
        caps.text_document_sync = protocol::TextDocumentSyncOptions{
            .open_close = true,
            .change = protocol::TextDocumentSyncKind::Incremental,
            .save = protocol::variant<protocol::boolean, protocol::SaveOptions>{true},
        };

        caps.hover_provider = true;
        caps.completion_provider = protocol::CompletionOptions{
            .trigger_characters = StringVec{".", "<", ">", ":", "\"", "/", " "},
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
        caps.document_highlight_provider = true;
        // RenameOptions only for a client that declared prepareSupport.
        auto& text_document = params.capabilities.text_document;
        if(text_document && text_document->rename && text_document->rename->prepare_support) {
            caps.rename_provider = protocol::RenameOptions{.prepare_provider = true};
        } else {
            caps.rename_provider = true;
        }
        caps.document_symbol_provider = true;
        caps.document_link_provider = protocol::DocumentLinkOptions{};
        caps.folding_range_provider = true;
        caps.selection_range_provider = true;
        caps.inlay_hint_provider = true;
        caps.call_hierarchy_provider = true;
        caps.type_hierarchy_provider = true;
        caps.workspace_symbol_provider = true;
        caps.document_formatting_provider = true;
        caps.document_range_formatting_provider = true;
        std::vector<protocol::CodeActionKind> kinds(feature::code_action_kinds.begin(),
                                                    feature::code_action_kinds.end());
        kinds.emplace_back(context_action_kind);
        kinds.emplace_back(configuration_action_kind);
        caps.code_action_provider =
            protocol::CodeActionOptions{.code_action_kinds = std::move(kinds)};
        caps.execute_command_provider = protocol::ExecuteCommandOptions{
            .commands = {std::string(switch_context_command),
                         std::string(reset_context_command),
                         std::string(switch_configuration_command)},
        };
        // No inter-file dependencies: an edit reaches no other document's
        // diagnostics, open buffers are never depended upon (see Session).
        // A saved file's dependents are recompiled on their next request
        // and refreshed then (see push_output).
        if(pull_diagnostics) {
            caps.diagnostic_provider = protocol::DiagnosticOptions{
                .inter_file_dependencies = false,
                .workspace_diagnostics = false,
            };
        }

        protocol::WorkspaceFoldersServerCapabilities folder_caps;
        folder_caps.supported = true;
        folder_caps.change_notifications = true;
        caps.workspace = protocol::WorkspaceOptions{.workspace_folders = std::move(folder_caps)};

        token_legend = feature::semantic_token_legend(srv.startup_token_options());
        for(auto& name: token_legend.unknown) {
            LOG_GUIDANCE("[semantic_tokens] renames `{}`, which names no token kind or modifier",
                         name);
        }
        result.capabilities.semantic_tokens_provider = protocol::SemanticTokensOptions{
            .legend = {.token_types = token_legend.types,
                       .token_modifiers = token_legend.modifiers},
            .range = true,
            .full = protocol::SemanticTokensFullDelta{.delta = true},
        };

        protocol::ServerInfo info;
        info.name = "clice";
        info.version = std::string(clice::version);
        result.server_info = std::move(info);

        return kota::co_invoke(
            [result = std::move(result)]() mutable -> RequestResult<protocol::InitializeParams> {
                co_return std::move(result);
            });
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
        this->forward_notify_messages();
        this->publish_settings_diagnostics();
        this->ask_for_restart();

        // Replay compile outputs that materialized while no ready client
        // was attached (documents opened and compiled before the
        // handshake, or while the server ran headless). Only outputs that
        // still describe the current buffer are replayed: an edit during
        // the handshake marks the session dirty, and the next compile
        // pushes fresh results instead.
        for(auto& [path_id, document]: srv.documents) {
            auto* owner = document.owner;
            auto& session = *document.session;
            if(owner && owner->current(path_id) && owner->latest_at(path_id, session.version)) {
                this->push_output(*owner, session, /*compiled=*/true);
            }
        }
    });

    // Each folder is a project: an added one starts serving, a removed
    // one hands its open documents to the projects routing picks now.
    peer.on_notification([this](const protocol::DidChangeWorkspaceFoldersParams& params) {
        auto& srv = this->server;
        if(srv.shutting_down()) {
            return;
        }
        std::vector<Spelling> removed;
        for(auto& folder: params.event.removed) {
            if(auto root = uri_to_path(folder.uri)) {
                removed.push_back(std::move(*root));
            }
        }
        std::vector<Spelling> added;
        for(auto& folder: params.event.added) {
            if(auto root = uri_to_path(folder.uri)) {
                added.push_back(std::move(*root));
            }
        }
        srv.change_folders(std::move(removed), std::move(added));
    });

    on_request<protocol::ShutdownParams>(
        [this](RequestContext& ctx,
               const protocol::ShutdownParams& params) -> RequestResult<protocol::ShutdownParams> {
            // At dispatch: an exit read right behind the request must find
            // the server shutting down.
            this->server.pool.foreground_pulse();
            this->server.lifecycle = ServerLifecycle::ShuttingDown;
            LOG_INFO("Shutdown requested");
            // Clients kill a server soon after `exit`, Zed at once: the
            // index is committed before the reply that lets them send it.
            return [](MasterServer& server) -> RequestResult<protocol::ShutdownParams> {
                co_await server.stop();
                co_return nullptr;
            }(this->server);
        });

    peer.on_notification([this]([[maybe_unused]] const protocol::ExitParams& params) {
        LOG_INFO("Exit notification received");
        this->server.exit_before_shutdown |= !this->server.shutting_down();
        this->server.schedule_shutdown();
    });
}

}  // namespace clice
