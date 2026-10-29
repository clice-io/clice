module clice;

import :project.command_resolver;
import :project.hosting;
import :project.project;
import :server.include_trees;
import :support.logging;
import :worker.pool;
import :worker.protocol;
import :worker.scheduler;

namespace clice {

kota::task<bool> IncludeTrees::take(TreeRequest request) {
    auto key = request.key;
    std::shared_ptr<kota::event> done;
    if(auto it = running.find(key); it != running.end()) {
        done = it->second;
    } else {
        done = std::make_shared<kota::event>(false);
        running.try_emplace(key, done);
        if(!runs.spawn(run(std::move(request), done))) {
            running.erase(key);
            co_return false;
        }
    }
    co_await done->wait();
    co_return project.hosting.trees.find(key) != nullptr;
}

kota::task<> IncludeTrees::run(TreeRequest request, std::shared_ptr<kota::event> done) {
    auto finish = llvm::make_scope_exit([&] {
        running.erase(request.key);
        done->set();
    });

    auto& command = request.command;
    worker::IncludeTreeParams params;
    params.file = project.file_table.resolve(request.key.first).str();
    params.workspace = project.config.workspace_root.str();
    params.directory = project.commands.get(command.command).directory;
    auto rendered = project.toolchain.render(command, project.cdb.spelling(command.file));
    params.arguments.assign(rendered.argv.begin(), rendered.argv.end());
    params.hidden = std::move(rendered.hidden);
    auto result = co_await pool.send_stateless(params, worker::Priority::High);
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

kota::task<> IncludeTrees::stop() {
    runs.cancel();
    co_await runs.join();
}

kota::task<Resolution> IncludeTrees::resolve(llvm::function_ref<Resolution()> resolve,
                                             llvm::function_ref<bool()> current) {
    auto resolution = resolve();
    llvm::SmallVector<UnitCommand, 2> taken;
    while(resolution.missing_tree && !ranges::contains(taken, resolution.missing_tree->key)) {
        taken.push_back(resolution.missing_tree->key);
        if(!co_await take(*resolution.missing_tree) || (current && !current())) {
            break;
        }
        resolution = resolve();
    }
    co_return resolution;
}

}  // namespace clice
