#include "server/project_server.h"

#include <memory>
#include <string>
#include <vector>

#include "version.h"
#include "index/writer_lock.h"
#include "sched/bootstrap.h"
#include "server/control_server.h"
#include "server/file_tracker.h"
#include "server/master_server.h"
#include "support/cache_store.h"
#include "support/filesystem.h"
#include "support/logging.h"
#include "worker/protocol.h"

#include "kota/async/async.h"
#include "kota/codec/json/json.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/Process.h"
#include "llvm/Support/xxhash.h"

namespace clice {

ProjectServer::ProjectServer(MasterServer& server, CanonicalPath root) :
    server(server), loop(server.loop), root(std::move(root)), project(server.files),
    sched(loop, project, commands, server.pool),
    ast(project, contexts, sched.graph, sched.pcm, sched.pch, server.pool, sessions, loop),
    dispatcher(project, contexts, ast, server.pool),
    live_sources(project, sched.pch, sessions, ast.projections),
    index_query(project.project_index, project.file_table, &freshness, &live_sources),
    features(ast, dispatcher, index_query, project, contexts, sched.pump, sessions),
    invalidator(project, sessions, contexts, ast.projections, sched.pcm, sched.store),
    bg_tasks(loop) {
    ast.register_runner();
    // The loaded-state budget follows the open-document count; the PCH
    // family cannot see SessionStore, so the project wires the provider.
    sched.pch.open_documents = [this] {
        return sessions.sessions.size();
    };
    // Metadata marks (a PCH landing, a context switch) flush through the
    // next save; the wiring schedules one soon after the first mark.
    project.request_flush = [this] {
        schedule_metadata_flush();
    };

    ast.on_indexing_needed = [this]() {
        sched.pump.schedule();
    };
    output_conn = ast.on_output.connect([this](const std::shared_ptr<Session>& session) {
        this->server.on_output.emit(*this, session);
    });
    progress_conn = sched.pump.on_progress_changed.connect(
        [this]() { this->server.index_progress_changed(*this); });

    // The pump is serving-neutral; the session-side policy hooks live on
    // this class and are installed here.
    sched.pump.compiled_by_session = [this](Fid path_id) {
        auto session = sessions.find(path_id);
        return session && session->serving == ServingMode::Escalated;
    };
    sched.pump.on_attempt_settled = [this](Fid path_id) {
        index_attempt_settled(path_id);
    };
    index_rows_conn = sched.pump.on_rows_changed.connect(
        [this](llvm::ArrayRef<Fid> path_ids) { index_rows_changed(path_ids); });

    // The AST family's pull-side staleness check found an input of an open
    // document changed on disk: the document gets the treatment the
    // changed file's cascade gives its dependents.
    ast.on_stale = [this](Fid path_id) {
        if(auto session = sessions.find(path_id)) {
            ast.invalidate(path_id);
            session->trial_done = false;
        }
        commands.forget_self_contained(path_id);
    };
}

ProjectServer::~ProjectServer() = default;

void ProjectServer::configure(llvm::StringRef init_options,
                              llvm::ArrayRef<CanonicalPath> taken_cache_dirs) {
    config_issues.clear();
    config_path.clear();
    // Load clice.toml raw and overlay initializationOptions BEFORE computing
    // defaults: derived fields (logging_dir, index_dir, ...) must follow the
    // final merged values (e.g. a cache_dir overridden by the client).
    project.config =
        Config::load_from_workspace(root, &config_issues, &config_path, /*finalized=*/false);
    for(auto& issue: config_issues) {
        LOG_GUIDANCE("Configuration problem in {}: {}", issue.file, issue.message);
    }
    std::string own_cache_dir = project.config.project.cache_dir;
    if(!init_options.empty()) {
        if(auto ov = kota::codec::json::from_string(init_options, project.config); !ov) {
            LOG_GUIDANCE("Failed to apply initializationOptions: {}", ov.error().to_string());
        }
    }
    auto overlaid = project.config;
    project.config.finalize(root);

    // A cache directory serves one project (see owned_elsewhere), and one
    // at a time: its writer lock is the process's, so a second project here
    // would take it too. Fall back to the project's own, then the default,
    // then none.
    auto& cache_dir = project.config.project.cache_dir;
    auto taken = [&] {
        return llvm::is_contained(taken_cache_dirs, CanonicalPath(cache_dir)) ||
               owned_elsewhere(cache_dir, root);
    };
    if(!root.empty() && taken()) {
        std::string requested = cache_dir;
        for(auto& fallback: {own_cache_dir, std::string()}) {
            project.config = overlaid;
            project.config.project.cache_dir = fallback;
            project.config.finalize(root);
            if(!taken()) {
                break;
            }
        }
        if(taken()) {
            cache_dir.clear();
            project.config.project.cache_dir_defaulted = false;
        }
        LOG_WARN("Cache directory {} already serves another project; {} uses {}",
                 requested,
                 root,
                 cache_dir.empty() ? std::string("none") : cache_dir);
    }

    auto& cfg = project.config.project;
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
    freshness.options.withhold = cfg.enable_indexing.value;
    if(cfg.cache_dir_defaulted.value) {
        CacheStore::write_ignore_markers(cfg.cache_dir);
    }
}

void ProjectServer::start() {
    started = true;
    if(!root.empty()) {
        auto report = bootstrap_project(project,
                                        sched.store,
                                        sched.pump,
                                        root,
                                        server.requested_configuration);
        contexts.load();
        if(report.opened_store) {
            bg_tasks.spawn(cache_checkpoint_task());
        }
        if(project.index_db && !project.index_db->read_only()) {
            start_control_listener();
        }
        if(!report.has_commands) {
            LOG_GUIDANCE(
                "No compile_commands.json found in workspace {}. Compile commands will be "
                "guessed; see https://clice.io/en/guide/quick-start for setup.",
                root);
        }
    }

    // Documents opened before the project started were created under the
    // default mode with no choice loaded; validate their persisted
    // context choices and derive their serving mode now that the
    // configuration governs. Settlement waits
    // until here — after the load — so divergence detection sees the
    // persisted shards it just loaded (a restored unsaved buffer must
    // escalate, not read as merely unindexed).
    for(auto& [path_id, session]: sessions.sessions) {
        if(session) {
            contexts.validate_saved_context(session->path_id);
            session->serving =
                ast.readonly == ReadonlyMode::Off ? ServingMode::Escalated : ServingMode::IndexOnly;
            settle_open_serving(session);
        }
    }

    if(root.empty()) {
        return;
    }
    // Construct after the project load: the tracker baselines each
    // database at the read its entries came from.
    tracker = std::make_unique<FileTracker>(project, sessions, root);
    // Documents opened before the project loaded missed their didOpen-time
    // discovery.
    for(auto& [path_id, session]: sessions.sessions) {
        discover_around(path_id);
    }
    auto& tracker_cfg = project.config.tracker;
    if(tracker_cfg.cdb_poll_seconds.value > 0) {
        bg_tasks.spawn(cdb_poll_task());
    }
    if(tracker_cfg.workspace_poll_seconds.value > 0) {
        bg_tasks.spawn(workspace_poll_task());
    }
}

kota::task<> ProjectServer::shutdown() {
    if(endpoint_recorded) {
        index::remove_endpoint(project.config.project.cache_dir);
        endpoint_recorded = false;
    }
    bg_tasks.cancel();
    co_await bg_tasks.join();
    // Quiesce in-flight compilation and indexing first so the persisted
    // snapshot below covers everything that actually completed.
    co_await kota::when_all(sched.pump.stop(), ast.stop());
    // Requests have unwound and released their interest; the shared tail
    // winds down the graph's rounds before the persistence pass.
    co_await sched.shutdown();
}

void ProjectServer::close() {
    sched.close();
}

void ProjectServer::discover_around(Fid path_id) {
    if(!tracker) {
        return;
    }
    auto events = tracker->discover_around(path_id);
    if(!events.empty()) {
        dispatch(events);
    }
}

std::shared_ptr<Session> ProjectServer::create_session(Fid path_id) {
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

void ProjectServer::settle_open_serving(std::shared_ptr<Session> session) {
    // An escalated session needs no settlement: builds stay pull-driven,
    // and boosting its file would enqueue work the indexer skips for
    // open non-IndexOnly documents.
    if(session->serving == ServingMode::Escalated) {
        return;
    }
    auto it = project.project_index.shards.find(session->path_id);
    if(it != project.project_index.shards.end()) {
        // A buffer that already diverges from the indexed content (a
        // restored unsaved file) can never be served read-only: escalate
        // now instead of answering empty until the first edit.
        if(!it->second.matches_content(session->text.size(), session->hash)) {
            ast.escalate(*session);
        }
        return;
    }
    // Nothing indexed yet: reading this file is the reason to index it
    // first — unless indexing is disabled, in which case no shard will
    // ever arrive and only an AST can serve the document. A boost the
    // pump cannot fulfill escalates through the adapter's attempt-settled
    // check.
    if(project.config.project.enable_indexing.value) {
        sched.pump.boost(session->path_id);
    } else {
        ast.escalate(*session);
    }
}

void ProjectServer::close_session(Fid path_id) {
    // Retract the document's published diagnostics through the standard
    // output path: materialize an empty output and signal the transports
    // (the server holds no peer — see MasterServer's charter). A transport
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
    // Route the eviction notification before dropping ownership:
    // notify_stateful uses the owner table to find the worker.
    auto path = project.file_table.resolve(path_id);
    server.pool.notify_stateful(path_id.raw, worker::EvictParams{std::string(path)});
    server.pool.remove_owner(path_id.raw);
    sessions.close(path_id);
    ast.drop(path_id);
    // The session's compile stood in for the file's background index
    // (IndexPump::compiled_by_session); the disk's turn again.
    if(sched.pump.enqueue(path_id, ReindexReason::DepsOnly)) {
        sched.pump.schedule(false);
    }
    // PCH entries are content-keyed and may be shared with other sessions,
    // so nothing entry-level to clean up — but the loaded-state budget
    // shrinks with the open count, and this is the moment it does.
    sched.pch.enforce_loaded_budget();
    LOG_DEBUG("Closed {}", path);
}

bool ProjectServer::knows(Fid path_id) {
    return sessions.find(path_id) != nullptr || !project.build.commands(path_id).empty() ||
           project.dep_graph.knows(path_id) || !invalidator.readers(path_id).empty();
}

void ProjectServer::open_session(Fid path_id, std::string text, int version) {
    auto session = create_session(path_id);
    sessions.apply_open(*session, std::move(text), version);
    // What the disk holds under the buffer: a later save or outside
    // write is then a change from it, even for a file nothing else knew.
    project.file_table.current(path_id);
    if(!started) {
        return;
    }
    contexts.validate_saved_context(path_id);
    settle_open_serving(session);
}

void ProjectServer::index_attempt_settled(Fid path_id) {
    // The boost in settle_open_serving promised the index would serve the
    // cold session; an attempt that settles without a servable shard ends
    // that promise — escalate like the disabled-indexing branch, or the
    // session answers empty until its first edit.
    auto session = sessions.find(path_id);
    if(!session || session->serving != ServingMode::IndexOnly) {
        return;
    }
    auto it = project.project_index.shards.find(path_id);
    if(it == project.project_index.shards.end() ||
       !it->second.matches_content(session->text.size(), session->hash)) {
        ast.escalate(*session);
    }
}

bool ProjectServer::serves_session_rows(Fid path_id) const {
    auto session = sessions.find(path_id);
    return session && !ast.projections.index_current(path_id) && session->index_served;
}

void ProjectServer::index_rows_changed(llvm::ArrayRef<Fid> path_ids) {
    // Sessions without a current file index serve these very rows
    // (freshness clause 4); index_served says the client pulled some of
    // them. Tell subscribers so those results get re-pulled.
    if(llvm::any_of(path_ids, [&](Fid id) { return serves_session_rows(id); })) {
        server.on_serving_rows_changed.emit();
    }
}

void ProjectServer::dispatch(llvm::ArrayRef<FileEvent> events) {
    auto dirty = invalidator.apply(events);

    for(auto path_id: dirty.reset_trial) {
        if(auto session = sessions.find(path_id)) {
            session->trial_done = false;
        }
    }

    for(auto path_id: dirty.reset_header_mode) {
        commands.reset_header_mode(path_id);
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
        commands.forget_self_contained(path_id);
    }

    for(auto path_id: dirty.mark_lost) {
        if(sessions.find(path_id)) {
            ast.invalidate(path_id);
        }
    }

    // The header's resolved context was derived from what changed — its
    // borrowed compile command, a file along its include chain — so the
    // next use must re-resolve. Session dirtying arrives in the same
    // DirtySet via mark_ast_dirty.
    for(auto path_id: dirty.drop_context) {
        contexts.drop_header_context(path_id);
    }

    for(auto path_id: dirty.drop_index) {
        sched.pump.claim_report(sched.store.drop_index(path_id));
    }

    for(auto path_id: dirty.reindex_content_changed) {
        sched.pump.enqueue(path_id, ReindexReason::ContentChanged);
    }
    for(auto path_id: dirty.reindex_deps_only) {
        sched.pump.enqueue(path_id, ReindexReason::DepsOnly);
    }
    // The engine keeps the reindex lists disjoint per file in event order
    // (see DirtySet's adders), so the clears may run in any order relative
    // to the enqueues above.
    for(auto path_id: dirty.clear_reindex) {
        sched.pump.clear_pending(path_id);
    }

    if(dirty.recheck_contexts && context_service.drop_orphaned_choices(sessions)) {
        contexts.mark_dirty();
    }

    // Not before the server is ready: document-sync events are accepted
    // early (a pre-ready didClose lands here), but the scheduler reads
    // configuration that initialize() has not applied yet. The reindex
    // queue filled above is kept — the post-ready workspace load kicks the
    // scheduler.
    if(dirty.reschedule_indexing && server.lifecycle == ServerLifecycle::Ready) {
        sched.pump.schedule();
    }

    // The database gained or lost entries: an open document may belong to
    // another project now.
    if(llvm::any_of(events, [](const FileEvent& event) {
           return event.kind == FileEvent::Kind::CDBChanged &&
                  (!event.cdb.added.empty() || !event.cdb.removed.empty());
       })) {
        server.builds_changed();
    }
}

void ProjectServer::schedule_metadata_flush() {
    if(metadata_flush_scheduled || server.lifecycle == ServerLifecycle::ShuttingDown ||
       server.lifecycle == ServerLifecycle::Exited) {
        return;
    }
    metadata_flush_scheduled = true;
    bg_tasks.spawn(metadata_flush_task());
}

kota::task<> ProjectServer::metadata_flush_task() {
    // One loop turn of debounce coalesces a burst of marks into one save;
    // the save itself batches whatever else is dirty by then. Marks
    // arriving while the save runs re-schedule through request_flush, and
    // a save that could not clear the flags (serialization or write
    // failure) retries on the next spawn with this backoff.
    co_await kota::sleep(std::chrono::milliseconds(50));
    metadata_flush_scheduled = false;
    sched.pump.claim_report(co_await sched.store.save(sched.pump.save_debt()));
    if(project.artifacts_dirty || sched.store.contexts.dirty) {
        co_await kota::sleep(std::chrono::seconds(5));
        schedule_metadata_flush();
    }
}

kota::task<> ProjectServer::cache_checkpoint_task() {
    constexpr auto interval = std::chrono::minutes(5);
    while(true) {
        co_await kota::sleep(interval);
        if(project.store) {
            // Offload to the thread pool: checkpoint writes the manifest.
            co_await kota::queue([this] { project.store->checkpoint(); });
            drain_store_evictions();
        }
    }
}

void ProjectServer::drain_store_evictions() {
    // The store's LRU evicted these blobs from disk (inside commit, maybe
    // on a worker thread); drop the derived pch_cache metadata here on the
    // event loop, or the content-keyed map grows for the server's
    // lifetime even for keys never requested again. An entry mid-rebuild
    // keeps its slot — its commit republishes fresh blobs over the
    // eviction.
    for(auto& evicted: project.store->take_evictions()) {
        if(evicted.ns != "pch") {
            continue;
        }
        // A key rebuilt after the eviction was recorded has live blobs
        // again — the record is stale, not the entry. Erase only when the
        // store still lacks the blob, and never mid-rebuild (the commit
        // republishes over the eviction).
        if(project.store->lookup("pch", evicted.key)) {
            continue;
        }
        if(auto it = project.pch_cache.find(evicted.key);
           it != project.pch_cache.end() && !sched.pch.building(evicted.key)) {
            project.pch_cache.erase(it);
        }
    }
}

void ProjectServer::start_control_listener() {
    constexpr llvm::StringLiteral host = "127.0.0.1";
    auto acceptor = kota::tcp::listen(host, 0, {}, loop);
    std::optional<int> port;
    if(acceptor) {
        if(auto bound = kota::tcp::local_port(*acceptor)) {
            port = *bound;
        }
    }
    if(!port) {
        LOG_WARN("Failed to start the control listener; `clice index` cannot ask this server");
        return;
    }
    auto& cache_dir = project.config.project.cache_dir;
    if(!index::write_endpoint(
           cache_dir,
           {.pid = static_cast<std::uint32_t>(llvm::sys::Process::getProcessId()),
            .version = std::string(clice::version),
            .host = host.str(),
            .port = *port})) {
        return;
    }
    endpoint_recorded = true;
    LOG_INFO("Control channel listening on {}:{}", host, *port);
    bg_tasks.spawn(serve_control(*this, std::move(*acceptor)));
}

kota::task<> ProjectServer::cdb_poll_task() {
    auto interval = std::chrono::seconds(project.config.tracker.cdb_poll_seconds.value);
    while(true) {
        co_await kota::sleep(interval);
        auto events = tracker->tick_cdb();
        if(!events.empty()) {
            dispatch(events);
        }
    }
}

kota::task<> ProjectServer::workspace_poll_task() {
    auto interval = std::chrono::seconds(project.config.tracker.workspace_poll_seconds.value);
    while(true) {
        co_await kota::sleep(interval);
        auto events = co_await tracker->tick_workspace();
        if(!events.empty()) {
            dispatch(events);
        }
        server.drain_disk_changes();
    }
}

}  // namespace clice
