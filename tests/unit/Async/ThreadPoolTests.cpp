#include "Test/Test.h"
#include "Async/Async.h"

namespace clice::testing {
namespace {
TEST_SUITE(Async) {

TEST_CASE(ThreadPool) {
    auto task_gen = []() -> async::Task<std::thread::id> {
        co_return co_await async::submit([]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            return std::this_thread::get_id();
        });
    };

    auto task1 = task_gen();
    auto task2 = task_gen();
    auto task3 = task_gen();

    task1.schedule();
    task2.schedule();
    task3.schedule();

    async::run();

    ASSERT_TRUE(task1.done());
    ASSERT_TRUE(task2.done());
    ASSERT_TRUE(task3.done());

    auto id1 = task1.result();
    auto id2 = task2.result();
    auto id3 = task3.result();

    ASSERT_NE(id1, id2);
    ASSERT_NE(id1, id3);
    ASSERT_NE(id2, id3);
}

};  // TEST_SUITE(Async)
}  // namespace
}  // namespace clice::testing
