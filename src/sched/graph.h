#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>

#include "kota/async/async.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseMapInfo.h"
#include "llvm/ADT/SmallVector.h"

namespace clice {

/// Identity of a task-graph node. Each family interns its own keys —
/// monotonically, never recycling: the graph may retain an id in edges or
/// rounds long after the family dropped the underlying artifact. Family
/// 0xFF is reserved for map sentinels.
struct NodeId {
    std::uint8_t family = 0;
    std::uint64_t key = 0;

    friend bool operator==(const NodeId&, const NodeId&) = default;
};

}  // namespace clice

template <>
struct llvm::DenseMapInfo<clice::NodeId> {
    static clice::NodeId getEmptyKey() {
        return {0xFF, 0};
    }

    static clice::NodeId getTombstoneKey() {
        return {0xFF, 1};
    }

    static unsigned getHashValue(const clice::NodeId& id) {
        return llvm::detail::combineHashValue(
            id.family,
            llvm::DenseMapInfo<std::uint64_t>::getHashValue(id.key));
    }

    static bool isEqual(const clice::NodeId& lhs, const clice::NodeId& rhs) {
        return lhs == rhs;
    }
};

namespace clice {

/// Result of one round, as reported by the family closure and then filtered
/// through round identity: a round that update() overtook mid-flight is
/// recorded Stale whatever it reported — its verdict is about content that
/// no longer exists.
enum class RoundOutcome : std::uint8_t {
    /// No verdict: the round was overtaken (content update, loss of
    /// interest, scheduler preemption). Waiters that still hold interest
    /// drive a fresh round. The closure may have published salvage results
    /// before reporting Stale; the graph has no say over them.
    Stale,
    Success,
    /// The round failed on current content; waiters propagate the failure
    /// instead of retrying. Not sticky: a later request tries again.
    Failed,
};

/// Terminal state of a request() join.
enum class JoinOutcome : std::uint8_t {
    Success,
    Failed,
    /// OneAttempt only: the observed attempt ended without a verdict.
    Stale,
    /// The validity predicate went false; the join stopped waiting.
    Abandoned,
    /// The graph is shutting down.
    Shutdown,
};

/// Result of RoundContext::depend().
enum class DependResult : std::uint8_t {
    /// The dependency reached a current successful state.
    Ready,
    /// The dependency failed terminally, or the edge closes a wait cycle.
    Failed,
    /// This round's advisory token fired while waiting; the closure should
    /// wind down and report Stale.
    Cancelled,
};

enum class JoinFlavor : std::uint8_t {
    /// Retry through stale attempts until Success or Failed.
    UntilTerminal,
    /// Observe exactly one attempt and return its outcome.
    OneAttempt,
};

struct JoinOptions {
    JoinFlavor flavor = JoinFlavor::UntilTerminal;

    /// Dispatch the awaited chain at user-visible priority.
    bool foreground = false;

    /// Requester validity, checked before every (re)wait; a false return
    /// abandons the join. Null means always valid. Gates continuation only
    /// — a terminal outcome already observed is returned as-is.
    std::function<bool()> validity;
};

class TaskGraph;

/// Handed to a family's round closure; valid until the round's task
/// completes. Every graph-node wait inside a round MUST go through
/// depend() — it is the only point that records the edge for cascade,
/// interest and cycle detection; a bare request() from a round bypasses
/// all three.
struct RoundContext {
    /// Record a candidate edge to `dep` and wait until it is usable. The
    /// edge takes effect for cascade and interest the moment this is
    /// called; it becomes durable only if this round lands Success while
    /// still current.
    kota::task<DependResult> depend(NodeId dep);

    /// depend() without the dispatch or the wait: record the candidate
    /// edge to a node that never runs (a sentinel a scan consumed the
    /// absence of). Same cascade/interest/foreground semantics, same
    /// durable promotion on a successful landing — which is the point: a
    /// declared-only edge would be replaced by the landing's candidates.
    void reference(NodeId dep);

    /// Whether this round's result would still count if it landed now.
    bool current() const;

    /// Live interest class of the node — read at dispatch time, a
    /// foreground requester may have joined after the round started.
    bool foreground() const;

    /// Advisory cancellation: fires when the round is overtaken or loses
    /// all interest. The closure picks its response points and still
    /// reports a real outcome — the graph never destroys a running round.
    const kota::cancellation_token& token() const {
        return tok;
    }

private:
    friend class TaskGraph;

    RoundContext(TaskGraph& graph, NodeId self, kota::cancellation_token tok) :
        graph(graph), self(self), tok(std::move(tok)) {}

    TaskGraph& graph;
    NodeId self;
    kota::cancellation_token tok;
};

/// Multi-family dependency graph with interest-counted cancellation — the
/// thin scheduling core. Only four things live here: node identity, data
/// edges, rounds, and interest/priority. Everything else (artifact
/// products, cache keys, crash ledgers, retry budgets) belongs to the
/// family closures.
///
/// Each dirty node is driven by rounds produced by its family's runner,
/// spawned into the graph's task group. Lifecycle by phase:
///
/// - Request arrival: request() takes a root reference and joins the
///   node's round, spawning one if none is live. A round declares its
///   dependencies through RoundContext::depend(), which acquires edge
///   references and waits for them before the closure dispatches its own
///   work.
/// - Request cancel: the requester's frame unwinds and drops its root
///   reference. A node whose interest stays at zero for an event-loop
///   tick has its round's advisory token fired; the closure winds down
///   and reports, and the finished round releases its edge references,
///   cascading the loss of interest. A transient drop (a retry
///   re-acquiring a retained dependency) does not disturb the running
///   round.
/// - Content update: update() marks the node and its transitive
///   dependents dirty and fires their rounds' tokens — the results are
///   stale. Interest is untouched; waiters observe the stale round and
///   drive a fresh one.
/// - Round completion: the reported outcome passes through round identity
///   (an overtaken round lands Stale regardless), Success promotes the
///   round's candidate edges to the durable edge set, and waiters wake.
class TaskGraph {
public:
    /// Produces one round of a node. The returned task runs to a real
    /// reply: cancellation is advisory (RoundContext::token()), and the
    /// graph never destroys a running round.
    using RoundRunner = std::function<kota::task<RoundOutcome>(RoundContext& ctx, NodeId id)>;

    explicit TaskGraph(kota::event_loop& loop);

    /// Families register once, before any request for their nodes.
    void register_family(std::uint8_t family, RoundRunner run);

    /// Join a node's compilation from outside the graph (root interest).
    kota::task<JoinOutcome> request(NodeId id, JoinOptions options = {});

    /// Mark a node and all transitive dependents dirty, firing in-flight
    /// rounds' advisory tokens (their results are stale). Returns the set
    /// of nodes that were marked dirty.
    llvm::SmallVector<NodeId> update(NodeId id);

    /// The artifact tier of invalidation: the node's landed verdict is
    /// still true of its inputs — only its on-disk product vanished
    /// (cache eviction). Marks the node alone so the next depend or
    /// request rebuilds it: no dependent cascade (dependents consumed
    /// the content, which did not change) and no round voiding (an
    /// in-flight round is already producing a fresh artifact). Content
    /// changes stay with update() — cascading eviction would void the
    /// very consumer round waiting to rebuild the import, and a working
    /// set larger than the cache budget would spin that waiter forever.
    void mark_dirty(NodeId id);

    /// Commit `deps` as the durable edge set of `id` without running a
    /// round — the no-round counterpart of a successful round's candidate
    /// promotion. Facades use it for topology that is scanner truth
    /// rather than build output: consumer TUs that never run rounds, and
    /// a unit's imports, which must survive a failed build or fixing an
    /// import could never re-dirty the unit. Materializes every named
    /// node, replaces the previous set (self and duplicate edges
    /// dropped), takes no interest and leaves dirtiness untouched —
    /// invalidation stays with update(). A later current successful round
    /// replaces the declaration with its candidates.
    void declare(NodeId id, llvm::ArrayRef<NodeId> deps);

    /// Fire every round's token, refuse new rounds, and wait for all round
    /// frames to unwind. Must be awaited exactly once before the graph is
    /// destroyed.
    kota::task<> shutdown();

    bool has_node(NodeId id) const;
    bool is_dirty(NodeId id) const;
    bool is_compiling(NodeId id) const;

    /// Current in-flight interest count (testing/diagnostics).
    std::uint32_t refcount(NodeId id) const;

    /// Durable edge set: the candidates of the last current successful
    /// round (testing/diagnostics).
    llvm::ArrayRef<NodeId> dependencies(NodeId id) const;

    /// All bookkeeping is quiesced: nothing compiling, no interest held
    /// and every round's completion has fired.
    bool idle() const;

    /// Structural sanity that holds at every drain boundary: a compiling
    /// node has an unfinished round, and a finished round never leaves the
    /// compiling flag behind.
    bool consistent() const;

private:
    friend struct RoundContext;
    struct RootGuard;

    /// State of one round. Waiters capture the shared_ptr before
    /// suspending so the completion event outlives map mutations and the
    /// round task that publishes the outcome.
    struct Round {
        kota::event completion;
        RoundOutcome outcome = RoundOutcome::Stale;

        /// Node generation this round was spawned against; the landing
        /// filter compares it to decide whether the result still counts.
        std::uint64_t generation = 0;

        /// Candidate edges declared via depend(), deduplicated, in
        /// declaration order; doubles as the edge-reference registry
        /// released when the round finishes.
        llvm::SmallVector<NodeId, 8> candidates;
    };

    struct Node {
        NodeId id;

        /// Durable edges, replaced only by a current successful round.
        llvm::SmallVector<NodeId> deps;

        /// Durable back-edges: nodes whose durable set contains this one.
        llvm::SmallVector<NodeId> dependents;

        /// Back-edges of live rounds' candidate sets — an edge is
        /// cascade-visible from the moment depend() records it.
        llvm::SmallVector<NodeId> candidate_dependents;

        bool dirty = true;
        bool compiling = false;

        /// In-flight interest count: one per request() join plus one per
        /// candidate edge held by a live round. Staying at zero while
        /// compiling fires this node's round token. Not a lifetime count.
        std::uint32_t refcount = 0;

        /// A foreground requester holds (or held) interest in this round.
        /// Sticky while any interest remains and reset when the count
        /// returns to zero.
        bool foreground = false;

        /// A zero-interest cancellation check is already queued.
        bool zero_check_pending = false;

        /// Monotonic counter bumped by update(); the round-identity filter
        /// compares against it at landing.
        std::uint64_t generation = 0;

        /// Advisory cancellation scope of the current round; replaced
        /// after every cancel so the next round starts with a fresh token.
        std::unique_ptr<kota::cancellation_source> source =
            std::make_unique<kota::cancellation_source>();

        /// Current (or most recent) round.
        std::shared_ptr<Round> round;
    };

    /// Interest +1; creates the node if needed.
    void acquire(NodeId id);

    /// Interest -1; schedules a zero-interest cancellation check when it
    /// drops to zero mid-compile.
    void release(NodeId id);

    /// Fires the node's round token if its interest is still zero one
    /// event-loop tick after release() saw it drop. The delay lets
    /// transient drops survive: a retry respawning after update()
    /// re-acquires its retained dependencies within the same drain cycle.
    kota::task<> zero_interest_check(NodeId id);

    /// Fire the current round's advisory token and install a fresh scope.
    void cancel_round(Node& node);

    /// Mark a node and its live edge closure foreground; nodes depended
    /// later inherit through depend()'s propagation.
    void mark_foreground(NodeId id);

    /// Start a round for a dirty node in the graph's task group. Returns
    /// false when the graph is shutting down.
    bool spawn_round(NodeId id);

    kota::task<> round_task(NodeId id,
                            std::shared_ptr<Round> round,
                            kota::cancellation_token token);

    /// Synchronous landing: filter the reported outcome through round
    /// identity, retire candidate back-edges, promote candidates to the
    /// durable set on current success, release edge references and wake
    /// waiters.
    void finish_round(NodeId id, Round& round, RoundOutcome reported);

    /// Replace the durable edge set and its back-edges. Shared by round
    /// landing (candidates promote) and declare(); every named node must
    /// already exist.
    void set_durable_edges(NodeId id, llvm::ArrayRef<NodeId> deps);

    /// RoundContext::depend() body.
    kota::task<DependResult> depend_from(NodeId self, kota::cancellation_token token, NodeId dep);

    /// See RoundContext::reference().
    void reference_from(NodeId self, NodeId dep);

    /// Wait until a node reaches a terminal outcome, respawning its round
    /// whenever a stale one invalidates the previous attempt. `waiter` is
    /// the node doing the waiting (for deadlock detection); requests pass
    /// none.
    kota::task<bool> join_node(NodeId id, std::optional<NodeId> waiter);

    /// Whether waiting on `target` would deadlock: walks live rounds'
    /// candidate edges to see if the wait chain reaches back to `waiter`.
    bool has_wait_cycle(NodeId target, NodeId waiter) const;

    llvm::DenseMap<unsigned, RoundRunner> families;
    llvm::DenseMap<NodeId, Node> nodes;

    /// Refuses new rounds once shutdown begins.
    bool closed = false;

    /// Owns every round task; structured shutdown via shutdown().
    /// Completed child frames are reclaimed eagerly by the group.
    kota::task_group<> tasks;
};

}  // namespace clice
