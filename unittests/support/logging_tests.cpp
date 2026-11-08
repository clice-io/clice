module;

#include "version.h"

module clice;

import :support.logging;
import :unittests.utils;

namespace clice::testing {
namespace {

ZEST_SUITE(Logging) {

ZEST_CASE(VersionInfoSet) {
    // Guards the cmake target-stamping plumbing: an unset CLICE_TARGET_STRING
    // would otherwise only show in crash logs.
    ZEXPECT(!clice::version.empty());
    ZEXPECT(!clice::target.empty());
}

};  // ZEST_SUITE(Logging)

}  // namespace
}  // namespace clice::testing
