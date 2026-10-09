module;

#include "modules/prelude.h"

module clice:sched.families.turun;

import :compile.compilation;
import :project.command_resolver;
import :project.index_store;
import :project.project;
import :sched.graph;
import :sched.index.ledger;
import :worker.pool;

namespace clice {

class PCMFamily;

/// One-shot whole-TU runs as a task-graph family: one round = one parse on
/// a stateless worker serving the products of the frozen plan — the full
/// index merged into the IndexStore, a clang-tidy pass, or both from the
/// single parse. The pump requests {index}; the batch lint driver requests
/// {tidy} or {index, tidy}. A plan (products and their configuration) is
/// frozen at the request; today each process hosts one consumer, so a
/// late joiner with a different plan cannot arise — that protocol comes
/// with the first concurrent consumer (in-server background checks).
///
/// The family owns the run policy: command resolution, module-PCM edges,
/// the worker dispatch, and the store merge with its supersede gate. The
/// pump owns the debt ledger, the queue and the requeue budget; it reads
/// this family's per-attempt outcome to settle them.
class TURunFamily {
public:
    TURunFamily(TaskGraph& graph,
                Project& project,
                CommandResolver& commands,
                PCMFamily& pcm,
                IndexStore& store,
                WorkerPool& pool);

    /// Register the production runner. Tests that drive the pump against a
    /// synthetic runner register their own under Family::TURun instead.
    void register_runner();

    /// The frozen product plan of one run.
    struct Plan {
        bool index = false;

        /// A tidy pass under this frozen configuration.
        std::optional<tidy::TidyParams> tidy;
    };

    enum class Verdict : std::uint8_t {
        /// The run produced its planned products (the index merged into
        /// the store, the tidy findings landed in the outcome).
        Completed,
        /// Deliberately produced nothing: the result was superseded, or
        /// the TU has no real command but keeps its last-known rows.
        Skipped,
        /// Terminal failure on current content: the worker rejected the
        /// TU, returned an empty or unverifiable result, the merge was
        /// rejected, or the TU has no real command and no surviving rows.
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
        /// The graph refused or unwound the round (shutdown).
        Shutdown,
    };

    /// What one observed attempt did, combined from the join outcome and
    /// the round's recorded detail.
    struct Outcome {
        Verdict verdict = Verdict::Shutdown;

        /// Merge debt and serving-row changes — the pump claims these
        /// before the attempt settles and its waiters wake.
        IndexStore::Report report;

        /// Findings of the tidy pass (plan product `tidy`).
        std::vector<worker::TidyDiagnostic> tidy_diagnostics;

        /// Failure detail for the requester's logs.
        std::string error;

        struct Perf {
            std::size_t bytes = 0;
            long long index_ms = 0;
            long long merge_ms = 0;
        } perf;
    };

    /// Attempt context the pump threads through one run — work-input
    /// ownership (the debt-claim contract), not staleness snapshots: the
    /// supersede check asks the live ledger at merge time.
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
    kota::task<RoundOutcome> round(RoundContext& ctx, Fid path_id);

    static NodeId node(Fid path_id) {
        return {Family::TURun, path_id.raw};
    }

    TaskGraph& graph;
    Project& project;
    CommandResolver& commands;
    PCMFamily& pcm;
    IndexStore& store;
    WorkerPool& pool;

    struct Inputs {
        Plan plan;
        Guards guards;
    };

    /// The stash-and-collect halves of run() around the graph join: the
    /// plan and guards go in before the request, the landed outcome comes
    /// out after it. Each consumer runs one attempt per file at a time
    /// (the pump's ledger, the lint driver's sweep), so at most one live
    /// entry per TU.
    llvm::DenseMap<Fid, Inputs> inputs;
    llvm::DenseMap<Fid, Outcome> landed;

    /// TUs whose last result named a variant by hash that the store did
    /// not hold for its file: the next run sends every section's bytes,
    /// so the rerun cannot fail the same way.
    llvm::DenseSet<Fid> send_in_full;
};

}  // namespace clice
