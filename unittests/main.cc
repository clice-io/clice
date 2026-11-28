module clice;

import :support.logging;

extern "C++" int main(int argc, char** argv) {
    clice::logging::stderr_logger("test");
    // The workers tests spawn crash on `#pragma clang __debug crash`.
    kotatsu::sys::set_env("CLICE_TEST_PRAGMA_CRASH", "1");
    // The PCHs and PCMs of a project a test opens go with its cache, not in
    // the user's cache directory.
    kotatsu::sys::set_env("CLICE_TEST_PRECOMPILED_DIR", "precompiled");
    return kotatsu::zest::run_cli(argc, argv);
}
