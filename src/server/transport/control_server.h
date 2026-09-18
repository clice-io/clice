#pragma once

#include "kota/async/async.h"

namespace clice {

class MasterServer;

/// Serve the control channel: accept connections on `acceptor` for as long
/// as the task runs, each answering the control request
/// (server/protocol/control.h) against the server.
kota::task<> serve_control(MasterServer& server, kota::tcp::acceptor acceptor);

}  // namespace clice
