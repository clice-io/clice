#include "server/transport/master_server.h"

#include <list>
#include <memory>
#include <string>
#include <vector>

#include "version.h"
#include "sched/bootstrap.h"
#include "server/state/file_tracker.h"
#include "server/transport/agent_client.h"
#include "server/transport/lsp_client.h"
#include "support/anomaly.h"
#include "support/cache_store.h"
#include "support/filesystem.h"
#include "support/logging.h"
#include "support/timer.h"
#include "worker/protocol.h"

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
    loop(loop), pool(loop), contexts(workspace),
    index_query(workspace, sessions, pump, ast.projections),
    agent_query(workspace, sessions, pump, ast.projections, {.disk_only = true}),
    features(ast, forwarder, index_query, workspace, contexts, pump, sessions),
    invalidator(workspace, sessions, contexts, pcm), bg_tasks(loop),
    self_path(std::move(self_path)) {
    pcm.register_runner();
    pch.register_runner();
    ast.register_runner();
    turun.register_runner();
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
                                                   /*finalized=*/false);
    // Capture the raw sources now: the configuration dump below can only run
    // once the merged config has named the log directory.
    std::string raw_toml;
    if(!config_path.empty()) {
        if(auto content = fs::read(config_path)) {
            raw_toml = std::move(*content);
        }
    }
    std::string raw_init_options = init_options_json;

    if(!init_options_json.empty()) {
        if(auto ov = kota::codec::json::from_string(init_options_json, workspace.config); !ov) {
            LOG_GUIDANCE("Failed to apply initializationOptions: {}", ov.error().to_string());
        } else {
            LOG_INFO("Applied initializationOptions overlay");
        }
        init_options_json.clear();
    }
    workspace.config.finalize(workspace_root);

    auto& cfg = workspace.config.project;

    if(cfg.readonly == "on") {
        ast.readonly = ReadonlyMode::On;
    } else if(cfg.readonly == "auto") {
        ast.readonly = ReadonlyMode::Auto;
    } else {
        if(cfg.readonly != "off") {
            LOG_WARN("Unknown readonly '{}'; using off", std::string(cfg.readonly));
        }
        ast.readonly = ReadonlyMode::Off;
    }

    if(cfg.cache_dir_defaulted.value) {
        CacheStore::write_ignore_markers(cfg.cache_dir);
    }

    if(!cfg.logging_dir.empty()) {
        auto now = std::chrono::system_clock::now();
        auto pid = llvm::sys::Process::getProcessId();
        session_log_dir =
            path::join(cfg.logging_dir, std::format("{:%Y-%m-%d_%H-%M-%S}_{}", now, pid));
        if(logging::file_logger("master", session_log_dir, logging::options)) {
            LOG_INFO("Session log directory: {}", session_log_dir);
        }
    }

    // Dump every configuration layer — the config file verbatim, the
    // client's initializationOptions overlay, and the merged result after
    // defaults. Absence is stated explicitly so "was my config even read?"
    // never needs a support round-trip; the stderr mirror puts all of it in
    // the editor's output panel.
    if(config_path.empty()) {
        LOG_INFO("Configuration file: Missing");
    } else {
        LOG_INFO("Configuration file {}:\n{}", config_path, raw_toml);
    }
    if(raw_init_options.empty()) {
        LOG_INFO("initializationOptions: Missing");
    } else {
        auto pretty = kota::codec::json::prettify(raw_init_options);
        LOG_INFO("initializationOptions:\n{}", pretty ? *pretty : raw_init_options);
    }
    if(auto json = kota::codec::json::to_string(workspace.config)) {
        auto pretty = kota::codec::json::prettify(*json);
        LOG_INFO("Effective configuration:\n{}", pretty ? *pretty : *json);
    }

    LOG_INFO("Server ready (stateful={}, stateless={}, idle={}ms)",
             cfg.stateful_worker_count.value,
             cfg.stateless_worker_count.value,
             cfg.idle_timeout_ms.value);

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
    // against an empty resolver and created under the default mode;
    // re-check their persisted context choices and re-derive their
    // serving mode now that the configuration governs. Settlement waits
    // until here — after load_workspace — so divergence detection sees
    // the persisted shards it just loaded (a restored unsaved buffer must
    // escalate, not read as merely unindexed).
    for(auto& [path_id, session]: sessions.sessions) {
        if(session) {
            contexts.validate_saved_context(session->path_id);
            session->serving =
                ast.readonly == ReadonlyMode::Off ? ServingMode::Escalated : ServingMode::IndexOnly;
            settle_open_serving(session);
        }
    }

    if(!workspace_root.empty()) {
        // Construct after the workspace load so the tracker's baseline CDB
        // stamp matches the database that was just loaded.
        tracker = std::make_unique<FileTracker>(workspace, sessions, workspace_root);
        auto& tracker_cfg = workspace.config.tracker;
        if(tracker_cfg.cdb_poll_seconds.value > 0) {
            bg_tasks.spawn(cdb_poll_task());
        }
        if(tracker_cfg.workspace_poll_seconds.value > 0) {
            bg_tasks.spawn(workspace_poll_task());
        }
    }
}

kota::task<> MasterServer::cdb_poll_task() {
    auto interval = std::chrono::seconds(workspace.config.tracker.cdb_poll_seconds.value);
    while(true) {
        co_await kota::sleep(interval);
        auto events = tracker->tick_cdb();
        if(!events.empty()) {
            dispatch(events);
        }
    }
}

kota::task<> MasterServer::workspace_poll_task() {
    auto interval = std::chrono::seconds(workspace.config.tracker.workspace_poll_seconds.value);
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
        // to their callers with dispatch_errc::worker_crashed — the families
        // resend idempotent builds, the pump requeues the file. No state
        // outlives the request, so there is nothing to invalidate and no
        // event to dispatch.
        if(!info.stateful)
            return;
        dispatch(FileEvent::worker_crashed(info.lost_documents));
    };

    pool.on_evicted = [this](const std::string& path, std::size_t worker_index) {
        auto id = workspace.path_pool.find(path);
        if(!id) {
            LOG_WARN("Evicted path not in pool: {}", path);
            return;
        }
        // Owner-table upkeep is pool-domain state and stays here; the
        // session-side consequence (the worker's AST is gone, same as a
        // crash) goes through the event pipeline like any invalidation.
        // Only the current owner's eviction counts: a stale copy left
        // behind by a probe reassignment says nothing about the document
        // the new owner still holds.
        if(pool.remove_owner_from(*id, worker_index)) {
            dispatch(FileEvent::document_evicted(*id));
        } else {
            LOG_INFO("Ignoring eviction of {} from non-owner worker {}", path, worker_index);
        }
    };

    ast.on_indexing_needed = [this]() {
        pump.schedule();
    };
    pcm.on_indexing_needed = [this]() {
        pump.schedule();
    };

    // The pump is serving-neutral; the session-side policy hooks live on
    // this class and are installed here.
    pump.admission = [this](std::uint32_t path_id) {
        return index_admission(path_id);
    };
    pump.on_attempt_settled = [this](std::uint32_t path_id) {
        index_attempt_settled(path_id);
    };
    index_rows_conn = pump.on_rows_changed.connect(
        [this](llvm::ArrayRef<std::uint32_t> path_ids) { index_rows_changed(path_ids); });

    // The AST family's pull-side staleness check found a dependency changed
    // on disk: route it through the same DiskChanged path the file
    // tracker's polling uses, so lazy detection and polling share one
    // invalidation cascade.
    ast.on_stale = [this](std::uint32_t path_id) {
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
    // A replaced live session (an editor resending didOpen) leaves a
    // projection describing the old session's compile; the fresh session
    // starts with none, exactly like the pre-projection world's fresh
    // Session fields.
    ast.drop(path_id);
    auto session = sessions.open(path_id);
    // The serving mode's creation write point; the only other write is
    // ASTFamily::escalate.
    session->serving =
        ast.readonly == ReadonlyMode::Off ? ServingMode::Escalated : ServingMode::IndexOnly;
    return session;
}

void MasterServer::settle_open_serving(std::shared_ptr<Session> session) {
    // An escalated session needs no settlement: builds stay pull-driven,
    // and boosting its file would enqueue work the indexer skips for
    // open non-IndexOnly documents.
    if(session->serving == ServingMode::Escalated) {
        return;
    }
    auto it = workspace.shards.find(session->path_id);
    if(it != workspace.shards.end()) {
        // A buffer that already diverges from the indexed content (a
        // restored unsaved file) can never be served read-only: escalate
        // now instead of answering empty until the first edit.
        if(!it->second.matches_content(session->text)) {
            ast.escalate(*session);
        }
        return;
    }
    // Nothing indexed yet: reading this file is the reason to index it
    // first — unless indexing is disabled, in which case no shard will
    // ever arrive and only an AST can serve the document. A boost the
    // pump cannot fulfill escalates through the adapter's attempt-settled
    // check.
    if(workspace.config.project.enable_indexing.value) {
        pump.boost(session->path_id);
    } else {
        ast.escalate(*session);
    }
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
        ast.publish_output(session,
                           CompileOutput{
                               .version = std::nullopt,
                               .source = CommandSource::CDBExact,
                               .diagnostics = {},
                               .line_limit = std::nullopt,
                           });
    }

    sessions.close(path_id);
    ast.drop(path_id);

    dispatch(FileEvent::buffer_closed(path_id));

    LOG_DEBUG("didClose: {}", path);
}

Admission MasterServer::index_admission(std::uint32_t server_path_id) const {
    // Open files whose session invests in an AST are skipped until an
    // agent shows up: the LSP side never reads their shards (the session
    // serves them), so indexing them is pure waste — but agents read disk
    // truth and need the shards, snapshot taken from disk regardless of
    // the live buffer. Skipping loses no debt: the veto settles the
    // claim, and BufferClosed re-checks the shard against the disk on
    // close. An index-only session is the opposite case — its shard IS
    // what the LSP serves (freshness clause 4), so it indexes like a
    // closed file, but only while its buffer matches the disk this index
    // would read: rows from a diverged disk fail clause 4's content gate
    // and would replace the one shard the session can serve from,
    // blanking its features until an escalation. Keep the last matching
    // rows instead — the close-time re-check covers the debt here too.
    auto session = sessions.find(server_path_id);
    if(!session) {
        return Admission::Admit;
    }
    if(session->serving != ServingMode::IndexOnly) {
        return index_open_files ? Admission::Admit : Admission::SkipAndSettle;
    }
    auto file_path = workspace.path_pool.resolve(server_path_id);
    if(auto disk = fs::read(file_path); !disk || *disk != session->text) {
        return Admission::SkipAndSettle;
    }
    return Admission::Admit;
}

void MasterServer::index_attempt_settled(std::uint32_t server_path_id) {
    // The boost in settle_open_serving promised the index would serve the
    // cold session; an attempt that settles without a servable shard ends
    // that promise — escalate like the disabled-indexing branch, or the
    // session answers empty until its first edit.
    auto session = sessions.find(server_path_id);
    if(!session || session->serving != ServingMode::IndexOnly) {
        return;
    }
    auto it = workspace.shards.find(server_path_id);
    if(it == workspace.shards.end() || !it->second.matches_content(session->text)) {
        ast.escalate(*session);
    }
}

bool MasterServer::serves_session_rows(std::uint32_t path_id) const {
    auto session = sessions.find(path_id);
    return session && !ast.projections.index_current(path_id) && session->index_served;
}

void MasterServer::index_rows_changed(llvm::ArrayRef<std::uint32_t> path_ids) {
    // Sessions without a current file index serve these very rows
    // (freshness clause 4); index_served says the client pulled some of
    // them. Tell subscribers so those results get re-pulled.
    if(llvm::any_of(path_ids, [&](std::uint32_t id) { return serves_session_rows(id); })) {
        on_serving_rows_changed.emit();
    }
}

void MasterServer::on_agentic_query() {
    if(index_open_files) {
        return;
    }
    // First agentic index query: agents read disk truth, so open files'
    // disk snapshots must be indexed too — background indexing skips
    // them otherwise, since the LSP side is fully served by their
    // sessions. Sticky for the server's lifetime.
    index_open_files = true;
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
        auto shard_it = workspace.shards.find(path_id);
        bool shard_current =
            shard_it != workspace.shards.end() && shard_it->second.matches_content(*disk);
        pump.enqueue(path_id,
                     shard_current ? ReindexReason::DepsOnly : ReindexReason::ContentChanged);
    }
    pump.schedule();
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

    // The Lost invalidation voids the projection's currency (and any
    // in-flight round's landing-as-current) without touching generation —
    // the buffer is still the same buffer, and the round still publishes
    // its product as bounded staleness; the next request recompiles.
    for(auto path_id: dirty.mark_ast_dirty) {
        if(auto session = sessions.find(path_id)) {
            ast.invalidate(path_id);
            session->trial_done = false;
        }
        contexts.forget_self_contained(path_id);
    }

    for(auto path_id: dirty.mark_lost) {
        if(sessions.find(path_id)) {
            ast.invalidate(path_id);
        }
    }

    // Headers whose synthesized preamble embeds changed chain content:
    // dropping the snapshot's fast paths forces deps_changed() to re-validate
    // every chain file by content hash; open sessions also recompile and
    // re-trial.
    for(auto path_id: dirty.force_revalidate) {
        contexts.invalidate_header_deps(path_id);
        if(auto session = sessions.find(path_id)) {
            ast.invalidate(path_id);
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

    for(auto path_id: dirty.drop_index) {
        pump.claim_report(index_store.drop_index(path_id));
    }

    for(auto path_id: dirty.reindex_content_changed) {
        pump.enqueue(path_id, ReindexReason::ContentChanged);
    }
    for(auto path_id: dirty.reindex_deps_only) {
        pump.enqueue(path_id, ReindexReason::DepsOnly);
    }
    // The engine keeps the reindex lists disjoint per file in event order
    // (see DirtySet's adders), so the clears may run in any order relative
    // to the enqueues above.
    for(auto path_id: dirty.clear_reindex) {
        pump.clear_pending(path_id);
    }

    bool save = dirty.save_cache;
    if(dirty.recheck_contexts) {
        save |= context_service.drop_orphaned_choices(sessions);
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
        pump.schedule();
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
    co_await kota::when_all(pump.stop(), ast.stop());
    // Requests have unwound and released their interest; wind down the
    // graph's rounds before the persistence pass and the pool stop
    // (contract 11: quiesce -> final save -> pool/store).
    co_await graph.shutdown();
    auto report = co_await index_store.save(pump.save_debt());
    pump.claim_report(report);
    if(report.snapshot_stale) {
        // Debt surfaced after the snapshot serialized (write-time
        // corruption recovery): one metadata retry, or a dropped
        // standalone header's repair debt dies with this process.
        pump.claim_report(co_await index_store.save(pump.save_debt()));
    }
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
           it != workspace.pch_cache.end() && !pch.building(evicted.key)) {
            workspace.pch_cache.erase(it);
        }
    }
}

void MasterServer::load_workspace() {
    if(workspace_root.empty())
        return;

    auto report = bootstrap_workspace(workspace, contexts, index_store, pump, workspace_root);
    if(report.opened_store) {
        bg_tasks.spawn(cache_checkpoint_task());
    }
    if(report.cdb_path.empty()) {
        LOG_GUIDANCE(
            "No compile_commands.json found in workspace {}. Compile commands will be "
            "guessed; see https://clice.io/en/guide/quick-start for setup.",
            workspace_root);
    }
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
