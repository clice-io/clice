module;

#include "support/prelude.h"

/// The control channel of a serving writer: what the commands that find
/// the index writer lock taken by a server ask of it, and the server's side
/// answering. Paths are absolute filesystem paths.

module clice:server.control;

import :index.writer_lock;

namespace clice {

class ProjectServer;

}  // namespace clice

namespace clice::control {

/// Run a pass over the build — every unit re-validated against the disk, the ones
/// the hash gate finds current skipped — and answer once the index is
/// persisted. Refused when `configuration` is not the one the server
/// indexes, or when its configuration keeps background indexing off.
struct IndexParams {
    std::string configuration;
};

struct IndexResult {
    /// Units whose index is not current after the pass: the attempt
    /// failed for good, or the serving side kept them out.
    std::vector<std::string> failed;
};

/// Ask the serving writer at `endpoint` to run a pass over the build and
/// wait for the index to be persisted. Runs to completion on an event loop of its
/// own; a refused connection or a server that goes away mid-request is
/// the error.
std::expected<IndexResult, std::string> request_index(const index::ServerEndpoint& endpoint,
                                                      llvm::StringRef configuration);

/// Serve a project's control channel: accept connections on `acceptor`
/// for as long as the task runs, each answering the control requests
/// against the project.
kota::task<> serve(ProjectServer& project, kota::tcp::acceptor acceptor);

}  // namespace clice::control

namespace kota::ipc::protocol {

template <>
struct RequestTraits<clice::control::IndexParams> {
    using Result = clice::control::IndexResult;
    constexpr inline static std::string_view method = "clice/index";
};

}  // namespace kota::ipc::protocol
