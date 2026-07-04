#include "server/service/master_server.h"

#include <list>
#include <memory>
#include <string>
#include <vector>

#include "feature/feature.h"
#include "server/protocol/worker.h"
#include "server/service/agent_client.h"
#include "server/service/lsp_client.h"
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
    loop(loop), bg_tasks(loop), pool(loop), compiler(loop, workspace, pool),
    indexer(
        loop,
        workspace,
        pool,
        compiler,
        [this](uint32_t server_path_id) { return sessions.contains(server_path_id); },
        [this](Indexer::SessionVisitor visitor) {
            for(auto& [path_id, session]: sessions) {
                // FIXME: when ast_dirty, consider awaiting recompilation
                // instead of silently falling back to MergedIndex.
                if(session && session->file_index && session->symbols && !session->ast_dirty) {
                    if(!visitor(path_id, *session))
                        break;
                }
            }
        }),
    self_path(std::move(self_path)) {}

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

    pool.on_crash = [this](const WorkerCrashInfo& info) {
        if(!info.stateful)
            return;
        for(auto path_id: info.lost_documents) {
            if(auto it = sessions.find(path_id); it != sessions.end())
                it->second->ast_dirty = true;
        }
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

    load_workspace();
}

void MasterServer::initialize(llvm::StringRef root) {
    workspace_root = root.str();
    initialize();
}

std::shared_ptr<Session> MasterServer::find_session(std::uint32_t path_id) {
    auto it = sessions.find(path_id);
    return it != sessions.end() ? it->second : nullptr;
}

std::shared_ptr<Session> MasterServer::open_session(std::uint32_t path_id) {
    auto it = sessions.find(path_id);
    if(it != sessions.end()) {
        it->second->generation++;
    }
    auto session = std::make_shared<Session>();
    session->path_id = path_id;
    sessions[path_id] = session;
    return session;
}

void MasterServer::close_session(std::uint32_t path_id, kota::ipc::JsonPeer& peer) {
    namespace protocol = kota::ipc::protocol;

    auto path = workspace.path_pool.resolve(path_id);
    workspace.on_file_closed(path_id);
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

    auto it = sessions.find(path_id);
    if(it != sessions.end()) {
        it->second->generation++;
        sessions.erase(it);
    }

    indexer.enqueue(path_id);
    indexer.schedule();

    LOG_DEBUG("didClose: {}", path);
}

void MasterServer::on_file_saved(std::uint32_t path_id) {
    // The saved file's own self-containment may have changed; re-evaluate
    // on its next compile.
    workspace.header_modes.erase(path_id);
    workspace.header_mode_hashes.erase(path_id);
    if(auto session = find_session(path_id)) {
        session->trial_done = false;
    }

    auto dirtied = workspace.rescan_after_save(path_id);
    for(auto dirty_id: dirtied) {
        auto session = find_session(dirty_id);
        if(session) {
            session->ast_dirty = true;
            session->trial_done = false;
            workspace.forget_self_contained(dirty_id);
        } else {
            indexer.enqueue(dirty_id);
        }
    }

    // Header sessions whose include chain contains the saved file must
    // re-synthesize their preamble: it embeds the chain files' content, so
    // neither the dependents cascade above nor clang's own dependency
    // tracking catches this. Zeroing build_at forces deps_changed() to
    // re-validate every chain file by content hash. Do NOT reset the
    // context itself: an in-flight compile can clobber ast_dirty when it
    // finishes, and the surviving snapshot is what lets is_stale() recover.
    for(auto& [session_id, session]: sessions) {
        if(session->header_context && llvm::is_contained(session->header_context->chain, path_id)) {
            session->header_context->deps.build_at = 0;
            session->ast_dirty = true;
            session->trial_done = false;
            // The chain change may have made the header self-contained
            // (e.g. a dependency now provides the missing declarations);
            // drop the persisted verdict so the trial can downgrade it.
            workspace.header_modes.erase(session_id);
            workspace.header_mode_hashes.erase(session_id);
        }
    }

    // A save can remove the include edge a user's context choice depends
    // on. A stale active_context suppresses automatic host resolution, so
    // it would strand the header on the fallback command (or silently pin
    // its command hash to a different host) — drop it instead. The include
    // graph was already rescanned above.
    bool dropped_saved = false;
    for(auto& [session_id, session]: sessions) {
        if(!session->active_context.has_value()) {
            continue;
        }
        auto host_id = session->active_context->host_path_id;
        auto& occurrence = session->active_context->occurrence;
        bool orphaned = workspace.dep_graph.find_include_chain(host_id, session_id).empty();
        // A pinned occurrence can vanish while other inclusions of the
        // header survive (the chain stays non-empty) — recount it.
        if(!orphaned && occurrence.has_value()) {
            auto count = workspace.count_occurrences(host_id, session_id);
            orphaned = count > 0 && *occurrence >= count;
        }
        if(orphaned) {
            LOG_INFO("Dropping orphaned context choice for {}: host {} no longer includes it",
                     workspace.path_pool.resolve(session_id),
                     workspace.path_pool.resolve(host_id));
            session->active_context.reset();
            session->header_context.reset();
            session->pch_ref.reset();
            session->ast_dirty = true;
            session->trial_done = false;
            // Invalidate in-flight compiles so they cannot clobber the
            // reset state when they finish (same as switchContext).
            session->generation += 1;
            dropped_saved |= workspace.saved_contexts.erase(session_id) > 0;
        }
    }
    if(dropped_saved) {
        workspace.save_cache();
    }

    indexer.schedule();
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
    workspace.save_cache();
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

    workspace.load_cache();
    bg_tasks.spawn(cache_checkpoint_task());
}

const std::vector<feature::DocumentLink>*
    MasterServer::find_preamble_links(const Session& session) {
    if(!session.pch_ref)
        return nullptr;
    auto it = workspace.pch_cache.find(session.pch_ref->key);
    if(it == workspace.pch_cache.end() || it->second.preamble_links.empty())
        return nullptr;
    return &it->second.preamble_links;
}

std::vector<protocol::Location>
    MasterServer::resolve_directive_definition(Session& session,
                                               const protocol::Position& position) {
    std::vector<protocol::Location> locations;

    // Preamble include lines: compiled into the PCH, invisible to the
    // worker's AST — the PCH's cached links carry the targets.
    if(auto* links = find_preamble_links(session)) {
        for(auto& link: *links) {
            if(link.range.start.line != position.line)
                continue;
            if(position.character < link.range.start.character ||
               position.character > link.range.end.character)
                continue;
            locations.push_back(protocol::Location{
                .uri = feature::to_uri(link.target),
                .range = protocol::Range{},
            });
            return locations;
        }
    }

    return locations;
}

void MasterServer::load_workspace() {
    if(workspace_root.empty())
        return;

    auto& cfg = workspace.config.project;

    open_cache_store();

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
        LOG_GUIDANCE(
            "No compile_commands.json found in workspace {}. Compile commands will be " "guessed; see https://clice.io/en/guide/quick-start for setup.",
            workspace_root);
        return;
    }

    ScopedTimer cdb_timer;
    auto count = workspace.cdb.load(cdb_path);
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
