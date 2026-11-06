module clice;

import :compile.compilation;
import :index.include_tree;
import :index.project_index;
import :index.tu_manifest;
import :project.command;
import :project.project;
import :support.logging;
import :vfs.file_system;
import :vfs.path;
import :worker.pool;
import :worker.protocol;

namespace clice {

/// include_chains() by one include tree, rooted at `root`.
static std::optional<llvm::SmallVector<Host>> tree_chains(Project& project,
                                                          Fid host,
                                                          Fid header,
                                                          VersionID root,
                                                          llvm::ArrayRef<index::IncludeNode> nodes,
                                                          llvm::ArrayRef<VersionID> absent) {
    using CheckResult = vfs::DiskState::CheckResult;
    auto& files = project.file_table;
    if(files.check_version(root) != CheckResult::Fresh ||
       ranges::any_of(absent, [&](VersionID place) {
           return files.disk.present(files.version(place).fid);
       })) {
        return std::nullopt;
    }
    auto file_of = [&](std::uint32_t node) {
        return files.version(VersionID{nodes[node].file}).fid;
    };
    auto fresh = [&](std::uint32_t node) {
        return files.check_version(VersionID{nodes[node].file}) == CheckResult::Fresh;
    };

    // The header's own guard decides which of its includes after the first
    // enter it.
    // FIXME: another file defining the same guard skips the first include too:
    // a header included once is not checked, so fixing its guard keeps the
    // tree's answer until the host's tree is taken again.
    auto repeated = ranges::count_if(nodes, [&](const index::IncludeNode& node) {
                        return files.version(VersionID{node.file}).fid == header;
                    }) > 1;

    struct Chain {
        /// The compile enters what its command forces in ahead of the
        /// unit's text, in the command's order: a forced chain sorts by
        /// its line in the command-line buffer, before every other.
        llvm::SmallVector<std::uint32_t> order;

        Host host;
    };

    llvm::SmallVector<Chain> found;
    llvm::DenseSet<Fid> named;
    for(std::uint32_t i = 0; i < nodes.size(); i += 1) {
        named.insert(file_of(i));
        if(nodes[i].skipped || file_of(i) != header) {
            continue;
        }
        llvm::SmallVector<std::uint32_t> path;
        for(auto node = i; node != index::no_node; node = nodes[node].parent) {
            path.push_back(node);
        }
        // A file the command forces in hangs off the unit like its own
        // directives, at a line of the command-line buffer: the chain
        // starts at it.
        auto root = path.back();
        // FIXME: by file, not by node: a file without a guard that the unit both
        // forces in and includes at its top level is taken for the forced one at
        // either root.
        bool forced = ranges::contains(project.dep_graph.get_forcing_units(file_of(root)), host);
        Chain chain{.host = {.file = host}};
        chain.order = forced ? llvm::SmallVector<std::uint32_t>{0, nodes[root].line}
                             : llvm::SmallVector<std::uint32_t>{1};
        if(!forced) {
            chain.host.chain.push_back(host);
        }
        for(auto node: views::reverse(path)) {
            if((node != i || repeated) && !fresh(node)) {
                return std::nullopt;
            }
            chain.host.chain.push_back(file_of(node));
            if(!forced || node != root) {
                chain.host.lines.push_back(nodes[node].line);
            }
        }
        chain.order.append(chain.host.lines.begin(), chain.host.lines.end());
        found.push_back(std::move(chain));
    }
    // Nodes are numbered by includer, not by when the compile entered
    // them; within one file the directives run top to bottom, so the
    // first line where two chains part orders them.
    ranges::sort(found, [](const Chain& a, const Chain& b) {
        return ranges::lexicographical_compare(a.order, b.order);
    });
    llvm::SmallVector<Host> hosts;
    for(auto& chain: found) {
        hosts.push_back(std::move(chain.host));
    }
    if(!hosts.empty()) {
        return hosts;
    }

    // A forced file a precompiled header stands for, and everything it
    // includes, was entered before the tree began: the tree does not name
    // it, nor tell whether the compile enters the header through it.
    auto& graph = project.dep_graph;
    for(auto file: graph.get_forced_includes(host)) {
        if(!named.contains(file) && !graph.find_include_chain(file, header).empty()) {
            return std::nullopt;
        }
    }

    // The files of the scan's chains to the header, from the unit and from
    // what it forces in, which may have gained the include since.
    llvm::DenseSet<Fid> on_chain;
    for(auto from: llvm::concat<const Fid>(llvm::ArrayRef(host), graph.get_forced_includes(host))) {
        auto chain = graph.find_include_chain(from, header);
        on_chain.insert(chain.begin(), chain.end());
    }
    on_chain.erase(header);
    for(std::uint32_t i = 0; i < nodes.size(); i += 1) {
        if(on_chain.contains(file_of(i)) && !fresh(i)) {
            return std::nullopt;
        }
    }
    return hosts;
}

/// The pool's run taking the tree `request` names, which belongs to no
/// asker: one cancelled leaves it to the others waiting on `done`, set when
/// it ends.
static async::task<> run_tree(Project& project,
                              TreeRequest request,
                              std::shared_ptr<async::event> done) {
    auto finish = llvm::make_scope_exit([&] {
        project.tree_runs.erase(request.key);
        done->set();
    });

    worker::IncludeTreeParams params;
    params.file = project.file_table.resolve(request.key.first).str();
    params.project_root = project.root.str();
    auto rendered = project.render(request.command);
    params.directory = std::move(rendered.directory);
    params.arguments = std::move(rendered.arguments);
    params.hidden = std::move(rendered.hidden);
    auto result = co_await project.pool->send_stateless(params, worker::Priority::High);
    if(!result.has_value()) {
        LOG_INFO("No include tree for {}: {}", params.file, result.error().message);
        // A run that kills its worker would kill one per resolution; any
        // other failure is not the unit's.
        if(result.error().code == worker::dispatch_errc::worker_crashed) {
            project.commit_failed_tree(request);
        }
        co_return;
    }
    auto& tree = result.value();
    if(!tree.success) {
        LOG_INFO("No include tree for {}: {}", params.file, tree.error);
        project.commit_failed_tree(request);
        co_return;
    }
    project.commit_tree(request,
                        tree.paths,
                        tree.content_hashes,
                        std::move(tree.nodes),
                        tree.deps,
                        tree.started_at);
}

TreeRequest Project::tree_request(Fid unit, const Candidate& candidate) {
    auto command = apply_rules(unit, candidate);
    return {
        .key = {unit, commands.command_hash(command.command)},
        .command = command
    };
}

std::optional<llvm::SmallVector<Host>> Project::include_chains(const UnitCommand& unit,
                                                               Fid header) {
    auto [host, command] = unit;
    // The manifest holds the tree of the command the index was built under.
    if(index.indexed_command(host) == command) {
        if(auto manifest = index.manifest(host)) {
            if(auto found = tree_chains(*this,
                                        host,
                                        header,
                                        manifest->tu_version,
                                        manifest->nodes,
                                        manifest->absent)) {
                return found;
            }
        }
    }
    // As with the index's trees, the header's own text moves nothing above
    // it: tree_chains checks it where its guard decides.
    auto* entry = hosting.trees.lookup(file_table, unit, header);
    if(!entry) {
        return std::nullopt;
    }
    if(!entry->result.root.valid()) {
        return llvm::SmallVector<Host>();
    }
    return tree_chains(*this, host, header, entry->result.root, entry->result.nodes, {});
}

void Project::commit_tree(const TreeRequest& request,
                          llvm::ArrayRef<std::string> paths,
                          llvm::ArrayRef<std::uint64_t> hashes,
                          std::vector<index::IncludeNode> nodes,
                          llvm::ArrayRef<ReportedInput> deps,
                          std::int64_t started_at) {
    auto& files = file_table;
    llvm::SmallVector<VersionID> versions;
    for(std::size_t i = 0; i < paths.size(); i += 1) {
        auto fid = files.intern(Spelling::absolute(paths[i]));
        if(hashes[i] != 0) {
            files.disk.consumed(fid, hashes[i]);
        }
        versions.push_back(files.intern_version(fid, hashes[i]));
    }
    for(auto& node: nodes) {
        node.file = versions[node.file].raw;
    }
    // The unit's own text is no dependency of its compile, but the tree's
    // root.
    auto inputs = intern_inputs(files, deps, started_at);
    inputs.push_back({.file = request.key.first, .version = versions.back()});
    hosting.trees.commit(request.key,
                         {.root = versions.back(), .nodes = std::move(nodes)},
                         std::move(inputs));
}

void Project::commit_failed_tree(const TreeRequest& request) {
    Inputs inputs;
    if(auto unit = current_input(file_table, request.key.first)) {
        inputs.push_back(*unit);
    }
    hosting.trees.commit(request.key, {}, std::move(inputs));
}

void Project::take_tree_in_process(const TreeRequest& request) {
    auto rendered = render(request.command);
    auto unit = clice::compile({
        .kind = CompilationKind::Preprocess,
        .input = {.arguments = std::move(rendered.arguments),
                  .directory = std::move(rendered.directory),
                  .project_root = root.str(),
                  .vfs = llvm::makeIntrusiveRefCnt<vfs::View>(std::move(rendered.hidden))},
    });
    if(!unit->completed()) {
        commit_failed_tree(request);
        return;
    }
    auto tree = index::IncludeTree::from(*unit);
    commit_tree(request,
                tree.paths,
                tree.content_hashes,
                std::move(tree.nodes),
                unit->deps(),
                unit->started_at().count());
}

async::task<bool> Project::take_tree(TreeRequest request) {
    if(!pool) {
        take_tree_in_process(request);
        co_return true;
    }
    auto key = request.key;
    std::shared_ptr<async::event> done;
    if(auto it = tree_runs.find(key); it != tree_runs.end()) {
        done = it->second;
    } else {
        done = std::make_shared<async::event>(false);
        tree_runs.try_emplace(key, done);
        if(!tree_tasks.spawn(run_tree(*this, std::move(request), done))) {
            tree_runs.erase(key);
            co_return false;
        }
    }
    co_await done->wait();
    co_return hosting.trees.find(key) != nullptr;
}

}  // namespace clice
