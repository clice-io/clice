module;

#include "support/prelude.h"

#include "version.h"
#include "support/anomaly.macros.h"
#include "support/logging.macros.h"

module clice;

import :command.file_kind;
import :compile.diagnostic;
import :feature.code_actions;
import :feature.diagnostics;
import :feature.protocol;
import :feature.semantic_tokens;
import :index.project_index;
import :index.shard;
import :project.save;
import :server.context_service;
import :server.counterparts;
import :server.editor_context;
import :server.extension;
import :server.file_tracker;
import :server.lsp_client;
import :server.master_server;
import :server.query_commands;
import :support.anomaly;
import :support.json;
import :support.logging;
import :syntax.header_context;
import :syntax.position;
import :syntax.source_text;
import :vfs.file_system;
import :vfs.path;
import :worker.probe;

namespace clice {

using ipc::RequestResult;
using RequestContext = ipc::JSONPeer::RequestContext;

/// The file a `file:` URI names; nullopt for any other URI.
static std::optional<Spelling> uri_to_path(llvm::StringRef uri) {
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

/// True once the server has begun shutting down; document-sync
/// notifications arriving past this point are dropped.
static bool past_shutdown(ServerLifecycle lifecycle) {
    return lifecycle == ServerLifecycle::ShuttingDown || lifecycle == ServerLifecycle::Exited;
}

static ipc::Error hooks_off() {
    return ipc::Error{protocol::ErrorCode::InvalidRequest, "test hooks are not enabled"};
}

/// Fire a workspace refresh request without awaiting the reply. Peer
/// lifetime discipline as in the progress handshake: the peer outlives the
/// client, and teardown fails pending requests before run() returns. A
/// captureless coroutine lambda invoked immediately: parameters are copied
/// into the frame, captures would dangle.
template <typename Params>
static void fire_refresh(kota::event_loop& loop, ipc::JSONPeer& peer, Params params) {
    loop.schedule([](ipc::JSONPeer* peer, Params request) -> kota::task<> {
        // Hang detection: nothing waits on the answer; the bound only frees
        // the request of a client that never answers.
        co_await peer->send_request(request, {.timeout = std::chrono::milliseconds(3000)});
    }(&peer, std::move(params)));
}

/// Awaits `task` with the request holding the project it is served in: a
/// removed project lives on while a request still runs in it.
template <typename T>
static kota::task<T, ipc::Error> holding(std::shared_ptr<ProjectServer> project,
                                         kota::task<T, ipc::Error> task) {
    co_return co_await std::move(task);
}

/// Tick the file trackers of every project, as clice/internal/poll asks.
/// Returns the events of all.
static kota::task<std::uint32_t> tick_trackers(MasterServer& srv, const ext::PollParams& params) {
    bool cdb = params.loop == "cdb";
    std::uint32_t count = 0;
    if(cdb) {
        srv.files.disk.look_flags();
    } else {
        srv.files.disk.look_all();
        // Before the sources walk suspends: the drain the looks scheduled
        // would take the changes uncounted.
        count += static_cast<std::uint32_t>(srv.drain_disk_changes());
    }
    for(std::size_t i = 0; i < srv.projects.size(); i += 1) {
        auto project = srv.projects[i];
        if(!project->tracker) {
            continue;
        }
        if(cdb) {
            count += static_cast<std::uint32_t>(
                project->weigh_databases(params.force.value_or(true)).value_or(1));
            continue;
        }
        auto events = co_await project->tracker->tick_sources();
        count += static_cast<std::uint32_t>(events.size());
        if(!events.empty()) {
            project->dispatch(events);
        }
    }
    co_return count;
}

/// The file a hold or gate is placed on, in the spelling the worker
/// requests carry it under. Refused once the server is shutting down: the
/// stopping pool released every one, and a later one would park work the
/// shutdown waits on.
static std::expected<std::string, ipc::Error> park_target(MasterServer& srv, const Spelling& path) {
    if(past_shutdown(srv.lifecycle)) {
        return std::unexpected(
            ipc::Error{protocol::ErrorCode::InvalidRequest, "the server is shutting down"});
    }
    return std::string(srv.files.resolve(srv.files.intern(path)));
}

LSPClient::LSPClient(MasterServer& server, ipc::JSONPeer& peer) : server(server), peer(peer) {
    output_conn = server.on_output.connect(
        [this](ProjectServer& project, const std::shared_ptr<Session>& session) {
            push_output(project, *session, /*compiled=*/true);
        });
    diagnostics_conn = server.on_diagnostics.connect(
        [this](ProjectServer& project, const std::shared_ptr<Session>& session) {
            push_output(project, *session, /*compiled=*/false);
        });
    projects_conn = server.on_projects_changed.connect([this]() {
        if(client_ready) {
            publish_config_diagnostics();
        }
    });
    progress_conn = server.on_index_progress.connect([this]() { report_index_progress(); });
    compile_phase_conn = server.on_compile_phase.connect(
        [this](Fid path_id, CompilePhase phase) { report_compile_phase(path_id, phase); });
    serving_conn = server.on_serving_index_changed.connect([this]() { refresh_index_served(); });
    inputs_conn = server.on_inputs_changed.connect([this]() { refresh_stale(); });
    held_conn = server.probe.on_held.connect([this](std::uint64_t id) {
        this->peer.send_notification("clice/internal/held", ext::HeldParams{id});
    });

    notify_conn = server.on_notify.connect([this]() {
        if(client_ready) {
            forward_notify_messages();
        }
    });

    register_lifecycle();
    register_document_sync();
    register_language_features();
    register_extensions();
}

void LSPClient::forward_notify_messages() {
    auto first_seq = server.notify_seq - server.notify_log.size();
    for(auto seq = std::max(notify_cursor, first_seq); seq < server.notify_seq; ++seq) {
        auto& message = server.notify_log[static_cast<std::size_t>(seq - first_seq)];
        if(message.kind == logging::NotifyKind::Guidance) {
            // A problem found again, a database reloaded with it still
            // there, is shown once; the log keeps every report.
            if(shown_guidance.insert(message.text).second) {
                peer.send_notification(protocol::ShowMessageParams{
                    .type = protocol::MessageType::Warning,
                    .message = message.text,
                });
            }
        } else {
            peer.send_notification(protocol::LogMessageParams{
                .type = protocol::MessageType::Error,
                .message = message.text,
            });
        }
    }
    notify_cursor = server.notify_seq;
}

/// Pushes `params`; a push that cannot be encoded is logged, not lost unseen.
static void publish(ipc::JSONPeer& peer, const protocol::PublishDiagnosticsParams& params) {
    if(auto sent = peer.send_notification(params); !sent) {
        LOG_WARN("Cannot publish the diagnostics of {}: {}", params.uri, sent.error().message);
    }
}

/// The answer to a diagnostics pull.
static codec::RawValue full_report(std::vector<protocol::Diagnostic> items) {
    return to_raw(protocol::RelatedFullDocumentDiagnosticReport{.items = std::move(items)});
}

std::vector<protocol::Diagnostic>
    LSPClient::to_client(llvm::ArrayRef<feature::FileDiagnostic> diagnostics,
                         llvm::StringRef path,
                         const PositionMap& positions) {
    // FIXME: another file a note points into is read when the diagnostic is
    // shown, as the compile read it from the disk; one that changed since
    // places the note off.
    llvm::StringMap<std::optional<SourceText>> disk;
    auto note_positions = [&](llvm::StringRef note_path) -> std::optional<PositionMap> {
        auto [it, inserted] = disk.try_emplace(note_path);
        if(inserted) {
            if(auto text = vfs::read_text(note_path)) {
                it->second.emplace(std::move(*text));
            }
        }
        return it->second ? std::optional(it->second->positions()) : std::nullopt;
    };
    std::vector<protocol::Diagnostic> shaped;
    for(const auto& diagnostic: diagnostics) {
        shaped.push_back(feature::to_protocol(diagnostic,
                                              related_information,
                                              server.position_encoding,
                                              path,
                                              positions,
                                              note_positions));
    }
    return shaped;
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
    if(!path) {
        // Not a file: no document of ours.
        return ResolvedDoc{.project = this->server.projects.front()};
    }
    auto path_id = this->server.files.intern(*path);
    auto project = this->server.owner_of(path_id).shared_from_this();
    // A document under a second name has a buffer of its own: it is the
    // file — its session, its index — only while both hold the same
    // text.
    auto session = this->server.find_session(path_id);
    if(auto* alias = find_alias(path_id, *path);
       alias && session && alias->buffer.text() != session->text()) {
        return ResolvedDoc{.path = std::move(*path).str(), .project = std::move(project)};
    }
    return ResolvedDoc{std::move(*path).str(), path_id, std::move(session), std::move(project)};
}

LSPClient::AliasDocument* LSPClient::find_alias(Fid path_id, llvm::StringRef spelling) {
    auto it = aliases.find(path_id);
    if(it == aliases.end()) {
        return nullptr;
    }
    auto alias = ranges::find_if(it->second, [&](const AliasDocument& document) {
        return document.spelling == spelling;
    });
    return alias != it->second.end() ? &*alias : nullptr;
}

LSPClient::AliasDocument* LSPClient::find_alias(llvm::StringRef spelling) {
    auto path_id =
        spelling.empty() ? std::nullopt : server.files.find(Spelling::absolute(spelling));
    return path_id ? find_alias(*path_id, spelling) : nullptr;
}

ipc::Error LSPClient::unserved(llvm::StringRef spelling) {
    if(find_alias(spelling)) {
        return content_modified();
    }
    return ipc::Error{protocol::ErrorCode::InvalidParams, "Document not open"};
}

std::string LSPClient::divergence_message(const AliasDocument& alias) {
    return std::format(
        "This file is also open as {}, which clice analyzes; edits here are not "
        "analyzed until the texts agree. Close one of the two.",
        server.files.display(alias.buffer.path_id));
}

void LSPClient::refresh_diagnostics() {
    if(diagnostic_refresh) {
        // The client pulls every document again: no report it pulled is
        // one it holds any more.
        pulled.clear();
        fire_refresh(server.loop, peer, protocol::DiagnosticRefreshParams{});
    }
}

void LSPClient::publish_alias(AliasDocument& alias, const Session* owner, ProjectServer& project) {
    if(!client_ready) {
        return;
    }
    bool diverged = !owner || owner->text() != alias.buffer.text();
    bool flipped = std::exchange(alias.warned, diverged) != diverged;
    if(diverged && flipped) {
        peer.send_notification(protocol::ShowMessageParams{
            .type = protocol::MessageType::Warning,
            .message = divergence_message(alias),
        });
    }
    if(pull_diagnostics) {
        if(flipped) {
            refresh_diagnostics();
        }
        return;
    }
    protocol::PublishDiagnosticsParams params;
    params.uri = feature::to_uri(alias.spelling);
    params.version = alias.buffer.version;
    if(diverged) {
        params.diagnostics = to_client({feature::file_warning(divergence_message(alias))},
                                       alias.spelling,
                                       alias.buffer.positions());
    } else if(auto compiled = project.ast.compiles.latest_at(owner->path_id, owner->version)) {
        params.diagnostics =
            to_client(project.requests.shown_diagnostics(*owner, *compiled->output),
                      alias.spelling,
                      compiled->output->text->positions());
    } else if(!flipped) {
        // Shared once the owner's compile caught up with the text; until
        // then only a divergence warning is taken down.
        // FIXME: a crashed compile leaves no compile at the owner's version, so
        // a push client gets nothing at the alias URI, the crash note included.
        return;
    }
    publish(peer, params);
}

void LSPClient::publish_aliases(Fid path_id) {
    auto it = aliases.find(path_id);
    if(it == aliases.end()) {
        return;
    }
    auto& project = server.owner_of(path_id);
    auto session = server.find_session(path_id);
    for(auto& alias: it->second) {
        publish_alias(alias, session.get(), project);
    }
}

LSPClient::AliasDocument LSPClient::take_alias(Fid path_id, AliasDocument* alias) {
    auto it = aliases.find(path_id);
    auto taken = std::move(*alias);
    it->second.erase(alias);
    if(it->second.empty()) {
        aliases.erase(it);
    }
    return taken;
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
        this->server.pool.foreground_pulse();
        auto& srv = this->server;
        if(srv.lifecycle != ServerLifecycle::Uninitialized) {
            co_await kota::fail(protocol::Error{"Server already initialized"});
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
        LOG_INFO("Initialized with folders: {}", llvm::join(srv.workspace_roots, ", "));

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
        this->forward_notify_messages();
        this->publish_config_diagnostics();

        // Replay compile outputs that materialized while no ready client
        // was attached (documents opened and compiled before the
        // handshake, or while the server ran headless). Only outputs that
        // still describe the current buffer are replayed: an edit during
        // the handshake marks the session dirty, and the next compile
        // pushes fresh results instead.
        for(auto& project: srv.projects) {
            project->sessions.for_each([&](Fid path_id, const Session& session) {
                auto& compiles = project->ast.compiles;
                if(compiles.current(path_id) && compiles.latest_at(path_id, session.version)) {
                    this->push_output(*project, session, /*compiled=*/true);
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

    peer.on_request(
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
                co_await server.drain();
                co_return nullptr;
            }(this->server);
        });

    peer.on_notification([this]([[maybe_unused]] const protocol::ExitParams& params) {
        LOG_INFO("Exit notification received");
        this->server.exit_before_shutdown |= !past_shutdown(this->server.lifecycle);
        this->server.schedule_shutdown();
    });
}

void LSPClient::register_document_sync() {
    peer.on_notification([this](const protocol::DidOpenTextDocumentParams& params) {
        auto& srv = this->server;
        if(past_shutdown(srv.lifecycle))
            return;
        srv.pool.foreground_pulse();

        auto spelled = uri_to_path(params.text_document.uri);
        if(!spelled) {
            LOG_INFO("didOpen: {} names no file; not served", params.text_document.uri);
            return;
        }
        auto& path = spelled->str();
        if(is_other_language_path(path)) {
            LOG_INFO("didOpen: {} is no C-family source; not served", path);
            return;
        }

        // A didOpen racing ahead of the initialize handshake is a client
        // protocol violation, but sessions are plain state with no worker
        // dependency — accept the buffer so the session is in place once
        // the server is ready. A compile attempted before then degrades to
        // an operational worker-unavailable error and leaves the session
        // dirty, so the first post-ready request recompiles it.
        if(srv.lifecycle != ServerLifecycle::Ready) {
            LOG_WARN("didOpen before the server is ready, accepting: {}", path);
        }

        auto path_id = srv.files.intern(*spelled);
        // One file, one buffer: a second document naming it through another
        // path (a symlink) would fold its own edits into the first one's.
        // The document opened first keeps it; this one waits with its own
        // text.
        if(auto owner = srv.files.shown_as(path_id); owner && *owner != path) {
            LOG_WARN("didOpen: {} is already open as {}; serving that one", path, *owner);
            auto& alias = aliases[path_id].emplace_back(
                AliasDocument{.spelling = path, .buffer = {.path_id = path_id}});
            auto& project = srv.owner_of(path_id);
            project.sessions.apply_open(alias.buffer,
                                        params.text_document.text,
                                        params.text_document.version);
            publish_alias(alias, srv.find_session(path_id).get(), project);
            return;
        }
        srv.files.show_as(path_id, path);
        srv.open_session(path_id, params.text_document.text, params.text_document.version);

        LOG_DEBUG("didOpen: {} (v{})", path, params.text_document.version);
    });

    peer.on_notification([this](const protocol::DidChangeTextDocumentParams& params) {
        auto& srv = this->server;
        if(past_shutdown(srv.lifecycle))
            return;
        srv.pool.foreground_pulse();

        // The edit reaches a second name's buffer whether or not it still
        // agrees with the first name's.
        auto spelled = uri_to_path(params.text_document.uri);
        if(!spelled || is_other_language_path(*spelled)) {
            return;
        }
        auto& path = spelled->str();
        auto path_id = srv.files.intern(*spelled);
        auto project = srv.owner_of(path_id).shared_from_this();
        auto session = srv.find_session(path_id);
        if(auto* alias = find_alias(path_id, path)) {
            project->sessions.apply_change(alias->buffer,
                                           params.content_changes,
                                           params.text_document.version,
                                           srv.position_encoding);
            publish_alias(*alias, srv.find_session(path_id).get(), *project);
            return;
        }
        if(!session) {
            // Dropping is the only safe move: without the didOpen baseline
            // there is no buffer to fold the edits into.
            LOG_ERROR("didChange for a document with no open session, dropping: {}", path);
            return;
        }
        if(srv.files.shown_as(path_id) != path) {
            LOG_WARN("didChange for {}, never opened under that name, dropping", path);
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
                                       params.text_document.version,
                                       srv.position_encoding);
        publish_aliases(path_id);

        // The edit just made any in-flight compile stale. Supersede it now
        // instead of waiting for the next AST-backed request to observe
        // it: the round's advisory token releases its dependency waits,
        // and the interrupt keeps a stale parse from holding up its
        // waiters.
        project->ast.supersede(path_id);

        // Editing is the canonical escalation trigger: from here on the
        // session invests in PCH/AST.
        project->ast.escalate(*session);

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
        auto spelled = uri_to_path(params.text_document.uri);
        if(!spelled) {
            return;
        }
        auto& path = spelled->str();
        auto path_id = srv.files.intern(*spelled);
        // A second name's diagnostics end with it, whether it closes or
        // takes the file over.
        auto clear_alias = [&](llvm::StringRef spelling) {
            if(client_ready && !pull_diagnostics) {
                peer.send_notification(
                    protocol::PublishDiagnosticsParams{.uri = feature::to_uri(spelling)});
            }
        };
        if(auto* alias = find_alias(path_id, path)) {
            take_alias(path_id, alias);
            clear_alias(path);
            return;
        }
        if(srv.files.shown_as(path_id) != path) {
            return;
        }
        // LSP versions are scoped to an open document: a reopen restarts
        // them, so a stale entry would misread the fresh document's first
        // compile as an unchanged-text recompile.
        output_versions.erase(path_id);
        pulled.erase(path_id);
        sent_tokens.erase(path_id);
        // The diagnostics clear goes out under the spelling being closed.
        srv.close_session(path_id);
        srv.files.unshow(path_id);
        // A document still open under another name takes the file over.
        if(auto it = aliases.find(path_id); it != aliases.end()) {
            auto next = take_alias(path_id, &it->second.front());
            clear_alias(next.spelling);
            srv.files.show_as(path_id, next.spelling);
            srv.open_session(path_id, next.buffer.text().str(), next.buffer.version);
            // A pulling client still holds its divergence warning.
            if(pull_diagnostics && next.warned) {
                refresh_diagnostics();
            }
        }
    });

    peer.on_notification([this](const protocol::DidSaveTextDocumentParams& params) {
        auto& srv = this->server;
        // Unlike didOpen/didChange, a save arriving before the server is
        // ready needs no special handling: a disk change only invalidates
        // derived state, none of which exists yet — the workspace load at
        // ready reads the saved disk content anyway.
        if(srv.lifecycle != ServerLifecycle::Ready)
            return;
        srv.pool.foreground_pulse();

        auto path = uri_to_path(params.text_document.uri);
        if(!path) {
            return;
        }
        srv.saved(srv.files.intern(*path));

        LOG_DEBUG("didSave: {}", *path);
    });
}

LSPClient::RawResult LSPClient::send_tokens(ResolvedDoc document,
                                            RequestContext& ctx,
                                            TokensRequest request) {
    auto& [path, path_id, session, project] = document;
    return kota::co_invoke(
        [this,
         project,
         path_id,
         session,
         request = std::move(request),
         tokens = project->requests.semantic_tokens(Ticket::take(session),
                                                    ctx.cancellation)]() mutable -> RawResult {
            auto found = co_await std::move(tokens).or_fail();
            if(!found) {
                co_return codec::RawValue{"null"};
            }
            auto positions = session->positions();
            if(request.window) {
                auto window = positions.offset_range(*request.window, server.position_encoding);
                co_return to_raw(feature::to_protocol(*found,
                                                      positions,
                                                      server.position_encoding,
                                                      token_legend,
                                                      window));
            }
            auto encoded =
                feature::to_protocol(*found, positions, server.position_encoding, token_legend);
            next_result_id += 1;
            encoded.result_id = std::to_string(next_result_id);
            auto& sent = sent_tokens[path_id];
            bool held = request.previous && sent.result_id == *request.previous;
            auto previous = std::exchange(sent, {*encoded.result_id, encoded.data});
            if(held) {
                co_return to_raw(protocol::SemanticTokensDelta{
                    .result_id = encoded.result_id,
                    .edits = feature::semantic_tokens_edits(previous.data, encoded.data),
                });
            }
            co_return to_raw(encoded);
        });
}

template <typename Params, typename Answer>
void LSPClient::on_document(Answer answer, DocumentOptions options) {
    peer.on_request([this, answer = std::move(answer), options](RequestContext& ctx,
                                                                const Params& params) -> RawResult {
        server.pool.foreground_pulse();
        auto document = [&] {
            if constexpr(requires { params.item; }) {
                return resolve_uri(params.item.uri);
            } else {
                return resolve_uri(params.text_document.uri);
            }
        }();
        if(!options.closed && !document.session) {
            return kota::outcome_error(unserved(document.path));
        }
        auto project = document.project;
        return holding(std::move(project), answer(ctx, params, std::move(document)));
    });
}

void LSPClient::register_language_features() {
    // The handlers of document requests take their snapshot — the ticket,
    // through the feature's entry point — before they return, as the
    // request is dispatched; their tasks run once the messages read with
    // the request are dispatched too.
    on_document<protocol::HoverParams>(
        [this](RequestContext& ctx, const auto& params, ResolvedDoc document) {
            return document.project->requests.hover(Ticket::take(document.session),
                                                    params.position,
                                                    hover_markup,
                                                    ctx.cancellation);
        });

    on_document<protocol::SemanticTokensParams>(
        [this](RequestContext& ctx, const auto& params, ResolvedDoc document) {
            return send_tokens(std::move(document), ctx, {});
        });

    on_document<protocol::SemanticTokensDeltaParams>(
        [this](RequestContext& ctx, const auto& params, ResolvedDoc document) {
            return send_tokens(std::move(document), ctx, {.previous = params.previous_result_id});
        });

    on_document<protocol::SemanticTokensRangeParams>(
        [this](RequestContext& ctx, const auto& params, ResolvedDoc document) {
            return send_tokens(std::move(document), ctx, {.window = params.range});
        });

    on_document<protocol::InlayHintParams>(
        [this](RequestContext& ctx, const auto& params, ResolvedDoc document) {
            return document.project->requests.inlay_hints(Ticket::take(document.session),
                                                          params.range,
                                                          label_parts,
                                                          ctx.cancellation);
        });

    on_document<protocol::FoldingRangeParams>(
        [this](RequestContext& ctx, const auto& params, ResolvedDoc document) {
            return document.project->requests.folding_ranges(Ticket::take(document.session),
                                                             folding_client,
                                                             ctx.cancellation);
        });

    on_document<protocol::SelectionRangeParams>(
        [this](RequestContext& ctx, const auto& params, ResolvedDoc document) {
            return document.project->requests.selection_ranges(Ticket::take(document.session),
                                                               params.positions,
                                                               ctx.cancellation);
        });

    on_document<protocol::DocumentSymbolParams>(
        [this](RequestContext& ctx, const auto& params, ResolvedDoc document) {
            return document.project->requests.document_symbols(Ticket::take(document.session),
                                                               symbol_client,
                                                               params.text_document.uri,
                                                               ctx.cancellation);
        });

    on_document<protocol::DocumentLinkParams>(
        [this](RequestContext& ctx, const auto& params, ResolvedDoc document) {
            return document.project->requests.document_links(Ticket::take(document.session),
                                                             ctx.cancellation);
        });

    // A pull is always answered with a report, never an error: on an
    // error, clients clear what they hold or stop pulling the document.
    on_document<protocol::DocumentDiagnosticParams>(
        [this](RequestContext& ctx, const auto& params, ResolvedDoc document) {
            return kota::co_invoke([this, document = std::move(document)]() -> RawResult {
                auto& [path, path_id, session, project] = document;
                std::vector<feature::FileDiagnostic> items;
                if(session) {
                    items = co_await project->requests.diagnostics(session);
                }
                // A second name answers the warning once its text parted
                // from the first name's, before the pull or while it waited.
                if(auto* alias = find_alias(path);
                   alias && (!session || alias->buffer.text() != session->text())) {
                    co_return full_report(
                        to_client({feature::file_warning(divergence_message(*alias))},
                                  path,
                                  alias->buffer.positions()));
                }
                // A pull's diagnostics are of the session's text
                // (finished_diagnostics).
                auto report = full_report(session ? to_client(items, path, session->positions())
                                                  : std::vector<protocol::Diagnostic>());
                if(session) {
                    // Recorded against the live session: one that replaced
                    // this pull's meanwhile (the document moved to another
                    // project) compiles the same text, and its commit must
                    // correct the empty answer the closed one gave.
                    if(auto live = server.find_session(path_id)) {
                        pulled[path_id] = {
                            .version = live->version,
                            .report = llvm::xxh3_64bits(report.data),
                        };
                    }
                }
                co_return report;
            });
        },
        {.closed = true});

    on_document<protocol::CodeActionParams>(
        [this](RequestContext& ctx, const auto& params, ResolvedDoc document) {
            auto only = params.context.only.value_or(std::vector<protocol::CodeActionKind>());
            // Only a user asking for actions gets the switches, which look
            // the listings up: an editor asks on every move of the cursor.
            bool offer_switches =
                params.context.trigger_kind != protocol::CodeActionTriggerKind::Automatic;
            auto& project = document.project;
            auto actions = project->requests.code_actions(Ticket::take(document.session),
                                                          params.range,
                                                          only,
                                                          ctx.cancellation);
            return kota::co_invoke([this,
                                    project,
                                    path_id = document.path_id,
                                    offer_switches,
                                    only = std::move(only),
                                    actions = std::move(actions)]() mutable -> RawResult {
                auto result = co_await std::move(actions).or_fail();
                if(!versioned_edits) {
                    for(auto& action: result) {
                        unversion(*action.edit);
                    }
                }
                // Listed after the compile the actions waited for: a
                // header's first one decides whether it is self-contained,
                // which changes its listing and outdates the epoch before.
                auto live = project->sessions.find(path_id);
                if(!offer_switches || !live) {
                    co_return to_raw(result);
                }
                auto uri = feature::to_uri(server.files.display(path_id));
                if(feature::admits(only, context_action_kind)) {
                    auto current = server.current_context(*project, live.get());
                    auto listing = server.query_contexts(path_id, {.uri = uri});
                    result.append_range(context_actions(uri, current, listing));
                }
                if(feature::admits(only, configuration_action_kind)) {
                    result.append_range(
                        configuration_actions(uri, project->context_service.list_configurations()));
                }
                co_return to_raw(result);
            });
        });

    // The index answers navigation for a document that is not open: an
    // empty result is a real answer, never an error.
    on_document<protocol::DefinitionParams>(
        [](RequestContext& ctx, const auto& params, ResolvedDoc document) {
            return document.project->requests.definition(Ticket::take(document.session),
                                                         document.path_id,
                                                         params.position,
                                                         ctx.cancellation);
        },
        {.closed = true});

    on_document<protocol::ReferenceParams>(
        [](RequestContext& ctx, const auto& params, ResolvedDoc document) {
            return document.project->requests.references(Ticket::take(document.session),
                                                         document.path_id,
                                                         params.position,
                                                         params.context.include_declaration);
        },
        {.closed = true});

    on_document<protocol::DocumentHighlightParams>(
        [](RequestContext& ctx, const auto& params, ResolvedDoc document) {
            return document.project->requests.document_highlights(Ticket::take(document.session),
                                                                  document.path_id,
                                                                  params.position);
        },
        {.closed = true});

    on_document<protocol::DeclarationParams>(
        [](RequestContext& ctx, const auto& params, ResolvedDoc document) {
            return document.project->requests.declaration(Ticket::take(document.session),
                                                          document.path_id,
                                                          params.position);
        },
        {.closed = true});

    on_document<protocol::TypeDefinitionParams>(
        [](RequestContext& ctx, const auto& params, ResolvedDoc document) {
            return document.project->requests.type_definition(Ticket::take(document.session),
                                                              document.path_id,
                                                              params.position);
        },
        {.closed = true});

    on_document<protocol::ImplementationParams>(
        [](RequestContext& ctx, const auto& params, ResolvedDoc document) {
            return document.project->requests.implementation(Ticket::take(document.session),
                                                             document.path_id,
                                                             params.position);
        },
        {.closed = true});

    on_document<protocol::PrepareRenameParams>(
        [](RequestContext& ctx, const auto& params, ResolvedDoc document) {
            return document.project->requests.prepare_rename(Ticket::take(document.session),
                                                             document.path_id,
                                                             params.position);
        },
        {.closed = true});

    on_document<protocol::RenameParams>(
        [this](RequestContext& ctx, const auto& params, ResolvedDoc document) {
            return kota::co_invoke([this,
                                    renamed = document.project->requests.rename(
                                        Ticket::take(document.session),
                                        document.path_id,
                                        params.position,
                                        params.new_name)]() mutable -> RawResult {
                auto result = co_await std::move(renamed).or_fail();
                if(!result) {
                    co_return codec::RawValue{"null"};
                }
                auto& [edit, notice] = *result;
                if(!versioned_edits) {
                    unversion(edit);
                }
                if(!notice.empty()) {
                    peer.send_notification(protocol::ShowMessageParams{
                        .type = protocol::MessageType::Warning,
                        .message = std::move(notice),
                    });
                }
                co_return to_raw(edit);
            });
        },
        {.closed = true});

    on_document<protocol::CallHierarchyPrepareParams>(
        [](RequestContext& ctx, const auto& params, ResolvedDoc document) {
            return document.project->requests.call_hierarchy_prepare(Ticket::take(document.session),
                                                                     document.path_id,
                                                                     params.position);
        },
        {.closed = true});

    on_document<protocol::CallHierarchyIncomingCallsParams>(
        [](RequestContext& ctx, const auto& params, ResolvedDoc document) {
            return document.project->requests.call_hierarchy_incoming(document.path_id,
                                                                      params.item);
        },
        {.closed = true});

    on_document<protocol::CallHierarchyOutgoingCallsParams>(
        [](RequestContext& ctx, const auto& params, ResolvedDoc document) {
            return document.project->requests.call_hierarchy_outgoing(document.path_id,
                                                                      params.item);
        },
        {.closed = true});

    on_document<protocol::TypeHierarchyPrepareParams>(
        [](RequestContext& ctx, const auto& params, ResolvedDoc document) {
            return document.project->requests.type_hierarchy_prepare(Ticket::take(document.session),
                                                                     document.path_id,
                                                                     params.position);
        },
        {.closed = true});

    on_document<protocol::TypeHierarchySupertypesParams>(
        [](RequestContext& ctx, const auto& params, ResolvedDoc document) {
            return document.project->requests.type_hierarchy_supertypes(document.path_id,
                                                                        params.item);
        },
        {.closed = true});

    on_document<protocol::TypeHierarchySubtypesParams>(
        [](RequestContext& ctx, const auto& params, ResolvedDoc document) {
            return document.project->requests.type_hierarchy_subtypes(document.path_id,
                                                                      params.item);
        },
        {.closed = true});

    on_document<protocol::CompletionParams>(
        [this](RequestContext& ctx, const auto& params, ResolvedDoc document) {
            llvm::StringRef trigger;
            if(params.context && params.context->trigger_character) {
                trigger = *params.context->trigger_character;
            }
            return document.project->requests.code_completion(document.session,
                                                              params.position,
                                                              completion_client,
                                                              trigger,
                                                              ctx.cancellation);
        });

    on_document<protocol::SignatureHelpParams>(
        [](RequestContext& ctx, const auto& params, ResolvedDoc document) {
            // The popup the client shows: the user may have moved it to
            // another overload.
            std::string selected;
            if(params.context && params.context->active_signature_help) {
                auto& shown = *params.context->active_signature_help;
                auto active = shown.active_signature.value_or(0);
                if(active < shown.signatures.size()) {
                    selected = shown.signatures[active].label;
                }
            }
            return document.project->requests.signature_help(document.session,
                                                             params.position,
                                                             std::move(selected),
                                                             ctx.cancellation);
        });

    on_document<protocol::DocumentFormattingParams>(
        [](RequestContext& ctx, const auto& params, ResolvedDoc document) {
            return document.project->requests.formatting(document.session,
                                                         std::nullopt,
                                                         ctx.cancellation);
        });

    on_document<protocol::DocumentRangeFormattingParams>(
        [](RequestContext& ctx, const auto& params, ResolvedDoc document) {
            return document.project->requests.formatting(document.session,
                                                         params.range,
                                                         ctx.cancellation);
        });

    peer.on_request(
        [this](RequestContext& ctx, const protocol::WorkspaceSymbolParams& params) -> RawResult {
            this->server.pool.foreground_pulse();
            co_return to_raw(this->server.workspace_symbols(params.query));
        });
}

kota::task<ext::SwitchContextResult>
    LSPClient::switch_context(const ext::SwitchContextParams& params) {
    auto path = uri_to_path(params.uri);
    auto context_path = uri_to_path(params.context_uri);
    if(!path || !context_path) {
        co_return ext::SwitchContextResult{};
    }
    // The session reset lives inside switch_context (single owner,
    // synchronous, no cross-file cascade — exempt from the event pipeline;
    // see the Invalidator charter).
    co_return co_await server.switch_context(server.files.intern(*path),
                                             server.files.intern(*context_path),
                                             params);
}

ext::SwitchContextResult LSPClient::reset_context(const ext::ResetContextParams& params) {
    auto path = uri_to_path(params.uri);
    if(!path) {
        return {};
    }
    return server.reset_context(server.files.intern(*path));
}

std::shared_ptr<ProjectServer> LSPClient::configured(const std::optional<std::string>& uri) {
    if(uri) {
        return resolve_uri(*uri).project;
    }
    auto& projects = server.projects;
    auto it = ranges::find_if(projects, [](auto& project) { return !project->root.empty(); });
    return it != projects.end() ? *it : projects.front();
}

ext::SwitchConfigurationResult
    LSPClient::switch_configuration(const ext::SwitchConfigurationParams& params) {
    return configured(params.uri)
        ->context_service.switch_configuration(params.name, server.requested_configuration);
}

/// The one argument of `params`, the parameters of the request its command
/// stands for.
template <typename Params>
static std::expected<Params, ipc::Error> argument_of(const protocol::ExecuteCommandParams& params) {
    Params decoded;
    if(!params.arguments || params.arguments->size() != 1 ||
       !codec::dyn::from_dyn<ipc::lsp_config>(params.arguments->front(), decoded)) {
        return std::unexpected(ipc::Error{
            protocol::ErrorCode::InvalidParams,
            std::format("{} takes one argument, the parameters of its request", params.command)});
    }
    return decoded;
}

LSPClient::RawResult LSPClient::execute_command(protocol::ExecuteCommandParams params) {
    // A client with no UI for these tells the user nothing of the outcome;
    // the server does.
    auto tell = [this](protocol::MessageType type, std::string message) {
        peer.send_notification(
            protocol::ShowMessageParams{.type = type, .message = std::move(message)});
    };
    if(params.command == switch_context_command || params.command == reset_context_command) {
        ext::SwitchContextResult result;
        if(params.command == switch_context_command) {
            auto argument = argument_of<ext::SwitchContextParams>(params);
            if(!argument) {
                co_await kota::fail(std::move(argument.error()));
            }
            result = co_await switch_context(*argument);
        } else {
            auto argument = argument_of<ext::ResetContextParams>(params);
            if(!argument) {
                co_await kota::fail(std::move(argument.error()));
            }
            result = reset_context(*argument);
        }
        if(result.stale) {
            tell(protocol::MessageType::Warning,
                 "clice: the contexts changed meanwhile; ask for the actions again");
        } else if(!result.success) {
            tell(protocol::MessageType::Warning, "clice: cannot switch the compilation context");
        }
        co_return to_raw(result);
    }
    if(params.command == switch_configuration_command) {
        auto argument = argument_of<ext::SwitchConfigurationParams>(params);
        if(!argument) {
            co_await kota::fail(std::move(argument.error()));
        }
        auto result = switch_configuration(*argument);
        if(result.success) {
            tell(protocol::MessageType::Info,
                 std::format("clice: {} applies once clice restarts", argument->name));
        } else {
            tell(protocol::MessageType::Warning,
                 std::format("clice: cannot switch the build configuration: {}", *result.reason));
        }
        co_return to_raw(result);
    }
    co_await kota::fail(ipc::Error{protocol::ErrorCode::InvalidParams,
                                   std::format("no command {}", params.command)});
}

void LSPClient::register_extensions() {
    // ── Compilation context helpers ─────────────────────────────────

    peer.on_request(
        "clice/queryContext",
        [this](RequestContext& ctx, const ext::QueryContextParams& params) -> RawResult {
            this->server.pool.foreground_pulse();
            auto path = uri_to_path(params.uri);
            if(!path) {
                co_return to_raw(ext::QueryContextResult{});
            }
            co_return to_raw(this->server.query_contexts(this->server.files.intern(*path), params));
        });

    peer.on_request(
        "clice/currentContext",
        [this](RequestContext& ctx, const ext::CurrentContextParams& params) -> RawResult {
            this->server.pool.foreground_pulse();
            auto [path, path_id, session, project] = resolve_uri(params.uri);
            co_return to_raw(this->server.current_context(*project, session.get()));
        });

    peer.on_request(
        "clice/switchContext",
        [this](RequestContext& ctx, const ext::SwitchContextParams& params) -> RawResult {
            this->server.pool.foreground_pulse();
            co_return to_raw(co_await switch_context(params));
        });

    peer.on_request(
        "clice/resetContext",
        [this](RequestContext& ctx, const ext::ResetContextParams& params) -> RawResult {
            this->server.pool.foreground_pulse();
            co_return to_raw(reset_context(params));
        });

    peer.on_request(
        "clice/listConfigurations",
        [this](RequestContext& ctx, const ext::ListConfigurationsParams& params) -> RawResult {
            co_return to_raw(configured(params.uri)->context_service.list_configurations());
        });

    peer.on_request(
        "clice/switchConfiguration",
        [this](RequestContext& ctx, const ext::SwitchConfigurationParams& params) -> RawResult {
            co_return to_raw(switch_configuration(params));
        });

    peer.on_request(
        [this](RequestContext& ctx, const protocol::ExecuteCommandParams& params) -> RawResult {
            this->server.pool.foreground_pulse();
            co_return co_await execute_command(params).or_fail();
        });

    peer.on_request(
        "clice/counterparts",
        [this](RequestContext& ctx, const ext::CounterpartsParams& params) -> RawResult {
            this->server.pool.foreground_pulse();
            auto resolved = resolve_uri(params.uri);
            ext::CounterpartsResult result;
            if(resolved.path.empty()) {
                co_return to_raw(result);
            }
            auto& project = *resolved.project;
            query::Context context{.project = project.project,
                                   .contexts = project.contexts,
                                   .query = project.index_query};
            auto found = query::counterparts(context, Spelling::absolute(resolved.path));
            // A buffer not on disk yet has no counterparts to offer.
            if(!found) {
                co_return to_raw(result);
            }
            for(auto& candidate: found->candidates) {
                result.candidates.push_back({.uri = feature::to_uri(candidate.path),
                                             .reasons = std::move(candidate.reasons)});
            }
            if(found->preferred) {
                result.preferred = feature::to_uri(*found->preferred);
            }
            co_return to_raw(result);
        });

    // ── Test hooks ──────────────────────────────────────────────────

    // Runs one file-tracker tick (see ext::PollParams).
    // Test-only and not a stable API.
    peer.on_request("clice/internal/poll",
                    [this](RequestContext& ctx, const ext::PollParams& params) -> RawResult {
                        auto& srv = this->server;
                        if(params.loop != "cdb" && params.loop != "workspace") {
                            co_await kota::fail(ipc::Error{protocol::ErrorCode::InvalidParams,
                                                           R"(loop must be "cdb" or "workspace")"});
                        }
                        if(ranges::none_of(srv.projects, [](auto& project) {
                               return project->tracker != nullptr;
                           })) {
                            co_await kota::fail(ipc::Error{protocol::ErrorCode::InvalidRequest,
                                                           "No workspace is loaded"});
                        }
                        co_return to_raw(ext::PollResult{co_await tick_trackers(srv, params)});
                    });

    peer.on_request(
        "clice/internal/sync",
        [this](RequestContext& ctx, const ext::SyncParams& params) -> RawResult {
            auto& srv = this->server;
            if(!srv.test_hooks()) {
                co_await kota::fail(hooks_off());
            }
            // The deadline counts from the request; a poll runs to its end,
            // a tick cut short would leave the trackers out of step.
            auto deadline = std::chrono::steady_clock::now() +
                            std::chrono::milliseconds(params.deadline_ms.value_or(240'000));
            if(params.poll.value_or(false)) {
                co_await tick_trackers(srv, {.loop = "workspace"});
            }
            auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::max(deadline - std::chrono::steady_clock::now(),
                         std::chrono::steady_clock::duration::zero()));
            // Hang detection: past the deadline, the answer names what
            // still runs.
            co_await kota::when_any(srv.wait_idle(this->peer), kota::sleep(left, srv.loop));
            ext::SyncResult result;
            for(auto& project: srv.projects) {
                for(auto id: project->sched.pump.failed()) {
                    result.failed.push_back(feature::to_uri(srv.files.display(id)));
                }
                result.unsaved = result.unsaved || has_unsaved_state(project->project);
            }
            ranges::sort(result.failed);
            result.pending = srv.pending_work(this->peer);
            co_return to_raw(result);
        });

    peer.on_request(
        "clice/internal/hold",
        [this](RequestContext& ctx, const ext::HoldParams& params) -> RawResult {
            auto& srv = this->server;
            if(!srv.test_hooks()) {
                co_await kota::fail(hooks_off());
            }
            auto kind = parse_build_kind(params.kind);
            auto path = uri_to_path(params.uri);
            if(!kind || !path) {
                co_await kota::fail(ipc::Error{
                    protocol::ErrorCode::InvalidParams,
                    R"(a hold names a kind of "compile", "pch", "pcm" or "index", and a file URI)"});
            }
            auto file = park_target(srv, *path);
            if(!file) {
                co_await kota::fail(std::move(file.error()));
            }
            co_return to_raw(ext::HoldResult{srv.probe.hold(*kind, std::move(*file))});
        });

    peer.on_request(
        "clice/internal/gate",
        [this](RequestContext& ctx, const ext::GateParams& params) -> RawResult {
            auto& srv = this->server;
            if(!srv.test_hooks()) {
                co_await kota::fail(hooks_off());
            }
            auto path = uri_to_path(params.uri);
            if(params.request.empty() || !path) {
                co_await kota::fail(ipc::Error{protocol::ErrorCode::InvalidParams,
                                               "a gate names a worker request and a file URI"});
            }
            auto file = park_target(srv, *path);
            if(!file) {
                co_await kota::fail(std::move(file.error()));
            }
            co_return to_raw(ext::HoldResult{
                srv.probe.gate(std::format("clice/worker/{} {}", params.request, *file))});
        });

    peer.on_request("clice/internal/release",
                    [this](RequestContext& ctx, const ext::ReleaseParams& params) -> RawResult {
                        auto& srv = this->server;
                        if(!srv.test_hooks()) {
                            co_await kota::fail(hooks_off());
                        }
                        if(!srv.probe.release(params.id)) {
                            co_await kota::fail(ipc::Error{protocol::ErrorCode::InvalidParams,
                                                           std::format("no hold {}", params.id)});
                        }
                        co_return to_raw(ext::ReleaseResult{});
                    });

    peer.on_request("clice/internal/watch",
                    [this](RequestContext& ctx, const ext::WatchParams& params) -> RawResult {
                        auto& srv = this->server;
                        if(!srv.test_hooks()) {
                            co_await kota::fail(hooks_off());
                        }
                        auto event = parse_loop_event(params.event);
                        if(!event) {
                            co_await kota::fail(ipc::Error{
                                protocol::ErrorCode::InvalidParams,
                                R"(a watch names "databaseChange", "diskChange" or "checkpoint")"});
                        }
                        co_return to_raw(ext::HoldResult{srv.probe.watch(*event)});
                    });

    peer.on_request("clice/internal/logFlood",
                    [this](RequestContext& ctx, const ext::LogFloodParams& params) -> RawResult {
                        // Load-generating hook: a stray client must not be able to
                        // bloat the file log.
                        if(!this->server.test_hooks()) {
                            co_await kota::fail(hooks_off());
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
    // master state on the event loop, so an assertion made after a finished
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
                stats.pch_stashed_bytes += served->sched.pch.stashed_bytes();

                stats.index_inmemory_shards += project.index.pending_shard_writes();
                stats.index_shard_content_bytes += project.index.shard_bytes();
                stats.last_save_shards += project.index.last_save_shards();

                if(project.store) {
                    stats.pending_tmp_files +=
                        static_cast<std::uint32_t>(project.store->pending_tmp_files());
                }

                stats.header_contexts +=
                    static_cast<std::uint32_t>(served->contexts.header_contexts.size());
                stats.synthesized_contexts += static_cast<std::uint32_t>(ranges::count_if(
                    views::transform(served->contexts.header_contexts,
                                     [](auto& entry) -> auto& { return entry.second; }),
                    [](const HeaderContext& context) { return context.synthesized != nullptr; }));
                stats.sessions += static_cast<std::uint32_t>(served->sessions.sessions.size());
                stats.import_scans += served->sched.pcm.import_scans;
                stats.index_idle = stats.index_idle && served->sched.pump.is_idle();
            }
            stats.workers = {.stateful = this->server.pool.worker_pids(true),
                             .stateless = this->server.pool.worker_pids(false)};
            stats.checks_looked = this->server.files.disk.checks.looked;
            stats.checks_trusted = this->server.files.disk.checks.trusted;
            for(auto& entry: this->server.probe.builds) {
                auto& counts = entry.getValue();
                stats.builds.push_back({
                    .uri = feature::to_uri(this->server.files.display(
                        this->server.files.intern(Spelling::absolute(entry.getKey())))),
                    .compile = counts[std::to_underlying(BuildKind::Compile)],
                    .pch = counts[std::to_underlying(BuildKind::PCH)],
                    .pcm = counts[std::to_underlying(BuildKind::PCM)],
                    .index = counts[std::to_underlying(BuildKind::Index)],
                });
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
    llvm::StringMap<std::vector<feature::FileDiagnostic>> by_file;
    // An issue is at the file as loaded; one that changed since is about to
    // load again. One that is gone places its issues at the top.
    llvm::StringMap<SourceText> texts;
    auto text_of = [&](llvm::StringRef file) -> SourceText& {
        auto it = texts.find(file);
        if(it == texts.end()) {
            auto read = vfs::read_text(file);
            it = texts.try_emplace(file, read ? std::move(*read) : std::string()).first;
        }
        return it->second;
    };
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
            // Issue positions are 1-based, their columns counted in code
            // points. An unknown position (0) maps to the file top.
            auto positions = text_of(issue.file).positions();
            auto offset = [&](std::uint32_t line, std::uint32_t column) {
                return positions.offset_clamped(
                    {.line = line > 0 ? line - 1 : 0, .character = column > 0 ? column - 1 : 0},
                    PositionEncoding::UTF32);
            };
            by_file[issue.file].push_back({
                .range = {offset(issue.line, issue.column),
                          offset(issue.end_line, issue.end_column)},
                .level = issue.severity == ConfigIssue::Severity::Error ? DiagnosticLevel::Error
                                                                        : DiagnosticLevel::Warning,
                .message = issue.message,
            });
            published_configs.insert(issue.file);
        }
        published_configs.insert(project->config_path);
    }

    for(auto& [file, diagnostics]: by_file) {
        protocol::PublishDiagnosticsParams params;
        params.uri = feature::to_uri(server.files.display_identity(file));
        if(!diagnostics.empty()) {
            params.diagnostics = to_client(diagnostics, file, text_of(file).positions());
        }
        publish(peer, params);
    }
}

void LSPClient::push_output(ProjectServer& project, const Session& session, bool compiled) {
    // Held back until the handshake completes (the LSP spec forbids
    // publishDiagnostics before the initialize response); the output stays
    // materialized in the latest compile and the initialized handler replays it.
    if(!client_ready) {
        return;
    }
    auto latest = project.ast.compiles.latest(session.path_id);
    if(!latest || !latest->output.has_value()) {
        return;
    }
    auto& output = *latest->output;

    if(pull_diagnostics) {
        if(auto it = pulled.find(session.path_id);
           it != pulled.end() && it->second.version == session.version &&
           it->second.report !=
               llvm::xxh3_64bits(
                   full_report(to_client(project.requests.finished_diagnostics(session),
                                         server.files.display(session.path_id),
                                         session.positions()))
                       .data)) {
            refresh_diagnostics();
        }
    } else {
        protocol::PublishDiagnosticsParams params;
        params.uri = feature::to_uri(server.files.display(session.path_id));
        params.version = output.version;
        params.diagnostics = to_client(project.requests.shown_diagnostics(session, output),
                                       server.files.display(session.path_id),
                                       output.text->positions());
        publish(peer, params);
        publish_aliases(session.path_id);
    }

    // Two cases make the client re-pull whole-document results it already
    // holds: results computed from the index served while this compile was pending
    // (their answers just got better), and a compile that finished for
    // unchanged text — a context switch or a changed dependency reshapes
    // tokens (including their inactive regions), hints and folds with no
    // didChange to trigger the client's own re-pull. Edit-driven compiles
    // need neither: the client re-pulls on didChange and that pull awaits
    // the fresh AST. A compile that failed before parsing reshaped nothing.
    if(compiled && output.version.has_value() && !output.failed) {
        auto [it, inserted] = output_versions.try_emplace(session.path_id, *output.version);
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
    // Inlay hints stay out: the index never computes them, so a
    // background merge cannot have changed what the client holds.
    if(semantic_tokens_refresh) {
        fire_refresh(server.loop, peer, protocol::SemanticTokensRefreshParams{});
    }
    if(folding_range_refresh) {
        fire_refresh(server.loop, peer, protocol::FoldingRangeRefreshParams{});
    }
}

void LSPClient::refresh_stale() {
    if(!client_ready) {
        return;
    }
    if(pull_diagnostics) {
        refresh_diagnostics();
    } else if(semantic_tokens_refresh) {
        fire_refresh(server.loop, peer, protocol::SemanticTokensRefreshParams{});
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
                // Hang detection: the bound ends the handshake of a client
                // that never responds; a short one drops the progress of a
                // round whose client is merely busy.
                auto create_result =
                    co_await state->reporter->create({.timeout = std::chrono::minutes(1)});
                state->create_inflight = false;
                // The whole connection may have been torn down while the
                // handshake was in flight; drop the token without announcing it.
                if(state->abandoned || create_result.has_error()) {
                    state->reporter.reset();
                    co_return;
                }
                // The client keeps a created token until it ends, and refuses
                // to create it again meanwhile: a round that ended during the
                // handshake ends it, or no later round could show.
                if(!state->round_active) {
                    state->reporter->begin("Indexing");
                    state->reporter->end();
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

/// What a compile in `phase` waits on, as its progress says.
static std::string_view describe(CompilePhase phase) {
    switch(phase) {
        case CompilePhase::Command: return "finding its command";
        case CompilePhase::Modules: return "building modules";
        case CompilePhase::Preamble: return "building preamble";
        case CompilePhase::Parsing: return "parsing";
        case CompilePhase::Idle: break;
    }
    std::unreachable();
}

void LSPClient::report_compile_phase(Fid path_id, CompilePhase phase) {
    if(!work_done_progress || !client_ready) {
        return;
    }
    auto [it, inserted] = compile_progress.try_emplace(path_id);
    if(inserted) {
        if(phase == CompilePhase::Idle) {
            compile_progress.erase(it);
            return;
        }
        it->second = std::make_shared<CompileProgress>(CompileProgress{.phase = phase});
        server.loop.schedule(announce_compile(it->second, path_id));
        return;
    }
    auto& state = *it->second;
    state.phase = phase;
    // Before the announcement, its begin() reads the phase.
    if(!state.reporter) {
        return;
    }
    if(phase == CompilePhase::Idle) {
        state.reporter->end();
        compile_progress.erase(it);
    } else {
        state.reporter->report(std::string(describe(phase)));
    }
}

kota::task<> LSPClient::announce_compile(std::shared_ptr<CompileProgress> state, Fid path_id) {
    // Time is the feature: the compile after an edit mostly finishes
    // sooner, and a progress shown for each would blink in the editor.
    co_await kota::sleep(std::chrono::milliseconds(500));
    if(state->abandoned) {
        co_return;
    }
    if(state->phase == CompilePhase::Idle) {
        compile_progress.erase(path_id);
        co_return;
    }
    auto shown = server.files.display(path_id);
    lsp::ProgressReporter<ipc::JSONPeer> reporter(
        peer,
        protocol::ProgressToken(std::format("clice/compile/{}", feature::to_uri(shown))));
    // Hang detection: the bound ends the handshake of a client that never
    // answers, as the index progress's does.
    auto created = co_await reporter.create({.timeout = std::chrono::minutes(1)});
    if(state->abandoned) {
        co_return;
    }
    if(created.has_error()) {
        compile_progress.erase(path_id);
        co_return;
    }
    // The client keeps a created token until it ends, and refuses to create
    // it again meanwhile: a compile that ended during the handshake ends it.
    if(state->phase == CompilePhase::Idle) {
        reporter.begin(std::string(path::filename(shown)));
        reporter.end();
        compile_progress.erase(path_id);
        co_return;
    }
    reporter.begin(std::string(path::filename(shown)), std::string(describe(state->phase)));
    state->reporter.emplace(std::move(reporter));
}

LSPClient::~LSPClient() {
    // A progress handshake may still be awaiting the client; it holds this
    // state alive and checks the flag before touching the peer.
    index_progress->abandoned = true;
    for(auto& [path_id, state]: compile_progress) {
        state->abandoned = true;
    }
}

}  // namespace clice
