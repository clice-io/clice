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

ZEST_CASE(ResidentPerGigabyte) {
    // One document per GB of the limit, 8 at least and 32 at most.
    constexpr std::uint64_t GB = 1'000'000'000;
    ZEXPECT(default_max_resident(0) == 8u);
    ZEXPECT(default_max_resident(8'200'000'000) == 8u);
    ZEXPECT(default_max_resident(16'600'000'000) == 16u);
    ZEXPECT(default_max_resident(20 * GB) == 20u);
    ZEXPECT(default_max_resident(64 * GB) == 32u);
}

};  // ZEST_SUITE(WorkerResources)

}  // namespace

}  // namespace clice::testing
