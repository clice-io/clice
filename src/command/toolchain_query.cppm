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

/// A command as clang's own driver reads it in place of a compiler clice
/// does not run.
struct ClangReading {
    /// The driver command clang's driver is handed.
    std::vector<std::string> argv;

    /// The job it builds, an error when it builds none.
    std::expected<Job, std::string> job;
};

/// `args`, a driver command of `family` (its driver, two tokens for `zig
/// cc`, then its flags), read as a clang installed under the driver's name
/// would read it — the name's mode (`g++` compiles C++) and target prefix
/// (`arm-none-eabi-gcc`, when its architecture is one clang knows), clang's
/// defaults for the rest — with GCC's own spellings as clang takes them,
/// the warnings GCC, cl and nvcc make errors as warnings, and without the
/// CPU, tuning and ABI names clang's target does not know. Run
/// in `cwd`; what it reads otherwise than written, and what the driver
/// reports, is logged unless `quiet`.
ClangReading read_as_clang(std::vector<std::string> args,
                           CompilerFamily family,
                           llvm::StringRef cwd,
                           bool quiet = false);

}  // namespace clice
