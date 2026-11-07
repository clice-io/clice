module clice;

import :project.discovery;
import :server.client;
import :server.master;
import :server.served_project;
import :support.logging;
import :vfs.path;

namespace clice {

/// The deepest of the projects `accept` takes whose root holds `path`.
static ServedProject* deepest_holding(llvm::ArrayRef<std::shared_ptr<ServedProject>> projects,
                                      CanonicalRef path,
                                      llvm::function_ref<bool(ServedProject&)> accept) {
    ServedProject* deepest = nullptr;
    for(auto& project: projects) {
        if(project->holds(path) && accept(*project) &&
           (!deepest || project->project.root.size() > deepest->project.root.size())) {
            deepest = project.get();
        }
    }
    return deepest;
}

/// Add what `progress` counts to `into`.
static void add_progress(BackgroundIndex::Progress& into,
                         const BackgroundIndex::Progress& progress) {
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
    // A provisional member is no listing: the project it was recorded in
    // holds it until a database lists it.
    auto lists = [&](ServedProject& project) {
        return project.project.declared(path_id);
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

ServedProject& MasterServer::pick_owner(Fid path_id) {
    assert(!projects.empty() && "picking an owner before the projects start");
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
    return pick_owner(path_id);
}

std::shared_ptr<ServedProject> MasterServer::make_project(CanonicalPath root) {
    // A removed project holds its cache directory (writer lock, database)
    // until its last reference — the stop, or a request still running in
    // it — goes: the project waiting for the directory starts then.
    auto released = [this](ServedProject* project) {
        delete project;
        if(lifecycle == ServerLifecycle::Ready) {
            bg_tasks.spawn([](MasterServer& server) -> async::task<> {
                server.serve_folders();
                co_return;
            }(*this));
        }
    };
    return std::shared_ptr<ServedProject>(new ServedProject(*this, std::move(root)), released);
}

std::vector<CanonicalPath> MasterServer::taken_cache_dirs(CanonicalRef successor) const {
    std::vector<CanonicalPath> dirs;
    auto take = [&](const ServedProject& project) {
        // The rootless project opens no store.
        auto cache_dir = project.project.settings.project.cache_identity;
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

void MasterServer::update_owners() {
    if(lifecycle != ServerLifecycle::Ready) {
        return;
    }
    for(auto& [path_id, document]: documents) {
        if(!document.owner) {
            continue;
        }
        if(auto& to = pick_owner(path_id); &to != document.owner) {
            change_owner(document, to);
            replace_shown(document);
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
    to.adopt(session.path_id);
    if(client) {
        client->moved(session.path_id);
    }
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
    if(owner.compiles(*session)) {
        owner.request_compile(session);
    } else if(client) {
        client->refresh();
    }
}

void MasterServer::discover_around(Fid path_id) {
    auto path = files.resolve(path_id);
    for(auto& project: projects) {
        if(project->holds(path)) {
            project->discover_around(path_id);
        }
    }
}

std::size_t MasterServer::apply_disk_changes() {
    auto changed = files.disk.take_changes();
    for(auto& project: projects) {
        project->disk_changed(changed);
    }
    return changed.size();
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
        // The documents it held while it waited.
        for(auto& [path_id, document]: documents) {
            if(document.owner == project) {
                replace_shown(document);
            }
        }
    }
    // A stopped project's choices give it nothing any more.
    for(auto& project: leaving) {
        forget_choices(*project);
    }
    // Documents may belong elsewhere now: a database under a new root
    // lists them, they sit inside it, or their project stopped serving.
    update_owners();
    for(auto& project: leaving) {
        LOG_INFO("No longer serving project {}", project->project.root);
        if(auto it = index_pass.find(project.get()); it != index_pass.end()) {
            add_progress(ended_passes, it->second);
            index_pass.erase(it);
            fold_index_progress();
        }
        bg_tasks.spawn(
            [](MasterServer& server, std::shared_ptr<ServedProject> project) -> async::task<> {
                co_await project->shutdown();
                project.reset();
                server.return_freed_memory();
            }(*this, std::move(project)));
    }
    if(client) {
        client->publish_settings_diagnostics();
    }
}

void MasterServer::index_progress_changed(ServedProject& changed) {
    using Stage = BackgroundIndex::Progress::Stage;
    // A stopping project's background index winds down outside the pass.
    if(ranges::none_of(projects, [&](auto& project) { return project.get() == &changed; })) {
        return;
    }
    auto& now = changed.project.background_index.progress();
    auto it = index_pass.find(&changed);
    if(it == index_pass.end()) {
        if(now.stage == Stage::End) {
            return;
        }
        it = index_pass.try_emplace(&changed).first;
    } else if(it->second.stage == Stage::End && now.stage != Stage::End) {
        // Another pass of a project whose last one ended within this one.
        add_progress(ended_passes, it->second);
    }
    it->second = now;
    fold_index_progress();
}

void MasterServer::fold_index_progress() {
    using Stage = BackgroundIndex::Progress::Stage;
    BackgroundIndex::Progress all = ended_passes;
    bool active = false;
    for(auto& one:
        views::transform(index_pass, [](auto& entry) -> auto& { return entry.second; })) {
        active = active || one.stage != Stage::End;
        add_progress(all, one);
    }
    if(active) {
        all.stage = index_progress.stage == Stage::End ? Stage::Begin : Stage::Report;
    } else {
        all.stage = Stage::End;
        index_pass.clear();
        ended_passes = {};
    }
    index_progress = all;
    if(client) {
        client->report_index_progress();
    }
}

}  // namespace clice
