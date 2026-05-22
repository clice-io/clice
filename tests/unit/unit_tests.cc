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

    DecoKVStyled(KVStyle::JoinedOrSeparate, meta_var = "<DIR>"; help = "test data directory";
                 required = false)
    <std::string> test_dir;

    DecoKVStyled(KVStyle::JoinedOrSeparate, meta_var = "<DIR>";
                 help = "corpus directory for snapshot glob tests";
                 required = false)
    <std::string> corpus_dir;
};

}  // namespace

int main(int argc, const char** argv) {
    auto args = kota::deco::util::argvify(argc, argv);
    auto parsed = kota::deco::cli::parse<TestOptions>(args);

    if(!parsed.has_value()) {
        return 1;
    }

    auto& opts = parsed->options;

    if(opts.test_dir.has_value())
        clice::testing::test_dir = *opts.test_dir;

    if(opts.corpus_dir.has_value())
        clice::testing::corpus_dir = *opts.corpus_dir;

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

    return kota::zest::run_tests(std::move(opts.zest));
}
