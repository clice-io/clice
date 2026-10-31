module clice;

import :driver.driver;
import :support.logging;
import :unittests.utils.platform;

namespace {

struct TestOptions {
    /// One test at a time unless asked: many start workers and compilers of
    /// their own, and running them side by side is not vetted for resource
    /// contention.
    kotatsu::zest::Options zest = [] {
        kotatsu::zest::Options options;
        options.jobs = 1u;
        return options;
    }();

    clice::driver::LogLevelOption log;
};

}  // namespace

extern "C++" int main(int argc, const char** argv) {
    auto args = kotatsu::deco::util::argvify(argc, argv);
    auto parsed = kotatsu::deco::cli::parse<TestOptions>(args);

    if(!parsed.has_value()) {
        return kotatsu::deco::cli::parse_error_exit_code;
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

    return kotatsu::zest::run_tests(std::move(opts.zest), argc, argv);
}
