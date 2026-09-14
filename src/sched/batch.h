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

    /// Check each declaration unit once across the run (the claim
    /// registry); off checks every TU whole.
    bool dedup = true;

    /// Also check every TU whole, without claims, and compare the merged
    /// findings of both — the deduplication's correctness check.
    bool verify = false;
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

    /// Declaration units every run that was granted them failed to check,
    /// even after the re-run; non-zero fails the run.
    std::size_t unchecked_units = 0;

    /// The merged findings of the run: identical findings from several
    /// TUs (a header's, a re-checked template's) appear once, sorted by
    /// file, line, column, check.
    std::vector<worker::TidyDiagnostic> findings;

    /// BatchLintOptions::verify: findings the whole runs produced that the
    /// deduplicated run lost, and the converse. Either non-empty fails the
    /// run.
    std::vector<worker::TidyDiagnostic> verify_missing;
    std::vector<worker::TidyDiagnostic> verify_extra;

    /// --index only: index state remained that the final save could not
    /// commit; a rerun cannot resume from it.
    bool unsaved = false;

    double seconds = 0;
};

/// Lint the workspace through TURun {tidy} (or {index, tidy}): bootstrap,
/// sweep every lintable TU through the pool under its frozen .clang-tidy
/// configuration, and return the merged findings.
BatchLintResult run_batch_lint(const BatchLintOptions& options);

}  // namespace clice
