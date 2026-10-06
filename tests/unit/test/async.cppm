module;

#include "modules/prelude.h"

module clice:tests.unit.test.async;

import :tests.unit.test.test;

namespace clice::testing {

/// Wait for event-loop cancellation cascades to settle before asserting state.
template <typename Pred>
kota::task<> settle(Pred pred) {
    for(int i = 0; i < 100 && !pred(); ++i) {
        co_await kota::sleep(1);
    }
    ZEXPECT(pred());
}

}  // namespace clice::testing
