#include "test/test.h"
#include "server/format.h"

namespace clice::testing {

namespace {

ZEST_SUITE(Format) {

ZEST_CASE(UnmatchedContextWarns) {
    /// A header whose includer context could not be rebuilt says so at its
    /// top, naming the host.
    auto diagnostics = format_diagnostics({
        .source = CommandSource::Inferred,
        .unmatched_host = "src/a.cpp",
    });
    ZASSERT(diagnostics.size() == 1u);
    auto* code = std::get_if<std::string>(&*diagnostics[0].code);
    ZASSERT(code != nullptr);
    ZEXPECT(*code == "unmatched-includer-context");
    ZEXPECT(diagnostics[0].range.start.line == 0u);
    ZEXPECT(format_diagnostics({.source = CommandSource::IncludeGraph}).empty());
};

};  // ZEST_SUITE(Format)

}  // namespace

}  // namespace clice::testing
