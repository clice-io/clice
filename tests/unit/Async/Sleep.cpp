#include "Test/Test.h"
#include "Async/Async.h"

namespace clice::testing {
namespace {
TEST_SUITE(Async) {

TEST_CASE(Sleep) {
    int x = 1;
    auto task_gen = [&]() -> async::Task<> {
        x = 2;
        co_await async::sleep(100);
        x = 3;
    };

    auto task = task_gen();
    async::run(task);

    ASSERT_EQ(x, 3);
}

};  // TEST_SUITE(Async)
}  // namespace
}  // namespace clice::testing
