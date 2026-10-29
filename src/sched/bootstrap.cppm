module clice:sched.bootstrap;

import :project.load;
import :project.project;
import :sched.index.pump;
import :support.prelude;

namespace clice {

/// The one project loading sequence, shared by the server's initialize
/// and the batch driver so the two can never drift apart: load the
/// project (load_project), take its index load's report into the pump
/// and seed the indexing pass. See load_project for the flags.
ProjectLoad bootstrap_project(Project& project,
                              IndexPump& pump,
                              CanonicalRef root,
                              llvm::StringRef requested_configuration,
                              bool read_only_index = false);

}  // namespace clice
