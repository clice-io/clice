module clice:sched.batch;

import :config.config;
import :support.prelude;
import :worker.protocol;

namespace clice {

/// A snapshot of a batch index run's progress within the current round:
/// units finished (indexed, skipped as fresh, or failed) against the
/// round's total, and the files failed for good so far.
struct BatchProgress {
    std::size_t completed = 0;
    std::size_t total = 0;
    std::size_t failed = 0;
};

/// The project a batch run works on, as the command read it.
struct BatchProject {
    /// The project's root as the command line spells it; the result names
    /// files under this spelling.
    Spelling root;

    /// Its configuration (load_config), without errors.
    LoadedConfig config;

    /// Stateless worker count override; 0 runs one per logical core for
    /// indexing, the configuration's count for linting.
    std::uint32_t workers = 0;
};

struct BatchOptions {
    BatchProject project;

    /// Called on the event loop when a round begins or ends and every ten
    /// seconds in between; null for no progress reporting.
    llvm::function_ref<void(const BatchProgress&)> on_progress;
};

/// What a batch indexing run did, for the driver's report. The run's own
/// story goes to the log; this carries only what the CLI prints and the
/// exit code encodes.
struct BatchResult {
    int exit_code = 0;

    /// A signal interrupted the run; the saved progress (all of it unless
    /// `unsaved`) lets a rerun resume.
    bool interrupted = false;

    /// The run reached its final summary (early failures skip it).
    bool completed = false;

    std::size_t indexed_tus = 0;

    std::size_t shard_count = 0;
    std::uint64_t shard_bytes = 0;
    std::size_t symbol_count = 0;

    /// Paths of the units that failed for good, sorted.
    std::vector<std::string> failed;

    /// A unit whose worker ran out of memory with no other work beside it,
    /// at `memory` bytes: it does not fit this machine.
    struct TooBig {
        std::string unit;

        std::uint64_t memory = 0;
    };

    /// The units too big for this machine, sorted by path.
    std::vector<TooBig> too_big;

    /// A unit indexed although its compile reported errors.
    struct UnitErrors {
        std::string unit;
        std::vector<worker::LocatedMessage> errors;
    };

    /// The units indexed although their compile reported errors, sorted
    /// by path: their index holds what parsed.
    std::vector<UnitErrors> compiled_with_errors;

    /// The session log directory; empty when file logging is off.
    std::string log_dir;

    /// Index state remained that the final save could not commit; a rerun
    /// cannot resume from it.
    bool unsaved = false;

    /// The index database was damaged: the final save discarded it, and
    /// the next run rebuilds the index.
    bool damaged = false;

    double seconds = 0;
};

/// One-shot batch indexing on the lean scheduling stack — no sessions, no
/// transports: open the project, drain the pump, persist, and wind
/// down in contract-11 order. Runs its own event loop to completion.
/// Rounds start immediately and indexing happens even when the config
/// keeps the background index disabled — running the command is the
/// request itself.
BatchResult run_batch_index(const BatchOptions& options);

struct BatchLintOptions {
    BatchProject project;

    /// Also produce and persist the project index from the same parses.
    bool with_index = false;
};

struct BatchLintResult {
    /// 0 = clean, 1 = findings, 2 = some TUs failed to run or the
    /// requested index could not be persisted (dominates),
    /// 130 = interrupted.
    int exit_code = 0;

    bool interrupted = false;

    /// The run reached its final summary (early failures skip it).
    bool completed = false;

    std::size_t checked_tus = 0;
    std::size_t failed_tus = 0;

    /// The findings of the run, merged: a finding several TUs produced
    /// identically, notes included (a header's), appears once; sorted by
    /// file, line, column, check.
    std::vector<worker::TidyDiagnostic> findings;

    /// --index only: index state remained that the final save could not
    /// commit; a rerun cannot resume from it.
    bool unsaved = false;

    double seconds = 0;
};

struct BatchFormatOptions {
    /// The project's root as the command line spells it.
    Spelling root;

    /// Its configuration (load_config), without errors.
    LoadedConfig config;

    /// What to format instead of the build's own files, canonical and
    /// absolute: a directory narrows the set to the files under it, a
    /// file is formatted whether or not the build knows it.
    std::vector<Spelling> paths;

    /// Files formatted at once, each on a thread; 0 takes the hardware
    /// concurrency.
    std::uint32_t jobs = 0;

    /// Report the files formatting would change instead of rewriting
    /// them.
    bool check = false;
};

struct BatchFormatResult {
    /// 0 = every file formatted (or, with --check, nothing to change),
    /// 1 = --check found files to format, 2 = the run could not complete:
    /// a file's style could not be read, a file could not be read or
    /// written.
    int exit_code = 0;

    /// Files formatted or checked.
    std::size_t files = 0;

    /// --check: the files formatting would change, sorted.
    std::vector<std::string> unformatted;

    /// --check: where each of them needs formatting, for the user.
    std::string output;

    /// Why the run could not complete, for the user; empty otherwise.
    std::string error;

    double seconds = 0;
};

/// Format the project as the editor formats a document: load the build,
/// take its own files (the translation units and the project's files they
/// include, outside the build trees and the commands' system include
/// directories, no rule saying `format = false`), and format them in
/// process, `jobs` at a time.
BatchFormatResult run_batch_format(const BatchFormatOptions& options);

/// Lint the project through TURun {tidy} (or {index, tidy}): open it,
/// run every lintable CDB entry through the family under its .clang-tidy
/// configuration, and return the merged findings. The background pump
/// stays off during the pass — the plan's own runs are the only consumer
/// of the graph; with --index the pump then drains the reindex debt the
/// pass's merges booked before the final save.
BatchLintResult run_batch_lint(const BatchLintOptions& options);

}  // namespace clice
