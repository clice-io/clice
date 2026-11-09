module clice;

import :project.discovery;
import :server.client;
import :server.master;
import :server.served_project;
import :support.logging;
import :support.text;
import :vfs.file_system;
import :vfs.path;

namespace clice {

MasterServer::Document* MasterServer::find_document(Fid path_id) {
    auto it = documents.find(path_id);
    return it != documents.end() ? &it->second : nullptr;
}

std::shared_ptr<Session> MasterServer::find_session(Fid path_id) {
    auto* document = find_document(path_id);
    return document ? document->session : nullptr;
}

MasterServer::SecondName* MasterServer::find_second_name(Fid path_id, llvm::StringRef spelling) {
    auto* document = find_document(path_id);
    if(!document) {
        return nullptr;
    }
    auto it = ranges::find(document->second_names, spelling, &SecondName::spelling);
    return it != document->second_names.end() ? &*it : nullptr;
}

void MasterServer::open_document(Fid path_id,
                                 llvm::StringRef spelling,
                                 std::string text,
                                 int version) {
    // One file, one buffer: a second document naming it through another
    // path (a symlink) would fold its own edits into the first one's. The
    // document opened first keeps it; this one waits with its own text.
    if(auto shown = files.shown_as(path_id); shown && *shown != spelling) {
        LOG_WARN("didOpen: {} is already open as {}; serving that one", spelling, *shown);
        auto& document = documents.find(path_id)->second;
        auto& second = document.second_names.emplace_back(SecondName{
            .spelling = spelling.str(),
            .buffer = Session(path_id, std::move(text), version),
        });
        if(client) {
            client->publish_second_name(document, second);
        }
        return;
    }
    files.show_as(path_id, spelling);
    open_first_name(path_id, std::move(text), version);
}

void MasterServer::open_first_name(Fid path_id, std::string text, int version) {
    // A file just created is a change the file table saw only now: it
    // cascades before the document resolves its command, which its host's
    // rescan gives the edge to it.
    files.disk.current(path_id);
    apply_disk_changes();

    auto [it, inserted] = documents.try_emplace(path_id);
    auto& document = it->second;
    // A second didOpen of an open document replaces its buffer.
    if(document.session) {
        end_session(document);
    }
    // The editor's text leaves out the byte order mark the file starts with.
    if(vfs::starts_with_bom(files.resolve(path_id))) {
        text.insert(0, utf8_bom);
    }
    document.session = std::make_shared<Session>(path_id, std::move(text), version);
    if(projects.empty()) {
        return;
    }

    discover_around(path_id);
    auto* compiling = compiling_project(path_id);
    if(lifecycle == ServerLifecycle::Ready && inserted && (!compiling || compiling->broken())) {
        // The project root nearest the file serves it: one no folder
        // holds, or one nested deeper than the folders holding it (a
        // subproject's clice.toml or database), a broken project's too. A
        // folder's own root, and a database a served project loads already
        // (a build directory above a generated file), stay that project's.
        auto path = files.resolve(path_id);
        auto above = find_project_root(Spelling(path.parent()));
        auto root = above.empty() ? CanonicalPath() : CanonicalPath(above);
        auto covered = [&](auto& project) {
            if(project->holds(path) && path::is_within(project->project.root, root)) {
                return true;
            }
            auto& cdb = project->project.cdb;
            Spelling spelled(root);
            return cdb.find_source(spelled) || cdb.find_source(Spelling("build", spelled));
        };
        if(!root.empty() && ranges::none_of(projects, covered)) {
            folders.push_back(std::move(root));
            serve_folders();
        }
    }
    auto& owner = pick_owner(path_id);
    document.owner = &owner;
    owner.adopt(path_id);
    watch_settings();
}

void MasterServer::end_session(Document& document) {
    auto& session = *document.session;
    session.generation += 1;
    session.closed = true;
    if(document.owner) {
        document.owner->release(session.path_id);
    }
}

void MasterServer::change_document(Fid path_id,
                                   llvm::StringRef spelling,
                                   llvm::ArrayRef<protocol::TextDocumentContentChangeEvent> changes,
                                   int version) {
    auto* document = find_document(path_id);
    if(auto* second = find_second_name(path_id, spelling)) {
        second->buffer.edit(changes, version, position_encoding);
        if(client) {
            client->publish_second_name(*document, *second);
        }
        return;
    }
    if(!document) {
        // Dropping is the only safe move: without the didOpen baseline
        // there is no buffer to fold the edits into.
        LOG_ERROR("didChange for a document with no open session, dropping: {}", spelling);
        return;
    }
    if(files.shown_as(path_id) != spelling) {
        LOG_WARN("didChange for {}, never opened under that name, dropping", spelling);
        return;
    }
    auto& session = *document->session;
    if(version <= session.version) {
        LOG_WARN("didChange version did not increase for {}: {} -> {}",
                 spelling,
                 session.version,
                 version);
    }
    session.edit(changes, version, position_encoding);
    if(client) {
        client->publish_second_names(*document);
    }
    // The edit made any in-flight compile outdated. Cancel it now instead
    // of waiting for the next AST-backed request to observe it: the
    // attempt's advisory token releases its dependency waits, and the
    // interrupt keeps a stale parse from holding up its waiters.
    if(document->owner) {
        document->owner->edited(session);
    }
    LOG_DEBUG("didChange: path={} version={} gen={}",
              spelling,
              session.version,
              session.generation);
}

void MasterServer::close_document(Fid path_id, llvm::StringRef spelling) {
    auto it = documents.find(path_id);
    if(it == documents.end()) {
        return;
    }
    auto& seconds = it->second.second_names;
    if(auto second = ranges::find(seconds, spelling, &SecondName::spelling);
       second != seconds.end()) {
        seconds.erase(second);
        if(client) {
            client->retract_second_name(spelling);
        }
        return;
    }
    if(files.shown_as(path_id) != spelling) {
        return;
    }
    end_session(it->second);
    auto document = std::move(it->second);
    documents.erase(it);
    // Taken down under the name being closed.
    if(client) {
        client->retract(path_id);
    }
    files.unshow(path_id);
    // Like an open and a save, a close is a moment to look at the disk: the
    // editor stops showing its buffer in place of the file.
    files.disk.current(path_id);
    apply_disk_changes();
    // A document still open under another name takes the file over.
    if(document.second_names.empty()) {
        return;
    }
    auto next = std::move(document.second_names.front());
    document.second_names.erase(document.second_names.begin());
    if(client) {
        client->retract_second_name(next.spelling);
    }
    files.show_as(path_id, next.spelling);
    open_first_name(path_id, next.buffer.text().str(), next.buffer.version);
    documents[path_id].second_names = std::move(document.second_names);
    // A pulling client still holds its divergence warning.
    if(client && next.warned) {
        client->refresh_diagnostics();
    }
}

void MasterServer::save_document(Fid path_id) {
    if(auto* document = find_document(path_id)) {
        // Saved in another encoding, the file gained or lost its byte order
        // mark: a change of the buffer the editor does not send.
        if(document->session->set_bom(vfs::starts_with_bom(files.resolve(path_id))) &&
           document->owner) {
            document->owner->edited(*document->session);
        }
        // The user's retry: whatever crashed on the saved document runs
        // again on its next request.
        if(document->owner) {
            document->owner->retry_after_save(*document->session);
        }
    }
    llvm::SmallVector<Fid> closures{path_id};
    for(auto& project: projects) {
        for(auto& open: project->open_files()) {
            closures.append(open.read);
        }
    }
    files.disk.look(closures);
    apply_disk_changes();
    // After the changes: a file the graph did not know has had its
    // includers rescanned, so a host that includes it is in sight.
    owner_of(path_id).record_saved(path_id);
}

}  // namespace clice
