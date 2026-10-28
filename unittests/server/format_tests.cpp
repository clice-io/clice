module;

#include "support/prelude.h"

module clice;

import :command.table;
import :server.format;
import :unittests.utils;

namespace clice::testing {

namespace {

ZEST_SUITE(Format) {

ZEST_CASE(UnmatchedContextWarns) {
    /// A header whose includer context could not be rebuilt says so at its
    /// top, naming the host.
    auto diagnostics = format_diagnostics(
        {
            .source = CommandSource::Inferred,
            .unmatched_host = "src/a.cpp",
        },
        {});
    ZASSERT(diagnostics.size() == 1u);
    ZEXPECT(diagnostics[0].code == "unmatched-includer-context");
    ZEXPECT(diagnostics[0].range.begin == 0u);
    ZEXPECT(format_diagnostics({.source = CommandSource::IncludeGraph}, {}).empty());
};

ZEST_CASE(QueryFailureWarns) {
    /// A compile whose compiler could not be queried says so at its top,
    /// with the query's error.
    auto diagnostics = format_diagnostics(
        {
            .source = CommandSource::CDBExact,
            .query_failure = "Driver riscv32-wch-elf-gcc not found or not executable",
        },
        {});
    ZASSERT(diagnostics.size() == 1u);
    ZEXPECT(diagnostics[0].code == "toolchain-query-failed");
    ZEXPECT(llvm::StringRef(diagnostics[0].message).contains("riscv32-wch-elf-gcc not found"));
    ZEXPECT(diagnostics[0].range.begin == 0u);

    /// A project that knows its compiler is not here hides it by its code.
    feature::DiagnosticsOptions options;
    options.suppress = {"toolchain-query-failed"};
    ZEXPECT(format_diagnostics({.query_failure = "Driver gcc not found or not executable"}, options)
                .empty());
};

};  // ZEST_SUITE(Format)

}  // namespace

}  // namespace clice::testing
