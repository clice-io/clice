#include "Test/Test.h"
#include "Async/Async.h"
#include "Async/ThreadPool.h"

namespace clice::testing {
namespace {
TEST_SUITE(Async) {

TEST_CASE(Run) {
    async::run();
}

TEST_CASE(TaskSchedule) {
    auto task_gen = []() -> async::Task<int> {
        co_return 1;
    };

    auto task = task_gen();
    task.schedule();

    async::run();

    ASSERT_TRUE(task.done());
    ASSERT_EQ(task.result(), 1);
}

TEST_CASE(TaskDispose) {
    static int x = 1;

    struct X {
        ~X() {
            x += 1;
        }
    };

    auto my_task = [&]() -> async::Task<> {
        X x;
        co_await async::sleep(300);
    };

    auto task = my_task();
    task.schedule();
    task.dispose();
    async::run();

    ASSERT_EQ(x, 2);

    auto main = [&]() -> async::Task<> {
        auto task = my_task();
        task.schedule();
        co_await async::sleep(100);
        task.cancel();
        task.dispose();
    };

    async::run(main());

    ASSERT_EQ(x, 3);
}

TEST_CASE(TaskCancel) {
    int x = 1;

    auto task1 = [&]() -> async::Task<> {
        x = 2;
        co_await async::sleep(300);
        x = 3;
    };

    auto main = [&]() -> async::Task<> {
        auto task = task1();
        task.schedule();
        co_await async::sleep(100);
        task.cancel();
        task.dispose();
    };

    async::run(main());

    ASSERT_EQ(x, 2);
}

TEST_CASE(TaskCancelRecursively) {
    int x = 0;
    int y = 0;
    int z = 0;

    auto task1 = [&]() -> async::Task<> {
        x = 1;
        co_await async::sleep(300);
        std::println("Task1 done");
        x = 2;
    };

    auto task2 = [&]() -> async::Task<> {
        auto task = task1();
        y = 1;
        co_await task;
        y = 2;
    };

    auto task3 = [&]() -> async::Task<> {
        auto task = task2();
        z = 1;
        co_await task;
        z = 2;
    };

    auto main = [&]() -> async::Task<> {
        auto task = task3();
        task.schedule();
        co_await async::sleep(100);
        task.cancel();
        task.dispose();
    };

    async::run(main());

    /// All tasks should be cancelled recursively.
    ASSERT_EQ(x, 1);
    ASSERT_EQ(y, 1);
    ASSERT_EQ(z, 1);
}

};  // TEST_SUITE(Async)
}  // namespace
}  // namespace clice::testing
