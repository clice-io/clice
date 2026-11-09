module clice:project.graph.task_graph;

import :support.prelude;
import :support.signal;
import :vfs.ids;

namespace clice {

/// The kinds partitioning the graph's node space, each with its own key
/// convention: PCM nodes are module-unit path ids (unresolved imports take
/// the high half of the key space), PCH nodes are the hash of the PCH's
/// key, AST nodes are open documents' path ids, unit nodes are the units'
/// path ids. 0xFF is reserved for map sentinels.
enum class NodeKind : std::uint8_t {
    PCM = 1,
    PCH = 2,
    AST = 3,
    Unit = 4,
};

/// Identity of a task-graph node.
struct NodeId {
    NodeKind kind = NodeKind{0};
    std::uint64_t key = 0;

    /// The file a PCM, AST or unit node is keyed by.
    Fid file() const {
        return Fid{static_cast<std::uint32_t>(key)};
    }

    friend bool operator==(const NodeId&, const NodeId&) = default;
};

/// The graph node of an open document's compile.
inline NodeId ast_node(Fid file) {
    return {NodeKind::AST, file.raw};
}

}  // namespace clice

template <>
struct llvm::DenseMapInfo<clice::NodeId> {
    static unsigned getHashValue(const clice::NodeId& id) {
        return llvm::detail::combineHashValue(
            std::to_underlying(id.kind),
            llvm::DenseMapInfo<std::uint64_t>::getHashValue(id.key));
    }

    static bool isEqual(const clice::NodeId& lhs, const clice::NodeId& rhs) {
        return lhs == rhs;
    }
};

namespace clice {

/// How an attempt, a dependency wait or a join ended.
enum class Result : std::uint8_t {
    /// The node holds a product of its current content.
    Success,
    /// The attempt failed on current content, or waiting would close a
    /// cycle. A later wait tries again, unless the work says the failure
    /// stands (Work::failure_stands).
    Failed,
    /// No result: the attempt was overtaken by a content change, lost all
    /// interest, or the graph is shutting down. An attempt overtaken
    /// mid-flight ends Stale whatever it reported.
    Stale,
};

/// What a clean node's product looks like when a waiter reaches it.
enum class Check : std::uint8_t {
    Holds,
    /// The product left the disk while what it was built from stayed: the
    /// node builds again, and what its dependents built from it stands.
    ProductGone,
    /// What it was built from changed: the node and its dependents are
    /// stale — but for the attempt that found it, which waits for the fresh
    /// product and has read nothing of the old one.
    InputsChanged,
};

class Attempt;

/// How to build a node, carried by whoever waits on it: a waiter that
/// finds the node dirty with no attempt running starts one with its own
/// work, so every waiter of a node passes equivalent work.
struct Work {
    /// One attempt. It runs to a real result: cancellation is advisory
    /// (Attempt::token()), and the graph never destroys a running attempt.
    std::function<async::task<Result>(Attempt&)> run;

    /// Asked when a waiter reaches a clean node; null when a clean node
    /// always holds.
    std::function<Check()> check;

    /// Asked when a waiter reaches a node whose last attempt failed and
    /// that no invalidation reached since: whether that failure stands, so
    /// the waiter takes it instead of starting an attempt. Null when every
    /// wait tries again.
    std::function<bool()> failure_stands;
};

struct JoinOptions {
    /// Dispatch the awaited chain at user-visible priority.
    bool foreground = false;

    /// Whether to wait out an attempt that ended Stale with a fresh one,
    /// asked after each; null for one attempt.
    std::function<bool()> retry;
};

class TaskGraph;

/// Handed to an attempt's work; valid until the attempt completes. Every
/// graph-node wait inside an attempt goes through depend(): it is the only
/// point that records the edge for invalidation, interest and cycle
/// detection.
class Attempt {
public:
    /// Wait until `dep` holds a current product, building it with `work`
    /// when it has to. The edge counts for invalidation and interest from
    /// the moment of the call, and stays one of this node's edges when the
    /// attempt succeeds. Stale when this attempt was overtaken while
    /// waiting: it should wind down and report Stale.
    async::task<Result> depend(NodeId dep, Work work);

    /// Whether this attempt's result would still count if it finished now:
    /// the node's generation is still the one it started at.
    bool current() const;

    /// The node's generation the attempt started at (TaskGraph::generation).
    std::uint64_t generation() const;

    /// Live interest class of the node — read at dispatch time, a
    /// foreground waiter may have joined after the attempt started.
    bool foreground() const;

    /// Emitted when a foreground waiter joins the attempt: what it sent
    /// Low runs at foreground class from then on.
    Signal<>& on_foreground() const;

    /// Advisory cancellation: fires when the attempt is overtaken or loses
    /// all interest.
    const async::cancellation_token& token() const {
        return tok;
    }

    NodeId node() const {
        return self;
    }

private:
    friend class TaskGraph;

    Attempt(TaskGraph& graph, NodeId self, async::cancellation_token tok) :
        graph(graph), self(self), tok(std::move(tok)) {}

    TaskGraph& graph;
    NodeId self;
    async::cancellation_token tok;
};

/// Dependency graph over nodes of several kinds with interest-counted
/// cancellation — the thin scheduling core: node identity, edges,
/// attempts, and interest and priority. What a node builds, its cache keys
/// and its failure records belong to the work its waiters carry.
///
/// Lifecycle by phase:
///
/// - Wait: join() takes a root reference, Attempt::depend() an edge
///   reference; a clean node is checked (Work::check) and a dirty one
///   gets an attempt of the waiter's work unless one runs already.
/// - Cancel: a waiter's frame unwinds and drops its reference. A node
///   whose interest stays at zero for an event-loop tick has its attempt's
///   advisory token fired; the work winds down and reports, and the
///   finished attempt releases its edge references, cascading the loss of
///   interest. A transient drop (a retry re-acquiring a retained
///   dependency) does not disturb the running attempt.
/// - Content change: invalidate() marks the node and its transitive
///   dependents dirty and fires the tokens of the attempts that read what
///   changed. Interest is untouched; waiters see the stale attempt and
///   drive a fresh one.
/// - Completion: the reported result passes through attempt identity (an
///   overtaken attempt ends Stale), Success makes the attempt's waits the
///   node's waited edges, and waiters wake.
///
/// A node's edges are the ones set_edges() declared and the ones its last
/// successful attempt waited on; invalidation follows both.
class TaskGraph {
public:
    /// Wait for a node's product from outside the graph (root interest).
    async::task<Result> join(NodeId id, Work work, JoinOptions options = {});

    /// Mark a node and all transitive dependents dirty, firing the advisory
    /// tokens of the in-flight attempts that read what changed — the product
    /// or failure of a node it reached them from, or the absence of one no
    /// attempt ever built (their results are stale). Returns the nodes
    /// marked.
    llvm::SmallVector<NodeId> invalidate(NodeId id);

    /// Declare the node's edges from what its sources say — a scan's
    /// imports, which must stay edges through a failed build, or fixing an
    /// import could never re-dirty the node. Materializes every named
    /// node, replaces the previous declaration (self and duplicate edges
    /// dropped), takes no interest and leaves dirtiness untouched.
    void set_edges(NodeId id, llvm::ArrayRef<NodeId> deps);

    /// Fire every attempt's token, refuse new attempts, and wait for all
    /// attempt frames to unwind. Must be awaited exactly once before the
    /// graph is destroyed.
    async::task<> shutdown();

    /// Make the node exist with no edges, if it does not: invalidations
    /// reach it from now on, before its first attempt.
    void add(NodeId id);

    bool has_node(NodeId id) const;
    bool is_dirty(NodeId id) const;
    bool is_compiling(NodeId id) const;

    /// How many invalidations reached the node: what an attempt started at
    /// one generation produced describes the node's inputs while it is still
    /// the node's. 0 for a node that does not exist.
    std::uint64_t generation(NodeId id) const;

    /// Current in-flight interest count: whether anyone still waits on the
    /// node.
    std::uint32_t refcount(NodeId id) const;

    /// The node's edges: declared and waited on by its last successful
    /// attempt, each once.
    llvm::SmallVector<NodeId> dependencies(NodeId id) const;

    /// All bookkeeping is quiesced: nothing compiling, no interest held
    /// and every attempt's completion has fired.
    bool idle() const;

    /// The nodes whose attempt is in flight.
    llvm::SmallVector<NodeId> compiling() const;

private:
    friend class Attempt;
    struct RootGuard;

    /// One attempt. Waiters capture the shared_ptr before suspending so the
    /// completion event outlives map mutations and the attempt task that
    /// publishes the result.
    struct AttemptState {
        async::event completion;
        Result result = Result::Stale;

        /// Node generation this attempt was started against; the commit
        /// filter compares it to decide whether the result still counts.
        std::uint64_t generation = 0;

        /// Edges recorded via depend(), deduplicated, in order; doubles as
        /// the edge-reference registry released when the attempt finishes.
        llvm::SmallVector<NodeId, 8> waits;

        /// The waits that returned Success: the products the attempt read.
        llvm::SmallVector<NodeId, 8> received;

        /// See Attempt::on_foreground.
        Signal<> on_foreground;
    };

    struct Node {
        /// The edges set_edges() declared.
        llvm::SmallVector<NodeId> declared;

        /// The edges the last current successful attempt waited on.
        llvm::SmallVector<NodeId> waited;

        /// Back-edges: nodes whose declared or waited edges name this one.
        llvm::SmallVector<NodeId> dependents;

        /// Back-edges of live attempts' waits — an edge is visible to
        /// invalidation from the moment depend() records it.
        llvm::SmallVector<NodeId> waiting_dependents;

        bool dirty = true;

        /// In-flight interest count: one per join plus one per edge a live
        /// attempt waits on. Staying at zero while compiling fires the
        /// attempt's token. Not a lifetime count.
        std::uint32_t refcount = 0;

        /// A foreground waiter holds (or held) interest in this attempt.
        /// Sticky while any interest remains and reset when the count
        /// returns to zero.
        bool foreground = false;

        /// A zero-interest cancellation check is already queued.
        bool zero_check_pending = false;

        /// Monotonic counter bumped by invalidate(); the attempt-identity
        /// filter compares against it at commit.
        std::uint64_t generation = 0;

        /// Advisory cancellation scope of the current attempt; replaced
        /// after every cancel so the next attempt starts with a fresh
        /// token.
        std::unique_ptr<async::cancellation_source> source =
            std::make_unique<async::cancellation_source>();

        /// Current (or most recent) attempt.
        std::shared_ptr<AttemptState> attempt;

        /// An attempt is in flight.
        bool compiling() const {
            return attempt && !attempt->completion.is_set();
        }
    };

    /// The live attempt of `waiter` read the product of `dep`.
    void mark_received(NodeId waiter, NodeId dep);

    /// Interest +1; creates the node if needed.
    void acquire(NodeId id);

    /// Interest -1; schedules a zero-interest cancellation check when it
    /// drops to zero mid-compile.
    void release(NodeId id);

    /// Fires the node's attempt token if its interest is still zero one
    /// event-loop tick after release() saw it drop. The delay lets
    /// transient drops survive: a retry starting after invalidate()
    /// re-acquires its retained dependencies within the same drain cycle.
    async::task<> zero_interest_check(NodeId id);

    /// Fire the current attempt's advisory token and install a fresh scope.
    void cancel_attempt(Node& node);

    /// Mark a node and its live edge closure foreground; nodes depended
    /// on later inherit through depend()'s propagation.
    void mark_foreground(NodeId id);

    /// Start an attempt of `work` for a dirty node in the graph's task
    /// group. Returns false when the graph is shutting down.
    bool start_attempt(NodeId id, Work work);

    async::task<> attempt_task(NodeId id,
                               std::shared_ptr<AttemptState> attempt,
                               Work work,
                               async::cancellation_token token);

    /// Synchronous commit: filter the reported result through attempt
    /// identity, remove the waits' back-edges, make them the waited edges
    /// on current success, release edge references and wake waiters.
    void finish_attempt(NodeId id, AttemptState& attempt, Result reported);

    /// Replace one of the node's edge sets and keep the back-edges of
    /// their union.
    void replace_edges(NodeId id,
                       llvm::SmallVector<NodeId> Node::* set,
                       llvm::ArrayRef<NodeId> deps);

    /// Register `dep` among the current attempt's waits of `self`: one
    /// interest reference, the back-edge, and foreground propagation, all
    /// at once. Idempotent per attempt.
    void record_wait(NodeId self, NodeId dep);

    /// Attempt::depend() body.
    async::task<Result>
        depend_from(NodeId self, async::cancellation_token token, NodeId dep, Work work);

    /// Wait for one attempt of a node: Success at once for a clean node
    /// whose product holds; otherwise the running attempt, or one of
    /// `work` started now. Failed when waiting would close a cycle back to
    /// `waiter`, the node doing the waiting (joins pass none); none when
    /// the graph is shutting down.
    async::task<std::optional<Result>> await_attempt(NodeId id,
                                                     const Work& work,
                                                     std::optional<NodeId> waiter);

    /// Wait until a node reaches Success or Failed, starting fresh attempts
    /// while stale ones end.
    async::task<Result> join_node(NodeId id, Work work, NodeId waiter);

    /// Whether waiting on `target` would deadlock: walks live attempts'
    /// waits to see if the wait chain reaches back to `waiter`.
    bool has_wait_cycle(NodeId target, NodeId waiter) const;

    llvm::DenseMap<NodeId, Node> nodes;

    /// Refuses new attempts once shutdown begins.
    bool closed = false;

    /// Owns every attempt task; structured shutdown via shutdown().
    /// Completed child frames are reclaimed eagerly by the group.
    async::task_group<> tasks;
};

}  // namespace clice
