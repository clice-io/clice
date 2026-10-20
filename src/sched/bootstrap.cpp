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
    pump.apply_report(load.index.report);
    if(read_only_index) {
        return load;
    }

    // The scan finds the units depending on a file that changed while no
    // server ran, and marks what their runs walked behind the disk — for
    // every session indexing units, the background pass or not.
    auto stale = project.index.stale_units();
    if(project.config.project.enable_indexing.value) {
        // Those, and the units the index holds nothing of: DepsOnly — a cold
        // start with a warm index cache must keep serving the stored shards,
        // not blank every query until the pass drains. The units owed their
        // discarded variants read fresh by content: ContentChanged.
        for(auto member: load.members) {
            if(!project.index.indexed(member)) {
                pump.enqueue(member, ReindexReason::DepsOnly);
            }
        }
        for(auto unit: stale) {
            pump.enqueue(unit, ReindexReason::DepsOnly);
        }
        for(auto unit: project.index.owed_units()) {
            pump.enqueue(unit, ReindexReason::ContentChanged);
        }
        pump.schedule();
    }
    return load;
}

}  // namespace clice
