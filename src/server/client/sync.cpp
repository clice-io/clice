module clice;

import :command.file_kind;
import :feature.protocol;
import :server.client;
import :server.master;
import :support.logging;
import :vfs.path;

namespace clice {

void LSPClient::register_document_sync() {
    peer.on_notification([this](const protocol::DidOpenTextDocumentParams& params) {
        auto& srv = this->server;
        if(srv.shutting_down())
            return;
        srv.pool.user_activity();

        auto path = feature::written_path(params.text_document.uri);
        if(!path) {
            LOG_INFO("didOpen: {} names no file; not served", params.text_document.uri);
            return;
        }
        if(is_other_language_path(*path)) {
            LOG_INFO("didOpen: {} is no C-family source; not served", *path);
            return;
        }

        // A didOpen racing ahead of the initialize handshake is a client
        // protocol violation, but documents are plain state with no worker
        // dependency — accept the buffer so the document is in place once
        // the server is ready: the projects adopt it as they start.
        if(srv.lifecycle != ServerLifecycle::Ready) {
            LOG_WARN("didOpen before the server is ready, accepting: {}", *path);
        }

        srv.open_document(srv.files.intern(Spelling::absolute(*path)),
                          *path,
                          params.text_document.text,
                          params.text_document.version);

        LOG_INFO("Opened {} (version {})", *path, params.text_document.version);
    });

    peer.on_notification([this](const protocol::DidChangeTextDocumentParams& params) {
        auto& srv = this->server;
        if(srv.shutting_down())
            return;
        srv.pool.user_activity();

        auto path = feature::written_path(params.text_document.uri);
        if(!path || is_other_language_path(*path)) {
            return;
        }
        srv.change_document(srv.files.intern(Spelling::absolute(*path)),
                            *path,
                            params.content_changes,
                            params.text_document.version);
        LOG_DEBUG("Changed {} (version {})", *path, params.text_document.version);
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
        auto path = feature::written_path(params.text_document.uri);
        if(!path) {
            return;
        }
        srv.close_document(srv.files.intern(Spelling::absolute(*path)), *path);
        LOG_INFO("Closed {}", *path);
    });

    peer.on_notification([this](const protocol::DidSaveTextDocumentParams& params) {
        auto& srv = this->server;
        // Unlike didOpen/didChange, a save arriving before the server is
        // ready needs no special handling: a disk change only invalidates
        // derived state, none of which exists yet — the project load at
        // ready reads the saved disk content anyway.
        if(srv.lifecycle != ServerLifecycle::Ready)
            return;
        srv.pool.user_activity();

        auto path = feature::to_path(params.text_document.uri);
        if(!path) {
            return;
        }
        srv.save_document(srv.files.intern(*path));
        // What a pulling client asks after a save — the edits it holds
        // checked, a crash the save retries — one that does not pull is
        // pushed.
        if(!pull_diagnostics) {
            refresh_diagnostics();
        }

        LOG_INFO("Saved {}", *path);
    });
}

}  // namespace clice
