#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

#include "support/signal.h"

#include "kota/async/async.h"
#include "kota/codec/json/json.h"
#include "kota/ipc/codec/json.h"
#include "kota/ipc/lsp/progress.h"

namespace clice {

class MasterServer;
struct Session;

class LSPClient {
public:
    LSPClient(MasterServer& server, kota::ipc::JsonPeer& peer);
    ~LSPClient();

private:
    using RawResult = kota::task<kota::codec::RawValue, kota::ipc::Error>;

    /// Shared front half of every document-addressed handler: URI → path →
    /// interned path_id → open session (null when the document is not open).
    struct ResolvedDoc {
        std::string path;
        std::uint32_t path_id;
        std::shared_ptr<Session> session;
    };

    ResolvedDoc resolve_uri(const std::string& uri);

    /// LSP handler registration, grouped by category. Each installs its
    /// handlers on `peer`; all four are invoked once from the constructor.
    void register_lifecycle();
    void register_document_sync();
    void register_language_features();
    void register_extensions();

    /// Push clice.toml load problems as diagnostics on the config file URI.
    void publish_config_diagnostics();

    /// Push a session's materialized compile output (diagnostics and
    /// inactive regions) to the client. Invoked by the compiler's
    /// on_output signal.
    void push_output(const Session& session);

    /// React to a background-indexing progress change: drive the LSP
    /// work-done progress token through its begin/report/end lifecycle,
    /// reading the counts from the Indexer. Invoked by the
    /// background indexer's on_progress_changed signal.
    void report_index_progress();

    MasterServer& server;
    kota::ipc::JsonPeer& peer;

    /// Subscription to compile outputs; disconnects on destruction.
    Signal<std::shared_ptr<Session>>::Connection output_conn;

    /// Subscription to background-index progress; disconnects on destruction.
    Signal<>::Connection progress_conn;

    /// Progress-token lifecycle, split into orthogonal facts so the
    /// asynchronous create() handshake can reconcile against rounds that
    /// begin or end while it is in flight. At most one create() is ever
    /// outstanding, and the reporter is never replaced while a handshake
    /// coroutine is awaiting on it.
    ///
    /// Held behind a shared_ptr because the handshake runs as a detached
    /// task on the event loop: the task captures this state (never the
    /// LSPClient), so a connection torn down mid-handshake cannot leave the
    /// task dereferencing a destroyed client — it observes `abandoned` and
    /// stops.
    struct IndexProgressState {
        /// A create() handshake is awaiting the client's acknowledgement.
        bool create_inflight = false;

        /// begin() has been announced on the token; reports may flow.
        bool token_active = false;

        /// An indexing round is running (Begin seen, End not yet).
        bool round_active = false;

        /// The owning LSPClient was destroyed; the handshake must not
        /// announce the token or touch the peer.
        bool abandoned = false;

        /// Total file count captured when the round began, for the begin
        /// message.
        std::uint32_t total = 0;

        /// The active work-done progress token, held across
        /// begin/report/end.
        std::optional<kota::ipc::lsp::ProgressReporter<kota::ipc::JsonPeer>> reporter;
    };

    std::shared_ptr<IndexProgressState> index_progress = std::make_shared<IndexProgressState>();
};

}  // namespace clice
