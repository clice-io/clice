module clice;

import :feature.diagnostics;
import :feature.protocol;
import :server.client;
import :server.master;
import :server.served_project;
import :support.json;
import :support.logging;
import :syntax.source_text;
import :vfs.file_system;

namespace clice {

void LSPClient::forward_notify_messages() {
    if(!client_ready) {
        return;
    }
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

/// Send the client a request without awaiting the reply. Peer
/// lifetime discipline as in the progress handshake: the peer outlives the
/// client, and teardown fails pending requests before run() returns. A
/// captureless coroutine lambda invoked immediately: parameters are copied
/// into the frame, captures would dangle.
template <typename Params>
static void fire_request(kota::event_loop& loop, ipc::JSONPeer& peer, Params params) {
    loop.schedule([](ipc::JSONPeer* peer, Params request) -> kota::task<> {
        // Hang detection: nothing waits on the answer; the bound only frees
        // the request of a client that never answers.
        co_await peer->send_request(request, {.timeout = std::chrono::milliseconds(3000)});
    }(&peer, std::move(params)));
}

/// Pushes `params`; a push that cannot be encoded is logged, not lost unseen.
static void publish_diagnostics(ipc::JSONPeer& peer,
                                const protocol::PublishDiagnosticsParams& params) {
    if(auto sent = peer.send_notification(params); !sent) {
        LOG_WARN("Cannot publish the diagnostics of {}: {}", params.uri, sent.error().message);
    }
}

codec::RawValue full_report(std::vector<protocol::Diagnostic> items) {
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

std::string LSPClient::divergence_message(const MasterServer::SecondName& second) {
    return std::format(
        "This file is also open as {}, which clice analyzes; edits here are not "
        "analyzed until the texts agree. Close one of the two.",
        server.files.display(second.buffer.path_id));
}

void LSPClient::refresh_diagnostics() {
    if(pull_diagnostics && diagnostic_refresh) {
        // The client pulls every document again: no report it pulled is
        // one it holds any more.
        pulled.clear();
        fire_request(server.loop, peer, protocol::DiagnosticRefreshParams{});
    }
}

void LSPClient::refresh() {
    if(!client_ready) {
        return;
    }
    if(semantic_tokens_refresh) {
        fire_request(server.loop, peer, protocol::SemanticTokensRefreshParams{});
    }
    if(inlay_hint_refresh) {
        fire_request(server.loop, peer, protocol::InlayHintRefreshParams{});
    }
    if(folding_range_refresh) {
        fire_request(server.loop, peer, protocol::FoldingRangeRefreshParams{});
    }
}

void LSPClient::publish_second_name(MasterServer::Document& document,
                                    MasterServer::SecondName& second) {
    if(!client_ready) {
        return;
    }
    auto* first = document.session.get();
    bool diverged = !first || first->text() != second.buffer.text();
    bool flipped = std::exchange(second.warned, diverged) != diverged;
    if(diverged && flipped) {
        peer.send_notification(protocol::ShowMessageParams{
            .type = protocol::MessageType::Warning,
            .message = divergence_message(second),
        });
    }
    if(pull_diagnostics) {
        if(flipped) {
            refresh_diagnostics();
        }
        return;
    }
    protocol::PublishDiagnosticsParams params;
    params.uri = feature::to_uri(second.spelling);
    params.version = second.buffer.version;
    auto* owner = document.owner;
    auto compiled = !diverged && owner ? owner->latest_at(first->path_id, first->version) : nullptr;
    if(diverged) {
        params.diagnostics = to_client({feature::file_warning(divergence_message(second))},
                                       second.spelling,
                                       second.buffer.positions());
    } else if(compiled) {
        params.diagnostics = to_client(owner->shown_diagnostics(*first, *compiled->output),
                                       second.spelling,
                                       compiled->output->text->positions());
    } else if(!flipped) {
        // Shared once the owner's compile caught up with the text; until
        // then only a divergence warning is taken down.
        // FIXME: a crashed compile leaves no compile at the owner's version, so
        // a push client gets nothing at the second name's URI, the crash note
        // included.
        return;
    }
    publish_diagnostics(peer, params);
}

void LSPClient::retract_second_name(llvm::StringRef spelling) {
    if(client_ready && !pull_diagnostics) {
        peer.send_notification(
            protocol::PublishDiagnosticsParams{.uri = feature::to_uri(spelling)});
    }
}

void LSPClient::retract(Fid path_id) {
    // LSP versions are scoped to an open document: a reopen restarts
    // them, so a stale entry would misread the fresh document's first
    // compile as an unchanged-text recompile.
    output_versions.erase(path_id);
    pulled.erase(path_id);
    sent_tokens.erase(path_id);
    index_served.erase(path_id);
    // Held back until the handshake completes: nothing was pushed, and
    // publishDiagnostics may not flow yet. A pulling client drops what it
    // pulled of a document it closed.
    if(client_ready && !pull_diagnostics) {
        publish_diagnostics(peer,
                            protocol::PublishDiagnosticsParams{
                                .uri = feature::to_uri(server.files.display(path_id)),
                            });
    }
}

void LSPClient::publish_second_names(MasterServer::Document& document) {
    for(auto& second: document.second_names) {
        publish_second_name(document, second);
    }
}

void LSPClient::ask_for_restart() {
    if(!client_ready) {
        return;
    }
    if(restart_server) {
        if(!restart_asked && !server.restart_reasons.empty()) {
            restart_asked = true;
            fire_request(
                server.loop,
                peer,
                ext::RestartServerParams{.reason = server.restart_reasons.front().message});
        }
        return;
    }
    publish_settings_diagnostics();
    for(auto& reason: server.restart_reasons) {
        if(reason.file.empty() && restarts_told.insert(reason.message).second) {
            LOG_GUIDANCE("{}; it applies once clice restarts", reason.message);
        }
    }
}

void LSPClient::push_output(ServedProject& project, const Session& session, bool compiled) {
    // Held back until the handshake completes (the LSP spec forbids
    // publishDiagnostics before the initialize response); the output stays
    // materialized in the latest compile and the initialized handler replays it.
    if(!client_ready) {
        return;
    }
    auto latest = project.latest(session.path_id);
    if(!latest || !latest->output.has_value()) {
        return;
    }
    auto& output = *latest->output;

    if(pull_diagnostics) {
        if(auto it = pulled.find(session.path_id);
           it != pulled.end() && it->second.version == session.version &&
           it->second.report !=
               llvm::xxh3_64bits(full_report(to_client(project.finished_diagnostics(session),
                                                       server.files.display(session.path_id),
                                                       session.positions()))
                                     .data)) {
            refresh_diagnostics();
        }
    } else {
        protocol::PublishDiagnosticsParams params;
        params.uri = feature::to_uri(server.files.display(session.path_id));
        params.version = output.version;
        params.diagnostics = to_client(project.shown_diagnostics(session, output),
                                       server.files.display(session.path_id),
                                       output.text->positions());
        publish_diagnostics(peer, params);
        if(auto* document = server.find_document(session.path_id)) {
            publish_second_names(*document);
        }
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
        if(index_served.contains(session.path_id) || same_text) {
            refresh();
        }
    }
}

/// Publish clice.toml load problems as diagnostics, each on its own file's
/// URI (multiple files can contribute issues when the first configuration file candidate
/// is malformed and the next one loads). The files are usually not open in
/// the editor — publishing to a closed file is fine, the client shows it in
/// the problems panel. The loaded file always gets a publish, so a clean
/// load clears diagnostics from a previous (broken) state.
void LSPClient::publish_settings_diagnostics() {
    if(!client_ready) {
        return;
    }
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
    for(auto& file: published_settings_files) {
        by_file.try_emplace(file.getKey());
    }
    published_settings_files.clear();
    for(auto& project: server.projects) {
        if(project->settings_file.empty()) {
            continue;
        }
        // The loaded file always gets a publish (even with zero issues), so
        // a clean load clears diagnostics from a previous broken state.
        by_file.try_emplace(project->settings_file);
        for(auto& issue: project->settings_issues) {
            if(issue.file.empty()) {
                continue;
            }
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
                .level = issue.severity == SettingsIssue::Severity::Error
                             ? DiagnosticLevel::Error
                             : DiagnosticLevel::Warning,
                .message = issue.message,
            });
            published_settings_files.insert(issue.file);
        }
        published_settings_files.insert(project->settings_file);
    }
    // A client that restarts the server when asked is restarting it.
    for(auto& reason: server.restart_reasons) {
        if(restart_server || reason.file.empty()) {
            continue;
        }
        by_file[reason.file].push_back({
            .level = DiagnosticLevel::Warning,
            .message = std::format("{}; it applies once clice restarts", reason.message),
        });
        published_settings_files.insert(reason.file);
    }

    for(auto& [file, diagnostics]: by_file) {
        protocol::PublishDiagnosticsParams params;
        params.uri = feature::to_uri(server.files.display_identity(file));
        if(!diagnostics.empty()) {
            params.diagnostics = to_client(diagnostics, file, text_of(file).positions());
        }
        publish_diagnostics(peer, params);
    }
}

}  // namespace clice
