module clice;

import :server.master;
import :support.logging;
import :unittests.utils;

namespace clice::testing {
namespace {

ZEST_SUITE(NotifyLog) {

ZEST_CASE(BoundedRetention) {
    async::event_loop loop;
    MasterServer server(loop, "");

    // Keep the guidance gate open but silence stderr: the test fires well
    // over a hundred reports.
    auto saved_level = logging::options.stderr_level;
    logging::options.stderr_level = logging::Level::Off;

    for(int i = 0; i < 130; i++) {
        LOG_GUIDANCE("notify retention probe {}", i);
    }

    logging::options.stderr_level = saved_level;

    ZEXPECT(server.notify_seq == 130u);
    ZASSERT(server.notify_log.size() == 128u);
    // Drop-oldest: the first two messages were evicted, and the sequence
    // arithmetic keeps addressing the retained window.
    ZEXPECT(server.notify_log.front().text.ends_with("probe 2"));
    ZEXPECT(server.notify_log.back().text.ends_with("probe 129"));
    ZEXPECT(server.notify_seq - server.notify_log.size() == 2u);
}

};  // ZEST_SUITE(NotifyLog)

}  // namespace
}  // namespace clice::testing
