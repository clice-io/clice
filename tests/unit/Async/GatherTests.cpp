#include <vector>

#include "Test/Test.h"
#include "Async/Async.h"

namespace clice::testing {
namespace {
TEST_SUITE(Async) {

TEST_CASE(GatherPack) {
    int x = 0;

    auto task_gen = [&]() -> async::Task<int> {
        co_await async::sleep(100);
        x += 1;
        co_return x;
    };

    auto [a, b, c] = async::run(task_gen(), task_gen(), task_gen());

    ASSERT_EQ(a, 1);
    ASSERT_EQ(b, 2);
    ASSERT_EQ(c, 3);
}

TEST_CASE(GatherRange) {
    std::vector<int> args;
    for(int i = 0; i < 30; ++i) {
        args.push_back(i);
    }

    std::vector<int> results;

    auto task_gen = [&](int x) -> async::Task<bool> {
        co_await async::sleep(10);
        results.push_back(x);
        co_return true;
    };

    auto core = async::gather(args, task_gen);
    async::run(core);

    ASSERT_EQ(args, results);
    ASSERT_EQ(core.result(), true);
}

TEST_CASE(GatherCancel) {
    std::vector<int> args;
    for(int i = 0; i < 30; ++i) {
        args.push_back(i);
    }

    std::vector<int> results;

    auto task_gen = [&](int x) -> async::Task<bool> {
        co_await async::sleep(10);
        results.push_back(x);
        co_return false;
    };

    auto core = async::gather(args, task_gen);
    async::run(core);

    ASSERT_EQ(results.size(), 1U);
    ASSERT_EQ(core.result(), false);
}

};  // TEST_SUITE(Async)
}  // namespace
}  // namespace clice::testing
