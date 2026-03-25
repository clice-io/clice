#include <string>
#include <string_view>

#include "eventide/deco/deco.h"
#include "eventide/zest/zest.h"
#include "support/filesystem.h"

namespace {

struct TestOptions {
    DecoKV(names = {"--test-filter"}; help = "Filter tests by name"; required = false;)
    <std::string> test_filter;
};

}  // namespace

int main(int argc, const char** argv) {
    if(auto result = clice::fs::init_resource_dir(argv[0]); !result) {
        return 1;
    }

    auto args = deco::util::argvify(argc, argv);
    auto parsed = deco::cli::parse<TestOptions>(args);

    std::string_view filter = {};
    if(parsed.has_value() && parsed->options.test_filter.has_value()) {
        filter = *parsed->options.test_filter;
    }

    return eventide::zest::Runner::instance().run_tests(filter);
}
