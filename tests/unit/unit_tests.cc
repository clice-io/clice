#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>

#include "eventide/zest/zest.h"
#include "support/logging.h"

static std::string_view find_arg(int argc, const char** argv, const char* name) {
    for(int i = 1; i < argc; ++i) {
        // --name=value
        auto len = std::strlen(name);
        if(std::strncmp(argv[i], name, len) == 0 && argv[i][len] == '=') {
            return argv[i] + len + 1;
        }
        // --name value
        if(std::strcmp(argv[i], name) == 0 && i + 1 < argc) {
            return argv[i + 1];
        }
    }
    return {};
}

int main(int argc, const char** argv) {
    auto filter = find_arg(argc, argv, "--test-filter");
    auto log_level = find_arg(argc, argv, "--log-level");

    if(!log_level.empty()) {
        if(log_level == "trace") {
            clice::logging::options.level = clice::logging::Level::trace;
        } else if(log_level == "debug") {
            clice::logging::options.level = clice::logging::Level::debug;
        } else if(log_level == "info") {
            clice::logging::options.level = clice::logging::Level::info;
        } else if(log_level == "warn") {
            clice::logging::options.level = clice::logging::Level::warn;
        } else if(log_level == "err") {
            clice::logging::options.level = clice::logging::Level::err;
        }
    }

    clice::logging::stderr_logger("test", clice::logging::options);

    return eventide::zest::Runner::instance().run_tests(filter);
}
