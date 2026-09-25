#include "server/master_server.h"

#include <list>
#include <memory>
#include <set>
#include <string>
#include <tuple>
#include <vector>

#include "version.h"
#include "server/features.h"
#include "server/lsp_client.h"
#include "support/anomaly.h"
#include "support/filesystem.h"
#include "support/logging.h"

#include "kota/async/async.h"
#include "kota/codec/json/json.h"
#include "kota/ipc/codec/json.h"
#include "kota/ipc/recording_transport.h"
#include "kota/ipc/transport.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/Process.h"

namespace clice {

/// Retention bound of the notify log. Subscribers drain promptly, so only
/// messages that fire before any client attaches accumulate (a handful of
/// startup guidance reports in practice); the cap is a safety net, not a
/// working-set size.
constexpr static std::size_t notify_log_limit = 128;

MasterServer::MasterServer(kota::event_loop& loop,
                           std::string self_path,
                           std::string requested_configuration) :
    loop(loop), pool(loop), requested_configuration(std::move(requested_configuration)),
    bg_tasks(loop), self_path(std::move(self_path)) {
    // Documents opened before initialize land in this project: sessions
    // are plain state, and initialize re-routes them once folders exist.
    projects.push_back(make_project(std::string()));
    // A disk change can be seen deep inside any operation — a staleness
    // check, a rescan inside a cascade: the drain runs on a later loop
    // turn, outside it.
    files.on_change = [this] {
        bg_tasks.spawn([](MasterServer& server) -> kota::task<> {
            co_await kota::sleep(std::chrono::milliseconds(0));
            server.drain_disk_changes();
        }(*this));
    };
    // The notify hook is process-wide because the logging layer cannot
    // depend on the server; the composition root owns it for the server's
    // lifetime and turns reports into state (notify_log) plus a wake-up
    // signal. Master-side reports only ever fire on the event-loop thread
    // (see support/anomaly.h), so no synchronization is needed here.
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
    // The projects go first, while the members their release reads live.
    lifecycle = ServerLifecycle::Exited;
    projects.clear();
    logging::set_notify_hook(nullptr);
}

/// Dump every configuration layer of a project — the config file verbatim,
/// the client's initializationOptions overlay, and the merged result after
/// defaults. Absence is stated explicitly so "was my config even read?"
/// never needs a support round-trip; the stderr mirror puts all of it in
/// the editor's output panel.
static void log_configuration(const ProjectServer& project, llvm::StringRef init_options) {
    if(project.config_path.empty()) {
        LOG_INFO("Configuration file: Missing (project {})", project.root);
    } else {
        auto raw = fs::read(project.config_path);
        LOG_INFO("Configuration file {}:\n{}",
                 project.config_path,
                 raw ? *raw : std::string("<unreadable>"));
    }
    if(init_options.empty()) {
        LOG_INFO("initializationOptions: Missing");
    } else {
        auto pretty = kota::codec::json::prettify(init_options);
        LOG_INFO("initializationOptions:\n{}", pretty ? *pretty : init_options.str());
    }
    if(auto json = kota::codec::json::to_string(project.project.config)) {
        auto pretty = kota::codec::json::prettify(*json);
        LOG_INFO("Effective configuration:\n{}", pretty ? *pretty : *json);
    }
}

void MasterServer::initialize() {
    std::shared_ptr<ProjectServer> placeholder;
    if(!workspace_roots.empty()) {
        placeholder = std::move(projects.front());
        projects.clear();
        for(auto& root: project_roots()) {
            projects.push_back(make_project(std::move(root)));
        }
        projects_generation += 1;
    }
    for(auto& project: projects) {
        project->configure(init_options_json, taken_cache_dirs());
    }

    // One pool serves every project, sized for the most demanding one;
    // the first project names the log directory.
    WorkerPoolOptions pool_opts;
    pool_opts.self_path = self_path;
    pool_opts.stateful_count = 0;
    pool_opts.stateless_count = 0;
    pool_opts.min_stateless = 0;
    pool_opts.max_stateless = 0;
    bool unbounded = false;
    for(auto& project: projects) {
        auto& cfg = project->project.config.project;
        pool_opts.stateful_count =
            std::max(pool_opts.stateful_count, cfg.stateful_worker_count.value);
        pool_opts.stateless_count =
            std::max(pool_opts.stateless_count, cfg.stateless_worker_count.value);
        pool_opts.min_stateless =
            std::max(pool_opts.min_stateless, cfg.min_stateless_worker_count.value);
        pool_opts.max_stateless =
            std::max(pool_opts.max_stateless, cfg.max_stateless_worker_count.value);
        // 0 leaves the ceiling to the core count, the largest one.
        unbounded = unbounded || cfg.max_stateless_worker_count.value == 0;
    }
    if(unbounded) {
        pool_opts.max_stateless = 0;
    }

    auto& first = projects.front()->project.config.project;
    if(!first.logging_dir.empty()) {
        session_log_dir = logging::session_log_directory(first.logging_dir);
        if(logging::file_logger("master", session_log_dir, logging::options)) {
            LOG_INFO("Session log directory: {}", session_log_dir);
        }
    }
    for(auto& project: projects) {
        log_configuration(*project, init_options_json);
    }

    LOG_INFO("Server ready (projects={}, stateful={}, stateless={})",
             projects.size(),
             pool_opts.stateful_count,
             pool_opts.stateless_count);

    pool_opts.log_dir = session_log_dir;
    if(pool.start(pool_opts)) {
        lifecycle = ServerLifecycle::Ready;
        wire();
        for(auto& project: projects) {
            project->start();
        }
    } else {
        LOG_ANOMALY(WorkerSpawnFail, "Failed to start worker pool");
    }
    if(placeholder) {
        rehome_sessions(*placeholder);
        retire(std::move(placeholder));
    }
    on_projects_changed.emit();
}

void MasterServer::initialize(llvm::StringRef root) {
    workspace_roots = {root.str()};
    files.spell_root(root);
    initialize();
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
        llvm::DenseMap<ProjectServer*, llvm::SmallVector<Fid>> lost;
        for(auto id: info.lost_documents) {
            lost[&owner_of(Fid{id})].push_back(Fid{id});
        }
        for(auto& [project, documents]: lost) {
            project->dispatch(FileEvent::worker_crashed(documents));
        }
    };

    pool.on_evicted = [this](const std::string& path, std::size_t worker_index) {
        auto id = files.find(path);
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
        if(pool.remove_owner_from(id->raw, worker_index)) {
            owner_of(*id).dispatch(FileEvent::document_evicted(*id));
        } else {
            LOG_INFO("Ignoring eviction of {} from non-owner worker {}", path, worker_index);
        }
    };
}

/// The deepest of the projects `accept` takes whose root holds `path`.
static ProjectServer* deepest_holding(llvm::ArrayRef<std::shared_ptr<ProjectServer>> projects,
                                      llvm::StringRef path,
                                      llvm::function_ref<bool(ProjectServer&)> accept) {
    ProjectServer* deepest = nullptr;
    for(auto& project: projects) {
        if(!project->root.empty() && path::under(path, project->root) && accept(*project) &&
           (!deepest || project->root.size() > deepest->root.size())) {
            deepest = project.get();
        }
    }
    return deepest;
}

ProjectServer* MasterServer::compiler(Fid path_id) {
    for(auto& project: projects) {
        if(project->contexts.holds_choice(path_id)) {
            return project.get();
        }
    }
    // Several databases listing the file (an outer folder discovers a
    // nested one's): the deepest project holding it wins, else the first.
    auto lists = [&](ProjectServer& project) {
        return !project.project.build.commands(path_id).empty();
    };
    auto path = files.resolve(path_id);
    if(auto* listing = deepest_holding(projects, path, lists)) {
        return listing;
    }
    if(auto it = llvm::find_if(projects, [&](auto& project) { return lists(*project); });
       it != projects.end()) {
        return it->get();
    }
    auto includes = [&](ProjectServer& project) {
        return !project.project.dep_graph.get_includers(path_id).empty();
    };
    if(auto* hosting = deepest_holding(projects, path, includes)) {
        return hosting;
    }
    auto borrowed = llvm::find_if(projects, [&](auto& project) { return includes(*project); });
    return borrowed != projects.end() ? borrowed->get() : nullptr;
}

ProjectServer* MasterServer::claimant(Fid path_id) {
    if(auto* compiling = compiler(path_id)) {
        return compiling;
    }
    return deepest_holding(projects, files.resolve(path_id), [](ProjectServer&) { return true; });
}

ProjectServer& MasterServer::route(Fid path_id) {
    auto* project = claimant(path_id);
    return project ? *project : *projects.front();
}

ProjectServer& MasterServer::owner_of(Fid path_id) {
    if(auto it = owners.find(path_id); it != owners.end()) {
        return *it->second;
    }
    return route(path_id);
}

std::shared_ptr<Session> MasterServer::find_session(Fid path_id) {
    auto it = owners.find(path_id);
    return it != owners.end() ? it->second->sessions.find(path_id) : nullptr;
}

std::shared_ptr<ProjectServer> MasterServer::make_project(std::string root) {
    // A removed project holds its cache directory (writer lock, database)
    // until its last reference — the retirement, or a request still
    // running in it — goes: a root waiting for the directory serves then.
    auto released = [this](ProjectServer* project) {
        delete project;
        if(lifecycle == ServerLifecycle::Ready) {
            bg_tasks.spawn([](MasterServer& server) -> kota::task<> {
                server.serve_folders();
                co_return;
            }(*this));
        }
    };
    std::shared_ptr<ProjectServer> made(new ProjectServer(*this, std::move(root)), released);
    made->features.peers = [this, project = made.get()] {
        llvm::SmallVector<const index::IndexQuery*> others;
        for(auto& other: projects) {
            if(other.get() != project) {
                others.push_back(&other->index_query);
            }
        }
        return others;
    };
    made->features.open_in = [this](Fid file) -> const index::IndexQuery* {
        auto it = owners.find(file);
        return it != owners.end() ? &it->second->index_query : nullptr;
    };
    return made;
}

std::vector<std::string> MasterServer::taken_cache_dirs() const {
    std::vector<std::string> dirs;
    auto take = [&](const ProjectServer& project) {
        // The rootless project opens no store.
        auto& cache_dir = project.project.config.project.cache_dir;
        if(!project.root.empty() && !cache_dir.empty()) {
            dirs.push_back(path::resolved(cache_dir));
        }
    };
    for(auto& project: projects) {
        take(*project);
    }
    for(auto& weak: retired) {
        if(auto project = weak.lock()) {
            take(*project);
        }
    }
    return dirs;
}

void MasterServer::builds_changed() {
    if(lifecycle != ServerLifecycle::Ready) {
        return;
    }
    for(auto& project: projects) {
        rehome_sessions(*project);
    }
}

void MasterServer::open_session(Fid path_id, std::string text, int version) {
    discover_around(path_id);
    if(lifecycle == ServerLifecycle::Ready && !owners.contains(path_id) && !compiler(path_id)) {
        // The project root nearest the file serves it: one no folder
        // holds, or one nested deeper than the folders holding it (a
        // subproject's clice.toml or database). A folder's own root, and a
        // database a served project loads already (a build directory above
        // a generated file), stay that project's.
        auto path = files.resolve(path_id);
        auto root = project_root_above(path::parent_path(path));
        auto covered = [&](auto& project) {
            if(!project->root.empty() && path::under(path, project->root) &&
               path::under(project->root, root)) {
                return true;
            }
            auto& cdb = project->project.cdb;
            return cdb.find_source(root) || cdb.find_source(path::join(root, "build"));
        };
        if(!root.empty() && llvm::none_of(projects, covered)) {
            workspace_roots.push_back(std::move(root));
            serve_folders();
        }
    }
    auto& project = owner_of(path_id);
    owners[path_id] = &project;
    project.open_session(path_id, std::move(text), version);
}

void MasterServer::close_session(Fid path_id) {
    auto it = owners.find(path_id);
    if(it == owners.end()) {
        return;
    }
    // Closed while still routed: the diagnostics clear it publishes goes
    // out through this project.
    it->second->close_session(path_id);
    owners.erase(path_id);
    // Like an open and a save, a close is a moment to look at the disk: the
    // editor stops showing its buffer in place of the file.
    files.current(path_id);
    drain_disk_changes();
}

void MasterServer::discover_around(Fid path_id) {
    auto path = files.resolve(path_id);
    for(auto& project: projects) {
        if(!project->root.empty() && path::under(path, project->root)) {
            project->discover_around(path_id);
        }
    }
}

void MasterServer::rehome_sessions(ProjectServer& from) {
    llvm::SmallVector<std::shared_ptr<Session>> leaving;
    for(auto& [path_id, session]: from.sessions.sessions) {
        if(session && &route(path_id) != &from) {
            leaving.push_back(session);
        }
    }
    bool index_served = false;
    for(auto& session: leaving) {
        auto path_id = session->path_id;
        from.close_session(path_id);
        auto& to = route(path_id);
        owners[path_id] = &to;
        to.open_session(path_id, session->text, session->version);
        // The client still shows what the old project gave it, and no
        // request of its own replaces it: compile under the new one, or
        // have an index-served document's features pulled again.
        if(auto moved = to.sessions.find(path_id); moved->serving == ServingMode::Escalated) {
            to.ast.request_compile(moved);
        } else {
            index_served = true;
        }
    }
    if(index_served) {
        on_serving_rows_changed.emit();
    }
}

void MasterServer::change_folders(std::vector<std::string> removed,
                                  std::vector<std::string> added) {
    for(auto* roots: {&removed, &added}) {
        for(auto& root: *roots) {
            path::canonicalize(root);
        }
    }
    llvm::erase_if(removed,
                   [&](const std::string& root) { return llvm::is_contained(added, root); });
    for(auto& root: removed) {
        llvm::erase(workspace_roots, root);
    }
    for(auto& root: added) {
        files.spell_root(root);
        if(!llvm::is_contained(workspace_roots, root)) {
            workspace_roots.push_back(std::move(root));
        }
    }
    if(lifecycle == ServerLifecycle::Ready) {
        serve_folders();
    }
}

std::string MasterServer::root_of(llvm::StringRef folder) const {
    if(defines_project(folder)) {
        return folder.str();
    }
    llvm::StringRef enclosing;
    for(auto& other: workspace_roots) {
        if(other.size() < folder.size() && other.size() > enclosing.size() &&
           path::under(folder, other)) {
            enclosing = other;
        }
    }
    return enclosing.empty() ? folder.str() : root_of(enclosing);
}

std::vector<std::string> MasterServer::project_roots() const {
    std::vector<std::string> roots;
    for(auto& folder: workspace_roots) {
        auto root = root_of(folder);
        if(!llvm::is_contained(roots, root)) {
            roots.push_back(std::move(root));
        }
    }
    if(roots.empty()) {
        roots.emplace_back();
    }
    return roots;
}

void MasterServer::serve_folders() {
    llvm::erase_if(retired, [](auto& weak) { return weak.expired(); });
    std::vector<std::shared_ptr<ProjectServer>> serving;
    llvm::SmallVector<ProjectServer*> fresh;
    auto serve = [&](std::string root) {
        if(auto it = llvm::find_if(projects, [&](auto& project) { return project->root == root; });
           it != projects.end()) {
            serving.push_back(*it);
            return;
        }
        LOG_INFO("Serving project {}", root);
        serving.push_back(make_project(std::move(root)));
        fresh.push_back(serving.back().get());
    };
    for(auto& root: project_roots()) {
        // A removed project holds its cache directory until it is gone.
        if(llvm::none_of(retired, [&](auto& weak) {
               auto project = weak.lock();
               return project && project->root == root;
           })) {
            serve(std::move(root));
        }
    }
    if(serving.empty()) {
        serve(std::string());
    }

    std::vector<std::shared_ptr<ProjectServer>> leaving;
    for(auto& project: projects) {
        if(!llvm::is_contained(serving, project)) {
            leaving.push_back(project);
        }
    }
    if(fresh.empty() && leaving.empty()) {
        return;
    }
    projects = std::move(serving);
    projects_generation += 1;
    for(auto& project: leaving) {
        retired.push_back(project);
    }
    for(auto* project: fresh) {
        project->configure(init_options_json, taken_cache_dirs());
        log_configuration(*project, init_options_json);
        project->start();
    }
    // Documents may belong elsewhere now: a database under a new root
    // lists them, they sit inside it, or their project stopped serving.
    for(auto& project: projects) {
        rehome_sessions(*project);
    }
    for(auto& project: leaving) {
        LOG_INFO("No longer serving project {}", project->root);
        rehome_sessions(*project);
        if(auto it = round.find(project.get()); it != round.end()) {
            carry(it->second);
            round.erase(it);
            fold_index_progress();
        }
        retire(std::move(project));
    }
    on_projects_changed.emit();
}

void MasterServer::retire(std::shared_ptr<ProjectServer> project) {
    bg_tasks.spawn([](std::shared_ptr<ProjectServer> project) -> kota::task<> {
        co_await project->shutdown();
        project->close();
    }(std::move(project)));
}

void MasterServer::carry(const IndexPump::Progress& progress) {
    carried.total += progress.total;
    carried.completed += progress.completed;
    carried.dispatched += progress.dispatched;
}

void MasterServer::index_progress_changed(ProjectServer& changed) {
    using Stage = IndexPump::Progress::Stage;
    // A retiring project's pump winds down outside the round.
    if(llvm::none_of(projects, [&](auto& project) { return project.get() == &changed; })) {
        return;
    }
    auto& now = changed.sched.pump.progress();
    auto it = round.find(&changed);
    if(it == round.end()) {
        if(now.stage == Stage::End) {
            return;
        }
        it = round.try_emplace(&changed).first;
    } else if(it->second.stage == Stage::End && now.stage != Stage::End) {
        // Another round of a project whose last one ended within this one.
        carry(it->second);
    }
    it->second = now;
    fold_index_progress();
}

void MasterServer::fold_index_progress() {
    using Stage = IndexPump::Progress::Stage;
    IndexPump::Progress all = carried;
    bool active = false;
    for(auto& one: llvm::make_second_range(round)) {
        active = active || one.stage != Stage::End;
        all.total += one.total;
        all.completed += one.completed;
        all.dispatched += one.dispatched;
    }
    if(active) {
        all.stage = index_progress.stage == Stage::End ? Stage::Begin : Stage::Report;
    } else {
        all.stage = Stage::End;
        round.clear();
        carried = {};
    }
    index_progress = all;
    on_index_progress.emit();
}

std::uint64_t MasterServer::context_epoch() {
    // Each project's epoch only grows, so their sum moves with any of them
    // while the projects stay the same.
    std::uint64_t sum = 0;
    for(auto& project: projects) {
        sum += project->project.context_epoch;
    }
    if(std::pair(projects_generation, sum) != context_seen) {
        context_seen = {projects_generation, sum};
        context_generation += 1;
    }
    return context_generation;
}

void MasterServer::saved(Fid path_id) {
    files.current(path_id);
    drain_disk_changes();
}

std::size_t MasterServer::drain_disk_changes() {
    auto events = take_disk_events(files);
    for(auto& project: projects) {
        llvm::SmallVector<FileEvent> known;
        for(auto& event: events) {
            if(project->knows(event.path_id)) {
                known.push_back(event);
            }
        }
        if(!known.empty()) {
            project->dispatch(known);
        }
    }
    return events.size();
}

ext::QueryContextResult MasterServer::query_contexts(llvm::StringRef path,
                                                     Fid path_id,
                                                     const ext::QueryContextParams& params) {
    constexpr std::size_t page_size = 10;
    auto& owner = owner_of(path_id);
    auto items = owner.context_service.contexts(path, path_id);
    for(auto& project: projects) {
        if(project.get() == &owner) {
            continue;
        }
        for(auto& item: project->context_service.contexts(path, path_id)) {
            auto same = [&](const ext::ContextItem& known) {
                return known.uri == item.uri && known.occurrence == item.occurrence &&
                       known.command_hash == item.command_hash;
            };
            if(llvm::none_of(items, same)) {
                items.push_back(std::move(item));
            }
        }
    }
    ext::QueryContextResult result;
    result.epoch = context_epoch();
    result.total = static_cast<int>(items.size());
    auto offset = static_cast<std::size_t>(std::max(0, params.offset.value_or(0)));
    for(auto i = offset; i < std::min(offset + page_size, items.size()); i += 1) {
        result.contexts.push_back(std::move(items[i]));
    }
    return result;
}

kota::task<ext::SwitchContextResult> MasterServer::switch_context(llvm::StringRef path,
                                                                  Fid path_id,
                                                                  llvm::StringRef context_path,
                                                                  Fid context_path_id,
                                                                  ext::SwitchContextParams params) {
    ext::SwitchContextResult result;
    // A choice made against an outdated listing may reference contexts
    // that no longer exist — make the client re-query.
    if(params.epoch && *params.epoch != context_epoch()) {
        result.stale = true;
        co_return result;
    }
    params.epoch.reset();

    // The project offering the chosen item: the file's own first, then the
    // others, in the order query_contexts listed them.
    auto owner = owner_of(path_id).shared_from_this();
    auto offers = [&](ProjectServer& project) {
        return llvm::any_of(project.context_service.contexts(path, path_id),
                            [&](const ext::ContextItem& item) {
                                return item.uri == params.context_uri &&
                                       item.occurrence == params.occurrence &&
                                       item.command_hash == params.command_hash;
                            });
    };
    auto target = owner;
    if(!offers(*owner)) {
        auto it = llvm::find_if(projects, [&](auto& project) { return offers(*project); });
        if(it == projects.end()) {
            co_return result;
        }
        target = *it;
    }
    // One project holds the file's choice (compiler routes by it).
    for(auto& project: projects) {
        if(project != target) {
            project->contexts.forget_selection(path_id);
        }
    }
    auto session = find_session(path_id);
    if(target != owner && session) {
        owner->close_session(path_id);
        owners[path_id] = target.get();
        target->open_session(path_id, session->text, session->version);
        session = find_session(path_id);
    }
    result = co_await target->context_service.switch_context(path,
                                                             path_id,
                                                             session.get(),
                                                             context_path,
                                                             context_path_id,
                                                             params);
    // A context choice asks for the context-pure AST view; the merged
    // index cannot give it (union rows). A rejected switch changed no
    // context and owes none.
    if(result.success) {
        target->ast.escalate(*session);
    }
    co_return result;
}

std::vector<protocol::SymbolInformation> MasterServer::workspace_symbol(llvm::StringRef query) {
    constexpr std::size_t limit = Features::workspace_symbol_limit;
    llvm::SmallVector<std::vector<protocol::SymbolInformation>> ranked;
    for(auto& project: projects) {
        ranked.push_back(project->features.workspace_symbol(query));
    }
    std::vector<protocol::SymbolInformation> merged;
    std::set<std::tuple<std::string, std::uint32_t, std::uint32_t, std::string>> seen;
    for(std::size_t rank = 0; merged.size() < limit; rank += 1) {
        bool any = false;
        for(auto& list: ranked) {
            if(rank >= list.size() || merged.size() == limit) {
                continue;
            }
            any = true;
            auto& info = list[rank];
            auto& start = info.location.range.start;
            if(seen.emplace(info.location.uri, start.line, start.character, info.name).second) {
                merged.push_back(std::move(info));
            }
        }
        if(!any) {
            break;
        }
    }
    return merged;
}

void MasterServer::schedule_shutdown() {
    if(lifecycle == ServerLifecycle::Exited)
        return;
    lifecycle = ServerLifecycle::ShuttingDown;
    shutdown_source.cancel();
}

kota::task<> MasterServer::shutdown_and_cleanup() {
    // A client that went away skipped the shutdown request; no project may
    // start from here on (a retirement finishing below would serve the
    // folders again, see make_project).
    lifecycle = ServerLifecycle::ShuttingDown;
    co_await bg_tasks.join();
    for(auto& project: projects) {
        co_await project->shutdown();
    }
    co_await pool.stop();
    for(auto& project: projects) {
        project->close();
    }
    lifecycle = ServerLifecycle::Exited;
}

struct Connection {
    std::unique_ptr<kota::ipc::JsonPeer> peer;
    std::unique_ptr<LSPClient> lsp_client;
};

static kota::task<> run_connection(kota::ipc::JsonPeer* peer,
                                   std::list<Connection>& connections,
                                   std::list<Connection>::iterator pos) {
    co_await peer->run();
    LOG_INFO("Client disconnected");
    connections.erase(pos);
}

/// Socket-mode serving body: the first connection gets the LSP slot,
/// later ones only a peer (the slot is never reclaimed).
static kota::task<> accept_connections(MasterServer& server,
                                       kota::tcp::acceptor acceptor,
                                       std::list<Connection>& connections) {
    auto& loop = kota::event_loop::current();
    kota::task_group<> group(loop);
    bool lsp_registered = false;

    group.spawn([](MasterServer& server,
                   kota::tcp::acceptor& acceptor,
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
            if(!lsp_registered) {
                lsp = std::make_unique<LSPClient>(server, *peer);
                lsp_registered = true;
            }

            auto* peer_ptr = peer.get();
            auto it = connections.emplace(connections.end(),
                                          Connection{
                                              .peer = std::move(peer),
                                              .lsp_client = std::move(lsp),
                                          });

            group.spawn(run_connection(peer_ptr, connections, it));
        }
    }(server, acceptor, connections, group, lsp_registered));

    co_await group.join();
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
    MasterServer server(loop, self_path, opts.configuration.value_or(""));

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

        loop.schedule(
            [](MasterServer& server, kota::ipc::JsonPeer& peer, std::string root) -> kota::task<> {
                // Pre-initialize for standalone (no-editor) use; LSP initialize
                // will be rejected. Runs inside the loop — before the peer
                // reads its first message — because initialize() spawns
                // background tasks that need the running loop context.
                if(!root.empty()) {
                    server.initialize(root);
                }
                co_await kota::with_token(peer.run(), server.shutdown_token());
                co_await server.shutdown_and_cleanup();
            }(server, lsp_peer, ws));
        loop.run();
        return 0;
    }

    if(mode == ServerMode::Socket) {
        auto acceptor = kota::tcp::listen(host, port, {}, loop);
        if(!acceptor) {
            LOG_ERROR("failed to listen on {}:{}", host, port);
            return 1;
        }

        std::list<Connection> connections;
        LOG_INFO("Listening on {}:{} ...", host, port);
        loop.schedule([](MasterServer& server,
                         kota::tcp::acceptor acceptor,
                         std::list<Connection>& connections,
                         std::string root) -> kota::task<> {
            // See the pipe-mode comment: pre-initialization must run
            // inside the loop.
            if(!root.empty()) {
                server.initialize(root);
            }
            co_await kota::with_token(accept_connections(server, std::move(acceptor), connections),
                                      server.shutdown_token());
            co_await server.shutdown_and_cleanup();
        }(server, std::move(*acceptor), connections, ws));
        loop.run();
        return 0;
    }

    LOG_ERROR("unexpected server mode");
    return 1;
}

}  // namespace clice
