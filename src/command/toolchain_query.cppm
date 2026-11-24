module clice:command.toolchain_query;

import :command.argument;
import :command.table;
import :command.toolchains.clang;
import :support.prelude;
import :support.process;

namespace clice {

/// One query request: everything query_one() needs, self-contained so warm()
/// can move it into a coroutine frame.
struct QuerySpec {
    /// Driver (+ subcommand) + non-user-content flags; no input among them.
    std::vector<const char*> argv;

    /// The command `argv` renders, which its table keeps while the query
    /// runs. Its compiler never runs when it does not query the toolchain:
    /// clang's own driver answers in its place.
    const Command* command = nullptr;

    /// Where the temp input is inserted (an index into argv).
    std::size_t slot = 0;

    /// The input language (InputKind value).
    std::string kind;

    /// Query working directory; empty inherits the process cwd.
    std::string cwd;
};

/// What the queries run together share: each one's bound, and the answers
/// cl and clang-cl gave. They are asked a command's `/std:` and driver mode
/// alone (`cl_question`), which a database's commands share whatever else
/// they say (Unreal's each name their module's `/imsvc` directories): a
/// question is asked once.
class QueryBatch {
public:
    QueryBatch();

    /// What the driver of `question`, a `cl_question`, run in `cwd`, answers
    /// over `input`, an empty file of the query's language it fills with
    /// `cl_query_source`; a query of the batch asking the same over a file of
    /// the same language waits for the answer its run gets instead.
    async::task<std::expected<ProgramOutput, std::string>> ask_cl(std::vector<std::string> question,
                                                                  llvm::StringRef input,
                                                                  std::string cwd);

    /// How long a query may run before it is stopped.
    const chrono::milliseconds bound;

private:
    struct Answer {
        async::event ready;

        std::expected<ProgramOutput, std::string> output;
    };

    llvm::StringMap<std::shared_ptr<Answer>> answers;
};

/// The job the driver runs for the query's command, over an empty file of
/// the query's language.
async::task<std::expected<Job, std::string>> query_one(const QuerySpec& spec, QueryBatch& batch);

/// Whether the query's driver is found where it would run.
bool driver_found(const QuerySpec& spec);

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
