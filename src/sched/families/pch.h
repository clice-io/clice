#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "sched/context.h"
#include "sched/graph.h"
#include "sched/workspace.h"
#include "worker/pool.h"

#include "llvm/ADT/StringMap.h"

namespace clice {

/// The PCH family's id in the task graph. Node keys are family-interned
/// content-addressed pch_keys (monotonic, never recycled).
constexpr inline std::uint8_t pch_family = 2;

/// Preamble artifacts (a PCH and its pch.idx envelope, committed as one
/// pair) as a task-graph family: one node per content-addressed pch_key,
/// no edges, one round = one revalidation or build. The facade is the
/// only surface consumers touch.
///
/// The family owns the shared side of the policy: the pair-atomic store
/// commit and cache metadata (a round runs to a real reply, so no
/// cancellation can split blob from metadata), the shared-key crash
/// budget, and the cooperative response to advisory cancellation. The
/// adoption side stays with the server: which session points at the key
/// and per-document quarantine — crash evidence reaches it through the
/// dispatch owner's probe (see acquire).
class PCHFamily {
public:
    PCHFamily(TaskGraph& graph, Workspace& workspace, ContextResolver& contexts, WorkerPool& pool);

    /// Register the production runner. Tests that drive the facade
    /// against a synthetic build register their own runner under
    /// pch_family instead.
    void register_runner();

    /// One acquisition's request conditions: the artifact identity plus
    /// the inputs a build needs if this acquire ends up dispatching one.
    struct Request {
        std::string pch_key;
        std::string file;
        std::string directory;
        std::vector<std::string> arguments;
        /// The requesting document's buffer text; the preamble is its
        /// prefix and the build sends the whole buffer.
        std::string content;
        std::uint32_t preamble_bound = 0;
    };

    enum class Outcome : std::uint8_t {
        /// A fresh pair is registered under the key (revalidated or just
        /// built) — adopt it.
        Ready,
        Failed,
        /// The observed attempt ended without a verdict (advisory
        /// cancellation, shutdown): no adoption, but no failure verdict
        /// either.
        Preempted,
    };

    /// Join the key's round, spawning one when none is live. The spawning
    /// acquire is the dispatch owner: its inputs feed the build, and its
    /// `on_crash` probe receives every worker death of the round at crash
    /// time — exactly once across all joiners, surviving the owner's own
    /// request going stale or unwinding (contract 12: a stale round's
    /// crashes still count). A non-spawning acquire's probe is simply
    /// never installed, so deaths are never replayed per joiner. One
    /// acquire observes exactly one attempt; retry policy stays with the
    /// caller.
    kota::task<Outcome> acquire(Request request, std::function<void(llvm::StringRef)> on_crash);

    /// A complete, store-backed, deps-current pair is registered under
    /// the key. Non-const: a passing deps check may repair the snapshot's
    /// stat fast path in place.
    bool fresh(llvm::StringRef pch_key);

    /// A round for the key is in flight; its commit will republish over
    /// concurrent store retractions.
    bool building(llvm::StringRef pch_key) const;

    /// Retract a pair the frontend could not consume: remove both blobs
    /// from the store and drop the settled cache entry, so the next
    /// acquire misses and rebuilds instead of trusting corrupt bytes for
    /// the life of the store.
    void invalidate(llvm::StringRef pch_key);

private:
    /// One PCH round: revalidate the registered pair, or dispatch a build
    /// with the dispatch owner's inputs and commit the pair.
    kota::task<RoundOutcome> run(RoundContext& ctx, std::uint64_t key_id);

    std::uint64_t intern(llvm::StringRef pch_key);

    static NodeId node(std::uint64_t key_id) {
        return {pch_family, key_id};
    }

    /// Per-key slot for the dispatch owner's stash; both fields are
    /// replaced by the spawning acquire and consumed by its round.
    struct KeyState {
        Request inputs;
        std::function<void(llvm::StringRef)> on_crash;
    };

    TaskGraph& graph;
    Workspace& workspace;
    ContextResolver& contexts;
    WorkerPool& pool;

    llvm::StringMap<std::uint64_t> ids;
    std::vector<KeyState> states;
};

}  // namespace clice
