module clice;

import :support.logging;

extern "C++" int main(int argc, char** argv) {
    clice::logging::stderr_logger("test");
    // The workers tests spawn crash on `#pragma clang __debug crash`.
    kotatsu::sys::set_env("CLICE_TEST_PRAGMA_CRASH", "1");
    return kotatsu::zest::run_cli(argc, argv);
}
