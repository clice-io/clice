#include "sched/bootstrap.h"

#include "project/load.h"
#include "project/project.h"
#include "sched/index/pump.h"

namespace clice {

ProjectLoad bootstrap_project(Project& project,
                              IndexStore& store,
                              IndexPump& pump,
                              llvm::StringRef root,
                              llvm::StringRef requested_configuration,
                              bool read_only_index,
                              bool scan_tree) {
    auto load =
        load_project(project, store, root, requested_configuration, read_only_index, scan_tree);
    bool owed = !load.index.report.reindex().empty();
    pump.claim_report(load.index.report);

    if(project.config.project.enable_indexing.value && (!load.members.empty() || owed)) {
        for(auto member: load.members) {
            // Bulk sweep of unknown staleness: the hash gate decides per
            // file. DepsOnly — a cold start with a warm index cache must
            // keep serving the loaded shards, not blank every query until
            // the sweep drains.
            pump.enqueue(member, ReindexReason::DepsOnly);
        }
        pump.schedule();
    }
    return load;
}

}  // namespace clice
