module clice;

import :command.file_kind;
import :server.client;
import :server.master;
import :support.logging;

namespace clice {

MasterServer::SecondName* LSPClient::second_name(llvm::StringRef spelling) {
    auto path_id =
        spelling.empty() ? std::nullopt : server.files.find(Spelling::absolute(spelling));
    return path_id ? server.find_second_name(*path_id, spelling) : nullptr;
}

LSPClient::ResolvedDoc LSPClient::resolve_uri(const std::string& uri) {
    auto path = uri_to_path(uri);
    if(!path) {
        // Not a file: no document of ours.
        return ResolvedDoc{.project = server.projects.front()};
    }
    auto path_id = server.files.intern(*path);
    auto project = server.owner_of(path_id).shared_from_this();
    auto session = server.find_session(path_id);
    auto version = session ? std::optional(session->version) : std::nullopt;
    // A document under a second name has a buffer of its own: it is the
    // file — its session, its index — only while both hold the same text.
    if(auto* second = server.find_second_name(path_id, *path)) {
        if(session && second->buffer.text() != session->text()) {
            return ResolvedDoc{.path = std::move(*path).str(), .project = std::move(project)};
        }
        version = second->buffer.version;
    }
    auto ticket = Ticket::take(session);
    return ResolvedDoc{
        .path = std::move(*path).str(),
        .path_id = path_id,
        .session = std::move(session),
        .project = std::move(project),
        .version = version,
        .ticket = std::move(ticket),
    };
}

void LSPClient::register_document_sync() {
    peer.on_notification([this](const protocol::DidOpenTextDocumentParams& params) {
        auto& srv = this->server;
        if(srv.shutting_down())
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
        // protocol violation, but documents are plain state with no worker
        // dependency — accept the buffer so the document is in place once
        // the server is ready: the projects adopt it as they start.
        if(srv.lifecycle != ServerLifecycle::Ready) {
            LOG_WARN("didOpen before the server is ready, accepting: {}", path);
        }

        srv.open_document(srv.files.intern(*spelled),
                          path,
                          params.text_document.text,
                          params.text_document.version);

        LOG_DEBUG("didOpen: {} (v{})", path, params.text_document.version);
    });

    peer.on_notification([this](const protocol::DidChangeTextDocumentParams& params) {
        auto& srv = this->server;
        if(srv.shutting_down())
            return;
        srv.pool.foreground_pulse();

        auto spelled = uri_to_path(params.text_document.uri);
        if(!spelled || is_other_language_path(*spelled)) {
            return;
        }
        srv.change_document(srv.files.intern(*spelled),
                            spelled->str(),
                            params.content_changes,
                            params.text_document.version);
    });

    peer.on_notification([this](const protocol::DidCloseTextDocumentParams& params) {
        auto& srv = this->server;
        if(srv.shutting_down())
            return;

        // Accepted before the server is ready for the same reason didOpen
        // is: a document opened during the handshake window must not stay
        // open forever when the editor already closed it. The diagnostics
        // clear is suppressed until the handshake completes — nothing was
        // pushed, and publishDiagnostics may not flow yet.
        auto spelled = uri_to_path(params.text_document.uri);
        if(!spelled) {
            return;
        }
        srv.close_document(srv.files.intern(*spelled), spelled->str());
    });

    peer.on_notification([this](const protocol::DidSaveTextDocumentParams& params) {
        auto& srv = this->server;
        // Unlike didOpen/didChange, a save arriving before the server is
        // ready needs no special handling: a disk change only invalidates
        // derived state, none of which exists yet — the project load at
        // ready reads the saved disk content anyway.
        if(srv.lifecycle != ServerLifecycle::Ready)
            return;
        srv.pool.foreground_pulse();

        auto path = uri_to_path(params.text_document.uri);
        if(!path) {
            return;
        }
        srv.save_document(srv.files.intern(*path));

        LOG_DEBUG("didSave: {}", *path);
    });
}

}  // namespace clice
