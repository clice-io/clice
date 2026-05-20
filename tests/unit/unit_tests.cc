#include <string>
#include <string_view>

#include "test/platform.h"
#include "support/logging.h"

#include "kota/deco/deco.h"
#include "kota/zest/zest.h"

namespace {

using kota::deco::decl::KVStyle;

struct TestOptions {
    DecoKV(style = KVStyle::JoinedOrSeparate,
           names = {"--test-filter", "--test-filter="},
           help = "Filter tests by name",
           required = false)
    <std::string> test_filter;

    DecoKV(style = KVStyle::JoinedOrSeparate,
           names = {"--log-level", "--log-level="},
           help = "Log level: trace/debug/info/warn/err",
           required = false)
    <std::string> log_level;

    DecoKV(style = KVStyle::JoinedOrSeparate,
           names = {"--test-dir", "--test-dir="},
           help = "Test data directory",
           required = false)
    <std::string> test_dir;

    DecoKV(style = KVStyle::JoinedOrSeparate,
           names = {"--snapshot-dir", "--snapshot-dir="},
           help = "Snapshot directory for snapshot tests",
           required = false)
    <std::string> snapshot_dir;

    DecoFlag(names = {"--update-snapshots"},
             help = "Create/overwrite snapshot files instead of comparing",
             required = false)
    update_snapshots;
};

}  // namespace

int main(int argc, const char** argv) {
    auto args = kota::deco::util::argvify(argc, argv);
    auto parsed = kota::deco::cli::parse<TestOptions>(args);

    kota::zest::RunnerOptions options;

    if(parsed.has_value()) {
        if(parsed->options.test_filter.has_value())
            options.filter = *parsed->options.test_filter;

        if(parsed->options.test_dir.has_value())
            clice::testing::test_dir = *parsed->options.test_dir;

        if(parsed->options.snapshot_dir.has_value())
            options.snapshot_dir = *parsed->options.snapshot_dir;

        options.update_snapshots = parsed->options.update_snapshots.value_or(false);

        if(parsed->options.log_level.has_value()) {
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
    }

    clice::logging::stderr_logger("test", clice::logging::options);

    return kota::zest::Runner::instance().run_tests(options);
}
