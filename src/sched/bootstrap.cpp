module;

#include "modules/prelude.h"

module clice;

import :project.command_resolver;
import :project.load;
import :project.project;
import :sched.bootstrap;
import :sched.index.pump;

namespace clice {

ProjectLoad bootstrap_project(Project& project,
                              CommandResolver& commands,
                              IndexPump& pump,
                              CanonicalRef root,
                              llvm::StringRef requested_configuration,
                              bool read_only_index) {
    auto load = load_project(project, commands, root, requested_configuration, read_only_index);
    bool owed = !load.index.report.reindex().empty();
    pump.apply_report(load.index.report);

    if(project.config.project.enable_indexing.value && (!load.members.empty() || owed)) {
        for(auto member: load.members) {
            // Bulk pass of unknown staleness: the hash gate decides per
            // file. DepsOnly — a cold start with a warm index cache must
            // keep serving the loaded shards, not blank every query until
            // the pass drains.
            pump.enqueue(member, ReindexReason::DepsOnly);
        }
        pump.schedule();
    }
    return load;
}

}  // namespace clice
