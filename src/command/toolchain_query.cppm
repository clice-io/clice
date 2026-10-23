module;

#include "support/prelude.h"

module clice:command.toolchain_query;

import :command.argument;
import :command.toolchains.clang;

namespace clice {

/// One query request: everything query_one() needs, self-contained so warm()
/// can move it into a coroutine frame.
struct QuerySpec {
    /// Driver (+ subcommand) + non-user-content flags; no input among them.
    std::vector<const char*> argv;

    /// Where the temp input is inserted (an index into argv).
    std::size_t slot = 0;

    /// The input language (InputKind value).
    std::string kind;

    CompilerFamily family = CompilerFamily::Unknown;

    /// False: clang's own driver answers in place of the command's, which
    /// never runs.
    bool query_toolchain = true;

    /// Query working directory; empty inherits the process cwd.
    std::string cwd;
};

/// The job the driver runs for the query's command, over an empty file of
/// the query's language.
kota::task<std::expected<Job, std::string>> query_one(const QuerySpec& spec);

}  // namespace clice
