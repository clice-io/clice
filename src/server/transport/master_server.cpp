#include "server/transport/master_server.h"

#include <list>
#include <memory>
#include <string>
#include <vector>

#include "server/protocol/worker.h"
#include "server/state/file_tracker.h"
#include "server/transport/agent_client.h"
#include "server/transport/lsp_client.h"
#include "support/anomaly.h"
#include "support/filesystem.h"
#include "support/logging.h"
#include "support/timer.h"

#include "kota/async/async.h"
#include "kota/codec/json/json.h"
#include "kota/ipc/codec/json.h"
#include "kota/ipc/lsp/protocol.h"
#include "kota/ipc/lsp/uri.h"
#include "kota/ipc/recording_transport.h"
#include "kota/ipc/transport.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/Process.h"

namespace clice {

namespace lsp = kota::ipc::lsp;
namespace protocol = kota::ipc::protocol;

MasterServer::MasterServer(kota::event_loop& loop, std::string self_path) :
    loop(loop), pool(loop), contexts(workspace), compiler(loop, workspace, contexts, pool),
    index_query(workspace, sessions), indexer(loop, workspace, pool, contexts, sessions),
    features(compiler, index_query, workspace, contexts, indexer),
    invalidator(workspace, sessions, contexts), bg_tasks(loop), self_path(std::move(self_path)) {}

MasterServer::~MasterServer() = default;

void MasterServer::initialize() {
    config_issues.clear();
    config_path.clear();
    // Load clice.toml raw and overlay initializationOptions BEFORE computing
    // defaults: derived fields (logging_dir, index_dir, ...) must follow the
    // final merged values (e.g. a cache_dir overridden by the client).
    workspace.config = Config::load_from_workspace(workspace_root,
                                                   &config_issues,
                                                   &config_path,
                                                   /*with_defaults=*/false);
    if(!init_options_json.empty()) {
        if(auto ov = kota::codec::json::parse(init_options_json, workspace.config); !ov) {
            LOG_GUIDANCE("Failed to apply initializationOptions: {}", ov.error().to_string());
        } else {
            LOG_INFO("Applied initializationOptions overlay");
        }
        init_options_json.clear();
    }
    workspace.config.apply_defaults(workspace_root);

    auto& cfg = workspace.config.project;

    if(!cfg.logging_dir.empty()) {
        auto now = std::chrono::system_clock::now();
        auto pid = llvm::sys::Process::getProcessId();
        session_log_dir =
            path::join(cfg.logging_dir, std::format("{:%Y-%m-%d_%H-%M-%S}_{}", now, pid));
        logging::file_logger("master", session_log_dir, logging::options);
        LOG_INFO("Session log directory: {}", session_log_dir);
    }

    LOG_INFO("Server ready (stateful={}, stateless={}, idle={}ms)",
             cfg.stateful_worker_count.value,
             cfg.stateless_worker_count.value,
             *cfg.idle_timeout_ms);

    WorkerPoolOptions pool_opts;
    pool_opts.self_path = self_path;
    pool_opts.stateful_count = cfg.stateful_worker_count;
    pool_opts.stateless_count = cfg.stateless_worker_count;
    pool_opts.min_stateless = cfg.min_stateless_worker_count;
    pool_opts.max_stateless = cfg.max_stateless_worker_count;
    pool_opts.worker_memory_limit = cfg.worker_memory_limit;
    pool_opts.log_dir = session_log_dir;
    if(!pool.start(pool_opts)) {
        LOG_ANOMALY(WorkerSpawnFail, "Failed to start worker pool");
        return;
    }

    lifecycle = ServerLifecycle::Ready;

    wire();

    load_workspace();

    if(!workspace_root.empty()) {
        // Construct after the workspace load so the tracker's baseline CDB
        // stamp matches the database that was just loaded.
        tracker = std::make_unique<FileTracker>(workspace, sessions, workspace_root);
        auto& tracker_cfg = workspace.config.tracker;
        if(*tracker_cfg.cdb_poll_seconds > 0) {
            bg_tasks.spawn(cdb_poll_task());
        }
        if(*tracker_cfg.workspace_poll_seconds > 0) {
            bg_tasks.spawn(workspace_poll_task());
        }
    }
}

kota::task<> MasterServer::cdb_poll_task() {
    auto interval = std::chrono::seconds(*workspace.config.tracker.cdb_poll_seconds);
    while(true) {
        co_await kota::sleep(interval);
        auto events = tracker->tick_cdb();
        if(!events.empty()) {
            dispatch(events);
        }
    }
}

kota::task<> MasterServer::workspace_poll_task() {
    auto interval = std::chrono::seconds(*workspace.config.tracker.workspace_poll_seconds);
    while(true) {
        co_await kota::sleep(interval);
        auto events = co_await tracker->tick_workspace();
        if(!events.empty()) {
            dispatch(events);
        }
    }
}

void MasterServer::wire() {
    pool.on_crash = [this](const WorkerCrashInfo& info) {
        if(!info.stateful)
            return;
        dispatch(FileEvent::worker_crashed(info.lost_documents));
    };

    pool.on_evicted = [this](const std::string& path) {
        auto it = workspace.path_pool.cache.find(path);
        if(it == workspace.path_pool.cache.end()) {
            LOG_WARN("Evicted path not in pool: {}", path);
            return;
        }
        pool.remove_owner(it->second);
    };

    compiler.on_indexing_needed = [this]() {
        indexer.schedule();
    };
}

void MasterServer::initialize(llvm::StringRef root) {
    workspace_root = root.str();
    initialize();
}

std::shared_ptr<Session> MasterServer::find_session(std::uint32_t path_id) {
    return sessions.find(path_id);
}

std::shared_ptr<Session> MasterServer::open_session(std::uint32_t path_id) {
    return sessions.open(path_id);
}

void MasterServer::close_session(std::uint32_t path_id, kota::ipc::JsonPeer& peer) {
    namespace protocol = kota::ipc::protocol;

    auto path = workspace.path_pool.resolve(path_id);
    // Route the eviction notification before dropping ownership:
    // notify_stateful uses the owner table to find the worker.
    pool.notify_stateful(path_id, worker::EvictParams{std::string(path)});
    pool.remove_owner(path_id);

    protocol::PublishDiagnosticsParams diag_params;
    auto uri = lsp::URI::from_file_path(std::string(path));
    if(uri)
        diag_params.uri = uri->str();
    diag_params.diagnostics = {};
    peer.send_notification(diag_params);

    sessions.close(path_id);

    dispatch(FileEvent::buffer_closed(path_id));

    LOG_DEBUG("didClose: {}", path);
}

void MasterServer::dispatch(llvm::ArrayRef<FileEvent> events) {
    auto dirty = invalidator.apply(events);

    for(auto path_id: dirty.reset_trial) {
        if(auto session = sessions.find(path_id)) {
            session->trial_done = false;
        }
    }

    for(auto path_id: dirty.reset_header_mode) {
        contexts.reset_header_mode(path_id);
    }

    for(auto path_id: dirty.mark_ast_dirty) {
        if(auto session = sessions.find(path_id)) {
            session->ast_dirty = true;
            session->trial_done = false;
            // Invalidate in-flight compiles so they cannot clobber the
            // reset state when they finish (same as switchContext).
            session->generation += 1;
        }
        contexts.forget_self_contained(path_id);
    }

    for(auto path_id: dirty.mark_lost) {
        if(auto session = sessions.find(path_id)) {
            session->ast_dirty = true;
        }
    }

    // Headers whose synthesized preamble embeds changed chain content:
    // zeroing build_at forces deps_changed() to re-validate every chain
    // file by content hash; open sessions also recompile and re-trial.
    for(auto path_id: dirty.force_revalidate) {
        contexts.invalidate_header_deps(path_id);
        if(auto session = sessions.find(path_id)) {
            session->ast_dirty = true;
            session->trial_done = false;
            session->generation += 1;
        }
    }

    // The header's borrowed compile command changed: its resolved context
    // (and synthesized preamble) describes flags that no longer exist, so
    // the next use must re-resolve. Session dirtying arrives in the same
    // DirtySet via mark_ast_dirty.
    for(auto path_id: dirty.drop_context) {
        contexts.drop_header_context(path_id);
    }

    for(auto path_id: dirty.enqueue_reindex) {
        indexer.enqueue(path_id);
    }

    if(dirty.ensure_compile_graph && !workspace.compile_graph) {
        compiler.init_compile_graph();
    }

    bool save = dirty.save_cache;
    if(dirty.recheck_contexts) {
        save |= contexts.drop_orphaned_choices(sessions);
    }
    if(save) {
        workspace.save_cache(contexts);
    }

    if(dirty.reschedule_indexing) {
        indexer.schedule();
    }
}

void MasterServer::schedule_shutdown() {
    if(lifecycle == ServerLifecycle::Exited)
        return;
    lifecycle = ServerLifecycle::ShuttingDown;
    shutdown_event.set();
}

kota::task<> MasterServer::shutdown_and_cleanup() {
    bg_tasks.cancel();
    co_await bg_tasks.join();
    // Quiesce in-flight compilation and indexing first so the persisted
    // snapshot below covers everything that actually completed.
    co_await kota::when_all(indexer.stop(), compiler.stop());
    co_await indexer.save();
    workspace.save_cache(contexts);
    co_await pool.stop();
    if(workspace.store) {
        workspace.store->shutdown();
    }
    lifecycle = ServerLifecycle::Exited;
}

kota::task<> MasterServer::cache_checkpoint_task() {
    constexpr auto interval = std::chrono::minutes(5);
    while(true) {
        co_await kota::sleep(interval);
        if(workspace.store) {
            // Offload to the thread pool: checkpoint writes the manifest.
            co_await kota::queue([this] { workspace.store->checkpoint(); });
        }
    }
}

void MasterServer::open_cache_store() {
    auto& cfg = workspace.config.project;
    if(workspace.store || cfg.cache_dir.empty())
        return;

    auto store = CacheStore::open(cfg.cache_dir, cache_format_version);
    if(!store) {
        LOG_WARN("Failed to open cache store at {}: {}",
                 std::string_view(cfg.cache_dir),
                 store.error().message());
        return;
    }

    // Size budgets are deliberately generous: eviction exists to bound
    // disk usage, not to keep the working set tight.
    constexpr std::uint64_t GiB = 1ull << 30;
    store->register_namespace(
        {.name = "pch", .extension = ".pch", .policy = CachePolicy::LRU, .max_bytes = 8 * GiB});
    store->register_namespace(
        {.name = "pcm", .extension = ".pcm", .policy = CachePolicy::LRU, .max_bytes = 8 * GiB});
    store->register_namespace(
        {.name = "index", .extension = ".idx", .policy = CachePolicy::Persistent});
    store->register_namespace(
        {.name = "header_context", .extension = ".h", .policy = CachePolicy::Scratch});
    workspace.store.emplace(std::move(*store));
    LOG_INFO("Cache store: {}", workspace.store->base_dir());

    workspace.load_cache(contexts);
    bg_tasks.spawn(cache_checkpoint_task());
}

void MasterServer::load_workspace() {
    if(workspace_root.empty())
        return;

    auto& cfg = workspace.config.project;

    open_cache_store();

    auto cdb_path = discover_compile_commands(workspace.config, workspace_root);
    if(cdb_path.empty()) {
        LOG_GUIDANCE(
            "No compile_commands.json found in workspace {}. Compile commands will be " "guessed; see https://clice.io/en/guide/quick-start for setup.",
            workspace_root);
        // Persisted index shards are CDB-independent; load them so a
        // database generated later (picked up by the CDB poll) starts from
        // the previous session's index.
        indexer.load();
        return;
    }

    ScopedTimer cdb_timer;
    auto count = workspace.cdb.load(cdb_path).value_or(0);
    LOG_INFO("Loaded CDB from {} with {} entries", cdb_path, count);
    LOG_PERF("startup", "phase=cdb_load entries={} elapsed_ms={}", count, cdb_timer.ms());

    auto report = scan_dependency_graph(workspace.cdb,
                                        workspace.toolchain,
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
    LOG_PERF("startup",
             "phase=dep_scan files={} edges={} elapsed_ms={}",
             report.total_files,
             report.total_edges,
             report.elapsed_ms);

    workspace.build_module_map();
    indexer.load();

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
    kota::task_group<> group(loop);
    bool lsp_registered = false;

    group.spawn([](MasterServer& server,
                   kota::tcp::acceptor& acceptor,
                   bool register_lsp,
                   std::list<Connection>& connections,
                   kota::task_group<>& group,
                   bool& lsp_registered) -> kota::task<> {
        auto& loop = kota::event_loop::current();

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

            group.spawn(run_connection(peer_ptr, connections, it));
        }
    }(server, acceptor, register_lsp, connections, group, lsp_registered));

    co_await group.join();
}

int run_serve_mode(const ServerOptions& opts, const char* self_path) {
    logging::stderr_logger("master", logging::options);

    auto mode = opts.mode.value_or(ServerMode::Pipe);
    auto host = opts.host.value_or("127.0.0.1");
    auto port = opts.port.value_or(0);
    auto record = opts.record.value_or("");
    auto ws = opts.workspace.value_or("");

    LOG_INFO("clice master starting: pid={}, mode={}, workspace={}",
             llvm::sys::Process::getProcessId(),
             mode == ServerMode::Pipe ? "pipe" : "socket",
             ws.empty() ? "<from LSP initialize>" : ws);

    if(mode == ServerMode::Socket && (port <= 0 || port > 65535)) {
        LOG_ERROR("--port must be between 1 and 65535 in socket mode");
        return 1;
    }

    kota::event_loop loop;
    MasterServer server(loop, self_path);
    std::list<Connection> connections;

    if(mode == ServerMode::Pipe) {
        auto transport = kota::ipc::StreamTransport::open_stdio(loop);
        if(!transport) {
            LOG_ERROR("failed to open stdio transport");
            return 1;
        }

        // Pre-initialize for standalone (no-editor) use; LSP initialize will be rejected.
        if(!ws.empty())
            server.initialize(ws);

        std::unique_ptr<kota::ipc::Transport> final_transport = std::move(*transport);
        if(!record.empty()) {
            final_transport =
                std::make_unique<kota::ipc::RecordingTransport>(std::move(final_transport), record);
        }

        kota::ipc::JsonPeer lsp_peer(loop, std::move(final_transport));
        LSPClient lsp_client(server, lsp_peer);

        kota::tcp::acceptor agent_acceptor;
        bool has_agent_acceptor = false;

        if(port > 0) {
            auto acceptor = kota::tcp::listen(host, port, {}, loop);
            if(acceptor) {
                LOG_INFO("Agentic protocol listening on {}:{}", host, port);
                agent_acceptor = std::move(*acceptor);
                has_agent_acceptor = true;
            } else {
                LOG_WARN("Failed to start agentic listener on {}:{}", host, port);
            }
        }

        loop.schedule([](MasterServer& server,
                         kota::ipc::JsonPeer& peer,
                         std::list<Connection>& connections,
                         kota::tcp::acceptor acceptor,
                         bool has_acceptor) -> kota::task<> {
            if(has_acceptor) {
                co_await kota::when_any(
                    peer.run(),
                    accept_connections(server, std::move(acceptor), false, connections),
                    server.get_shutdown_event().wait());
            } else {
                co_await kota::when_any(peer.run(), server.get_shutdown_event().wait());
            }
            co_await server.shutdown_and_cleanup();
        }(server, lsp_peer, connections, std::move(agent_acceptor), has_agent_acceptor));
        loop.run();
        return 0;
    }

    if(mode == ServerMode::Socket) {
        auto acceptor = kota::tcp::listen(host, port, {}, loop);
        if(!acceptor) {
            LOG_ERROR("failed to listen on {}:{}", host, port);
            return 1;
        }

        if(!ws.empty())
            server.initialize(ws);

        bool register_lsp = ws.empty();
        LOG_INFO("Listening on {}:{} ...", host, port);
        loop.schedule([](MasterServer& server,
                         kota::tcp::acceptor acceptor,
                         bool register_lsp,
                         std::list<Connection>& connections) -> kota::task<> {
            co_await kota::when_any(
                accept_connections(server, std::move(acceptor), register_lsp, connections),
                server.get_shutdown_event().wait());
            co_await server.shutdown_and_cleanup();
        }(server, std::move(*acceptor), register_lsp, connections));
        loop.run();
        return 0;
    }

    LOG_ERROR("unexpected server mode");
    return 1;
}

}  // namespace clice
