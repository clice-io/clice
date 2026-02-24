#include <string_view>

#include "support/filesystem.h"
#include "eventide/zest/runner.h"

namespace {

std::string_view parse_filter(int argc, const char** argv) {
    constexpr std::string_view prefix = "--test-filter=";
    for(int i = 1; i < argc; ++i) {
        std::string_view arg = argv[i];
        if(arg.starts_with(prefix)) {
            return arg.substr(prefix.size());
        }

        if(arg == "--test-filter" && i + 1 < argc) {
            return argv[i + 1];
        }
    }

    return {};
}

}  // namespace

int main(int argc, const char** argv) {
    if(auto result = clice::fs::init_resource_dir(argv[0]); !result) {
        return 1;
    }
    return eventide::zest::Runner::instance().run_tests(parse_filter(argc, argv));
}
