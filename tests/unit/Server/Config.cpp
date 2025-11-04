#include "Test/Test.h"
#include "Server/Server.h"

#include <string>

namespace clice::testing {
namespace {

const std::string NAIVE_WORKSPACE = "tests/data/config/naive";

suite<"Config"> config = [] {
    test("Load naive workspace config") = [&] {
        clice::config::Config conf;
        const auto res = conf.parse(NAIVE_WORKSPACE);

        expect(that % res.has_value() == true);
        expect(that % conf.workspace == "clice");

        const auto &proj = conf.project;

        expect(that % proj.cache_dir == "${workspace}/.clice/naive_cache");
        expect(that % proj.index_dir == "${workspace}/.clice/naive_index");

        expect(that % conf.rules.size() == 3);
        const auto &str_rule = conf.rules[0];
        const auto &arr_rule = conf.rules[1];
        const auto &empty_rule = conf.rules[2];

        expect(that % str_rule.patterns.size() == 1);
        expect(that % str_rule.patterns[0] == "**/*.cc");
        expect(that % str_rule.append.size() == 1);
        expect(that % str_rule.append[0] == "-std=c++114514");
        expect(that % str_rule.remove.size() == 1);
        expect(that % str_rule.remove[0] == "-foo");

        expect(that % arr_rule.patterns.size() == 2);
        expect(that % arr_rule.patterns[0] == "../*.ts");
        expect(that % arr_rule.patterns[1] == "../*.tsx");
        expect(that % arr_rule.append.size() == 2);
        expect(that % arr_rule.append[0] == "--target=es6");
        expect(that % arr_rule.append[1] == "--tell-agent='我操大哥你别用 any 啊'");
        expect(that % arr_rule.remove.size() == 2);
        expect(that % arr_rule.remove[0] == "--target=es5");
        expect(that % arr_rule.remove[1] == "--allow-js");

        expect(that % empty_rule.patterns.empty());
        expect(that % empty_rule.append.empty());
        expect(that % empty_rule.remove.empty());
    };
};

} // namespace
} // namespace clice::testing
