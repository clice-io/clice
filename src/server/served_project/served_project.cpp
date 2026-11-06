module;

#include "version.h"

module clice;

import :command.table;
import :index.project_index;
import :index.writer_lock;
import :project.background;
import :project.graph.task_graph;
import :server.client;
import :server.control;
import :server.master;
import :server.served_project;
import :settings.settings;
import :support.logging;
import :vfs.cache_store;
import :vfs.path;
import :worker.probe;
import :worker.protocol;

namespace clice {

ServedProject::ServedProject(MasterServer& server, CanonicalPath root) :
    server(server), project(server.files, std::move(root), &server.loop, &server.pool) {
    project.index_query.attach(*this);
    project.serving = this;
    progress_conn = project.background_index.on_progress_changed.connect(
        [this]() { this->server.index_progress_changed(*this); });
}

ServedProject::~ServedProject() = default;

void ServedProject::attempt_finished(Fid path_id) {
    auto* document = find_document(path_id);
    if(document && document->serving == ServingMode::IndexOnly &&
       !matching_shard(*document->session)) {
        escalate(path_id);
    }
}

void ServedProject::index_changed(llvm::ArrayRef<Fid> path_ids) {
    auto* client = server.client;
    if(client && ranges::any_of(path_ids, [&](Fid id) {
           return find_document(id) && !index_current(id) && client->index_served.contains(id);
       })) {
        client->refresh();
    }
}

/// Where a problem of the settings is, as compilers name a place, its file
/// spelled `file`.
static std::string place(llvm::StringRef file, const SettingsIssue& issue) {
    return issue.line == 0 ? file.str() : std::format("{}:{}:{}", file, issue.line, issue.column);
}

void ServedProject::configure(const std::optional<codec::dyn::Value>& init_options,
                              llvm::ArrayRef<CanonicalPath> taken_cache_dirs) {
    auto loaded = load_settings(project.root,
                                {
                                    .overlay = init_options,
                                    .configuration = server.requested_configuration,
                                    .strict = false,
                                    .taken_cache_dirs = taken_cache_dirs,
                                });
    project.settings = std::move(loaded.settings);
    configuration = std::move(loaded.configuration);
    settings_file = std::move(loaded.file);
    settings_issues = std::move(loaded.issues);

    // The file's problems are listed as its diagnostics, with one message;
    // the editor settings' are told each, the client showing a message once
    // however many projects find it.
    auto unserved = project.root.empty()
                        ? std::string("Nothing")
                        : std::format("Nothing under {}", project.file_table.display(project.root));
    const SettingsIssue* first = nullptr;
    std::size_t count = 0;
    for(auto& issue: settings_issues) {
        if(issue.file.empty()) {
            if(issue.severity == SettingsIssue::Severity::Warning) {
                LOG_GUIDANCE("{}", issue.message);
            } else {
                LOG_ERROR("{}", issue.message);
                LOG_GUIDANCE("{}. {} is served until they are fixed and clice restarts",
                             issue.message,
                             unserved);
            }
            continue;
        }
        if(issue.severity == SettingsIssue::Severity::Error) {
            LOG_ERROR("Invalid clice.toml {}: {}", place(issue.file, issue), issue.message);
        } else {
            LOG_WARN("clice.toml {}: {}", place(issue.file, issue), issue.message);
        }
        if(!first || (first->severity != issue.severity &&
                      issue.severity == SettingsIssue::Severity::Error)) {
            first = &issue;
        }
        count += 1;
    }
    if(first) {
        LOG_GUIDANCE("Configuration problem in {}: {}{}{}",
                     place(project.file_table.display_identity(first->file), *first),
                     first->message,
                     count == 1 ? std::string()
                                : std::format(" (and {} more, listed as its problems)", count - 1),
                     first->severity == SettingsIssue::Severity::Error
                         ? std::format(". {} is served until it is fixed and saved", unserved)
                         : std::string());
    }
    if(broken()) {
        return;
    }
    auto& cfg = project.settings.project;
    if(cfg.cache_dir_defaulted) {
        CacheStore::write_ignore_markers(cfg.cache_dir);
    }
}

void ServedProject::start() {
    started = true;
    if(broken()) {
        return;
    }
    if(!project.root.empty()) {
        auto opened = project.open({
            .configuration = configuration,
            .cache = Project::OpenOptions::Cache::Write,
            .freshness = {.withhold = project.settings.project.enable_indexing},
        });
        if(project.store) {
            bg_tasks.spawn(checkpoint_task());
        }
        if(project.database && !project.database->read_only()) {
            start_control_listener();
        }
        commands_missing =
            ranges::none_of(opened.members, [&](Fid member) { return project.declared(member); }) &&
            !project.rules.declares_sources() && !project.found_unreadable_database();
    }
    if(project.settings.project.enable_indexing) {
        project.background_index.start();
    }
    server.load_choices(*this);

    // Documents adopted before the project started — held while its root's
    // previous project shut down — have their serving decided now, after
    // the load: divergence detection sees the persisted shards it just
    // loaded (a restored unsaved buffer must escalate, not read as merely
    // unindexed).
    bool recorded = false;
    for(auto& [path_id, document]: documents) {
        recorded = project.record_provisional(path_id) || recorded;
    }
    if(recorded) {
        commands_changed({});
        schedule_metadata_flush();
    }
    for(auto& [path_id, document]: documents) {
        decide_serving(document);
    }
    loaded.set();

    if(project.root.empty()) {
        return;
    }
    auto poll_seconds = chrono::seconds(project.settings.tracker.workspace_poll_seconds);
    project.file_table.disk.add_root(project.root, {.max = poll_seconds});
    // Watched from the project load on: each database is baselined at the
    // read its entries came from.
    watching = true;
    for(std::size_t i = 0; i < project.cdb.source_count(); i += 1) {
        watch_database(SourceID(i));
    }
    watch_checkout();
    for(auto& [path_id, document]: documents) {
        discover_around(path_id);
    }
    server.watch_settings();
    if(poll_seconds.count() > 0) {
        bg_tasks.spawn(sources_poll_task());
        server.polling_files = true;
    }
}

async::task<> ServedProject::shutdown() {
    if(endpoint_recorded) {
        index::remove_endpoint(project.settings.project.cache_dir);
        endpoint_recorded = false;
    }
    bg_tasks.cancel();
    co_await bg_tasks.join();
    // Quiesce in-flight compilation and indexing first so the persisted
    // snapshot below covers everything that actually completed.
    co_await stop_compiles();
    // Requests have unwound and released their interest; the shared tail
    // stops the background index and winds down the graph's attempts before
    // the persistence pass.
    co_await project.shutdown();
}

ServedDocument* ServedProject::find_document(Fid path_id) {
    auto it = documents.find(path_id);
    return it != documents.end() ? &it->second : nullptr;
}

const ServedDocument* ServedProject::find_document(Fid path_id) const {
    auto it = documents.find(path_id);
    return it != documents.end() ? &it->second : nullptr;
}

bool ServedProject::owns(const Session& session) const {
    auto* document = find_document(session.path_id);
    return document && document->session.get() == &session;
}

void ServedProject::adopt(std::shared_ptr<Session> session) {
    // A database generated since the start counts: the poll, or the
    // discovery around this file, has loaded it by now. A provisional
    // member's command is a guess too.
    if(std::exchange(commands_missing, false) &&
       ranges::none_of(project.members(), [&](Fid member) { return project.declared(member); }) &&
       !project.found_unreadable_database()) {
        auto why = project.why_no_members(project.file_table.display(project.root));
        why.front() = llvm::toUpper(why.front());
        LOG_GUIDANCE(
            "{} Compile commands will be guessed; see "
            "https://docs.clice.io/clice/guide/quick-start#project-setup for setup.",
            why);
    }
    auto path_id = session->path_id;
    // Every invalidation from now on counts in the generation its compiles
    // and its requests' PCH adoptions read, before its first compile too.
    project.graph.add(ast_node(path_id));
    auto& document = documents[path_id];
    document = ServedDocument{
        .session = std::move(session),
        .serving = project.settings.project.readonly == ReadonlyMode::Off ? ServingMode::Escalated
                                                                          : ServingMode::IndexOnly,
    };
    // What the disk holds under the buffer: a later save or outside
    // write is then a change from it, even for a file nothing else knew.
    project.file_table.disk.current(path_id);
    if(!started || broken()) {
        return;
    }
    if(project.record_provisional(path_id)) {
        commands_changed({});
        schedule_metadata_flush();
    }
    server.recheck_choice(path_id);
    // The change may have moved other documents in: the table may have
    // grown since the reference above was taken.
    decide_serving(documents.find(path_id)->second);
}

void ServedProject::decide_serving(ServedDocument& document) {
    // An escalated document needs no decision: builds stay pull-driven. When
    // they skip the bodies of the headers, the file's background run shows
    // what those hold (report_skipped_diagnostics), and goes first when none
    // has reported on its text.
    auto& session = *document.session;
    auto path_id = session.path_id;
    auto& background = project.background_index;
    if(document.serving == ServingMode::Escalated) {
        if(project.settings.project.skip_header_bodies && background.started() &&
           lacks_skipped_diagnostics(path_id) && !background.queue().in_flight(path_id) &&
           !background.failed(path_id)) {
            background.index_first(path_id);
        }
        return;
    }
    if(auto* shard = project.index.shard(path_id)) {
        // A buffer that already diverges from the indexed content (a
        // restored unsaved file) can never be served read-only: escalate
        // now instead of answering empty until the first edit.
        if(!shard->matches_content(session.text().size(), session.hash)) {
            escalate(path_id);
        }
        return;
    }
    // Nothing indexed yet: reading this file is the reason to index it
    // first — unless indexing is disabled or the file is no unit of the
    // build (a header, which only the units reading it index), in which
    // case no shard will arrive soon and only an AST can serve the
    // document. An index_first the background index cannot fulfill
    // escalates through the adapter's attempt-finished check.
    if(background.started() && project.is_unit(path_id)) {
        background.index_first(path_id);
    } else {
        escalate(path_id);
    }
}

void ServedProject::release(Fid path_id) {
    auto path = project.file_table.resolve(path_id);
    server.pool.forget_document(path_id.raw);
    project.graph.invalidate(ast_node(path_id));
    project.import_scans.buffers.erase(path_id);
    documents.erase(path_id);
    // The document's compile may have stood in for the file's background
    // index (ServingSide::compiled_by_session); the disk's turn again, unless
    // it was deleted meanwhile or its index run failed for good, which
    // waits for a change.
    auto& background = project.background_index;
    if(!project.file_table.disk.seen_missing(path_id) && !background.failed(path_id) &&
       background.enqueue(path_id, ReindexReason::DepsOnly)) {
        background.schedule();
    }
    // PCH entries are content-keyed and may be shared with other documents,
    // so nothing entry-level to clean up — but the loaded-index budget
    // shrinks with the open count, and this is the moment it does.
    project.enforce_loaded_budget();
    server.return_freed_memory();
    LOG_DEBUG("Released {}", path);
}

void ServedProject::record_saved(Fid path_id) {
    if(broken()) {
        return;
    }
    // The open files the save made stale show what their background run
    // finds as soon as it can (report_skipped_diagnostics).
    if(project.settings.project.skip_header_bodies) {
        auto& background = project.background_index;
        for(auto& [open, document]: documents) {
            if(background.queue().pending_reason(open) && !background.queue().in_flight(open)) {
                background.index_first(open);
            }
        }
    }
    if(project.record_provisional(path_id)) {
        commands_changed({});
        schedule_metadata_flush();
    }
}

void ServedProject::schedule_metadata_flush() {
    if(metadata_flush_scheduled || server.shutting_down()) {
        return;
    }
    metadata_flush_scheduled = true;
    bg_tasks.spawn(metadata_flush_task());
}

async::task<> ServedProject::metadata_flush_task() {
    // One loop turn of debounce coalesces a burst of marks into one save;
    // the save itself batches whatever else is dirty by then. Marks
    // arriving while the save runs schedule one again, and
    // a save that could not clear the flags (serialization or write
    // failure) retries on the next spawn with this backoff.
    // Time is the feature: the debounce.
    co_await async::sleep(chrono::milliseconds(50));
    metadata_flush_scheduled = false;
    co_await project.save();
    if(project.precompiled_dirty || project.choices.dirty) {
        // Time is the feature: the retry's backoff.
        co_await async::sleep(chrono::seconds(5));
        schedule_metadata_flush();
    }
}

async::task<> ServedProject::checkpoint_task() {
    while(true) {
        // Time is the feature: the checkpoints' pace.
        co_await async::sleep(checkpoint_interval());
        co_await project.checkpoint();
        if(project.index.last_save_shards() > 0) {
            server.probe.happened(LoopEvent::Checkpoint);
        }
    }
}

void ServedProject::start_control_listener() {
    constexpr llvm::StringLiteral host = "127.0.0.1";
    auto acceptor = async::tcp::listen(host, 0, {}, server.loop);
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
    auto& cache_dir = project.settings.project.cache_dir;
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

async::task<> ServedProject::sources_poll_task() {
    auto interval = chrono::seconds(project.settings.tracker.workspace_poll_seconds);
    while(true) {
        // Time is the feature: the workspace loop's pace.
        co_await async::sleep(interval);
        co_await poll_sources();
    }
}

}  // namespace clice
