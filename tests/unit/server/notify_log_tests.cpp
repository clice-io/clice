#include <string>

#include "test/test.h"
#include "server/transport/master_server.h"
#include "support/anomaly.h"
#include "support/logging.h"

#include "kota/async/async.h"

namespace clice::testing {
namespace {

TEST_SUITE(NotifyLog) {

TEST_CASE(BoundedRetention) {
    kota::event_loop loop;
    MasterServer server(loop, "clice-test");

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

    EXPECT_EQ(server.notify_seq, 130u);
    EXPECT_EQ(wakeups, 130u);
    ASSERT_EQ(server.notify_log.size(), 128u);
    // Drop-oldest: the first two messages were evicted, and the sequence
    // arithmetic keeps addressing the retained window.
    EXPECT_TRUE(server.notify_log.front().text.ends_with("probe 2"));
    EXPECT_TRUE(server.notify_log.back().text.ends_with("probe 129"));
    EXPECT_EQ(server.notify_seq - server.notify_log.size(), 2u);
}

};  // TEST_SUITE(NotifyLog)

}  // namespace
}  // namespace clice::testing
