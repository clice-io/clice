module clice:worker.stateful;

import :support.prelude;

namespace clice {

/// Run the stateful worker process mode.
/// The worker holds compiled ASTs and handles feature requests
/// (hover, semantic tokens, etc.) alongside compile requests; which ASTs
/// it keeps is the master's pool's decision.
int run_stateful_worker_mode();

}  // namespace clice
