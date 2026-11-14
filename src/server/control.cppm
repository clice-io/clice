/// The control channel of a serving writer: what the commands that find
/// the index writer lock taken by a server ask of it, and the server's side
/// answering. Paths are absolute filesystem paths.

module clice:server.control;

import :index.database.writer_lock;
import :support.prelude;

namespace clice {

class ServedProject;

}  // namespace clice

namespace clice::control {

/// Run a pass over the build — every unit re-validated against the disk, the ones
/// the hash gate finds current skipped — and answer once the index is
/// persisted. Refused when `configuration` is not the one the server
/// indexes, or when its settings keep background indexing off.
struct IndexParams {
    std::string configuration;
};

struct IndexResult {
    /// Units whose index is not current after the pass: the attempt
    /// failed for good, or an open document's compile serves it.
    std::vector<std::string> failed;
};

/// What a server answers first, within a bound: that it serves at all.
struct StatusParams {};

struct StatusResult {
    std::uint32_t pid = 0;
};

/// Why a request to the serving writer failed.
struct AskError {
    std::string message;

    /// The server did not answer within the bound — hung, or stopped — so
    /// the asker may go on without it.
    bool unanswered = false;
};

/// Ask the serving writer at `endpoint` to run a pass over the build and
/// wait for the index to be persisted. Runs to completion on an event loop of its
/// own; a refused connection, a server that does not answer its status
/// within the bound (`CLICE_TEST_CONTROL_WAIT_MS` for the tests), or one
/// that goes away mid-request is the error.
std::expected<IndexResult, AskError> request_index(const index::ServerEndpoint& endpoint,
                                                   llvm::StringRef configuration);

/// Where a project's control channel listens.
struct Listener {
    async::pipe::acceptor acceptor;

    /// The socket's path, a named pipe's name on Windows.
    std::string name;

    /// The directory of the user's own a socket is made in when its path
    /// in the cache directory would not fit a socket's; removed with it.
    std::optional<fs::TempDirectory> directory;
};

/// Listen for the control channel of the project whose cache directory is
/// `cache_dir`, which only its user can connect to: a socket in the cache
/// directory, a named pipe on Windows.
std::expected<Listener, std::string> listen(llvm::StringRef cache_dir, async::event_loop& loop);

/// Serve a project's control channel: accept connections on `acceptor`
/// for as long as the task runs, each answering the control requests
/// against the project.
async::task<> serve(ServedProject& project, async::pipe::acceptor acceptor);

}  // namespace clice::control

namespace kotatsu::ipc::protocol {

template <>
struct RequestTraits<clice::control::StatusParams> {
    using Result = clice::control::StatusResult;
    constexpr inline static std::string_view method = "clice/status";
};

template <>
struct RequestTraits<clice::control::IndexParams> {
    using Result = clice::control::IndexResult;
    constexpr inline static std::string_view method = "clice/index";
};

}  // namespace kotatsu::ipc::protocol
