#pragma once

#include <cstdint>
#include <memory>
#include <optional>

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

    /// Push clice.toml load problems as diagnostics on the config file URI.
    void publish_config_diagnostics();

    /// Push a session's materialized compile output (diagnostics and
    /// inactive regions) to the client. Invoked by the compiler's
    /// on_output signal.
    void push_output(const Session& session);

    /// React to a background-indexing progress change: drive the LSP
    /// work-done progress token through its begin/report/end lifecycle,
    /// reading the counts from the BackgroundIndexer. Invoked by the
    /// background indexer's on_progress_changed signal.
    void report_index_progress();

    MasterServer& server;
    kota::ipc::JsonPeer& peer;

    /// Subscription to compile outputs; disconnects on destruction.
    Signal<std::shared_ptr<Session>>::Connection output_conn;

    /// Subscription to background-index progress; disconnects on destruction.
    Signal<>::Connection progress_conn;

    /// Progress-token lifecycle, split into three orthogonal facts so the
    /// asynchronous create() handshake can reconcile against rounds that
    /// begin or end while it is in flight. At most one create() is ever
    /// outstanding, and index_progress is never replaced while a handshake
    /// coroutine is awaiting on it.

    /// A create() handshake is awaiting the client's acknowledgement.
    bool progress_create_inflight = false;

    /// begin() has been announced on the token; reports may flow.
    bool progress_token_active = false;

    /// An indexing round is running (Begin seen, End not yet).
    bool progress_round_active = false;

    /// Total file count captured when the round began, for the begin message.
    std::uint32_t progress_total = 0;

    /// The active work-done progress token, held across begin/report/end.
    std::optional<kota::ipc::lsp::ProgressReporter<kota::ipc::JsonPeer>> index_progress;
};

}  // namespace clice
