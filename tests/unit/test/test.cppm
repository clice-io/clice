module;

#include <csignal>

#ifndef _WIN32
#include <fcntl.h>
#include <unistd.h>
#endif

#include "kota/meta/compare.h"
#include "kota/zest/zest.h"
#include "llvm/Config/llvm-config.h"

export module clice.test;

// Re-exported: every test TU is a non-module unit that imports clice.test, and
// the zest EXPECT_*/ASSERT_* expansions name std::source_location, std::bool_constant
// and friends. std is the harness's universal surface, so it travels with it.
export import stdlib;
import llvm;
import clang;
import kota;
import clice.support;
import clice.command;
import clice.syntax;
import clice.semantic;
import clice.visit;
import clice.compile;
import clice.feature;
import clice.index;
import clice.protocol;
import clice.worker;
import clice.server;

// zest's declarations reach the test TUs through import clice.test (the macros
// themselves are force-included per the CMake ZEST prelude). The comparison
// helpers kota::meta::{eq,ne,lt,le,gt,ge} are named by EXPECT_*/ASSERT_* macro
// expansions, so they travel with zest rather than through kota.
export namespace kota::zest {

using ::kota::zest::Options;
using ::kota::zest::TestAttrs;
using ::kota::zest::TestSuiteDef;
using ::kota::zest::check_binary_failure;
using ::kota::zest::check_snapshot;
using ::kota::zest::check_snapshot_expr;
using ::kota::zest::check_snapshot_glob;
using ::kota::zest::check_type_eq_failure;
using ::kota::zest::check_unary_failure;
using ::kota::zest::failure;
using ::kota::zest::parse_binary_exprs;
using ::kota::zest::print_trace;
using ::kota::zest::run_tests;

}  // namespace kota::zest

export namespace kota::meta {

using ::kota::meta::eq;
using ::kota::meta::ge;
using ::kota::meta::gt;
using ::kota::meta::le;
using ::kota::meta::lt;
using ::kota::meta::ne;

}  // namespace kota::meta

// The harness headers reference names (std/llvm/clang/clice) from the enclosing
// module purview above and are consumed by non-module test TUs through
// `import clice.test;`; export their declarations so importers can see them.
export {
#include "test/annotation.h"
#include "test/cdb_helper.h"
#include "test/fixture.h"
#include "test/platform.h"
#include "test/snap_region.h"
#include "test/temp_dir.h"
#include "test/tester.h"
#include "server/worker_test_helpers.h"
#include "syntax/module_scan_fixture.h"
}
