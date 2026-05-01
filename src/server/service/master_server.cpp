#include "server/service/master_server.h"

#include <list>
#include <memory>
#include <string>
#include <vector>

#include "server/protocol/worker.h"
#include "server/service/agent_client.h"
#include "server/service/lsp_client.h"
#include "support/filesystem.h"
#include "support/logging.h"

#include "kota/async/async.h"
#include "kota/codec/json/json.h"
#include "kota/ipc/codec/json.h"
#include "kota/ipc/lsp/protocol.h"
#include "kota/ipc/lsp/uri.h"
#include "kota/ipc/recording_transport.h"
#include "kota/ipc/transport.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Process.h"

namespace clice {

namespace lsp = kota::ipc::lsp;
namespace protocol = kota::ipc::protocol;

MasterServer::MasterServer(kota::event_loop& loop, std::string self_path) :
    loop(loop), pool(loop), compiler(loop, workspace, pool, sessions),
    indexer(loop,
            workspace,
            sessions,
            pool,
            compiler,
            [this](uint32_t proj_path_id) {
                auto path = workspace.project_index.path_pool.path(proj_path_id);
                auto server_id = workspace.path_pool.intern(path);
                return sessions.contains(server_id);
            }),
    self_path(std::move(self_path)) {}

MasterServer::~MasterServer() = default;

void MasterServer::initialize() {
    workspace.config = Config::load_from_workspace(workspace_root);
    if(!init_options_json.empty()) {
        if(auto ov = kota::codec::json::parse(init_options_json, workspace.config); !ov) {
            LOG_WARN("Failed to apply initializationOptions: {}", ov.error().to_string());
        } else {
            workspace.config.apply_defaults(workspace_root);
            LOG_INFO("Applied initializationOptions overlay");
        }
        init_options_json.clear();
    }

    auto& cfg = workspace.config.project;

    if(!cfg.logging_dir.empty()) {
        auto now = std::chrono::system_clock::now();
        auto pid = llvm::sys::Process::getProcessId();
        session_log_dir =
            path::join(cfg.logging_dir, std::format("{:%Y-%m-%d_%H-%M-%S}_{}", now, pid));
        logging::file_logger("master", session_log_dir, logging::options);
    }

    LOG_INFO("Server ready (stateful={}, stateless={}, idle={}ms)",
             cfg.stateful_worker_count.value,
             cfg.stateless_worker_count.value,
             *cfg.idle_timeout_ms);

    WorkerPoolOptions pool_opts;
    pool_opts.self_path = self_path;
    pool_opts.stateful_count = cfg.stateful_worker_count;
    pool_opts.stateless_count = cfg.stateless_worker_count;
    pool_opts.worker_memory_limit = cfg.worker_memory_limit;
    pool_opts.log_dir = session_log_dir;
    if(!pool.start(pool_opts)) {
        LOG_ERROR("Failed to start worker pool");
        return;
    }

    lifecycle = ServerLifecycle::Ready;

    compiler.on_indexing_needed = [this]() {
        indexer.schedule();
    };

    indexer.set_max_concurrency(cfg.stateless_worker_count.value);

    load_workspace();
}

Session* MasterServer::find_session(std::uint32_t path_id) {
    auto it = sessions.find(path_id);
    return it != sessions.end() ? &it->second : nullptr;
}

Session& MasterServer::open_session(std::uint32_t path_id) {
    auto [it, inserted] = sessions.try_emplace(path_id);
    auto& session = it->second;
    if(!inserted)
        session = Session{};
    session.path_id = path_id;
    return session;
}

void MasterServer::close_session(std::uint32_t path_id, kota::ipc::JsonPeer& peer) {
    namespace protocol = kota::ipc::protocol;

    auto path = workspace.path_pool.resolve(path_id);
    workspace.on_file_closed(path_id);
    pool.notify_stateful(path_id, worker::EvictParams{std::string(path)});

    protocol::PublishDiagnosticsParams diag_params;
    auto uri = lsp::URI::from_file_path(std::string(path));
    if(uri)
        diag_params.uri = uri->str();
    diag_params.diagnostics = {};
    peer.send_notification(diag_params);

    sessions.erase(path_id);

    indexer.enqueue(path_id);
    indexer.schedule();

    LOG_DEBUG("didClose: {}", path);
}

void MasterServer::on_file_saved(std::uint32_t path_id) {
    auto dirtied = workspace.on_file_saved(path_id);
    for(auto dirty_id: dirtied) {
        if(auto* session = find_session(dirty_id)) {
            session->ast_dirty = true;
        } else {
            indexer.enqueue(dirty_id);
        }
    }

    for(auto& [hdr_id, session]: sessions) {
        if(session.header_context && session.header_context->host_path_id == path_id) {
            session.header_context.reset();
            session.ast_dirty = true;
        }
    }

    indexer.schedule();
}

void MasterServer::schedule_shutdown() {
    indexer.save(workspace.config.project.index_dir);
    workspace.save_cache();

    loop.schedule([this]() -> kota::task<> {
        co_await compiler.stop();
        co_await pool.stop();
        loop.stop();
    }());
}

void MasterServer::load_workspace() {
    if(workspace_root.empty())
        return;

    auto& cfg = workspace.config.project;

    if(!cfg.cache_dir.empty()) {
        auto ec = llvm::sys::fs::create_directories(cfg.cache_dir);
        if(ec) {
            LOG_WARN("Failed to create cache directory {}: {}",
                     std::string_view(cfg.cache_dir),
                     ec.message());
        } else {
            LOG_INFO("Cache directory: {}", std::string_view(cfg.cache_dir));
        }

        for(auto* subdir: {"cache/pch", "cache/pcm"}) {
            auto dir = path::join(cfg.cache_dir, subdir);
            if(auto ec2 = llvm::sys::fs::create_directories(dir))
                LOG_WARN("Failed to create {}: {}", dir, ec2.message());
        }

        workspace.cleanup_cache();
        workspace.load_cache();
    }

    std::string cdb_path;
    for(auto& configured: cfg.compile_commands_paths) {
        if(llvm::sys::fs::is_directory(configured)) {
            auto candidate = path::join(configured, "compile_commands.json");
            if(llvm::sys::fs::exists(candidate)) {
                cdb_path = std::move(candidate);
                break;
            }
        } else if(llvm::sys::fs::exists(configured)) {
            cdb_path = configured;
            break;
        } else {
            LOG_WARN("Configured compile_commands_path not found: {}", configured);
        }
    }

    if(cdb_path.empty()) {
        auto try_candidate = [&](llvm::StringRef dir) -> bool {
            auto candidate = path::join(dir, "compile_commands.json");
            if(llvm::sys::fs::exists(candidate)) {
                cdb_path = std::move(candidate);
                return true;
            }
            return false;
        };

        if(!try_candidate(workspace_root)) {
            std::error_code ec;
            for(llvm::sys::fs::directory_iterator it(workspace_root, ec), end; it != end && !ec;
                it.increment(ec)) {
                if(it->type() == llvm::sys::fs::file_type::directory_file) {
                    if(try_candidate(it->path()))
                        break;
                }
            }
        }
    }

    if(cdb_path.empty()) {
        LOG_WARN("No compile_commands.json found in workspace {}", workspace_root);
        return;
    }

    auto count = workspace.cdb.load(cdb_path);
    LOG_INFO("Loaded CDB from {} with {} entries", cdb_path, count);

    auto report = scan_dependency_graph(workspace.cdb,
                                        workspace.path_pool,
                                        workspace.dep_graph,
                                        /*cache=*/nullptr,
                                        [this](llvm::StringRef path,
                                               std::vector<std::string>& append,
                                               std::vector<std::string>& remove) {
                                            workspace.config.match_rules(path, append, remove);
                                        });
    workspace.dep_graph.build_reverse_map();

    auto unresolved = report.includes_found - report.includes_resolved;
    double accuracy =
        report.includes_found > 0
            ? 100.0 * static_cast<double>(report.includes_resolved) / report.includes_found
            : 100.0;
    LOG_INFO(
        "Dependency scan: {}ms, {} files ({} source + {} header), " "{} edges, {}/{} resolved ({:.1f}%), {} waves",
        report.elapsed_ms,
        report.total_files,
        report.source_files,
        report.header_files,
        report.total_edges,
        report.includes_resolved,
        report.includes_found,
        accuracy,
        report.waves);
    if(unresolved > 0)
        LOG_WARN("{} unresolved includes", unresolved);

    workspace.build_module_map();
    indexer.load(cfg.index_dir);

    if(*cfg.enable_indexing) {
        for(auto& entry: workspace.cdb.get_entries()) {
            auto file = workspace.cdb.resolve_path(entry.file);
            auto server_id = workspace.path_pool.intern(file);
            indexer.enqueue(server_id);
        }
        indexer.schedule();
    }

    compiler.init_compile_graph();
}

struct Connection {
    std::unique_ptr<kota::ipc::JsonPeer> peer;
    std::unique_ptr<LSPClient> lsp_client;
    std::unique_ptr<AgentClient> agent_client;
};

static kota::task<> run_connection(kota::ipc::JsonPeer* peer,
                                   std::list<Connection>& connections,
                                   std::list<Connection>::iterator pos) {
    co_await peer->run();
    LOG_INFO("Client disconnected");
    connections.erase(pos);
}

static kota::task<> accept_connections(MasterServer& server,
                                       kota::tcp::acceptor acceptor,
                                       bool register_lsp,
                                       std::list<Connection>& connections) {
    auto& loop = kota::event_loop::current();
    kota::task_group<> connection_group(loop);
    bool lsp_registered = false;

    while(true) {
        auto conn = co_await acceptor.accept();
        if(!conn.has_value())
            break;

        LOG_INFO("Client connected");

        auto transport = std::make_unique<kota::ipc::StreamTransport>(std::move(*conn));
        auto peer = std::make_unique<kota::ipc::JsonPeer>(loop, std::move(transport));

        std::unique_ptr<LSPClient> lsp;
        if(register_lsp && !lsp_registered) {
            lsp = std::make_unique<LSPClient>(server, *peer);
            lsp_registered = true;
        }
        auto agent = std::make_unique<AgentClient>(server, *peer);

        auto* peer_ptr = peer.get();
        auto it = connections.emplace(connections.end(),
                                      Connection{
                                          .peer = std::move(peer),
                                          .lsp_client = std::move(lsp),
                                          .agent_client = std::move(agent),
                                      });

        connection_group.spawn(run_connection(peer_ptr, connections, it));
    }

    co_await connection_group.join();
}

int run_server_mode(const ServerOptions& opts) {
    logging::stderr_logger("master", logging::options);

    kota::event_loop loop;
    MasterServer server(loop, opts.self_path);
    std::list<Connection> connections;

    if(opts.mode == "pipe") {
        auto transport = kota::ipc::StreamTransport::open_stdio(loop);
        if(!transport) {
            LOG_ERROR("failed to open stdio transport");
            return 1;
        }

        std::unique_ptr<kota::ipc::Transport> final_transport = std::move(*transport);
        if(!opts.record.empty()) {
            final_transport =
                std::make_unique<kota::ipc::RecordingTransport>(std::move(final_transport),
                                                                opts.record);
        }

        kota::ipc::JsonPeer lsp_peer(loop, std::move(final_transport));
        LSPClient lsp_client(server, lsp_peer);

        if(opts.port > 0) {
            auto acceptor = kota::tcp::listen(opts.host, opts.port, {}, loop);
            if(acceptor) {
                LOG_INFO("Agentic protocol listening on {}:{}", opts.host, opts.port);
                loop.schedule(accept_connections(server, std::move(*acceptor), false, connections));
            } else {
                LOG_WARN("Failed to start agentic listener on {}:{}", opts.host, opts.port);
            }
        }

        loop.schedule(lsp_peer.run());
        loop.run();
        return 0;
    }

    if(opts.mode == "socket") {
        auto acceptor = kota::tcp::listen(opts.host, opts.port, {}, loop);
        if(!acceptor) {
            LOG_ERROR("failed to listen on {}:{}", opts.host, opts.port);
            return 1;
        }

        LOG_INFO("Listening on {}:{} ...", opts.host, opts.port);
        loop.schedule(accept_connections(server, std::move(*acceptor), true, connections));
        loop.run();
        return 0;
    }

    LOG_ERROR("unknown server mode '{}'", opts.mode);
    return 1;
}

}  // namespace clice
