module clice;

import :command.file_kind;
import :server.client;
import :server.master;
import :support.logging;

namespace clice {

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
