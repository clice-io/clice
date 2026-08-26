#pragma once

#include <cstdint>
#include <functional>
#include <string>

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

/// One-shot whole-TU runs as a task-graph family: today one round = one
/// background index parse dispatched to a stateless worker and merged into
/// the IndexStore. The product plan of a round is frozen at spawn; this
/// stage's plan is always {index} — the batch driver widens it to
/// {index, tidy} and becomes the second consumer, at which point a late
/// joiner whose plan the frozen one does not cover waits for the settle
/// and starts a fresh round.
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

    enum class Verdict : std::uint8_t {
        /// The worker's TUIndex merged into the store.
        Indexed,
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

        /// Failure detail for the pump's logs.
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

    /// Run one index attempt for the TU through its graph node and return
    /// the attempt's outcome. The node is re-marked dirty first: a claim's
    /// existence means work is owed, and a clean node left by an earlier
    /// success would otherwise satisfy the join without running anything.
    kota::task<Outcome> run_index(std::uint32_t path_id, Guards guards);

private:
    kota::task<RoundOutcome> run(RoundContext& ctx, std::uint32_t path_id);

    static NodeId node(std::uint32_t path_id) {
        return {turun_family, path_id};
    }

    TaskGraph& graph;
    Workspace& workspace;
    ContextResolver& contexts;
    PCMFamily& pcm;
    IndexStore& store;
    WorkerPool& pool;

    /// The stash-and-collect halves of run_index around the graph join:
    /// guards go in before the request, the landed outcome comes out after
    /// it. The ledger's single-flight-per-file discipline means at most
    /// one live entry per TU.
    llvm::DenseMap<std::uint32_t, Guards> inputs;
    llvm::DenseMap<std::uint32_t, Outcome> landed;
};

}  // namespace clice
