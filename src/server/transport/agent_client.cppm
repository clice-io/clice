module;

// kota::ipc::JsonPeer is Peer<JsonCodec>. Forward-declare it instead of including
// kota/ipc/codec/json.h (see the note in lsp_client.cppm): its decode.h explicit
// template instantiations must not leak into clice importers. Interface uses are
// by reference only.
namespace kota::ipc {

template <typename Codec>
class Peer;
class JsonCodec;
using JsonPeer = Peer<JsonCodec>;

}  // namespace kota::ipc

export module clice.server:transport.agent_client;

import :transport.master_server;

export namespace clice {

class AgentClient {
public:
    AgentClient(MasterServer& server, kota::ipc::JsonPeer& peer);

private:
    MasterServer& server;
    kota::ipc::JsonPeer& peer;
};

}  // namespace clice
