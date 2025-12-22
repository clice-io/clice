#include "Test/Test.h"
#include "Async/Async.h"

namespace clice::testing {
namespace {
TEST_SUITE(Async) {

TEST_CASE(Lock) {
    async::Lock lock;

    int x = 0;

    auto task1 = [&]() -> async::Task<> {
        auto guard = co_await lock.try_lock();
        co_await async::sleep(5);
        CO_ASSERT_EQ(x, 0);
        co_await async::sleep(10);
        CO_ASSERT_EQ(x, 0);
        co_await async::sleep(5);
        x = 1;
    };

    auto task2 = [&]() -> async::Task<> {
        auto guard = co_await lock.try_lock();
        co_await async::sleep(5);
        CO_ASSERT_EQ(x, 1);
        co_await async::sleep(5);
        CO_ASSERT_EQ(x, 1);
        co_await async::sleep(10);
        x = 2;
    };

    auto task3 = [&]() -> async::Task<> {
        auto guard = co_await lock.try_lock();
        co_await async::sleep(10);
        CO_ASSERT_EQ(x, 2);
        co_await async::sleep(5);
        CO_ASSERT_EQ(x, 2);
        co_await async::sleep(5);
    };

    async::run(task1(), task2(), task3());
}

TEST_CASE(LockCancel) {
    async::Lock lock;

    int x = 0;
    int y = 0;

    auto task = [&]() -> async::Task<> {
        x += 1;
        auto guard = co_await lock.try_lock();
        co_await async::sleep(100);
        y += 1;
    };

    auto task1 = task();
    auto task2 = task();
    auto task3 = task();

    auto cancel = [&task2]() -> async::Task<> {
        co_await async::sleep(10);
        task2.cancel();
        task2.dispose();
    };

    task1.schedule();
    task2.schedule();
    task3.schedule();

    async::run(cancel());

    ASSERT_EQ(x, 3);
    ASSERT_EQ(y, 2);
}

};  // TEST_SUITE(Async)
}  // namespace
}  // namespace clice::testing
