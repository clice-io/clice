#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "worker/protocol.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/STLFunctionalExtras.h"
#include "llvm/ADT/StringRef.h"

namespace clice {

/// A snapshot of a batch index run's progress within the current round:
/// units settled (indexed, skipped as fresh, or failed) against the
/// round's total, and the files failed for good so far.
struct BatchProgress {
    std::size_t completed = 0;
    std::size_t total = 0;
    std::size_t failed = 0;
};

struct BatchOptions {
    std::string root;

    /// The build configuration to activate (`--configuration`); empty
    /// takes the persisted selection, else the default.
    std::string configuration;

    /// Stateless worker count override; 0 keeps the config's values.
    std::uint32_t workers = 0;

    /// Path of the clice binary, for spawning workers.
    std::string self_path;

    /// Called on the event loop when a round begins or ends and every ten
    /// seconds in between; null for no progress reporting.
    llvm::function_ref<void(const BatchProgress&)> on_progress;
};

/// What a batch indexing run did, for the driver's report. The run's own
/// story goes to the log; this carries only what the CLI prints and the
/// exit code encodes.
struct BatchResult {
    int exit_code = 0;

    /// A signal interrupted the run; progress was saved and a rerun
    /// resumes from it.
    bool interrupted = false;

    /// The run reached its final summary (early failures skip it).
    bool completed = false;

    std::size_t indexed_tus = 0;

    /// Units in the index without a compile command of their own: headers
    /// indexed standalone under a borrowed host command, by this run or an
    /// earlier one.
    std::size_t standalone_headers = 0;
    std::size_t shard_count = 0;
    std::uint64_t shard_bytes = 0;
    std::size_t symbol_count = 0;

    /// Paths of the units that failed for good, sorted.
    std::vector<std::string> failed;

    /// The session log directory; empty when file logging is off.
    std::string log_dir;

    /// Index state remained that the final save could not commit; a rerun
    /// cannot resume from it.
    bool unsaved = false;

    double seconds = 0;
};

/// One-shot batch indexing on the lean scheduling stack — no sessions, no
/// transports: bootstrap the workspace, drain the pump, persist, and wind
/// down in contract-11 order. Runs its own event loop to completion.
/// Rounds start immediately and indexing happens even when the config
/// keeps the background index disabled — running the command is the
/// request itself.
BatchResult run_batch_index(const BatchOptions& options);

struct BatchLintOptions {
    std::string root;

    /// The build configuration to activate (`--configuration`); empty
    /// takes the persisted selection, else the default.
    std::string configuration;

    /// Stateless worker count override; 0 keeps the config's values.
    std::uint32_t workers = 0;

    /// Path of the clice binary, for spawning workers.
    std::string self_path;

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

    /// The findings of the run, merged: a finding several TUs produced (a
    /// header's) appears once, sorted by file, line, column, check.
    std::vector<worker::TidyDiagnostic> findings;

    /// --index only: index state remained that the final save could not
    /// commit; a rerun cannot resume from it.
    bool unsaved = false;

    double seconds = 0;
};

/// Lint the workspace through TURun {tidy} (or {index, tidy}): bootstrap,
/// run every lintable CDB entry through the family under its .clang-tidy
/// configuration, and return the merged findings. The background pump
/// stays off during the sweep — the plan's own runs are the only consumer
/// of the graph; with --index the pump then drains the reindex debt the
/// sweep's merges booked before the final save.
BatchLintResult run_batch_lint(const BatchLintOptions& options);

}  // namespace clice
