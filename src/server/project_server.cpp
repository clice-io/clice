module;

#include "support/prelude.h"

#include "version.h"
#include "support/anomaly.macros.h"
#include "support/logging.macros.h"

module clice;

import :command.file_kind;
import :command.table;
import :feature.diagnostics;
import :index.project_index;
import :index.writer_lock;
import :project.build;
import :project.load;
import :sched.bootstrap;
import :server.control;
import :server.file_tracker;
import :server.master_server;
import :server.project_server;
import :support.logging;
import :vfs.cache_store;
import :vfs.path;
import :worker.probe;
import :worker.protocol;

namespace clice {

ProjectServer::ProjectServer(MasterServer& server, CanonicalPath root) :
    server(server), loop(server.loop), root(std::move(root)), project(server.files),
    sched(loop, project, commands, server.pool),
    ast(project, contexts, sched.graph, sched.pcm, sched.pch, server.pool, sessions),
    dispatcher(project, contexts, ast, server.pool, server.position_encoding),
    live_sources(project, sched.pch, sessions, ast.compiles),
    index_query(project.index, project.file_table, &freshness, &live_sources),
    requests(ast,
             dispatcher,
             index_query,
             project,
             contexts,
             sched.pump,
             sessions,
             server.position_encoding),
    invalidator(project, sessions, contexts, ast, sched.pcm) {
    ast.register_runner();
    // The loaded-state budget follows the open-document count; the PCH
    // family cannot see SessionStore, so the project connects the provider.
    sched.pch.open_documents = [this] {
        return sessions.sessions.size();
    };
    // Metadata marks (a PCH commit, a context switch) flush through the
    // next save; the callback schedules one soon after the first mark.
    project.request_flush = [this] {
        schedule_metadata_flush();
    };

    ast.on_indexing_needed = [this]() {
        sched.pump.schedule();
    };
    output_conn = ast.on_output.connect([this](const std::shared_ptr<Session>& session) {
        this->server.on_output.emit(*this, session);
    });
    diagnostics_conn = ast.on_diagnostics.connect([this](const std::shared_ptr<Session>& session) {
        this->server.on_diagnostics.emit(*this, session);
    });
    phase_conn = ast.on_phase.connect([this](Fid path_id, CompilePhase phase) {
        this->server.on_compile_phase.emit(path_id, phase);
    });
    progress_conn = sched.pump.on_progress_changed.connect(
        [this]() { this->server.index_progress_changed(*this); });

    // The pump is serving-neutral; the session-side policy hooks live on
    // this class and are installed here.
    sched.pump.compiled_by_session = [this](Fid path_id) {
        return invalidator.compiled_by_session(path_id);
    };
    sched.pump.on_skipped_diagnostics = [this](Fid path_id,
                                               std::uint64_t text_hash,
                                               std::vector<feature::FileDiagnostic> diagnostics) {
        ast.report_skipped(path_id, text_hash, std::move(diagnostics));
    };
    sched.pump.lacks_skipped_diagnostics = [this](Fid path_id) {
        return ast.lacks_skipped(path_id);
    };
    sched.pump.on_attempt_finished = [this](Fid path_id) {
        index_attempt_finished(path_id);
    };
    index_changed_conn = sched.pump.on_index_changed.connect(
        [this](llvm::ArrayRef<Fid> path_ids) { on_index_changed(path_ids); });

    // The AST family's pull-side staleness check found an input of an open
    // document changed on disk: the document gets the treatment the
    // changed file's cascade gives its dependents.
    ast.on_stale = [this](Fid path_id) {
        this->server.drain_disk_changes();
        if(auto session = sessions.find(path_id)) {
            ast.invalidate(path_id);
            session->trial_done = false;
        }
        commands.forget_self_contained(path_id);
    };
}

ProjectServer::~ProjectServer() = default;

void ProjectServer::configure(const std::optional<codec::dyn::Value>& init_options,
                              llvm::ArrayRef<CanonicalPath> taken_cache_dirs) {
    config_issues.clear();
    config_path.clear();
    // Load clice.toml raw and overlay initializationOptions BEFORE computing
    // defaults: derived fields (logging_dir, index_dir, ...) must follow the
    // final merged values (e.g. a cache_dir overridden by the client).
    project.config =
        Config::load_from_workspace(root, &config_issues, &config_path, /*finalized=*/false);
    // One message: the configuration file lists each as a diagnostic.
    if(!config_issues.empty()) {
        auto& first = config_issues.front();
        LOG_GUIDANCE(
            "Configuration problem in {}: {}{}",
            project.file_table.display_identity(first.file),
            first.message,
            config_issues.size() == 1
                ? std::string()
                : std::format(" (and {} more, listed as its problems)", config_issues.size() - 1));
    }
    std::string own_cache_dir = project.config.project.cache_dir;
    if(init_options) {
        if(auto ov = codec::dyn::from_dyn(*init_options, project.config); !ov) {
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
        return !cache_dir.empty() &&
               (ranges::contains(taken_cache_dirs, CanonicalPath(Spelling::absolute(cache_dir))) ||
                owned_elsewhere(cache_dir, root));
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
        LOG_GUIDANCE("Cache directory {} already serves another project; {} {}",
                     requested,
                     root,
                     cache_dir.empty() ? std::string("keeps no cache")
                                       : std::format("keeps its cache in {}", cache_dir));
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
    if(const std::string& arguments = project.config.code_completion.arguments;
       arguments != "placeholders" && arguments != "delimiters" && arguments != "none") {
        LOG_WARN("Unknown code_completion.arguments '{}'; using placeholders", arguments);
    }
    freshness.options.withhold = cfg.enable_indexing.value;
    if(cfg.cache_dir_defaulted.value) {
        CacheStore::write_ignore_markers(cfg.cache_dir);
    }
}

void ProjectServer::start() {
    started = true;
    if(!root.empty()) {
        auto report =
            bootstrap_project(project, commands, sched.pump, root, server.requested_configuration);
        contexts.load();
        if(report.opened_store) {
            bg_tasks.spawn(checkpoint_task());
        }
        if(project.database && !project.database->read_only()) {
            start_control_listener();
        }
        commands_missing = !report.has_commands && !found_unreadable_database(project);
    }

    // Documents opened before the project started — held while its root's
    // previous project shut down — had no context choice loaded; validate
    // their persisted choices and decide their serving now. The serving
    // decision waits until here — after the load — so divergence detection
    // sees the persisted shards it just loaded (a restored unsaved buffer
    // must escalate, not read as merely unindexed).
    for(auto& [path_id, session]: sessions.sessions) {
        if(session) {
            contexts.validate_saved_context(session->path_id);
            decide_open_serving(session);
        }
    }
    ast.loaded.set();

    if(root.empty()) {
        return;
    }
    auto poll_seconds = std::chrono::seconds(project.config.tracker.workspace_poll_seconds.value);
    project.file_table.disk.add_root(root, {.max = poll_seconds});
    // Construct after the project load: the tracker baselines each
    // database at the read its entries came from.
    tracker = std::make_unique<FileTracker>(project, sessions, root);
    // FIXME: empty here: documents arrive after start() (rehome_sessions),
    // whose open_session skips discovery, so a nested database of a file
    // opened before `initialized` waits for the next CDB poll.
    for(auto& [path_id, session]: sessions.sessions) {
        discover_around(path_id);
    }
    if(poll_seconds.count() > 0) {
        bg_tasks.spawn(sources_poll_task());
        server.start_polling();
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

void ProjectServer::saved(Fid path_id) {
    // The open files the save made stale show what their background run
    // finds as soon as it can (ASTFamily::report_skipped).
    if(project.config.project.skip_header_bodies) {
        for(auto& [open, session]: sessions.sessions) {
            if(sched.pump.pending_reason(open) && !sched.pump.in_flight(open)) {
                sched.pump.boost(open);
            }
        }
    }
    // A header compiles through a host or a lender, never as a member; a
    // build tree or the cache holds no sources of the workspace's own.
    auto path = project.file_table.resolve(path_id);
    if(root.empty() || !is_source_path(path) || project.build.declared(path_id)) {
        return;
    }
    CanonicalPath cache_dir;
    if(!project.config.project.cache_dir.empty()) {
        cache_dir = CanonicalPath(Spelling::absolute(project.config.project.cache_dir));
    }
    if(!workspace_file(root, cache_dir, path) || !project.build.record(path_id)) {
        return;
    }
    LOG_INFO("Recorded {} as a provisional member", path);
    dispatch({FileEvent::cdb_changed({})});
    schedule_metadata_flush();
}

std::shared_ptr<Session> ProjectServer::create_session(Fid path_id) {
    // A replaced live session (an editor resending didOpen) leaves a
    // latest compile describing the old session's; the fresh session
    // starts with none.
    ast.drop(path_id);
    auto session = sessions.open(path_id);
    // The serving mode's creation write point; the only other write is
    // ASTFamily::escalate.
    session->serving =
        ast.readonly == ReadonlyMode::Off ? ServingMode::Escalated : ServingMode::IndexOnly;
    return session;
}

void ProjectServer::decide_open_serving(std::shared_ptr<Session> session) {
    // An escalated session needs no decision: builds stay pull-driven. When
    // they skip the bodies of the headers, the file's background run shows
    // what those hold (ASTFamily::report_skipped), and goes first when none
    // has reported on its text.
    if(session->serving == ServingMode::Escalated) {
        auto path_id = session->path_id;
        if(project.config.project.skip_header_bodies &&
           project.config.project.enable_indexing.value && ast.lacks_skipped(path_id) &&
           !sched.pump.in_flight(path_id) && !sched.pump.failed().contains(path_id)) {
            sched.pump.boost(path_id);
        }
        return;
    }
    if(auto* shard = project.index.shard(session->path_id)) {
        // A buffer that already diverges from the indexed content (a
        // restored unsaved file) can never be served read-only: escalate
        // now instead of answering empty until the first edit.
        if(!shard->matches_content(session->text().size(), session->hash)) {
            ast.escalate(*session);
        }
        return;
    }
    // Nothing indexed yet: reading this file is the reason to index it
    // first — unless indexing is disabled, in which case no shard will
    // ever arrive and only an AST can serve the document. A boost the
    // pump cannot fulfill escalates through the adapter's attempt-finished
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
    // flow before the initialize response.
    if(auto session = sessions.find(path_id)) {
        // The crash records stay with the file for a reopen; the
        // retraction must not carry their notes.
        sessions.park(*session);
        ast.publish_output(session, CompileOutput{.text = session->source});
    }
    // Route the eviction notification before dropping ownership:
    // notify_stateful uses the owner table to find the worker.
    auto path = project.file_table.resolve(path_id);
    server.pool.notify_stateful(path_id.raw, worker::EvictParams{std::string(path)});
    server.pool.remove_owner(path_id.raw);
    sessions.close(path_id);
    ast.drop(path_id);
    // The session's compile may have stood in for the file's background
    // index (IndexPump::compiled_by_session); the disk's turn again, unless
    // it was deleted meanwhile or its index run failed for good, which
    // waits for a change.
    if(!project.file_table.disk.seen_missing(path_id) && !sched.pump.failed().contains(path_id) &&
       sched.pump.enqueue(path_id, ReindexReason::DepsOnly)) {
        sched.pump.schedule(false);
    }
    // PCH entries are content-keyed and may be shared with other sessions,
    // so nothing entry-level to clean up — but the loaded-state budget
    // shrinks with the open count, and this is the moment it does.
    sched.pch.enforce_loaded_budget();
    LOG_DEBUG("Closed {}", path);
}

void ProjectServer::open_closures(llvm::SmallVectorImpl<Fid>& files) {
    for(auto& [path_id, session]: sessions.sessions) {
        ast.closure(path_id, files);
    }
}

bool ProjectServer::knows(Fid path_id) {
    return sessions.find(path_id) != nullptr || !project.build.commands(path_id).empty() ||
           project.dep_graph.knows(path_id) || !invalidator.readers(path_id).empty();
}

void ProjectServer::open_session(Fid path_id, std::string text, int version) {
    // A database generated since the start counts: the poll, or the
    // discovery around this file, has loaded it by now.
    if(std::exchange(commands_missing, false) && project.build.members().empty() &&
       !found_unreadable_database(project)) {
        LOG_GUIDANCE(
            "No compile_commands.json found in workspace {} or its direct subdirectories. "
            "Compile commands will be guessed; see "
            "https://docs.clice.io/clice/guide/quick-start#project-setup for setup.{}",
            project.file_table.display(root),
            deeper_databases_hint(project));
    }
    auto session = create_session(path_id);
    sessions.apply_open(*session, std::move(text), version);
    // A reopened document still barred by a crash says so at once: its
    // requests answer empty, and no compile will run to publish the note.
    if(!session->quarantine->empty()) {
        ast.republish(session);
    }
    // What the disk holds under the buffer: a later save or outside
    // write is then a change from it, even for a file nothing else knew.
    project.file_table.disk.current(path_id);
    if(!started) {
        return;
    }
    contexts.validate_saved_context(path_id);
    decide_open_serving(session);
}

void ProjectServer::index_attempt_finished(Fid path_id) {
    // The boost in decide_open_serving promised the index would serve the
    // cold session; an attempt that finishes without a servable shard ends
    // that promise — escalate like the disabled-indexing branch, or the
    // session answers empty until its first edit.
    auto session = sessions.find(path_id);
    if(!session || session->serving != ServingMode::IndexOnly) {
        return;
    }
    auto* shard = project.index.shard(path_id);
    if(!shard || !shard->matches_content(session->text().size(), session->hash)) {
        ast.escalate(*session);
    }
}

bool ProjectServer::session_served_by_shard(Fid path_id) const {
    auto session = sessions.find(path_id);
    return session && !ast.compiles.index_current(path_id) && session->index_served;
}

void ProjectServer::on_index_changed(llvm::ArrayRef<Fid> path_ids) {
    // Sessions without a current file index serve these very shards
    // (freshness clause 4); index_served says the client pulled from
    // them. Tell subscribers so those results get re-pulled.
    if(ranges::any_of(path_ids, [&](Fid id) { return session_served_by_shard(id); })) {
        server.on_serving_index_changed.emit();
    }
}

std::optional<std::size_t> ProjectServer::weigh_databases(bool force) {
    if(!tracker) {
        return 0;
    }
    if(tracker->moved()) {
        server.restart(*this);
        return std::nullopt;
    }
    auto events = tracker->tick_cdb(force);
    if(!events.empty()) {
        dispatch(events);
    }
    return events.size();
}

void ProjectServer::tick_databases() {
    if(project.config.tracker.workspace_poll_seconds.value > 0 &&
       weigh_databases(false).value_or(1) > 0) {
        server.probe.happened(LoopEvent::DatabaseChange);
    }
}

void ProjectServer::dispatch(llvm::ArrayRef<FileEvent> events) {
    auto dirty = invalidator.apply(events);

    for(auto path_id: dirty.reset_header_mode) {
        commands.reset_header_mode(path_id);
    }

    // The Lost invalidation voids the latest compile's currency (and any
    // in-flight round's commit-as-current) without touching generation —
    // the buffer is still the same buffer, and the round still publishes
    // its product as bounded staleness; the next request recompiles.
    bool inputs_changed = false;
    for(auto path_id: dirty.mark_ast_dirty) {
        if(auto session = sessions.find(path_id)) {
            ast.invalidate(path_id);
            session->trial_done = false;
            session->quarantine->on_change(Quarantine::Clock::now());
            inputs_changed |= session->serving == ServingMode::Escalated;
        }
        commands.forget_self_contained(path_id);
    }
    if(inputs_changed) {
        server.on_inputs_changed.emit();
    }

    // A live round sends a compile of its own, which puts the AST back on
    // a worker — invalidating it would only make it finish stale and compile
    // twice.
    for(auto path_id: dirty.mark_lost) {
        if(sessions.find(path_id) && !ast.compiling(path_id)) {
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
        index::IndexReport report;
        project.index.drop(path_id, report);
        sched.pump.apply_report(report);
    }

    for(auto path_id: dirty.reindex_content_changed) {
        ast.forget_skipped(path_id);
        sched.pump.enqueue(path_id, ReindexReason::ContentChanged);
    }
    for(auto path_id: dirty.reindex_deps_only) {
        ast.forget_skipped(path_id);
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
    // early (a pre-ready didClose arrives here), but the scheduler reads
    // configuration that initialize() has not applied yet. The reindex
    // queue filled above is kept — the post-ready workspace load kicks the
    // scheduler.
    if(dirty.reschedule_indexing && server.lifecycle == ServerLifecycle::Ready) {
        sched.pump.schedule();
    }

    // The database gained or lost entries: an open document may belong to
    // another project now.
    if(ranges::any_of(events, [](const FileEvent& event) {
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
    // Time is the feature: the debounce.
    co_await kota::sleep(std::chrono::milliseconds(50));
    metadata_flush_scheduled = false;
    co_await sched.pump.persist();
    if(project.artifacts_dirty || project.contexts.dirty) {
        // Time is the feature: the retry's backoff.
        co_await kota::sleep(std::chrono::seconds(5));
        schedule_metadata_flush();
    }
}

kota::task<> ProjectServer::checkpoint_task() {
    while(true) {
        // Time is the feature: the checkpoints' pace.
        co_await kota::sleep(SchedulingStack::checkpoint_interval());
        co_await sched.checkpoint();
        if(project.index.last_save_shards() > 0) {
            server.probe.happened(LoopEvent::Checkpoint);
        }
        drain_store_evictions();
    }
}

void ProjectServer::drain_store_evictions() {
    // The store's LRU evicted these blobs from disk (inside commit, maybe
    // on a worker thread); drop the derived pch_cache metadata here on the
    // event loop, or the content-keyed map grows for the server's
    // lifetime even for keys never requested again. An entry mid-rebuild
    // keeps its slot — its commit publishes a fresh pair. A key rebuilt
    // after the eviction was recorded names another blob by now.
    for(auto& evicted: project.store->take_evictions()) {
        if(evicted.ns != "pch") {
            continue;
        }
        auto it = ranges::find_if(project.pch_cache, [&](const auto& entry) {
            return entry.second.blob == evicted.key;
        });
        if(it != project.pch_cache.end() && !sched.pch.building(it->getKey())) {
            project.pch_cache.erase(it);
        }
    }
}

void ProjectServer::start_control_listener() {
    constexpr llvm::StringLiteral host = "127.0.0.1";
    auto acceptor = kota::tcp::listen(host, 0, {}, loop);
    std::optional<int> port;
    if(acceptor) {
        if(auto bound = acceptor->getsockname()) {
            port = bound->port;
        }
    }
    if(!port) {
        LOG_WARN("Failed to start the control listener; `clice index` cannot ask this server");
        return;
    }
    auto& cache_dir = project.config.project.cache_dir;
    if(!index::write_endpoint(cache_dir,
                              {.pid = static_cast<std::uint32_t>(sys::pid()),
                               .version = std::string(clice::version),
                               .host = host.str(),
                               .port = *port})) {
        return;
    }
    endpoint_recorded = true;
    LOG_INFO("Control channel listening on {}:{}", host, *port);
    bg_tasks.spawn(control::serve(*this, std::move(*acceptor)));
}

kota::task<> ProjectServer::sources_poll_task() {
    auto interval = std::chrono::seconds(project.config.tracker.workspace_poll_seconds.value);
    while(true) {
        // Time is the feature: the workspace loop's pace.
        co_await kota::sleep(interval);
        auto events = co_await tracker->tick_sources();
        if(!events.empty()) {
            dispatch(events);
        }
    }
}

}  // namespace clice
