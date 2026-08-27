#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "worker/protocol.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/STLFunctionalExtras.h"
#include "llvm/ADT/StringRef.h"

namespace clice {

struct BatchOptions {
    std::string root;

    /// Stateless worker count override; 0 keeps the config's values.
    std::uint32_t workers = 0;

    /// Path of the clice binary, for spawning workers.
    std::string self_path;
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
    std::size_t shard_count = 0;
    std::uint64_t shard_bytes = 0;
    std::size_t symbol_count = 0;
    std::size_t failed_files = 0;

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
    std::size_t findings = 0;

    /// --index only: index state remained that the final save could not
    /// commit; a rerun cannot resume from it.
    bool unsaved = false;

    double seconds = 0;
};

/// Lint the workspace through TURun {tidy} (or {index, tidy}): bootstrap,
/// run every CDB entry through the family under its .clang-tidy
/// configuration, and hand each TU's sorted findings to `on_findings` as
/// it lands. The background pump stays off during the sweep — the plan's
/// own runs are the only consumer of the graph; with --index the pump
/// then drains the reindex debt the sweep's merges booked before the
/// final save.
BatchLintResult run_batch_lint(
    const BatchLintOptions& options,
    llvm::function_ref<void(llvm::StringRef file, llvm::ArrayRef<worker::TidyDiagnostic>)>
        on_findings);

}  // namespace clice
