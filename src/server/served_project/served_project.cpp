module clice;

import :command.table;
import :index.database;
import :index.project_index;
import :index.database.writer_lock;
import :project.background;
import :project.discovery;
import :project.graph.task_graph;
import :project.project;
import :server.client;
import :server.control;
import :server.master;
import :server.served_project;
import :settings.settings;
import :support.logging;
import :support.text;
import :support.version;
import :vfs.cache_store;
import :vfs.path;
import :worker.protocol;
import :worker.test_hooks;

namespace clice {

ServedProject::ServedProject(MasterServer& server, CanonicalPath root) :
    server(server), project(server.files, std::move(root), &server.loop, &server.pool) {
    auto& background = project.background_index;
    progress_conn = background.on_progress_changed.connect(
        [this] { this->server.index_progress_changed(*this); });
    attempt_conn = background.on_attempt_finished.connect([this](Fid path_id) {
        auto* document = find_document(path_id);
        if(!document) {
            return;
        }
        if(!document->compiles && !query().serving(path_id)) {
            start_compiling(path_id);
            // The editor holds what the index gave while the attempt ran,
            // nothing: it pulls again, from the compile now.
            if(auto* client = this->server.client) {
                client->index_changed({path_id});
            }
        }
        show_skipped_diagnostics(path_id);
    });
    index_conn = project.on_index_changed.connect([this](llvm::ArrayRef<Fid> path_ids) {
        // The AST's answers take the file's other contexts from these
        // shards, and documents without a current file index serve them
        // (freshness clause 4): the client asks again for what it pulled.
        bool completed = false;
        llvm::SmallVector<Fid> reading;
        for(auto id: path_ids) {
            completed |= merge_again(id);
            if(find_document(id) && !index_current(id)) {
                reading.push_back(id);
            }
        }
        auto* client = this->server.client;
        if(!client) {
            return;
        }
        if(completed) {
            client->refresh();
        } else {
            client->index_changed(reading);
        }
    });
    metadata_conn = project.on_metadata_changed.connect([this] { schedule_metadata_flush(); });
}

ServedProject::~ServedProject() = default;

namespace {

/// Where a problem of the settings is, as compilers name a place, its file
/// spelled `file`.
std::string place(llvm::StringRef file, const SettingsIssue& issue) {
    return issue.line == 0 ? file.str() : std::format("{}:{}:{}", file, issue.line, issue.column);
}

/// Whether the project has its build to open before a document of it
/// opens: a database to index, one its rules name or one discovery finds,
/// or the index an earlier session built.
bool has_build(const Project& project) {
    if(project.rules.declares_sources() || !discovered_databases(project.root).empty()) {
        return true;
    }
    auto& cache_dir = project.settings.project.cache_dir;
    if(cache_dir.empty()) {
        return false;
    }
    auto store = CacheStore::open(cache_dir, cache_format_version, /*read_only=*/true);
    return store && index::database_exists(*store, project.database_key);
}

}  // namespace

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
                LOG_GUIDANCE("{}. {} is served until they are fixed and clice restarts",
                             issue.message,
                             unserved);
            }
            continue;
        }
        LOG_WARN("clice.toml {}: {}", place(issue.file, issue), issue.message);
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
}

void ServedProject::start() {
    if(!broken()) {
        project.use_configuration(configuration);
        if(!project.root.empty() && documents.empty() && !has_build(project)) {
            waiting = true;
            return;
        }
    }
    open();
}

void ServedProject::open() {
    waiting = false;
    started = true;
    server.log_into(*this);
    if(broken()) {
        return;
    }
    if(!project.root.empty()) {
        llvm::SmallVector<CanonicalPath> open_files;
        for(auto& [path_id, document]: documents) {
            open_files.emplace_back(project.file_table.resolve(path_id));
        }
        auto opened = project.open({
            .configuration = configuration,
            .cache = Project::OpenOptions::Cache::Write,
            .freshness = {.withhold = project.settings.project.enable_indexing},
            .files = std::move(open_files),
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
        // What the load read the databases and the saved index with.
        server.return_freed_memory();
    }
    if(project.settings.project.enable_indexing) {
        project.background_index.start(
            {.skipped_diagnostics = project.settings.project.skip_header_bodies});
    }
    server.load_choices(*this);

    // Documents adopted before the project started — held while its root's
    // previous project shut down — have it decided now whether they
    // compile, after the load: divergence detection sees the persisted
    // shards it just loaded (a restored unsaved buffer must compile, not
    // read as merely unindexed).
    bool recorded = false;
    for(auto& [path_id, document]: documents) {
        recorded = project.record_provisional(path_id) || recorded;
    }
    if(recorded) {
        commands_changed({});
        schedule_metadata_flush();
    }
    for(auto& [path_id, document]: documents) {
        book_skipped_diagnostics(path_id, true);
        decide_compiling(path_id);
    }
    loaded.set();

    if(project.root.empty()) {
        return;
    }
    auto poll_seconds = chrono::seconds(project.settings.tracker.workspace_poll_seconds);
    project.file_table.disk.add_root(project.root, {.max = poll_seconds});
    // Watched from the project load on: each database is baselined at the
    // read its entries came from.
    project.watch_databases();
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
    auto* document = server.find_document(session.path_id);
    return document && document->session.get() == &session && document->owner == this;
}

std::shared_ptr<const DocumentCompile> ServedProject::latest(Fid path_id) const {
    auto* document = find_document(path_id);
    return document ? document->latest : nullptr;
}

std::shared_ptr<const DocumentCompile> ServedProject::latest_at(Fid path_id, int version) const {
    auto compiled = latest(path_id);
    return compiled && compiled->output && compiled->output->version == version ? compiled
                                                                                : nullptr;
}

bool ServedProject::current(Fid path_id) const {
    auto compiled = latest(path_id);
    return compiled && compiled->generation &&
           *compiled->generation == project.graph.generation(ast_node(path_id));
}

bool ServedProject::index_current(Fid path_id) const {
    auto compiled = latest(path_id);
    return current(path_id) && compiled->index && compiled->index->loaded();
}

void ServedProject::update_latest(Fid path_id, llvm::function_ref<void(DocumentCompile&)> edit) {
    auto& document = *find_document(path_id);
    auto next = document.latest ? DocumentCompile(*document.latest) : DocumentCompile();
    edit(next);
    document.latest = std::make_shared<const DocumentCompile>(std::move(next));
}

void ServedProject::adopt(Fid path_id) {
    // Every invalidation from now on counts in the generation its compiles
    // and its requests' PCH adoptions read, before its first compile too.
    project.graph.add(ast_node(path_id));
    documents[path_id] = {};
    if(project.settings.project.readonly == ReadonlyMode::Off ||
       server.own_context_only.contains(path_id)) {
        start_compiling(path_id);
    }
    // What the disk holds under the buffer: a later save or outside
    // write is then a change from it, even for a file nothing else knew.
    project.file_table.disk.current(path_id);
    // Its first document opens a project start() left waiting, and goes
    // with the documents open() takes.
    bool opening = waiting;
    if(opening) {
        open();
    }
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
    if(opening || !started || broken()) {
        return;
    }
    if(project.record_provisional(path_id)) {
        commands_changed({});
        schedule_metadata_flush();
    }
    server.recheck_choice(path_id);
    book_skipped_diagnostics(path_id, true);
    decide_compiling(path_id);
}

void ServedProject::decide_compiling(Fid path_id) {
    // A compiled document needs no decision: builds stay pull-driven.
    if(find_document(path_id)->compiles) {
        return;
    }
    auto& session = *server.find_session(path_id);
    auto& background = project.background_index;
    if(auto* shard = project.index.shard(path_id)) {
        // A buffer that already diverges from the indexed content (a
        // restored unsaved file) can never be served read-only: it compiles
        // now instead of answering empty until the first edit.
        if(!shard->matches_content(session.text().size(), session.source->hash())) {
            start_compiling(path_id);
        }
        return;
    }
    // Nothing indexed yet: reading this file is the reason to index it
    // first — unless indexing is disabled or the file is no unit of the
    // build (a header, which only the units reading it index), in which
    // case no shard will arrive soon and only an AST can serve the
    // document. An index_first the background index cannot fulfill
    // starts compiling through the attempt-finished check.
    if(background.started() && project.is_unit(path_id)) {
        background.index_first(path_id);
    } else {
        start_compiling(path_id);
    }
}

bool ServedProject::look_for_database(bool force) {
    if(!waiting || server.shutting_down()) {
        return false;
    }
    if(!force) {
        auto poll = chrono::seconds(project.settings.tracker.workspace_poll_seconds);
        auto now = chrono::steady_clock::now();
        if(poll.count() == 0 || now < next_look) {
            return false;
        }
        next_look = now + poll;
    }
    if(!has_build(project)) {
        return false;
    }
    open();
    server.update_owners();
    return true;
}

void ServedProject::release(Fid path_id) {
    auto path = project.file_table.resolve(path_id);
    server.pool.forget_document(path_id.raw);
    project.graph.invalidate(ast_node(path_id));
    project.buffer_scans.forget(path_id);
    documents.erase(path_id);
    server.return_freed_memory();
    LOG_DEBUG("Released {}", path);
}

void ServedProject::record_saved(Fid path_id) {
    if(broken()) {
        return;
    }
    // LIMITATION: the editor's compile indexes the buffer for the reads of
    // this session alone, never for the project's index: each save of an
    // open source also compiles it in the background, in full.
    //
    // The open files the save made stale show what their background run
    // finds as soon as it can (show_skipped_diagnostics).
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
    // What the saves the database refuses may leave in memory before the
    // background index waits for one to commit.
    constexpr std::uint64_t unsaved_limit = 256ull << 20;
    std::optional<ScopedPause> waiting;
    while(true) {
        // Time is the feature: the checkpoints' pace.
        co_await async::sleep(checkpoint_interval());
        co_await project.checkpoint();
        if(project.index.last_save_shards() > 0) {
            server.hooks.happened(LoopEvent::Checkpoint);
        }
        if(project.unsaved_bytes <= unsaved_limit) {
            waiting.reset();
        } else if(!waiting) {
            LOG_GUIDANCE(
                "Background indexing waits until the index can be saved under {}: {} of "
                "it are unsaved",
                project.store->root_dir(),
                format_bytes(project.unsaved_bytes));
            waiting.emplace(project.background_index);
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
        if(!project.ask_compilers_again()) {
            continue;
        }
        Affected affected;
        for(auto& [path_id, document]: documents) {
            auto& latest = document.latest;
            if(latest && latest->output && !latest->output->diagnostics.query_failure.empty()) {
                affected.stale.push_back(path_id);
            }
        }
        follow(affected);
        server.hooks.happened(LoopEvent::CompilersAnswered);
    }
}

CommandRequest ServedProject::command_request(Fid path_id, const ContextChoice* candidate) const {
    return {.choice = candidate ? candidate : active_choice(path_id), .open = true};
}

const ContextChoice* ServedProject::active_choice(Fid path_id) const {
    auto* entry = server.choice(path_id);
    return entry && entry->project == this && entry->active ? &entry->choice : nullptr;
}

Fid ServedProject::host_of(Fid path_id) {
    if(auto* choice = active_choice(path_id); choice && choice->host.valid()) {
        return choice->host;
    }
    return project.host_of(path_id);
}

const clang::LangOptions& ServedProject::dialect(Fid path_id) {
    return project.dialect(path_id, active_choice(path_id));
}

}  // namespace clice
