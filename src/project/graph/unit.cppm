module clice:project.unit;

import :compile.tidy;
import :feature.diagnostics;
import :index.project_index;
import :project.task_graph;
import :support.prelude;
import :vfs.ids;
import :worker.protocol;

namespace clice {

/// What one compile of a unit on a stateless worker serves: the full
/// index merged into the project's index, a clang-tidy pass, or both from
/// the single parse. The background index asks for {index}; `clice lint`
/// for {tidy} or {index, tidy}. Each process hosts one consumer of a unit's
/// compiles, so a late joiner asking for something else cannot arise — that
/// protocol comes with the first concurrent consumer (in-server background
/// checks).
///
/// A unit is a node of the task graph whose attempt is the compile; the
/// project owns its policy: command resolution, edges to the module PCMs
/// and to the precompiled header the build declares, the worker dispatch,
/// and the merge with its outdated check. The background index owns the
/// queue and the requeue budget; it reads the compile's result to update
/// them.
struct UnitRequest {
    bool index = false;

    /// The errors the compile reports, in the result.
    bool errors = false;

    /// The diagnostics an editor skipping header bodies can get wrong, in
    /// the result: none but for a file of its own database entry, which
    /// the editor compiles as this compile does.
    bool skipped_diagnostics = false;

    /// A tidy pass under this frozen configuration.
    std::optional<tidy::TidyParams> tidy;

    /// Whether the index the compile built no longer describes the unit when
    /// it would merge (ReindexQueue::outdated): a newer content-level
    /// invalidation during the build, or a removal clearing the entry. The
    /// merge is dropped then; the follow-up slot redoes it. Null when every
    /// result merges.
    std::function<bool()> outdated;
};

/// What one compile of a unit did.
struct UnitResult {
    enum class Status : std::uint8_t {
        /// The compile produced what was asked (the index merged into the
        /// project index, the tidy findings arrived in the result).
        Completed,
        /// Deliberately produced nothing: the result is outdated, or the TU
        /// has no real command but keeps its last-known index.
        Skipped,
        /// Terminal failure on current content: the worker rejected the TU,
        /// returned an empty or unverifiable result, the merge was
        /// rejected, or the TU has no real command and no surviving index.
        Failed,
        /// The compile killed its worker: the worker named it, or it ran
        /// past the pool's deadline. The same bytes would again.
        Crashed,
        /// The worker died under it for some other reason; requeue-worthy
        /// on the budget.
        Lost,
        /// Preempted (deliberate cancellation, or an outage the pool will
        /// revive from): budget-free requeue.
        Preempted,
        /// Its worker's memory ran out beside other work: budget-free
        /// requeue, its next compile expected to take at least `memory`.
        OutOfMemory,
        /// Its worker's memory ran out with no other work beside it, at
        /// `memory`: the TU does not fit this machine, and the same bytes
        /// would not again.
        TooBig,
    };

    Status status = Status::Failed;

    /// The reindexes the merge owes and the serving-index changes — the
    /// background index takes these before the attempt finishes and its
    /// waiters wake.
    index::IndexReport report;

    /// Findings of the tidy pass (UnitRequest::tidy).
    std::vector<worker::TidyDiagnostic> tidy_diagnostics;

    /// Failure detail for the requester's logs.
    std::string error;

    /// OutOfMemory and TooBig: the memory the worker reached.
    std::uint64_t memory = 0;

    /// The errors a completed compile reported (UnitRequest::errors).
    std::vector<worker::LocatedMessage> errors;

    /// See worker::UnitRunResult::skipped_diagnostics
    /// (UnitRequest::skipped_diagnostics).
    std::vector<feature::FileDiagnostic> skipped_diagnostics;

    /// The hash of the text skipped_diagnostics describe.
    std::uint64_t text_hash = 0;

    struct Perf {
        std::size_t bytes = 0;

        long long index_ms = 0;

        long long merge_ms = 0;
    } perf;
};

}  // namespace clice
