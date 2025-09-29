#include "Server/Server.h"
#include "Test/Test.h"

namespace clice::testing {
namespace {

const std::string SIMPLE_CONFIG_FILE = "tests/data/simple.toml";

suite<"Config"> config = [] {
    test("Load simple.toml") = [&] {
        auto res = clice::config::load("clice", SIMPLE_CONFIG_FILE);
        expect(that % res.has_value() == true);
        expect(that % clice::config::binary == "clice");
        expect(that % clice::config::server.max_active_file == 8);

        expect(that % clice::config::cache.dir == "${workspace}/.clice/cache");
        expect(that % clice::config::cache.limit == 0);

        expect(that % clice::config::index.dir == "${workspace}/.clice/index");

        expect(that % clice::config::rules.size() == 1);

        const auto &rule = clice::config::rules[0];
        expect(that % rule.pattern == "**/*");
        expect(that % rule.append.size() == 1);
        expect(that % rule.append[0] == "-std=c++114514");
        expect(that % rule.remove.size() == 1);
        expect(that % rule.remove[0] == "-foo");
    };
};

} // namespace
} // namespace clice::testing
