module;

#include "modules/prelude.h"

#include "support/anomaly.macros.h"

module clice;

import :server.master_server;
import :support.anomaly;
import :support.logging;
import :tests.unit.test.test;

namespace clice::testing {
namespace {

ZEST_SUITE(NotifyLog) {

ZEST_CASE(BoundedRetention) {
    kota::event_loop loop;
    MasterServer server(loop, "clice-test", "");

    // Keep the guidance gate open but silence the console sink: the test
    // fires well over a hundred reports.
    auto saved_level = spdlog::get_level();
    spdlog::set_level(spdlog::level::off);

    std::size_t wakeups = 0;
    auto conn = server.on_notify.connect([&] { wakeups += 1; });

    for(int i = 0; i < 130; i++) {
        LOG_GUIDANCE("notify retention probe {}", i);
    }

    spdlog::set_level(saved_level);

    ZEXPECT(server.notify_seq == 130u);
    ZEXPECT(wakeups == 130u);
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
