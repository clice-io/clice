#pragma once

#include <cstdint>
#include <functional>

#include "sched/context.h"
#include "sched/graph.h"
#include "sched/workspace.h"
#include "worker/pool.h"

#include "llvm/ADT/SmallVector.h"

namespace clice {

/// The PCM family's id in the task graph. Node keys are module-unit
/// path_ids widened into NodeId::key.
constexpr inline std::uint8_t pcm_family = 1;

/// C++20 module artifacts (PCM) as a task-graph family: one node per
/// module unit, edges to the modules it imports, one round = one PCM
/// build (or cache revalidation). The facade is the only surface
/// consumers touch — serve and batch never speak to the graph about PCM
/// nodes directly.
///
/// The family owns the PCM policy: cache key computation and hit checks,
/// store commits, the shared-artifact crash budget, and the cooperative
/// response to advisory cancellation (a voided round tells its worker to
/// stop and still reports the real outcome — contract 2). The graph owns
/// identity, edges, rounds and interest.
class PCMFamily {
public:
    PCMFamily(TaskGraph& graph, Workspace& workspace, ContextResolver& contexts, WorkerPool& pool);

    /// Register the production runner. Tests that drive the facade
    /// against a synthetic topology register their own runner under
    /// pcm_family instead.
    void register_runner();

    /// Build a module unit's PCM and all its transitive dependencies.
    /// `foreground` marks the chain's dispatches High-priority (a user
    /// request waits on them).
    kota::task<bool> build(std::uint32_t path_id, bool foreground = false);

    /// Build all transitive module dependencies of path_id, but NOT
    /// path_id itself — for plain TUs that import modules.
    kota::task<bool> build_deps(std::uint32_t path_id, bool foreground = false);

    /// Re-validate on-disk PCM blobs and build the module dependencies of
    /// `path_id`. Building dependencies can itself evict another clean
    /// module's PCM under budget pressure, which reopens the window the
    /// scan just closed — hence the bounded retry until the set is stable.
    kota::task<bool> prepare_deps(std::uint32_t path_id, bool foreground = false);

    /// One pass of the on-disk revalidation: LRU eviction can remove a
    /// blob while its node is still clean, so evicted units are
    /// invalidated instead of handing clang a dangling path. Returns
    /// whether anything was evicted.
    ///
    /// FIXME: this scans every pcm_paths entry (one stat() per module) on
    /// every compile, even in steady state when nothing was evicted. For
    /// large modular projects on NFS this adds measurable latency.
    /// Consider having CacheStore notify on eviction or caching the scan
    /// result.
    bool revalidate_blobs();

    /// Whether the graph has a node for this module unit (it was built or
    /// depended on before).
    bool tracks(std::uint32_t path_id) const;

    /// Mark a module unit and its transitive importers dirty, voiding
    /// in-flight rounds and dropping their cached PCM state — the single
    /// write point for PCM content invalidation. Artifact-only loss
    /// (cache eviction) goes through revalidate_blobs' graph.mark_dirty
    /// instead: no cascade, importers' results still describe unchanged
    /// content. Returns the dirtied path_ids.
    llvm::SmallVector<std::uint32_t> invalidate(std::uint32_t path_id);

    /// Invoked after a PCM lands so background indexing can pick up the
    /// new artifact.
    std::function<void()> on_indexing_needed;

    /// Scan a file for its direct module dependencies (lazy, on every
    /// use — a re-resolve is inherent, so a CDB or import change is
    /// always seen by the next round). Consumers that wait on the
    /// resulting PCM nodes through their own rounds (the AST family)
    /// resolve here and depend on {pcm_family, dep} directly. A non-empty
    /// `content` scans it in place of the file's on-disk text (an open
    /// buffer's imports count before they are saved).
    llvm::SmallVector<std::uint32_t> direct_deps(std::uint32_t path_id,
                                                 llvm::StringRef content = {});

    /// The already-resolved-command flavor: scans under exactly the
    /// arguments the caller will compile with. The AST path uses it so a
    /// context choice or donated header host cannot diverge between the
    /// scan and the parse — the path_id flavor re-picks a CDB entry,
    /// which is only right for whole-TU runs on real commands.
    llvm::SmallVector<std::uint32_t> direct_deps(std::uint32_t path_id,
                                                 llvm::ArrayRef<const char*> arguments,
                                                 llvm::StringRef directory,
                                                 llvm::StringRef content);

private:
    /// Commit the resolved imports as the unit's durable edges (see
    /// TaskGraph::declare).
    void declare_deps(std::uint32_t path_id, llvm::ArrayRef<std::uint32_t> deps);

    /// One PCM round: declare dependency edges, revalidate the cache, and
    /// dispatch the build.
    kota::task<RoundOutcome> run(RoundContext& ctx, std::uint32_t path_id);

    static NodeId node(std::uint32_t path_id) {
        return {pcm_family, path_id};
    }

    TaskGraph& graph;
    Workspace& workspace;
    ContextResolver& contexts;
    WorkerPool& pool;
};

}  // namespace clice
