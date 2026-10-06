module;

#include "modules/prelude.h"

module clice:sched.bootstrap;

import :project.load;

namespace clice {

class IndexPump;

/// The one project loading sequence, shared by the server's initialize
/// and the batch driver so the two can never drift apart: load the
/// project (load_project), claim its index load's report into the pump
/// and seed the indexing sweep. See load_project for the flags.
ProjectLoad bootstrap_project(Project& project,
                              IndexStore& store,
                              IndexPump& pump,
                              CanonicalRef root,
                              llvm::StringRef requested_configuration,
                              bool read_only_index = false,
                              bool scan_tree = false);

}  // namespace clice
