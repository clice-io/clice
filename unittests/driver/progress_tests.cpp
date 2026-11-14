module clice;

import :driver.run;
import :unittests.utils;

namespace clice::testing {

namespace {

ZEST_SUITE(BatchProgress) {

ZEST_CASE(SlowUnitsNamed) {
    // A progress line names the units compiling for long, the longest
    // three of them.
    driver::BatchProgress progress;
    ZEXPECT(progress.slow_units().empty());
    progress.slow = {
        {"a.cpp", 95},
        {"b.cpp", 41},
        {"c.cpp", 33},
        {"d.cpp", 31},
    };
    ZEXPECT(progress.slow_units() == "; compiling 95s: a.cpp, 41s: b.cpp, 33s: c.cpp and 1 more");
}

};  // ZEST_SUITE(BatchProgress)

}  // namespace

}  // namespace clice::testing
