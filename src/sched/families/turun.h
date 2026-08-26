#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "compile/compilation.h"
#include "sched/context.h"
#include "sched/graph.h"
#include "sched/index/ledger.h"
#include "sched/index/store.h"
#include "sched/workspace.h"
#include "worker/pool.h"

#include "llvm/ADT/DenseMap.h"

namespace clice {

class PCMFamily;

/// The TURun family's id in the task graph. Node keys are TU path_ids
/// widened into NodeId::key.
constexpr inline std::uint8_t turun_family = 4;

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
/// the worker dispatch, and the store merge with its supersede and
/// landing-admission gates. The pump owns the debt ledger, the queue and
/// the requeue budget; it reads this family's per-attempt outcome to
/// settle them.
class TURunFamily {
public:
    TURunFamily(TaskGraph& graph,
                Workspace& workspace,
                ContextResolver& contexts,
                PCMFamily& pcm,
                IndexStore& store,
                WorkerPool& pool);

    /// Register the production runner. Tests that drive the pump against a
    /// synthetic runner register their own under turun_family instead.
    void register_runner();

    /// The frozen product plan of one run.
    struct Plan {
        bool index = false;
        bool tidy = false;

        /// Frozen tidy configuration; meaningful only with `tidy` set.
        tidy::TidyParams tidy_params;
    };

    enum class Verdict : std::uint8_t {
        /// The run produced its planned products (the index merged into
        /// the store, the tidy findings landed in the outcome).
        Completed,
        /// Deliberately produced nothing: the result was superseded or
        /// vetoed at landing, or the TU has no real command but keeps its
        /// last-known rows.
        Skipped,
        /// Terminal failure on current content: the worker rejected the
        /// TU, returned an empty or unverifiable result, the merge was
        /// rejected, or the TU has no real command and no surviving rows.
        Failed,
        /// The worker died mid-parse; requeue-worthy on the crash budget.
        Crashed,
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

        /// The landing-time admission verdict; Defer keeps the claimed
        /// debt for a later round.
        Admission landing = Admission::Admit;

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
    /// supersede check asks the live ledger, and the landing admission
    /// asks the serving side, both at merge time.
    struct Guards {
        std::function<bool()> superseded;
        std::function<Admission()> landing;
    };

    /// Run one attempt of the plan for the TU through its graph node and
    /// return the attempt's outcome. The node is re-marked dirty first: a
    /// request's existence means work is owed, and a clean node left by an
    /// earlier success would otherwise satisfy the join without running
    /// anything.
    kota::task<Outcome> run(std::uint32_t path_id, Plan plan, Guards guards = {});

private:
    kota::task<RoundOutcome> round(RoundContext& ctx, std::uint32_t path_id);

    static NodeId node(std::uint32_t path_id) {
        return {turun_family, path_id};
    }

    TaskGraph& graph;
    Workspace& workspace;
    ContextResolver& contexts;
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
    llvm::DenseMap<std::uint32_t, Inputs> inputs;
    llvm::DenseMap<std::uint32_t, Outcome> landed;
};

}  // namespace clice
