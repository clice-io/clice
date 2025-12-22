#include "Test/Test.h"
#include "Async/Async.h"

namespace clice::testing {
namespace {
TEST_SUITE(Async) {

TEST_CASE(Event) {
    async::Event event;

    int x = 0;

    auto task1 = [&]() -> async::Task<> {
        EXPECT_EQ(x, 0);
        co_await event;
        EXPECT_EQ(x, 1);
        x = 2;
    };

    auto task2 = [&]() -> async::Task<> {
        EXPECT_EQ(x, 0);
        co_await event;
        EXPECT_EQ(x, 2);
        x = 3;
    };

    auto main = [&]() -> async::Task<> {
        x = 1;
        event.set();
        co_return;
    };

    async::run(task1(), task2(), main());
}

TEST_CASE(EventClear) {
    async::Event event;

    int x = 0;

    auto task1 = [&]() -> async::Task<> {
        EXPECT_EQ(x, 0);
        co_await event;
        EXPECT_EQ(x, 1);
        x = 2;
    };

    auto task2 = [&]() -> async::Task<> {
        EXPECT_EQ(x, 0);
        co_await event;
        EXPECT_EQ(x, 2);
        x = 3;
    };

    auto main = [&]() -> async::Task<> {
        x = 1;
        event.set();
        co_return;
    };

    async::run(task1(), task2(), main());
}

};  // TEST_SUITE(Async)
}  // namespace
}  // namespace clice::testing
