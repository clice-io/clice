#pragma once

#include "kota/async/async.h"
#include "kota/codec/json/json.h"
#include "kota/ipc/codec/json.h"

namespace clice {

class MasterServer;

class LSPClient {
public:
    LSPClient(MasterServer& server, kota::ipc::JsonPeer& peer);
    ~LSPClient();

private:
    using RawResult = kota::task<kota::codec::RawValue, kota::ipc::Error>;

    /// Push clice.toml load problems as diagnostics on the config file URI.
    void publish_config_diagnostics();

    MasterServer& server;
    kota::ipc::JsonPeer& peer;
};

}  // namespace clice
