module clice;

import :settings.settings;
import :index.peer_queries;
import :server.client;
import :server.master;
import :server.served_project;
import :support.logging;
import :vfs.path;

namespace clice {

/// Whether the project's root holds `path`; the rootless project's holds
/// none.
static bool holds_path(const ServedProject& project, CanonicalRef path) {
    return !project.project.root.empty() && path::is_within(path, project.project.root);
}

/// The deepest of the projects `accept` takes whose root holds `path`.
static ServedProject* deepest_holding(llvm::ArrayRef<std::shared_ptr<ServedProject>> projects,
                                      CanonicalRef path,
                                      llvm::function_ref<bool(ServedProject&)> accept) {
    ServedProject* deepest = nullptr;
    for(auto& project: projects) {
        if(holds_path(*project, path) && accept(*project) &&
           (!deepest || project->project.root.size() > deepest->project.root.size())) {
            deepest = project.get();
        }
    }
    return deepest;
}

/// Add what `progress` counts to `into`.
static void add_progress(IndexPump::Progress& into, const IndexPump::Progress& progress) {
    into.total += progress.total;
    into.completed += progress.completed;
    into.dispatched += progress.dispatched;
}

ServedProject* MasterServer::compiling_project(Fid path_id) {
    // An outer project listing them would serve them silently under its own
    // settings.
    auto path = files.resolve(path_id);
    if(auto* holding = deepest_holding(projects, path, [](ServedProject&) { return true; });
       holding && holding->broken()) {
        return holding;
    }
    if(auto* entry = choice(path_id); entry && entry->active) {
        return entry->project;
    }
    // Several databases listing the file (an outer folder discovers a
    // nested one's): the deepest project holding it wins, else the first.
    auto lists = [&](ServedProject& project) {
        return !project.project.build.commands(path_id).empty();
    };
    if(auto* listing = deepest_holding(projects, path, lists)) {
        return listing;
    }
    if(auto it = ranges::find_if(projects, [&](auto& project) { return lists(*project); });
       it != projects.end()) {
        return it->get();
    }
    auto includes = [&](ServedProject& project) {
        return !project.project.dep_graph.get_includers(path_id).empty();
    };
    if(auto* hosting = deepest_holding(projects, path, includes)) {
        return hosting;
    }
    auto borrowed = ranges::find_if(projects, [&](auto& project) { return includes(*project); });
    return borrowed != projects.end() ? borrowed->get() : nullptr;
}

ServedProject& MasterServer::route(Fid path_id) {
    assert(!projects.empty() && "routing before the projects start");
    if(auto* compiling = compiling_project(path_id)) {
        return *compiling;
    }
    auto* holding =
        deepest_holding(projects, files.resolve(path_id), [](ServedProject&) { return true; });
    return holding ? *holding : *projects.front();
}

ServedProject& MasterServer::owner_of(Fid path_id) {
    if(auto* document = find_document(path_id); document && document->owner) {
        return *document->owner;
    }
    return route(path_id);
}

std::shared_ptr<ServedProject> MasterServer::make_project(CanonicalPath root) {
    // A removed project holds its cache directory (writer lock, database)
    // until its last reference — the stop, or a request still running in
    // it — goes: the project waiting for the directory starts then.
    auto released = [this](ServedProject* project) {
        delete project;
        if(lifecycle == ServerLifecycle::Ready) {
            bg_tasks.spawn([](MasterServer& server) -> kota::task<> {
                server.serve_folders();
                co_return;
            }(*this));
        }
    };
    return std::shared_ptr<ServedProject>(new ServedProject(*this, std::move(root)), released);
}

llvm::SmallVector<const index::IndexQuery*>
    MasterServer::peer_queries(const ServedProject& project) const {
    llvm::SmallVector<const index::IndexQuery*> peers;
    for(auto& other: projects) {
        if(other.get() != &project) {
            peers.push_back(&other->project.index_query);
        }
    }
    return peers;
}

std::vector<CanonicalPath> MasterServer::taken_cache_dirs(CanonicalRef successor) const {
    std::vector<CanonicalPath> dirs;
    auto take = [&](const ServedProject& project) {
        // The rootless project opens no store.
        auto cache_dir = project.project.settings.project.cache_identity();
        if(!project.project.root.empty() && !cache_dir.empty()) {
            dirs.push_back(std::move(cache_dir));
        }
    };
    for(auto& project: projects) {
        take(*project);
    }
    for(auto& weak: removed_projects) {
        if(auto project = weak.lock(); project && project->project.root != successor) {
            take(*project);
        }
    }
    return dirs;
}

void MasterServer::reroute() {
    if(lifecycle != ServerLifecycle::Ready) {
        return;
    }
    for(auto& [path_id, document]: documents) {
        if(!document.owner) {
            continue;
        }
        if(auto& to = route(path_id); &to != document.owner) {
            change_owner(document, to);
        }
    }
}

void MasterServer::change_owner(Document& document, ServedProject& to) {
    auto& session = *document.session;
    session.generation += 1;
    // Another project compiles it under its own command: what crashed may
    // run again.
    session.changed(Session::Clock::now());
    document.owner->release(session.path_id);
    document.owner = &to;
    to.adopt(document.session);
    replace_shown(document);
}

void MasterServer::replace_shown(Document& document) {
    auto& owner = *document.owner;
    auto& session = document.session;
    if(owner.broken()) {
        if(client) {
            client->retract(session->path_id);
        }
        return;
    }
    // A compile finishing on unchanged text has the client re-pull what it
    // holds.
    if(owner.find_document(session->path_id)->serving == ServingMode::Escalated) {
        owner.request_compile(session);
    } else if(client) {
        client->refresh();
    }
}

void MasterServer::ask_restart(RestartReason reason) {
    if(!reason.topic.empty()) {
        std::erase_if(restart_reasons,
                      [&](const RestartReason& earlier) { return earlier.topic == reason.topic; });
    }
    if(ranges::contains(restart_reasons, reason.message, &RestartReason::message)) {
        return;
    }
    LOG_INFO("Restart needed: {}", reason.message);
    restart_reasons.push_back(std::move(reason));
    if(client) {
        client->ask_for_restart();
    }
}

void MasterServer::withdraw_restart(llvm::StringRef topic) {
    if(std::erase_if(restart_reasons,
                     [&](const RestartReason& reason) { return reason.topic == topic; }) > 0) {
        if(client) {
            client->ask_for_restart();
        }
    }
}

void MasterServer::discover_around(Fid path_id) {
    auto path = files.resolve(path_id);
    for(auto& project: projects) {
        if(holds_path(*project, path)) {
            project->discover_around(path_id);
        }
    }
}

void MasterServer::change_folders(std::vector<Spelling> removed, std::vector<Spelling> added) {
    for(auto& root: removed) {
        files.unspell_root(root);
    }
    for(auto& root: added) {
        files.spell_root(root);
    }
    auto identities = [](llvm::ArrayRef<Spelling> roots) {
        return ranges::to<llvm::SmallVector<CanonicalPath>>(
            views::transform(roots, [](auto& root) { return CanonicalPath(root); }));
    };
    auto gone = identities(removed);
    auto fresh = identities(added);
    for(auto& root: gone) {
        if(!ranges::contains(fresh, root)) {
            std::erase(folders, root);
        }
    }
    for(auto& root: fresh) {
        if(!ranges::contains(folders, root)) {
            folders.push_back(std::move(root));
        }
    }
    if(lifecycle == ServerLifecycle::Ready) {
        serve_folders();
    }
}

CanonicalPath MasterServer::root_of(CanonicalRef folder) const {
    if(defines_project(folder)) {
        return folder;
    }
    CanonicalRef enclosing;
    for(auto& other: folders) {
        if(other.size() < folder.size() && other.size() > enclosing.size() &&
           path::is_within(folder, other)) {
            enclosing = other;
        }
    }
    if(enclosing.empty()) {
        return folder;
    }
    return root_of(enclosing);
}

std::vector<CanonicalPath> MasterServer::project_roots() const {
    std::vector<CanonicalPath> roots;
    for(auto& folder: folders) {
        auto root = root_of(folder);
        if(!ranges::contains(roots, root)) {
            roots.push_back(std::move(root));
        }
    }
    if(roots.empty()) {
        roots.emplace_back();
    }
    return roots;
}

void MasterServer::serve_folders() {
    std::erase_if(removed_projects, [](auto& weak) { return weak.expired(); });
    watch_settings();
    std::vector<std::shared_ptr<ServedProject>> serving;
    llvm::SmallVector<ServedProject*> made;
    for(auto& root: project_roots()) {
        if(auto it = ranges::find_if(projects,
                                     [&](auto& project) { return project->project.root == root; });
           it != projects.end()) {
            serving.push_back(*it);
            continue;
        }
        LOG_INFO("Serving project {}", root);
        serving.push_back(make_project(std::move(root)));
        // Nothing compiles or indexes under no build until it starts.
        serving.back()->loaded.reset();
        serving.back()->sched.pump.pause_indexing();
        waiting_projects.push_back(serving.back().get());
        made.push_back(serving.back().get());
    }

    std::vector<std::shared_ptr<ServedProject>> leaving;
    for(auto& project: projects) {
        if(!ranges::contains(serving, project)) {
            leaving.push_back(project);
        }
    }
    projects = std::move(serving);
    for(auto& project: leaving) {
        std::erase(waiting_projects, project.get());
        removed_projects.push_back(project);
    }
    // Configured at once, so the documents it holds while it waits are
    // served under its own options; the cache directory its root's
    // previous project still holds passes to it.
    for(auto* project: made) {
        project->configure(init_options, taken_cache_dirs(project->project.root));
        log_settings(*project);
    }
    // A removed project holds its cache directory until it is gone: one
    // taking its root over waits for that, holding the root's documents.
    llvm::SmallVector<ServedProject*> starting;
    std::erase_if(waiting_projects, [&](ServedProject* project) {
        bool held = ranges::any_of(removed_projects, [&](auto& weak) {
            auto previous = weak.lock();
            return previous && previous->project.root == project->project.root;
        });
        if(!held) {
            starting.push_back(project);
        }
        return !held;
    });
    if(made.empty() && leaving.empty() && starting.empty()) {
        return;
    }
    projects_generation += 1;
    for(auto* project: starting) {
        project->start();
        project->sched.pump.resume_indexing();
        // The documents it held while it waited.
        for(auto& [path_id, document]: documents) {
            if(document.owner == project) {
                replace_shown(document);
            }
        }
    }
    // A stopped project's choices route nothing any more.
    for(auto& project: leaving) {
        forget_choices(*project);
    }
    // Documents may belong elsewhere now: a database under a new root
    // lists them, they sit inside it, or their project stopped serving.
    reroute();
    for(auto& project: leaving) {
        LOG_INFO("No longer serving project {}", project->project.root);
        if(auto it = index_round.find(project.get()); it != index_round.end()) {
            add_progress(ended_rounds, it->second);
            index_round.erase(it);
            fold_index_progress();
        }
        bg_tasks.spawn([](std::shared_ptr<ServedProject> project) -> kota::task<> {
            co_await project->shutdown();
            project->sched.close();
        }(std::move(project)));
    }
    if(client) {
        client->publish_settings_diagnostics();
    }
}

void MasterServer::index_progress_changed(ServedProject& changed) {
    using Stage = IndexPump::Progress::Stage;
    // A stopping project's pump winds down outside the round.
    if(ranges::none_of(projects, [&](auto& project) { return project.get() == &changed; })) {
        return;
    }
    auto& now = changed.sched.pump.progress();
    auto it = index_round.find(&changed);
    if(it == index_round.end()) {
        if(now.stage == Stage::End) {
            return;
        }
        it = index_round.try_emplace(&changed).first;
    } else if(it->second.stage == Stage::End && now.stage != Stage::End) {
        // Another round of a project whose last one ended within this one.
        add_progress(ended_rounds, it->second);
    }
    it->second = now;
    fold_index_progress();
}

void MasterServer::fold_index_progress() {
    using Stage = IndexPump::Progress::Stage;
    IndexPump::Progress all = ended_rounds;
    bool active = false;
    for(auto& one:
        views::transform(index_round, [](auto& entry) -> auto& { return entry.second; })) {
        active = active || one.stage != Stage::End;
        add_progress(all, one);
    }
    if(active) {
        all.stage = index_progress.stage == Stage::End ? Stage::Begin : Stage::Report;
    } else {
        all.stage = Stage::End;
        index_round.clear();
        ended_rounds = {};
    }
    index_progress = all;
    if(client) {
        client->report_index_progress();
    }
}

void MasterServer::watch_settings() {
    std::vector<SettingsWatch> watches;
    for(auto& folder: folders) {
        if(auto it = ranges::find(settings_watches, folder, &SettingsWatch::folder);
           it != settings_watches.end()) {
            watches.push_back(std::move(*it));
            continue;
        }
        auto& watch = watches.emplace_back(SettingsWatch{
            .folder = folder,
            .file = {.path = path::join(folder, settings_file_names[0])},
            .hidden = {.path = path::join(folder, settings_file_names[1])},
        });
        watch.file.look();
        watch.hidden.look();
    }
    settings_watches = std::move(watches);
}

void MasterServer::look_at_settings() {
    // Whether a configuration file is there and what it holds, unreadable
    // or not: a save that keeps the content changes nothing.
    auto content = [](const vfs::Flag& flag) {
        return std::pair(flag.stamp.has_value(), flag.hash);
    };
    for(auto& watch: settings_watches) {
        auto file = content(watch.file);
        auto hidden = content(watch.hidden);
        watch.file.look();
        watch.hidden.look();
        if(content(watch.file) == file && (watch.file.stamp || content(watch.hidden) == hidden)) {
            continue;
        }
        auto& in_use = watch.file.stamp ? watch.file : watch.hidden;
        if(!in_use.stamp) {
            ask_restart({.message = std::format("The configuration file of {} was deleted",
                                                files.display(watch.folder))});
            continue;
        }
        ask_restart({
            .file = in_use.path,
            .message = std::format("{} changed", files.display_identity(in_use.path)),
        });
    }
}

}  // namespace clice
