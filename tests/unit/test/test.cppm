module;

#include <csignal>

#ifndef _WIN32
#include <fcntl.h>
#include <unistd.h>
#endif

#include "kota/zest/macro.h"
#include "llvm/Config/llvm-config.h"

export module clice.test;

import stdlib;
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
