module;

#include "support/prelude.h"

module clice;

import :unittests.utils;
import :worker.resources;

namespace clice::testing {

namespace {

ZEST_SUITE(WorkerResources) {

ZEST_CASE(MachineMemory) {
    auto memory = memory_state({});
    ZEXPECT(memory.limit > 0u);
    ZEXPECT(memory.headroom <= memory.limit);
    ZEXPECT(memory.reserve >= 512ull << 20);
}

};  // ZEST_SUITE(WorkerResources)

}  // namespace

}  // namespace clice::testing
