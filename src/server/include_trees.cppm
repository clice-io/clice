module clice:server.include_trees;

import :project.command_resolver;
import :project.hosting;
import :project.project;
import :support.prelude;
import :worker.pool;

namespace clice {

/// The include trees the editor's header resolution asks for, taken by
/// preprocess runs on the worker pool, one run per tree at a time. Each
/// result goes into hosting (commit_tree), a failed run included, so a
/// resolution asks for each tree once while what the run read holds.
class IncludeTrees {
public:
    IncludeTrees(Project& project, WorkerPool& pool) : project(project), pool(pool) {}

    /// `resolve` again until the host search it runs misses no include
    /// tree, taking each tree it misses first, each once. Stops with the
    /// tree missing when a run does not answer, or once `current` no
    /// longer holds.
    kota::task<Resolution> resolve(llvm::function_ref<Resolution()> resolve,
                                   llvm::function_ref<bool()> current = {});

    /// Cancel the runs in flight and wait for them to end.
    kota::task<> stop();

private:
    /// Take the tree `request` names, or wait for the run taking it. False
    /// when the run took none: the pool stopped, the run was cancelled, or
    /// the worker failed for a cause not the unit's.
    kota::task<bool> take(TreeRequest request);

    /// The run taking the tree, which belongs to no asker: one cancelled
    /// leaves it to the others waiting on `done`, set when it ends.
    kota::task<> run(TreeRequest request, std::shared_ptr<kota::event> done);

    Project& project;

    WorkerPool& pool;

    /// The runs in flight, by tree: a second asker waits for the first.
    llvm::DenseMap<UnitCommand, std::shared_ptr<kota::event>> running;

    kota::task_group<> runs;
};

}  // namespace clice
