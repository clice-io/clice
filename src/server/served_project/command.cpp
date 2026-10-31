module clice;

import :project.command_resolver;
import :project.hosting;
import :project.project;
import :server.master;
import :server.served_project;
import :support.logging;
import :worker.pool;
import :worker.protocol;
import :worker.scheduler;

namespace clice {

Resolution ServedProject::resolve_command(Fid path_id,
                                          std::string& directory,
                                          std::vector<std::string>& arguments,
                                          const ContextChoice* candidate) {
    auto resolution =
        commands.resolve_command(path_id,
                                 directory,
                                 arguments,
                                 {
                                     .choice = candidate ? candidate : active_choice(path_id),
                                     .open = true,
                                 });
    if(auto* document = find(path_id); document && !candidate) {
        document->guessed = resolution.source == CommandSource::Inferred ||
                            resolution.source == CommandSource::Fallback;
    }
    return resolution;
}

kota::task<Resolution> ServedProject::document_command(Fid path_id,
                                                       std::string& directory,
                                                       std::vector<std::string>& arguments,
                                                       const ContextChoice* candidate,
                                                       llvm::function_ref<bool()> current) {
    auto resolution = resolve_command(path_id, directory, arguments, candidate);
    llvm::SmallVector<UnitCommand, 2> taken;
    while(resolution.missing_tree && !ranges::contains(taken, resolution.missing_tree->key)) {
        taken.push_back(resolution.missing_tree->key);
        if(!co_await take_tree(*resolution.missing_tree) || (current && !current())) {
            break;
        }
        resolution = resolve_command(path_id, directory, arguments, candidate);
    }
    co_return resolution;
}

kota::task<bool> ServedProject::take_tree(TreeRequest request) {
    auto key = request.key;
    std::shared_ptr<kota::event> done;
    if(auto it = tree_runs.find(key); it != tree_runs.end()) {
        done = it->second;
    } else {
        done = std::make_shared<kota::event>(false);
        tree_runs.try_emplace(key, done);
        if(!tree_tasks.spawn(tree_run(std::move(request), done))) {
            tree_runs.erase(key);
            co_return false;
        }
    }
    co_await done->wait();
    co_return project.hosting.trees.find(key) != nullptr;
}

kota::task<> ServedProject::tree_run(TreeRequest request, std::shared_ptr<kota::event> done) {
    auto finish = llvm::make_scope_exit([&] {
        tree_runs.erase(request.key);
        done->set();
    });

    auto& command = request.command;
    worker::IncludeTreeParams params;
    params.file = project.file_table.resolve(request.key.first).str();
    params.project_root = project.root.str();
    params.directory = project.commands.get(command.command).directory;
    auto rendered = project.toolchain.render(command, project.cdb.spelling(command.file));
    params.arguments.assign(rendered.argv.begin(), rendered.argv.end());
    params.hidden = std::move(rendered.hidden);
    auto result = co_await server.pool.send_stateless(params, worker::Priority::High);
    if(!result.has_value()) {
        LOG_INFO("No include tree for {}: {}", params.file, result.error().message);
        // A run that kills its worker would kill one per resolution; any
        // other failure is not the unit's.
        if(result.error().code == worker::dispatch_errc::worker_crashed) {
            commit_failed_tree(project, request);
        }
        co_return;
    }
    auto& tree = result.value();
    if(!tree.success) {
        LOG_INFO("No include tree for {}: {}", params.file, tree.error);
        commit_failed_tree(project, request);
        co_return;
    }
    commit_tree(project,
                request,
                tree.paths,
                tree.content_hashes,
                std::move(tree.nodes),
                tree.deps,
                tree.build_at);
}

const ContextChoice* ServedProject::active_choice(Fid path_id) const {
    auto* entry = server.choice(path_id);
    return entry && entry->project == this && entry->active ? &entry->choice : nullptr;
}

Fid ServedProject::host_of(Fid path_id) {
    if(auto* choice = active_choice(path_id); choice && choice->host_path_id.valid()) {
        return choice->host_path_id;
    }
    return clice::host_of(project, path_id);
}

const clang::LangOptions& ServedProject::dialect(Fid path_id) {
    return clice::dialect(project, path_id, active_choice(path_id), host_of(path_id));
}

}  // namespace clice
