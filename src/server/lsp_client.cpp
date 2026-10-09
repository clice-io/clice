module;

#include "modules/prelude.h"

#include "version.h"
#include "support/logging.macros.h"

module clice;

import :command.argument_parser;
import :feature.feature;
import :semantic.symbol;
import :server.editor_context;
import :server.extension;
import :server.file_tracker;
import :server.format;
import :server.lsp_client;
import :server.master_server;
import :server.uri;
import :support.anomaly;
import :support.logging;
import :syntax.preamble_synthesis;
import :vfs.path;
import :worker.probe;
import :worker.serialize;

namespace clice {

namespace protocol = kota::ipc::protocol;
namespace refl = kota::meta;
using kota::ipc::RequestResult;
using RequestContext = kota::ipc::JSONPeer::RequestContext;

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
static void fire_refresh(kota::event_loop& loop, kota::ipc::JSONPeer& peer, Params params) {
    loop.schedule([](kota::ipc::JSONPeer* peer, Params request) -> kota::task<> {
        co_await peer->send_request(request, {.timeout = std::chrono::milliseconds(3000)});
    }(&peer, std::move(params)));
}

/// Awaits `task` with the request holding the project it is served in: a
/// removed project lives on while a request still runs in it.
template <typename T>
static kota::task<T, kota::ipc::Error> holding(std::shared_ptr<ProjectServer> project,
                                               kota::task<T, kota::ipc::Error> task) {
    co_return co_await std::move(task);
}

/// Tick the file trackers of every project, as clice/internal/poll asks:
/// the database loop (`force` skips its gates, see ext::PollParams) or the
/// workspace loop. Every project ticks; returns the events of all.
static kota::task<std::uint32_t> tick_trackers(MasterServer& srv, bool cdb, bool force) {
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
        llvm::SmallVector<FileEvent> events;
        if(cdb) {
            events = project->tracker->tick_cdb(force);
        } else {
            events = co_await project->tracker->tick_sources();
        }
        count += static_cast<std::uint32_t>(events.size());
        if(!events.empty()) {
            project->dispatch(events);
        }
    }
    co_return count;
}

LSPClient::LSPClient(MasterServer& server, kota::ipc::JSONPeer& peer) : server(server), peer(peer) {
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
    held_conn = server.probe.on_held.connect([this](std::uint64_t id) {
        this->peer.send_notification("clice/internal/held", ext::HeldParams{id});
    });

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

/// Pushes `params`; a push that cannot be encoded is logged, not lost unseen.
static void publish(kota::ipc::JSONPeer& peer, const protocol::PublishDiagnosticsParams& params) {
    if(auto sent = peer.send_notification(params); !sent) {
        LOG_WARN("Cannot publish the diagnostics of {}: {}", params.uri, sent.error().message);
    }
}

/// The answer to a diagnostics pull.
static kota::codec::RawValue full_report(std::vector<protocol::Diagnostic> items) {
    return to_raw(protocol::RelatedFullDocumentDiagnosticReport{.items = std::move(items)});
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
    // file — its session, its index rows — only while both hold the same
    // text.
    auto session = this->server.find_session(path_id);
    if(auto* alias = find_alias(path_id, *path);
       alias && session && alias->buffer.text != session->text) {
        return ResolvedDoc{.path = path->str(), .project = std::move(project)};
    }
    return ResolvedDoc{path->str(), path_id, std::move(session), std::move(project)};
}

LSPClient::AliasDocument* LSPClient::find_alias(Fid path_id, llvm::StringRef spelling) {
    auto it = aliases.find(path_id);
    if(it == aliases.end()) {
        return nullptr;
    }
    auto alias = llvm::find_if(it->second, [&](const AliasDocument& document) {
        return document.spelling == spelling;
    });
    return alias != it->second.end() ? &*alias : nullptr;
}

LSPClient::AliasDocument* LSPClient::find_alias(llvm::StringRef spelling) {
    auto path_id =
        spelling.empty() ? std::nullopt : server.files.find(Spelling::absolute(spelling));
    return path_id ? find_alias(*path_id, spelling) : nullptr;
}

kota::ipc::Error LSPClient::unserved(llvm::StringRef spelling) {
    return find_alias(spelling) ? content_modified() : document_not_open();
}

std::string LSPClient::divergence_message(const AliasDocument& alias) {
    return std::format(
        "This file is also open as {}, which clice analyzes; edits here are not "
        "analyzed until the texts agree. Close one of the two.",
        server.files.display(alias.buffer.path_id));
}

void LSPClient::refresh_diagnostics() {
    if(diagnostic_refresh) {
        fire_refresh(server.loop, peer, protocol::DiagnosticRefreshParams{});
    }
}

void LSPClient::publish_alias(AliasDocument& alias, const Session* owner, ProjectServer& project) {
    if(!client_ready) {
        return;
    }
    bool diverged = !owner || owner->text != alias.buffer.text;
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
        params.diagnostics.push_back(feature::file_warning(divergence_message(alias)));
    } else if(auto projection =
                  project.ast.projections.projection_at(owner->path_id, owner->version)) {
        params.diagnostics = format_diagnostics(*projection->output);
        append_crash_notes(*owner, params.diagnostics);
    } else if(!flipped) {
        // Shared once the owner's compile caught up with the text; until
        // then only a divergence warning is taken down.
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

void LSPClient::register_lifecycle() {
    using StringVec = std::vector<std::string>;

    peer.on_request([this](RequestContext& ctx, const protocol::InitializeParams& params)
                        -> RequestResult<protocol::InitializeParams> {
        this->server.pool.foreground_pulse();
        auto& srv = this->server;
        if(srv.lifecycle != ServerLifecycle::Uninitialized) {
            co_await kota::fail(protocol::Error{"Server already initialized"});
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
        if(params.process_id && srv.client_on_host) {
            srv.watch_client(static_cast<std::uint32_t>(*params.process_id));
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

        label_parts = params.capabilities.text_document.has_value() &&
                      params.capabilities.text_document->inlay_hint.has_value();

        if(params.capabilities.text_document.has_value() &&
           params.capabilities.text_document->folding_range.has_value()) {
            line_folding_only = params.capabilities.text_document->folding_range->line_folding_only;
        }

        if(params.capabilities.text_document.has_value() &&
           params.capabilities.text_document->completion.has_value() &&
           params.capabilities.text_document->completion->completion_item.has_value()) {
            auto& item = *params.capabilities.text_document->completion->completion_item;
            completion_client = {
                .snippets = item.snippet_support,
                .insert_replace = item.insert_replace_support,
            };
        }

        if(params.initialization_options.has_value() && !params.initialization_options->is_null()) {
            srv.init_options = *params.initialization_options;
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
        caps.code_action_provider = protocol::CodeActionOptions{
            .code_action_kinds =
                std::vector<protocol::CodeActionKind>(feature::code_action_kinds.begin(),
                                                      feature::code_action_kinds.end()),
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
                if(projections.current(path_id) &&
                   projections.projection_at(path_id, session.version)) {
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
        if(!spelled) {
            return;
        }
        auto& path = spelled->str();
        auto path_id = srv.files.intern(*spelled);
        auto project = srv.owner_of(path_id).shared_from_this();
        auto session = srv.find_session(path_id);
        if(auto* alias = find_alias(path_id, path)) {
            project->sessions.apply_change(alias->buffer,
                                           params.content_changes,
                                           params.text_document.version);
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
                                       params.text_document.version);
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
        // The diagnostics clear goes out under the spelling being closed.
        srv.close_session(path_id);
        srv.files.unshow(path_id);
        // A document still open under another name takes the file over.
        if(auto it = aliases.find(path_id); it != aliases.end()) {
            auto next = take_alias(path_id, &it->second.front());
            clear_alias(next.spelling);
            srv.files.show_as(path_id, next.spelling);
            srv.open_session(path_id, std::move(next.buffer.text), next.buffer.version);
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

void LSPClient::register_language_features() {
    // The handlers of document requests take their snapshot — the ticket,
    // through the feature's entry point — before they return, as the
    // request is dispatched; their tasks run once the messages read with
    // the request are dispatched too.
    peer.on_request([this](RequestContext& ctx, const protocol::HoverParams& params) -> RawResult {
        this->server.pool.foreground_pulse();
        auto [path, path_id, session, project] = resolve_uri(params.text_document.uri);
        if(!session)
            return kota::outcome_error(unserved(path));
        return holding(
            project,
            project->features.hover(Ticket::take(session), params.position, ctx.cancellation));
    });

    peer.on_request([this](RequestContext& ctx,
                           const protocol::SemanticTokensParams& params) -> RawResult {
        this->server.pool.foreground_pulse();
        auto [path, path_id, session, project] = resolve_uri(params.text_document.uri);
        if(!session)
            return kota::outcome_error(unserved(path));
        return holding(project,
                       project->features.semantic_tokens(Ticket::take(session), ctx.cancellation));
    });

    peer.on_request(
        [this](RequestContext& ctx, const protocol::InlayHintParams& params) -> RawResult {
            this->server.pool.foreground_pulse();
            auto [path, path_id, session, project] = resolve_uri(params.text_document.uri);
            if(!session)
                return kota::outcome_error(unserved(path));
            return holding(project,
                           project->features.inlay_hints(Ticket::take(session),
                                                         params.range,
                                                         label_parts,
                                                         ctx.cancellation));
        });

    peer.on_request(
        [this](RequestContext& ctx, const protocol::FoldingRangeParams& params) -> RawResult {
            this->server.pool.foreground_pulse();
            auto [path, path_id, session, project] = resolve_uri(params.text_document.uri);
            if(!session)
                return kota::outcome_error(unserved(path));
            return holding(project,
                           project->features.folding_range(Ticket::take(session),
                                                           line_folding_only,
                                                           ctx.cancellation));
        });

    peer.on_request(
        [this](RequestContext& ctx, const protocol::SelectionRangeParams& params) -> RawResult {
            this->server.pool.foreground_pulse();
            auto [path, path_id, session, project] = resolve_uri(params.text_document.uri);
            if(!session)
                return kota::outcome_error(unserved(path));
            return holding(project,
                           project->features.selection_range(Ticket::take(session),
                                                             params.positions,
                                                             ctx.cancellation));
        });

    peer.on_request([this](RequestContext& ctx,
                           const protocol::DocumentSymbolParams& params) -> RawResult {
        this->server.pool.foreground_pulse();
        auto [path, path_id, session, project] = resolve_uri(params.text_document.uri);
        if(!session)
            return kota::outcome_error(unserved(path));
        return holding(project,
                       project->features.document_symbol(Ticket::take(session), ctx.cancellation));
    });

    peer.on_request([this](RequestContext& ctx,
                           const protocol::DocumentLinkParams& params) -> RawResult {
        this->server.pool.foreground_pulse();
        auto [path, path_id, session, project] = resolve_uri(params.text_document.uri);
        if(!session)
            return kota::outcome_error(unserved(path));
        return kota::co_invoke(
            [project,
             links = project->features.document_links(Ticket::take(session),
                                                      ctx.cancellation)]() mutable -> RawResult {
                co_return to_raw(co_await std::move(links).or_fail());
            });
    });

    // A pull is always answered with a report, never an error: on an
    // error, clients clear what they hold or stop pulling the document.
    peer.on_request(
        [this](RequestContext& ctx, const protocol::DocumentDiagnosticParams& params) -> RawResult {
            this->server.pool.foreground_pulse();
            auto [path, path_id, session, project] = resolve_uri(params.text_document.uri);
            std::vector<protocol::Diagnostic> items;
            if(session) {
                items = co_await project->features.diagnostics(session);
            }
            // A second name answers the warning once its text parted from the
            // first name's, before the pull or while it waited.
            if(auto* alias = find_alias(path);
               alias && (!session || alias->buffer.text != session->text)) {
                co_return full_report({feature::file_warning(divergence_message(*alias))});
            }
            auto report = full_report(std::move(items));
            if(session) {
                // Recorded against the live session: one that replaced this
                // pull's meanwhile (the document moved to another project)
                // compiles the same text, and its landing must correct the
                // empty answer the closed one gave.
                if(auto live = server.find_session(path_id)) {
                    pulled[path_id] = {
                        .version = live->version,
                        .report = llvm::xxh3_64bits(report.data),
                    };
                }
            }
            co_return report;
        });

    peer.on_request(
        [this](RequestContext& ctx, const protocol::CodeActionParams& params) -> RawResult {
            this->server.pool.foreground_pulse();
            auto [path, path_id, session, project] = resolve_uri(params.text_document.uri);
            if(!session)
                return kota::outcome_error(unserved(path));
            return kota::co_invoke(
                [this,
                 project,
                 actions = project->features.code_action(
                     Ticket::take(session),
                     params.range,
                     params.context.only ? llvm::ArrayRef(*params.context.only)
                                         : llvm::ArrayRef<protocol::CodeActionKind>(),
                     ctx.cancellation)]() mutable -> RawResult {
                    auto result = co_await std::move(actions).or_fail();
                    if(!versioned_edits) {
                        for(auto& action: result) {
                            unversion(*action.edit);
                        }
                    }
                    co_return to_raw(result);
                });
        });

    peer.on_request(
        [this](RequestContext& ctx, const protocol::DefinitionParams& params) -> RawResult {
            this->server.pool.foreground_pulse();
            auto [path, path_id, session, project] = resolve_uri(params.text_document.uri);
            return holding(project,
                           project->features.definition(Ticket::take(session),
                                                        path_id,
                                                        params.position,
                                                        ctx.cancellation));
        });

    // The navigation handlers below are index-only: closed documents are
    // fully serveable from the index, and an empty result is a real answer,
    // returned as [] — never an error.
    peer.on_request(
        [this](RequestContext& ctx, const protocol::ReferenceParams& params) -> RawResult {
            this->server.pool.foreground_pulse();
            auto [path, path_id, session, project] = resolve_uri(params.text_document.uri);
            return holding(project,
                           project->features.references(Ticket::take(session),
                                                        path_id,
                                                        params.position,
                                                        params.context.include_declaration));
        });

    peer.on_request([this](RequestContext& ctx,
                           const protocol::DocumentHighlightParams& params) -> RawResult {
        this->server.pool.foreground_pulse();
        auto [path, path_id, session, project] = resolve_uri(params.text_document.uri);
        return holding(
            project,
            project->features.document_highlight(Ticket::take(session), path_id, params.position));
    });

    peer.on_request(
        [this](RequestContext& ctx, const protocol::PrepareRenameParams& params) -> RawResult {
            this->server.pool.foreground_pulse();
            auto [path, path_id, session, project] = resolve_uri(params.text_document.uri);
            return holding(
                project,
                project->features.prepare_rename(Ticket::take(session), path_id, params.position));
        });

    peer.on_request([this](RequestContext& ctx, const protocol::RenameParams& params) -> RawResult {
        this->server.pool.foreground_pulse();
        auto [path, path_id, session, project] = resolve_uri(params.text_document.uri);
        return kota::co_invoke(
            [this,
             project,
             renamed = project->features.rename(Ticket::take(session),
                                                path_id,
                                                params.position,
                                                params.new_name)]() mutable -> RawResult {
                auto result = co_await std::move(renamed).or_fail();
                if(!result) {
                    co_return kota::codec::RawValue{"null"};
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
    });

    peer.on_request(
        [this](RequestContext& ctx, const protocol::TypeDefinitionParams& params) -> RawResult {
            this->server.pool.foreground_pulse();
            auto [path, path_id, session, project] = resolve_uri(params.text_document.uri);
            return holding(
                project,
                project->features.type_definition(Ticket::take(session), path_id, params.position));
        });

    peer.on_request(
        [this](RequestContext& ctx, const protocol::ImplementationParams& params) -> RawResult {
            this->server.pool.foreground_pulse();
            auto [path, path_id, session, project] = resolve_uri(params.text_document.uri);
            return holding(
                project,
                project->features.implementation(Ticket::take(session), path_id, params.position));
        });

    peer.on_request(
        [this](RequestContext& ctx, const protocol::DeclarationParams& params) -> RawResult {
            this->server.pool.foreground_pulse();
            auto [path, path_id, session, project] = resolve_uri(params.text_document.uri);
            return holding(
                project,
                project->features.declaration(Ticket::take(session), path_id, params.position));
        });

    peer.on_request(
        [this](RequestContext& ctx, const protocol::CompletionParams& params) -> RawResult {
            this->server.pool.foreground_pulse();
            auto [path, path_id, session, project] = resolve_uri(params.text_document.uri);
            if(!session)
                return kota::outcome_error(unserved(path));
            llvm::StringRef trigger;
            if(params.context && params.context->trigger_character) {
                trigger = *params.context->trigger_character;
            }
            return holding(project,
                           project->features.completion(session,
                                                        params.position,
                                                        completion_client,
                                                        trigger,
                                                        ctx.cancellation));
        });

    peer.on_request(
        [this](RequestContext& ctx, const protocol::SignatureHelpParams& params) -> RawResult {
            this->server.pool.foreground_pulse();
            auto [path, path_id, session, project] = resolve_uri(params.text_document.uri);
            if(!session)
                return kota::outcome_error(unserved(path));
            return holding(
                project,
                project->features.signature_help(session, params.position, ctx.cancellation));
        });

    peer.on_request(
        [this](RequestContext& ctx, const protocol::DocumentFormattingParams& params) -> RawResult {
            this->server.pool.foreground_pulse();
            auto [path, path_id, session, project] = resolve_uri(params.text_document.uri);
            if(!session)
                return kota::outcome_error(unserved(path));
            return holding(project,
                           project->features.formatting(session, std::nullopt, ctx.cancellation));
        });

    peer.on_request([this](RequestContext& ctx,
                           const protocol::DocumentRangeFormattingParams& params) -> RawResult {
        this->server.pool.foreground_pulse();
        auto [path, path_id, session, project] = resolve_uri(params.text_document.uri);
        if(!session)
            return kota::outcome_error(unserved(path));
        return holding(project,
                       project->features.formatting(session, params.range, ctx.cancellation));
    });

    peer.on_request([this](RequestContext& ctx,
                           const protocol::CallHierarchyPrepareParams& params) -> RawResult {
        this->server.pool.foreground_pulse();
        auto [path, path_id, session, project] = resolve_uri(params.text_document.uri);
        return holding(project,
                       project->features.call_hierarchy_prepare(Ticket::take(session),
                                                                path_id,
                                                                params.position));
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
        auto [path, path_id, session, project] = resolve_uri(params.text_document.uri);
        return holding(project,
                       project->features.type_hierarchy_prepare(Ticket::take(session),
                                                                path_id,
                                                                params.position));
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
            auto path = uri_to_path(params.uri);
            auto context_path = uri_to_path(params.context_uri);
            if(!path || !context_path) {
                co_return to_raw(ext::SwitchContextResult{});
            }
            auto path_id = this->server.files.intern(*path);
            auto context_path_id = this->server.files.intern(*context_path);
            // The session reset lives inside switch_context (single owner,
            // synchronous, no cross-file cascade — exempt from the event
            // pipeline; see the Invalidator charter).
            co_return to_raw(
                co_await this->server.switch_context(path_id, context_path_id, params));
        });

    peer.on_request(
        "clice/resetContext",
        [this](RequestContext& ctx, const ext::ResetContextParams& params) -> RawResult {
            this->server.pool.foreground_pulse();
            auto path = uri_to_path(params.uri);
            if(!path) {
                co_return to_raw(ext::SwitchContextResult{});
            }
            co_return to_raw(this->server.reset_context(this->server.files.intern(*path)));
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

    // ── Test hooks ──────────────────────────────────────────────────

    // Runs one file-tracker tick (see ext::PollParams).
    // Test-only and not a stable API.
    peer.on_request(
        "clice/internal/poll",
        [this](RequestContext& ctx, const ext::PollParams& params) -> RawResult {
            auto& srv = this->server;
            if(params.loop != "cdb" && params.loop != "workspace") {
                co_await kota::fail(kota::ipc::Error{protocol::ErrorCode::InvalidParams,
                                                     R"(loop must be "cdb" or "workspace")"});
            }
            if(llvm::none_of(srv.projects,
                             [](auto& project) { return project->tracker != nullptr; })) {
                co_await kota::fail(kota::ipc::Error{protocol::ErrorCode::InvalidRequest,
                                                     "No workspace is loaded"});
            }
            co_return to_raw(ext::PollResult{
                co_await tick_trackers(srv, params.loop == "cdb", params.force.value_or(true))});
        });

    auto hooks_off = [] {
        return kota::ipc::Error{protocol::ErrorCode::InvalidRequest, "test hooks are not enabled"};
    };

    peer.on_request(
        "clice/internal/sync",
        [this, hooks_off](RequestContext& ctx, const ext::SyncParams& params) -> RawResult {
            auto& srv = this->server;
            if(!srv.test_hooks()) {
                co_await kota::fail(hooks_off());
            }
            if(params.poll.value_or(false)) {
                co_await tick_trackers(srv, /*cdb=*/false, /*force=*/true);
            }
            auto deadline = std::chrono::milliseconds(params.deadline_ms.value_or(240'000));
            co_await kota::when_any(srv.settle(), kota::sleep(deadline, srv.loop));
            ext::SyncResult result;
            for(auto& project: srv.projects) {
                for(auto id: project->sched.pump.failed()) {
                    result.failed.push_back(feature::to_uri(srv.files.display(id)));
                }
                result.unsaved = result.unsaved || project->sched.store.has_unsaved_state();
            }
            llvm::sort(result.failed);
            result.pending = srv.pending_work();
            co_return to_raw(result);
        });

    peer.on_request(
        "clice/internal/hold",
        [this, hooks_off](RequestContext& ctx, const ext::HoldParams& params) -> RawResult {
            auto& srv = this->server;
            if(!srv.test_hooks()) {
                co_await kota::fail(hooks_off());
            }
            auto kind = parse_build_kind(params.kind);
            auto path = uri_to_path(params.uri);
            if(!kind || !path) {
                co_await kota::fail(kota::ipc::Error{
                    protocol::ErrorCode::InvalidParams,
                    R"(a hold names a kind of "compile", "pch", "pcm" or "index", and a file URI)"});
            }
            // The spelling the builds send the file under.
            auto file = srv.files.resolve(srv.files.intern(*path));
            co_return to_raw(ext::HoldResult{srv.probe.hold(*kind, std::string(file))});
        });

    peer.on_request(
        "clice/internal/release",
        [this, hooks_off](RequestContext& ctx, const ext::ReleaseParams& params) -> RawResult {
            auto& srv = this->server;
            if(!srv.test_hooks()) {
                co_await kota::fail(hooks_off());
            }
            if(!srv.probe.release(params.id)) {
                co_await kota::fail(kota::ipc::Error{protocol::ErrorCode::InvalidParams,
                                                     std::format("no hold {}", params.id)});
            }
            co_return to_raw(ext::ReleaseResult{});
        });

    peer.on_request(
        "clice/internal/logFlood",
        [this, hooks_off](RequestContext& ctx, const ext::LogFloodParams& params) -> RawResult {
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
                stats.synthesized_contexts += static_cast<std::uint32_t>(llvm::count_if(
                    llvm::make_second_range(served->contexts.header_contexts),
                    [](const HeaderContext& context) { return context.synthesized != nullptr; }));
                stats.sessions += static_cast<std::uint32_t>(served->sessions.sessions.size());
                stats.import_scans += served->sched.pcm.import_scans;
            }
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
        protocol::PublishDiagnosticsParams params;
        params.uri = feature::to_uri(
            server.files.display(server.files.intern(Spelling::absolute(file.str()))));
        params.diagnostics = std::move(diagnostics);
        publish(peer, params);
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

    if(pull_diagnostics) {
        if(auto it = pulled.find(session.path_id);
           it != pulled.end() && it->second.version == session.version &&
           it->second.report !=
               llvm::xxh3_64bits(full_report(project.features.settled_diagnostics(session)).data)) {
            refresh_diagnostics();
        }
    } else {
        protocol::PublishDiagnosticsParams params;
        params.uri = feature::to_uri(server.files.display(session.path_id));
        params.version = output.version;
        params.diagnostics = format_diagnostics(output);
        append_crash_notes(session, params.diagnostics);
        publish(peer, params);
        publish_aliases(session.path_id);
    }

    // Two cases make the client re-pull whole-document results it already
    // holds: index projections served while this compile was pending
    // (their answers just got better), and a compile that landed for
    // unchanged text — a context switch or a changed dependency reshapes
    // tokens (including their inactive regions), hints and folds with no
    // didChange to trigger the client's own re-pull. Edit-driven compiles
    // need neither: the client re-pulls on didChange and that pull awaits
    // the fresh AST.
    if(output.version.has_value()) {
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
