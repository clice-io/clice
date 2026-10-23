module;

#include "support/prelude.h"

module clice:unittests.utils.async;

import :unittests.utils;

namespace clice::testing {

/// Wait for event-loop cascades to finish before asserting state; the test
/// runner's timeout ends one that never does.
template <typename Pred>
kota::task<> wait_until(Pred pred) {
    while(!pred()) {
        // No event: the graph announces no change of interest.
        co_await kota::sleep(1);
    }
}

}  // namespace clice::testing
