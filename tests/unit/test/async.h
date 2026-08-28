#pragma once

#include "test/test.h"

#include "kota/async/async.h"

namespace clice::testing {

/// Wait for event-loop cancellation cascades to settle before asserting state.
template <typename Pred>
kota::task<> settle(Pred pred) {
    for(int i = 0; i < 100 && !pred(); ++i) {
        co_await kota::sleep(1);
    }
    EXPECT_TRUE(pred());
}

}  // namespace clice::testing
