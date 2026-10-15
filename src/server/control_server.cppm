module;

#include "modules/prelude.h"

module clice:server.control_server;

import :server.project_server;

namespace clice {

/// Serve a project's control channel: accept connections on `acceptor`
/// for as long as the task runs, each answering the control request
/// (server/control.h) against the project.
kota::task<> serve_control(ProjectServer& project, kota::tcp::acceptor acceptor);

}  // namespace clice
