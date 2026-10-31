module clice:project.unit_run;

import :compile.tidy;
import :feature.diagnostics;
import :index.project_index;
import :project.graph;
import :support.prelude;
import :vfs.ids;
import :worker.protocol;

namespace clice {

struct Project;

/// The frozen product plan of one unit run: one parse of a unit on a
/// stateless worker serving the plan's products — the full index merged
/// into the project's index, a clang-tidy pass, or both from the single
/// parse. The background index plans {index}; `clice lint` plans {tidy} or
/// {index, tidy}. Each process hosts one consumer of a unit's runs, so a
/// late joiner with a different plan cannot arise — that protocol comes
/// with the first concurrent consumer (in-server background checks).
///
/// A unit run is a node of the task graph whose round is the run; the
/// project owns the run policy: command resolution, edges to the module
/// PCMs and to the precompiled header the build declares, the worker
/// dispatch, and the merge with its outdated check. The background index
/// owns the queue and the requeue budget; it reads the run's outcome to
/// update them.
struct UnitRunPlan {
    bool index = false;

    /// The errors the compile reports, in the outcome.
    bool errors = false;

    /// The diagnostics an editor skipping header bodies can get wrong, in
    /// the outcome: none but for a file of its own database entry, which
    /// the editor compiles as this run does.
    bool skipped_diagnostics = false;

    /// A tidy pass under this frozen configuration.
    std::optional<tidy::TidyParams> tidy;

    /// Whether the index the run built no longer describes the unit when it
    /// would merge (ReindexQueue::outdated): a newer content-level
    /// invalidation during the build, or a removal clearing the entry. The
    /// merge is dropped then; the follow-up slot redoes it. Null when every
    /// result merges.
    std::function<bool()> outdated;
};

/// What one observed unit run did.
struct UnitRunOutcome {
    enum class Result : std::uint8_t {
        /// The run produced its planned products (the index merged into the
        /// project index, the tidy findings arrived in the outcome).
        Completed,
        /// Deliberately produced nothing: the result is outdated, or the TU
        /// has no real command but keeps its last-known index.
        Skipped,
        /// Terminal failure on current content: the worker rejected the TU,
        /// returned an empty or unverifiable result, the merge was
        /// rejected, or the TU has no real command and no surviving index.
        Failed,
        /// The run killed its worker: the worker named it, or it ran past
        /// the pool's deadline. The same bytes would again.
        Crashed,
        /// The worker died under it for some other reason; requeue-worthy
        /// on the budget.
        Lost,
        /// Preempted (deliberate cancellation, or an outage the pool will
        /// revive from): budget-free requeue.
        Preempted,
        /// Its worker's memory ran out beside other work: budget-free
        /// requeue, its next run expected to take at least `memory`.
        OutOfMemory,
        /// Its worker's memory ran out with no other work beside it, at
        /// `memory`: the TU does not fit this machine, and the same bytes
        /// would not again.
        TooBig,
        /// The graph refused or unwound the round (shutdown).
        Shutdown,
    };

    Result result = Result::Shutdown;

    /// The reindexes the merge owes and the serving-index changes — the
    /// background index takes these before the attempt finishes and its
    /// waiters wake.
    index::IndexReport report;

    /// Findings of the tidy pass (plan product `tidy`).
    std::vector<worker::TidyDiagnostic> tidy_diagnostics;

    /// Failure detail for the requester's logs.
    std::string error;

    /// OutOfMemory and TooBig: the memory the worker reached.
    std::uint64_t memory = 0;

    /// The errors a completed run compiled with (plan product `errors`).
    std::vector<worker::LocatedMessage> errors;

    /// See worker::UnitRunResult::skipped_diagnostics (plan product
    /// `skipped_diagnostics`).
    std::vector<feature::FileDiagnostic> skipped_diagnostics;

    /// The hash of the text skipped_diagnostics describe.
    std::uint64_t text_hash = 0;

    struct Perf {
        std::size_t bytes = 0;

        long long index_ms = 0;

        long long merge_ms = 0;
    } perf;
};

/// Run one attempt of the plan for the unit through its graph node and
/// return the attempt's outcome. The node is re-marked dirty first: a
/// request's existence means work is owed, and a clean node left by an
/// earlier success would otherwise satisfy the join without running
/// anything. Each consumer runs one attempt per file at a time (the
/// background index's queue, the lint pass), so at most one plan per unit
/// waits in Project::unit_run_plans.
async::task<UnitRunOutcome> run_unit(Project& project, Fid path_id, UnitRunPlan plan);

/// One round of a unit-run node, the graph's runner for NodeKind::UnitRun.
async::task<RoundOutcome> unit_run_round(Project& project, RoundContext& ctx, NodeId id);

}  // namespace clice
