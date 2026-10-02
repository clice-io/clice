#include <string>

#include "test/test.h"
#include "server/master_server.h"
#include "support/anomaly.h"
#include "support/logging.h"

#include "kota/async/async.h"

namespace clice::testing {
namespace {

ZEST_SUITE(NotifyLog){

    ZEST_CASE(BoundedRetention){kota::event_loop loop;
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

EXPECT(server.notify_seq == 130u);
EXPECT(wakeups == 130u);
ASSERT(server.notify_log.size() == 128u);
// Drop-oldest: the first two messages were evicted, and the sequence
// arithmetic keeps addressing the retained window.
EXPECT(server.notify_log.front().text.ends_with("probe 2"));
EXPECT(server.notify_log.back().text.ends_with("probe 129"));
EXPECT(server.notify_seq - server.notify_log.size() == 2u);

}  // namespace

};  // namespace clice::testing

}  // namespace
}  // namespace clice::testing
