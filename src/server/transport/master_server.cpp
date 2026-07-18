#include "server/transport/master_server.h"

#include <list>
#include <memory>
#include <string>
#include <vector>

#include "version.h"
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
#include "kota/ipc/recording_transport.h"
#include "kota/ipc/transport.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/Process.h"

namespace clice {

/// Retention bound of the notify log. Subscribers drain promptly, so only
/// messages that fire before any client attaches accumulate (a handful of
/// startup guidance reports in practice); the cap is a safety net, not a
/// working-set size.
constexpr static std::size_t notify_log_limit = 128;

MasterServer::MasterServer(kota::event_loop& loop, std::string self_path) :
    loop(loop), pool(loop), contexts(workspace), compiler(loop, workspace, contexts, pool),
    indexer(loop, workspace, pool, contexts, sessions), index_query(workspace, sessions, indexer),
    agent_query(workspace, sessions, indexer, {.disk_only = true}),
    features(compiler, index_query, workspace, contexts, indexer),
    invalidator(workspace, sessions, contexts), bg_tasks(loop), self_path(std::move(self_path)) {
    // The notify hook is process-wide because the logging layer cannot
    // depend on the server; the composition root owns it for the server's
    // lifetime and turns reports into state (notify_log) plus a wake-up
    // signal. Master-side reports only ever fire on the event-loop thread
    // (see support/anomaly.h), so no synchronization is needed here.
    // The loaded-state budget follows the open-document count; Workspace
    // cannot see SessionStore, so the master wires the provider.
    workspace.open_documents = [this] {
        return sessions.sessions.size();
    };

    logging::set_notify_hook([this](logging::NotifyLevel level, std::string_view message) {
        notify_log.push_back(NotifyMessage{level, std::string(message)});
        if(notify_log.size() > notify_log_limit) {
            notify_log.pop_front();
        }
        notify_seq += 1;
        on_notify.emit();
    });
}

MasterServer::~MasterServer() {
    logging::set_notify_hook(nullptr);
}

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

    // Documents opened before the server became ready were validated
    // against an empty resolver; re-check their persisted context choices
    // now that the workspace cache is loaded.
    for(auto& [path_id, session]: sessions.sessions) {
        if(session) {
            contexts.validate_saved_context(*session);
        }
    }

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
        // A stateless crash loses only in-flight requests, which fail back
        // to their callers with dispatch_errc::worker_crashed — the compiler
        // resends idempotent builds, the indexer requeues the file. No state
        // outlives the request, so there is nothing to invalidate and no
        // event to dispatch.
        if(!info.stateful)
            return;
        dispatch(FileEvent::worker_crashed(info.lost_documents));
    };

    pool.on_evicted = [this](const std::string& path, std::size_t worker_index) {
        auto it = workspace.path_pool.cache.find(path);
        if(it == workspace.path_pool.cache.end()) {
            LOG_WARN("Evicted path not in pool: {}", path);
            return;
        }
        // Owner-table upkeep is pool-domain state and stays here; the
        // session-side consequence (the worker's AST is gone, same as a
        // crash) goes through the event pipeline like any invalidation.
        // Only the current owner's eviction counts: a stale copy left
        // behind by a probe reassignment says nothing about the document
        // the new owner still holds.
        if(pool.remove_owner_from(it->second, worker_index)) {
            dispatch(FileEvent::document_evicted(it->second));
        } else {
            LOG_INFO("Ignoring eviction of {} from non-owner worker {}", path, worker_index);
        }
    };

    compiler.on_indexing_needed = [this]() {
        indexer.schedule();
    };

    // The compiler's pull-side staleness check found a dependency changed
    // on disk: route it through the same DiskChanged path the file
    // tracker's polling uses, so lazy detection and polling share one
    // invalidation cascade.
    compiler.on_stale = [this](std::uint32_t path_id) {
        dispatch(FileEvent::disk_changed(path_id));
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

void MasterServer::close_session(std::uint32_t path_id) {
    auto path = workspace.path_pool.resolve(path_id);
    // Route the eviction notification before dropping ownership:
    // notify_stateful uses the owner table to find the worker.
    pool.notify_stateful(path_id, worker::EvictParams{std::string(path)});
    pool.remove_owner(path_id);

    // Retract the document's published diagnostics through the standard
    // output path: materialize an empty output and signal the transports
    // (MasterServer holds no peer — see the class charter). A transport
    // whose client has not completed the handshake drops the push: nothing
    // was ever published for it to clear, and publishDiagnostics may not
    // flow before the initialize response. CDBExact keeps
    // format_diagnostics from decorating the empty set with guidance.
    if(auto session = sessions.find(path_id)) {
        session->output = CompileOutput{
            .version = std::nullopt,
            .source = CommandSource::CDBExact,
            .diagnostics = {},
            .line_limit = std::nullopt,
            .inactive_regions = std::nullopt,
        };
        compiler.on_output.emit(session);
    }

    sessions.close(path_id);

    dispatch(FileEvent::buffer_closed(path_id));

    LOG_DEBUG("didClose: {}", path);
}

void MasterServer::on_agentic_query() {
    if(indexer.index_open_files) {
        return;
    }
    // First agentic index query: agents read disk truth, so open files'
    // disk snapshots must be indexed too — background indexing skips
    // them otherwise, since the LSP side is fully served by their
    // sessions. Sticky for the server's lifetime.
    indexer.index_open_files = true;
    for(auto& [path_id, session]: sessions.sessions) {
        if(!session) {
            continue;
        }
        // Same disk-vs-shard arbitration as BufferClosed: a current shard
        // keeps serving through the catch-up, while a stale or missing one
        // (a save that landed while its reindex slot was still skipped)
        // must not answer agents with the pre-save rows.
        auto disk = fs::read(workspace.path_pool.resolve(path_id));
        if(!disk) {
            continue;
        }
        auto shard_it = workspace.merged_indices.find(path_id);
        bool shard_current =
            shard_it != workspace.merged_indices.end() && *disk == shard_it->second.content();
        indexer.enqueue(path_id,
                        shard_current ? ReindexReason::DepsOnly : ReindexReason::ContentChanged);
    }
    indexer.schedule();
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

    // The Lost reset bumps dirty_epoch so an in-flight compile that
    // consumed the pre-event world cannot clear ast_dirty when it lands;
    // generation stays put — the buffer is still the same buffer, and
    // results that pass their generation check may still be published
    // (bounded staleness: the flag stays dirty, the next request recompiles).
    for(auto path_id: dirty.mark_ast_dirty) {
        if(auto session = sessions.find(path_id)) {
            SessionStore::reset_compile_state(*session, ResetDepth::Lost);
            session->trial_done = false;
        }
        contexts.forget_self_contained(path_id);
    }

    for(auto path_id: dirty.mark_lost) {
        if(auto session = sessions.find(path_id)) {
            SessionStore::reset_compile_state(*session, ResetDepth::Lost);
        }
    }

    // Headers whose synthesized preamble embeds changed chain content:
    // dropping the snapshot's fast paths forces deps_changed() to re-validate
    // every chain file by content hash; open sessions also recompile and
    // re-trial.
    for(auto path_id: dirty.force_revalidate) {
        contexts.invalidate_header_deps(path_id);
        if(auto session = sessions.find(path_id)) {
            SessionStore::reset_compile_state(*session, ResetDepth::Lost);
            session->trial_done = false;
        }
    }

    // The header's borrowed compile command changed: its resolved context
    // (and synthesized preamble) describes flags that no longer exist, so
    // the next use must re-resolve. Session dirtying arrives in the same
    // DirtySet via mark_ast_dirty.
    for(auto path_id: dirty.drop_context) {
        contexts.drop_header_context(path_id);
    }

    for(auto path_id: dirty.reindex_content_changed) {
        indexer.enqueue(path_id, ReindexReason::ContentChanged);
    }
    for(auto path_id: dirty.reindex_deps_only) {
        indexer.enqueue(path_id, ReindexReason::DepsOnly);
    }
    // The engine keeps the reindex lists disjoint per file in event order
    // (see DirtySet's adders), so the clears may run in any order relative
    // to the enqueues above.
    for(auto path_id: dirty.clear_reindex) {
        indexer.clear_pending(path_id);
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

    // Not before the server is ready: document-sync events are accepted
    // early (a pre-ready didClose lands here), but the scheduler reads
    // configuration that initialize() has not applied yet. The reindex
    // queue filled above is kept — the post-ready workspace load kicks the
    // scheduler.
    if(dirty.reschedule_indexing && lifecycle == ServerLifecycle::Ready) {
        indexer.schedule();
    }
}

void MasterServer::schedule_shutdown() {
    if(lifecycle == ServerLifecycle::Exited)
        return;
    lifecycle = ServerLifecycle::ShuttingDown;
    shutdown_source.cancel();
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
            drain_store_evictions();
        }
    }
}

void MasterServer::drain_store_evictions() {
    // The store's LRU evicted these blobs from disk (inside commit, maybe
    // on a worker thread); drop the derived pch_cache metadata here on the
    // event loop, or the content-keyed map grows for the server's
    // lifetime even for keys never requested again. An entry mid-rebuild
    // keeps its slot — its commit republishes fresh blobs over the
    // eviction.
    for(auto& evicted: workspace.store->take_evictions()) {
        if(evicted.ns != "pch") {
            continue;
        }
        // A key rebuilt after the eviction was recorded has live blobs
        // again — the record is stale, not the entry. Erase only when the
        // store still lacks the blob, and never mid-rebuild (the commit
        // republishes over the eviction).
        if(workspace.store->lookup("pch", evicted.key)) {
            continue;
        }
        if(auto it = workspace.pch_cache.find(evicted.key);
           it != workspace.pch_cache.end() && !it->second.building) {
            workspace.pch_cache.erase(it);
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
    store->register_namespace({.name = "pch",
                               .extension = ".pch",
                               .aux_extension = ".pch.idx",
                               .policy = CachePolicy::LRU,
                               .max_bytes = 8 * GiB});
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
            // Bulk sweep of unknown staleness: the hash gate decides per
            // file. DepsOnly — a cold start with a warm index cache must
            // keep serving the loaded shards, not blank every query until
            // the sweep drains.
            indexer.enqueue(server_id, ReindexReason::DepsOnly);
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

/// Pipe-mode serving body: the LSP peer plus the agentic acceptor as a
/// single task, so the caller can bound both with the shutdown scope.
static kota::task<> serve_peer(kota::ipc::JsonPeer& peer, kota::task<> acceptor) {
    co_await kota::when_any(peer.run(), std::move(acceptor));
}

int run_serve_mode(const ServerOptions& opts, const char* self_path) {
    logging::stderr_logger("master", logging::options);

    auto mode = opts.mode.value_or(ServerMode::Pipe);
    auto host = opts.host.value_or("127.0.0.1");
    auto port = opts.port.value_or(0);
    auto record = opts.record.value_or("");
    auto ws = opts.workspace.value_or("");

    LOG_INFO("clice master starting: version={}, target={}, pid={}, mode={}, workspace={}",
             clice::version,
             clice::target,
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
                         bool has_acceptor,
                         std::string workspace) -> kota::task<> {
            // Pre-initialize for standalone (no-editor) use; LSP initialize
            // will be rejected. Runs inside the loop — before the peer
            // reads its first message — because initialize() spawns
            // background tasks that need the running loop context.
            if(!workspace.empty()) {
                server.initialize(workspace);
            }
            if(has_acceptor) {
                co_await kota::with_token(
                    serve_peer(peer,
                               accept_connections(server, std::move(acceptor), false, connections)),
                    server.shutdown_token());
            } else {
                co_await kota::with_token(peer.run(), server.shutdown_token());
            }
            co_await server.shutdown_and_cleanup();
        }(server, lsp_peer, connections, std::move(agent_acceptor), has_agent_acceptor, ws));
        loop.run();
        return 0;
    }

    if(mode == ServerMode::Socket) {
        auto acceptor = kota::tcp::listen(host, port, {}, loop);
        if(!acceptor) {
            LOG_ERROR("failed to listen on {}:{}", host, port);
            return 1;
        }

        bool register_lsp = ws.empty();
        LOG_INFO("Listening on {}:{} ...", host, port);
        loop.schedule([](MasterServer& server,
                         kota::tcp::acceptor acceptor,
                         bool register_lsp,
                         std::list<Connection>& connections,
                         std::string workspace) -> kota::task<> {
            // See the pipe-mode comment: pre-initialization must run
            // inside the loop.
            if(!workspace.empty()) {
                server.initialize(workspace);
            }
            co_await kota::with_token(
                accept_connections(server, std::move(acceptor), register_lsp, connections),
                server.shutdown_token());
            co_await server.shutdown_and_cleanup();
        }(server, std::move(*acceptor), register_lsp, connections, ws));
        loop.run();
        return 0;
    }

    LOG_ERROR("unexpected server mode");
    return 1;
}

}  // namespace clice
