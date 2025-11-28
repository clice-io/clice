#include "Test/Test.h"
#include "Async/Async.h"
#include "Support/FileSystem.h"

namespace clice::testing {
namespace {
TEST_SUITE(Async) {

TEST_CASE(FileRead) {
    auto path = fs::createTemporaryFile("prefix", "suffix");
    ASSERT_TRUE(path.has_value());

    auto result = fs::write(*path, "hello");
    ASSERT_TRUE(result.has_value());

    auto main = [&] -> async::Task<> {
        auto content = co_await async::fs::read(*path);
        CO_ASSERT_TRUE(content.has_value());
        CO_ASSERT_EQ(*content, std::string_view("hello"));
    };

    async::run(main());
}

TEST_CASE(FileWrite) {
    auto path = fs::createTemporaryFile("prefix", "suffix");
    ASSERT_TRUE(path.has_value());

    auto main = [&] -> async::Task<> {
        char buffer[] = "hello";

        auto result = co_await async::fs::write(*path, buffer, 5);
        CO_ASSERT_TRUE(result.has_value());
    };

    async::run(main());

    auto content = fs::read(*path);
    ASSERT_TRUE(content.has_value());
    ASSERT_EQ(*content, std::string_view("hello"));
}

};  // TEST_SUITE(Async)
}  // namespace
}  // namespace clice::testing
