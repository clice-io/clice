module clice;

import :command.table;
import :compile.diagnostic;
import :feature.diagnostics;
import :project.background;
import :server.client;
import :server.master;
import :server.served_project;
import :server.session;
import :support.logging;
import :vfs.file_table;

namespace clice {

void ServedProject::record_crash(const std::shared_ptr<Session>& session,
                                 feature::RequestKind kind,
                                 llvm::StringRef death,
                                 std::string cause) {
    auto path_id = session->path_id;
    // A document that left the project compiles under another command now:
    // what crashed here is no record of its.
    if(!owns(*session)) {
        LOG_WARN("{} crashed a worker after it left the project: {}",
                 project.file_table.resolve(path_id),
                 cause);
        return;
    }
    bool shown = session->crashed(kind, death, std::move(cause), Session::Clock::now());
    auto* record = session->crash_record(kind);
    LOG_WARN("{} crashed a worker: {}",
             project.file_table.resolve(path_id),
             feature::crash_note({.kind = kind,
                                  .cause = record->cause,
                                  .count = record->count,
                                  .save_only = record->save_only(Session::policy)},
                                 server.session_dir)
                 .message);
    // A silent first crash leaves the published state alone (see Session
    // invariant 4).
    if(!shown) {
        return;
    }
    // A stopped compile leaves no AST behind the old diagnostics: they go,
    // and the note says why.
    auto previous = latest(path_id);
    if(previous && previous->output.has_value() &&
       (kind == feature::RequestKind::Compile || kind == feature::RequestKind::PCH)) {
        update_latest(path_id, [&](DocumentCompile& compiled) {
            compiled.output = CompileOutput{
                .text = session->source,
                .diagnostics = {.source = previous->output->diagnostics.source},
            };
        });
    }
    republish(session);
}

void ServedProject::republish(const std::shared_ptr<Session>& session) {
    if(!owns(*session)) {
        return;
    }
    if(auto previous = latest(session->path_id); !previous || !previous->output.has_value()) {
        update_latest(session->path_id, [&](DocumentCompile& compiled) {
            compiled.output = CompileOutput{.text = session->source};
        });
    }
    push_output(*session, true);
}

std::vector<feature::FileDiagnostic>
    ServedProject::shown_diagnostics(const Session& session, const CompileOutput& output) const {
    auto diagnostics = feature::shown_diagnostics(output.diagnostics, project.settings.diagnostics);
    if(ranges::any_of(diagnostics, [](const feature::FileDiagnostic& diagnostic) {
           return diagnostic.level >= DiagnosticLevel::Error;
       })) {
        std::vector<std::string> unsaved;
        each_input(session.path_id, [&](const Inputs& inputs) {
            for(auto& input: inputs) {
                auto other = server.find_session(input.file);
                if(other && input.file != session.path_id && input.version.valid() &&
                   other->source->hash() !=
                       project.file_table.version(input.version).content_hash) {
                    unsaved.push_back(project.file_table.display(input.file));
                }
            }
        });
        ranges::sort(unsaved);
        unsaved.erase(ranges::unique(unsaved).begin(), unsaved.end());
        if(!unsaved.empty()) {
            diagnostics.push_back(feature::unsaved_inputs_note(unsaved));
        }
    }
    for(auto& note: session.crash_notes()) {
        diagnostics.push_back(feature::crash_note(note, server.session_dir));
    }
    return diagnostics;
}

std::vector<feature::FileDiagnostic>
    ServedProject::finished_diagnostics(const Session& session) const {
    if(session.closed) {
        return {};
    }
    // A compile that failed or is stopped left an output of an older
    // buffer, whose ranges a versionless report would place on this one.
    if(auto compiled = latest_at(session.path_id, session.version)) {
        return shown_diagnostics(session, *compiled->output);
    }
    return shown_diagnostics(session, {});
}

void ServedProject::push_output(const Session& session, bool compiled) {
    if(server.client && owns(session)) {
        server.client->push_output(*this, session, compiled);
    }
}

std::vector<feature::FileDiagnostic>
    ServedProject::shown_skipped_diagnostics(const Session& session,
                                             const CompileOutput& output) const {
    auto* skipped = project.background_index.skipped_diagnostics(session.path_id);
    if(!skipped || skipped->text_hash != session.source->hash() ||
       output.diagnostics.source != CommandSource::Database || active_choice(session.path_id)) {
        return {};
    }
    return feature::confirmed_skipped(skipped->diagnostics, output.diagnostics);
}

void ServedProject::book_skipped_diagnostics(Fid path_id, bool first) {
    auto& session = *server.find_session(path_id);
    if(!project.settings.project.skip_header_bodies || !find_document(path_id)->compiles ||
       project.entries(path_id).empty()) {
        return;
    }
    // LIMITATION: these diagnostics live in memory alone, so after a
    // restart every open unit of a database compiles once more in full in
    // the background to show them, its index fresh or not.
    auto& background = project.background_index;
    if(auto* skipped = background.skipped_diagnostics(path_id);
       skipped && skipped->text_hash == session.source->hash()) {
        return;
    }
    if(first) {
        if(background.started() && !background.queue().in_flight(path_id) &&
           !background.failed(path_id)) {
            background.index_first(path_id, /*diagnostics=*/true);
        }
    } else {
        background.enqueue(path_id, ReindexReason::DepsOnly, /*diagnostics=*/true);
    }
}

void ServedProject::show_skipped_diagnostics(Fid path_id) {
    // A compile owed shows them as it commits (CompileAttempt::commit).
    if(!current(path_id)) {
        return;
    }
    auto& session = *server.find_session(path_id);
    auto compiled = latest_at(path_id, session.version);
    if(!compiled) {
        return;
    }
    auto shown = shown_skipped_diagnostics(session, *compiled->output);
    if(ranges::equal(compiled->output->diagnostics.skipped, shown, feature::same_diagnostic)) {
        return;
    }
    update_latest(path_id, [&](DocumentCompile& compiled) {
        compiled.output->diagnostics.skipped = std::move(shown);
    });
    push_output(session, false);
}

}  // namespace clice
