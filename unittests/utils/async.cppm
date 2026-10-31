module clice:unittests.utils.async;

import :support.prelude;
import :unittests.utils;

namespace clice::testing {

/// Wait for event-loop cascades to finish before asserting state; the test
/// runner's timeout ends one that never does.
template <typename Pred>
async::task<> wait_until(Pred pred) {
    while(!pred()) {
        // No event: the graph announces no change of interest.
        co_await async::sleep(1);
    }
}

}  // namespace clice::testing
