#pragma once

#include <cstdint>
#include <functional>
#include <optional>

#include "sched/context.h"
#include "sched/graph.h"
#include "sched/workspace.h"
#include "worker/pool.h"

#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/xxhash.h"

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

    /// Re-validate on-disk PCM blobs and build the module dependencies of
    /// a request that compiles under `arguments` with `content` as the
    /// main file (the forwarder's per-request builds — the scan must see
    /// the buffer's imports under the request's command). Building a
    /// dependency can itself evict another clean module's PCM under
    /// budget pressure, which reopens the window the revalidation just
    /// closed — hence the bounded retry until the set is stable.
    kota::task<bool> prepare_deps(std::uint32_t path_id,
                                  llvm::ArrayRef<const char*> arguments,
                                  llvm::StringRef directory,
                                  std::optional<llvm::StringRef> content,
                                  bool foreground);

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

    /// A scan's module dependencies, split by what a consumer does with
    /// them: `resolved` names module units to wait on; `declared` is the
    /// full durable edge set — resolved units' nodes plus one sentinel
    /// per unresolved name.
    struct ModuleDeps {
        llvm::SmallVector<std::uint32_t> resolved;
        llvm::SmallVector<NodeId, 8> declared;
    };

    /// The graph identity of an import that resolves to nothing: a node
    /// that never runs a round and exists only to be edged at. When the
    /// name's first provider appears, provider_appeared() updates it and
    /// the ordinary cascade reaches every consumer whose scan declared
    /// the edge — no side bookkeeping of who failed against the name.
    /// The high bit keeps the key space disjoint from path_ids.
    static NodeId unresolved_node(llvm::StringRef name) {
        return {pcm_family, (1ull << 63) | (llvm::xxh3_64bits(name) >> 1)};
    }

    /// Whether a node is an unresolved-import sentinel.
    static bool is_unresolved(NodeId id) {
        return id.family == pcm_family && (id.key >> 63) != 0;
    }

    /// A module name gained a provider it lacked: void the sentinel's
    /// dependents (dropping dirtied units' cached PCM state on the way)
    /// and return them for serving-side treatment.
    llvm::SmallVector<NodeId> provider_appeared(llvm::StringRef name);

    /// Scan a file for its direct module dependencies (lazy, on every
    /// use — a re-resolve is inherent, so a CDB or import change is
    /// always seen by the next round). Consumers declare the full edge
    /// set and wait on the resolved subset. An engaged `content` scans
    /// it in place of the file's on-disk text — even when empty (an open
    /// buffer's imports count before they are saved, and an emptied
    /// buffer has none).
    ModuleDeps direct_deps(std::uint32_t path_id,
                           std::optional<llvm::StringRef> content = std::nullopt);

    /// The already-resolved-command flavor: scans under exactly the
    /// arguments the caller will compile with. The AST path uses it so a
    /// context choice or donated header host cannot diverge between the
    /// scan and the parse — the path_id flavor re-picks a CDB entry,
    /// which is only right for whole-TU runs on real commands.
    ModuleDeps direct_deps(std::uint32_t path_id,
                           llvm::ArrayRef<const char*> arguments,
                           llvm::StringRef directory,
                           std::optional<llvm::StringRef> content);

private:
    /// Commit the scan's full edge set as the unit's durable edges (see
    /// TaskGraph::declare).
    void declare_deps(std::uint32_t path_id, llvm::ArrayRef<NodeId> deps);

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
