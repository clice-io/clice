#include <cstdlib>
#include <string>
#include <string_view>

#include "test/platform.h"
#include "support/logging.h"

#include "kota/deco/deco.h"
#include "kota/zest/zest.h"

namespace {

using kota::deco::decl::KVStyle;

struct TestOptions {
    kota::zest::Options zest;

    DecoKVStyled(KVStyle::JoinedOrSeparate, help = "log level: trace/debug/info/warn/err";
                 required = false)
    <std::string> log_level;
};

}  // namespace

int main(int argc, const char** argv) {
    auto args = kota::deco::util::argvify(argc, argv);
    auto parsed = kota::deco::cli::parse<TestOptions>(args);

    if(!parsed.has_value()) {
        return 1;
    }

    auto& opts = parsed->options;

    if(opts.log_level.has_value()) {
        auto level = *opts.log_level;
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

    // The workers tests spawn crash on `#pragma clang __debug crash`.
#ifdef _WIN32
    _putenv_s("CLICE_TEST_PRAGMA_CRASH", "1");
#else
    setenv("CLICE_TEST_PRAGMA_CRASH", "1", 1);
#endif

    return kota::zest::run_tests(std::move(opts.zest));
}
