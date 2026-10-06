module;

#include "modules/prelude.h"

#ifdef __linux__
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

module clice;

import :support.process;
import :tests.unit.test.test;

namespace clice::testing {
namespace {

ZEST_SUITE(Process) {

#ifdef __linux__
ZEST_CASE(LoweringStaysOnThread) {
    auto niceness = [] {
        return getpriority(PRIO_PROCESS, static_cast<id_t>(::syscall(SYS_gettid)));
    };
    auto before = niceness();
    int lowered = 0;
    std::thread thread([&] {
        lower_thread_priority();
        lowered = niceness();
    });
    thread.join();
    ZEXPECT(lowered == std::min(before + 10, 19));
    ZEXPECT(niceness() == before);
}
#endif

};  // ZEST_SUITE(Process)

}  // namespace
}  // namespace clice::testing
