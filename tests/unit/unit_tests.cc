#include <string>
#include <string_view>

#include "eventide/deco/deco.h"
#include "eventide/zest/zest.h"
#include "support/logging.h"

namespace {

constexpr auto kv_style = static_cast<char>(deco::decl::KVStyle::Joined) |
                          static_cast<char>(deco::decl::KVStyle::Separate);

struct TestOptions {
    DecoKVStyled(kv_style, names = {"--test-filter", "--test-filter="};
                 help = "Filter tests by name";
                 required = false;)
    <std::string> test_filter;

    DecoKVStyled(kv_style, names = {"--log-level", "--log-level="};
                 help = "Log level: trace/debug/info/warn/err";
                 required = false;)
    <std::string> log_level;

    DecoKVStyled(kv_style, names = {"--test-dir", "--test-dir="}; help = "Test data directory";
                 required = false;)
    <std::string> test_dir;
};

}  // namespace

int main(int argc, const char** argv) {
    auto args = deco::util::argvify(argc, argv);
    auto parsed = deco::cli::parse<TestOptions>(args);

    std::string_view filter = {};
    if(parsed.has_value() && parsed->options.test_filter.has_value()) {
        filter = *parsed->options.test_filter;
    }

    if(parsed.has_value() && parsed->options.log_level.has_value()) {
        auto level = *parsed->options.log_level;
        if(level == "trace") {
            clice::logging::options.level = clice::logging::Level::trace;
        } else if(level == "debug") {
            clice::logging::options.level = clice::logging::Level::debug;
        } else if(level == "info") {
            clice::logging::options.level = clice::logging::Level::info;
        } else if(level == "warn") {
            clice::logging::options.level = clice::logging::Level::warn;
        } else if(level == "err") {
            clice::logging::options.level = clice::logging::Level::err;
        }
    }

    clice::logging::stderr_logger("test", clice::logging::options);

    return eventide::zest::Runner::instance().run_tests(filter);
}
