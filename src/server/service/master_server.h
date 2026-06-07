#pragma once

#include <cstdint>
#include <string>

#include "server/compiler/compiler.h"
#include "server/compiler/indexer.h"
#include "server/service/session.h"
#include "server/worker/worker_pool.h"
#include "server/workspace/workspace.h"

#include "kota/async/async.h"
#include "kota/deco/deco.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/StringRef.h"

namespace clice {

namespace deco = kota::deco;
using deco::decl::KVStyle;

enum class ServerMode : std::uint8_t { Pipe, Socket, Relay, Daemon };

struct ServerOptions {
    DecoFlag(names = {"-h", "--help"}, help = "Show help", required = false)
    help;

    DecoKV(style = KVStyle::JoinedOrSeparate,
           help = "Server mode: pipe (default), socket, relay, or daemon",
           required = false)
    <ServerMode> mode = ServerMode::Pipe;

    DecoKV(style = KVStyle::JoinedOrSeparate, help = "Socket mode address", required = false)
    <std::string> host = "127.0.0.1";

    DecoKV(style = KVStyle::JoinedOrSeparate,
           help = "Agentic TCP port (0 = disabled)",
           required = false)
    <int> port = 0;

    DecoKV(style = KVStyle::JoinedOrSeparate,
           help = "Record LSP input to file for replay testing",
           required = false)
    <std::string> record;

    DecoKV(style = KVStyle::JoinedOrSeparate,
           help = "Unix domain socket path (relay/daemon mode)",
           required = false)
    <std::string> socket;

    DecoKV(style = KVStyle::JoinedOrSeparate,
           help = "Workspace root directory (daemon mode)",
           required = false)
    <std::string> workspace;

    DecoKV(style = KVStyle::JoinedOrSeparate,
           names = {"--log-level", "--log-level="},
           help = "Log level: trace, debug, info, warn, error, off",
           required = false)
    <std::string> log_level = "info";
};

enum class ServerLifecycle : std::uint8_t {
    Uninitialized,
    Initialized,
    Ready,
    ShuttingDown,
    Exited,
};

/// Core server state — owns the two-layer state model (Workspace + Sessions),
/// the worker pool, compilation engine, and indexer.
///
/// Does NOT own any transport or peer.  Protocol-specific handler registration
/// is done by LSPClient and AgentClient, which access private members directly.
class MasterServer {
    friend class LSPClient;
    friend class AgentClient;

public:
    MasterServer(kota::event_loop& loop, std::string self_path);
    ~MasterServer();

    void initialize();
    void initialize(llvm::StringRef root);

    kota::task<> file_watcher_task();
    kota::task<> shutdown_and_cleanup();

    Session* find_session(std::uint32_t path_id);
    Session& open_session(std::uint32_t path_id);
    void close_session(std::uint32_t path_id, kota::ipc::JsonPeer& peer);

    void on_file_saved(std::uint32_t path_id);

    void schedule_shutdown();

    kota::event& get_shutdown_event() {
        return shutdown_event;
    }

private:
    kota::event shutdown_event;
    void load_workspace();

    kota::event_loop& loop;

    Workspace workspace;
    llvm::DenseMap<std::uint32_t, Session> sessions;
    WorkerPool pool;
    Compiler compiler;
    Indexer indexer;

    ServerLifecycle lifecycle = ServerLifecycle::Uninitialized;
    std::string self_path;
    std::string workspace_root;
    std::string session_log_dir;
    std::string init_options_json;
};

int run_server_mode(const ServerOptions& opts, const char* self_path);

int run_daemon_mode(const ServerOptions& opts, const char* self_path);

}  // namespace clice
