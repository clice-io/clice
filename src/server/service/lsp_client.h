#pragma once

#include <memory>

#include "support/signal.h"

#include "kota/async/async.h"
#include "kota/codec/json/json.h"
#include "kota/ipc/codec/json.h"

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

    MasterServer& server;
    kota::ipc::JsonPeer& peer;

    /// Subscription to compile outputs; disconnects on destruction.
    Signal<std::shared_ptr<Session>>::Connection output_conn;
};

}  // namespace clice
