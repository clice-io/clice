module clice:sched.builders.turun;

import :compile.compilation;
import :feature.diagnostics;
import :project.command_resolver;
import :index.project_index;
import :project.project;
import :sched.builders.pch;
import :sched.builders.pcm;
import :sched.graph;
import :sched.index.pending_table;
import :support.prelude;
import :worker.pool;
import :worker.protocol;

namespace clice {

/// One-shot whole-TU runs as a task-graph family: one round = one parse on
/// a stateless worker serving the products of the frozen plan — the full
/// index merged into the project's index, a clang-tidy pass, or both from the
/// single parse. The pump requests {index}; the batch lint driver requests
/// {tidy} or {index, tidy}. A plan (products and their configuration) is
/// frozen at the request; today each process hosts one consumer, so a
/// late joiner with a different plan cannot arise — that protocol comes
/// with the first concurrent consumer (in-server background checks).
///
/// The family owns the run policy: command resolution, edges to the
/// module PCMs and to the precompiled header the build declares, the
/// worker dispatch, and the merge with its supersede gate. The
/// pump owns the debt table, the queue and the requeue budget; it reads
/// this family's per-attempt outcome to update them.
class TURunFamily {
public:
    TURunFamily(TaskGraph& graph,
                Project& project,
                CommandResolver& commands,
                PCMBuilder& pcm,
                PCHBuilder& pch,
                WorkerPool& pool);

    /// Register the production runner. Tests that drive the pump against a
    /// synthetic runner register their own under Family::TURun instead.
    void register_runner();

    /// The frozen product plan of one run.
    struct Plan {
        bool index = false;

        /// The errors the compile reports, in the outcome.
        bool errors = false;

        /// The diagnostics an editor skipping header bodies can get wrong,
        /// in the outcome: none but for a file of its own database entry,
        /// which the editor compiles as this run does.
        bool skipped_diagnostics = false;

        /// A tidy pass under this frozen configuration.
        std::optional<tidy::TidyParams> tidy;
    };

    enum class Result : std::uint8_t {
        /// The run produced its planned products (the index merged into
        /// the project index, the tidy findings arrived in the outcome).
        Completed,
        /// Deliberately produced nothing: the result was superseded, or
        /// the TU has no real command but keeps its last-known index.
        Skipped,
        /// Terminal failure on current content: the worker rejected the
        /// TU, returned an empty or unverifiable result, the merge was
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

    /// What one observed attempt did, combined from the join outcome and
    /// the round's recorded detail.
    struct Outcome {
        Result result = Result::Shutdown;

        /// Merge debt and serving-index changes — the pump takes these
        /// before the attempt finishes and its waiters wake.
        index::IndexReport report;

        /// Findings of the tidy pass (plan product `tidy`).
        std::vector<worker::TidyDiagnostic> tidy_diagnostics;

        /// Failure detail for the requester's logs.
        std::string error;

        /// OutOfMemory and TooBig: the memory the worker reached.
        std::uint64_t memory = 0;

        /// The errors a completed run compiled with (plan product
        /// `errors`).
        std::vector<worker::LocatedMessage> errors;

        /// See worker::TURunResult::skipped_diagnostics (plan product
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

    /// Attempt context the pump threads through one run — work-input
    /// ownership (the take/finish contract), not staleness snapshots: the
    /// supersede check asks the live pending table at merge time.
    struct Guards {
        std::function<bool()> superseded;
    };

    /// Run one attempt of the plan for the TU through its graph node and
    /// return the attempt's outcome. The node is re-marked dirty first: a
    /// request's existence means work is owed, and a clean node left by an
    /// earlier success would otherwise satisfy the join without running
    /// anything.
    kota::task<Outcome> run(Fid path_id, Plan plan, Guards guards = {});

private:
    struct Inputs {
        Plan plan;

        Guards guards;
    };

    kota::task<RoundOutcome> round(RoundContext& ctx, Fid path_id);

    static NodeId node(Fid path_id) {
        return {Family::TURun, path_id.raw};
    }

private:
    TaskGraph& graph;

    Project& project;

    CommandResolver& commands;

    PCMBuilder& pcm;

    PCHBuilder& pch;

    WorkerPool& pool;

    /// The stash-and-collect halves of run() around the graph join: the
    /// plan and guards go in before the request, the finished outcome comes
    /// out after it. Each consumer runs one attempt per file at a time
    /// (the pump's pending table, the lint driver's pass), so at most one live
    /// entry per TU.
    llvm::DenseMap<Fid, Inputs> inputs;

    llvm::DenseMap<Fid, Outcome> finished;

    /// TUs whose last result named a variant by hash that the project
    /// index did not hold for its file: the next run sends every section's bytes,
    /// so the rerun cannot fail the same way.
    llvm::DenseSet<Fid> send_in_full;

    /// What each TU's runs in this session measured: the peak of the last
    /// finished one, at least what a run killed for memory before it
    /// reached, and the memory one killed since reached.
    /// Ahead of the manifest's peak, which only an index that merged
    /// records.
    llvm::DenseMap<Fid, RequestCost> costs;
};

}  // namespace clice
