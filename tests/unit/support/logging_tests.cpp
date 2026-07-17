#include "test/test.h"
#include "support/logging.h"

namespace clice::testing {
namespace {

TEST_SUITE(Logging) {

TEST_CASE(MainExecutableBase) {
    // Linux relies on the binary being PIE — a non-PIE image has bias 0,
    // which would silently void the crash-log rebase contract; Windows
    // always maps the image at a nonzero base. The macOS slide may
    // legitimately be zero, so only availability is exercised there.
    [[maybe_unused]] auto base = logging::main_executable_base();
#if !defined(__APPLE__)
    EXPECT_NE(base, 0u);
#endif
}

};  // TEST_SUITE(Logging)

}  // namespace
}  // namespace clice::testing
