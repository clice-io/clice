#include <cstdlib>
#include <string>
#include <string_view>

#include "test/platform.h"
#include "driver/driver.h"
#include "support/logging.h"

#include "kota/deco/deco.h"
#include "kota/zest/zest.h"

namespace {

struct TestOptions {
    /// One test at a time unless asked: many start workers and compilers of
    /// their own, and running them side by side is not vetted for resource
    /// contention.
    kota::zest::Options zest = [] {
        kota::zest::Options options;
        options.jobs = 1u;
        return options;
    }();

    clice::driver::LogLevelOption log;
};

}  // namespace

int main(int argc, const char** argv) {
    auto args = kota::deco::util::argvify(argc, argv);
    auto parsed = kota::deco::cli::parse<TestOptions>(args);

    if(!parsed.has_value()) {
        return kota::deco::cli::parse_error_exit_code;
    }

    auto& opts = parsed->options;

    opts.log.apply();
    clice::logging::stderr_logger("test", clice::logging::options);

    // The workers tests spawn crash on `#pragma clang __debug crash`.
#ifdef _WIN32
    _putenv_s("CLICE_TEST_PRAGMA_CRASH", "1");
#else
    setenv("CLICE_TEST_PRAGMA_CRASH", "1", 1);
#endif

    return kota::zest::run_tests(std::move(opts.zest), argc, argv);
}
