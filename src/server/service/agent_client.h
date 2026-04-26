#pragma once

#include "kota/ipc/codec/json.h"

namespace clice {

class MasterServer;

class AgentClient {
public:
    AgentClient(MasterServer& server, kota::ipc::JsonPeer& peer);

private:
    MasterServer& server;
    kota::ipc::JsonPeer& peer;
};

}  // namespace clice
