module clice;

import :project.graph;

namespace clice {

/// Join scope: holds the root reference for one join(). The destructor
/// runs on every exit path, including cancellation unwind of the waiter's
/// frame.
struct TaskGraph::RootGuard {
    TaskGraph& graph;
    NodeId id;

    RootGuard(TaskGraph& graph, NodeId id) : graph(graph), id(id) {
        graph.acquire(id);
    }

    RootGuard(const RootGuard&) = delete;
    RootGuard& operator=(const RootGuard&) = delete;

    ~RootGuard() {
        graph.release(id);
    }
};

async::task<Result> Attempt::depend(NodeId dep, Work work) {
    return graph.depend_from(self, tok, dep, std::move(work));
}

bool Attempt::current() const {
    auto& node = graph.nodes.find(self)->second;
    return node.attempt->generation == node.generation;
}

bool Attempt::foreground() const {
    return graph.nodes.find(self)->second.foreground;
}

void TaskGraph::acquire(NodeId id) {
    nodes[id].refcount += 1;
}

void TaskGraph::release(NodeId id) {
    auto& node = nodes.find(id)->second;
    assert(node.refcount > 0 && "released more interest than acquired");
    node.refcount -= 1;
    if(node.refcount == 0) {
        node.foreground = false;
    }

    // No interest left in an in-flight attempt. The drop is often
    // transient — a stale attempt's edges being re-acquired by the retry
    // that replaces it — so don't cancel right away: defer the decision by
    // one event-loop tick. Synchronous re-acquisition happens within the
    // current drain cycle, strictly before the check fires, so retained
    // dependencies are handed over to the new attempt instead of being
    // killed and restarted; only a sustained zero cancels.
    if(node.refcount == 0 && node.compiling && !node.zero_check_pending) {
        node.zero_check_pending = true;
        if(!tasks.spawn(zero_interest_check(id))) {
            // Graph is shutting down; every attempt token fires anyway.
            nodes.find(id)->second.zero_check_pending = false;
        }
    }
}

async::task<> TaskGraph::zero_interest_check(NodeId id) {
    // Resumes on the next event-loop iteration, strictly after every
    // deferred resume of the current one — i.e. after the release/
    // re-acquire cascade that scheduled this check has fully finished.
    co_await async::yield();

    auto& node = nodes.find(id)->second;
    node.zero_check_pending = false;
    if(node.refcount == 0 && node.compiling) {
        // Advisory: the work winds down and reports, and the finished
        // attempt releases its own edge references in turn (cascading the
        // loss of interest).
        cancel_attempt(node);
    }
}

void TaskGraph::cancel_attempt(Node& node) {
    node.source->cancel();
    node.source = std::make_unique<async::cancellation_source>();
}

void TaskGraph::mark_foreground(NodeId id) {
    auto it = nodes.find(id);
    assert(it != nodes.end() && "foreground mark on unknown node");
    auto& node = it->second;

    // Already-marked doubles as the cycle/shared-dep visit guard.
    if(node.foreground) {
        return;
    }
    node.foreground = true;

    // A late join must upgrade the whole live edge closure, not just the
    // root: a dependency already running at Low re-reads its class when a
    // preempted attempt retries, and without the closure mark that retry
    // would stay Low — cancellable by the very foreground waiting on it.
    // Only live attempts' waits propagate: they hold interest, so the flag
    // is guaranteed its refcount-zero reset. Settled edges would tag clean
    // cached dependencies no waiter ever acquires — the mark could never
    // clear, and an unrelated background rebuild much later would run at
    // foreground class. Edges the attempt waits on later inherit through
    // depend().
    if(node.compiling && node.attempt) {
        for(auto dep: node.attempt->waits) {
            mark_foreground(dep);
        }
    }
}

bool TaskGraph::start_attempt(NodeId id, Work work) {
    if(closed) {
        return false;
    }

    auto& node = nodes.find(id)->second;
    assert(!node.compiling && "attempt started while one is in flight");
    node.compiling = true;
    node.attempt = std::make_shared<AttemptState>();
    node.attempt->generation = node.generation;
    auto attempt = node.attempt;
    auto token = node.source->token();

    // spawn resumes the attempt synchronously up to its first suspension,
    // which may insert nodes and invalidate references — don't touch
    // `node` below.
    [[maybe_unused]] bool spawned =
        tasks.spawn(attempt_task(id, attempt, std::move(work), std::move(token)));
    assert(spawned && "task group refused a spawn before shutdown");
    return true;
}

async::task<> TaskGraph::attempt_task(NodeId id,
                                      std::shared_ptr<AttemptState> attempt,
                                      Work work,
                                      async::cancellation_token token) {
    Attempt context(*this, id, std::move(token));
    auto reported = co_await work.run(context);
    finish_attempt(id, *attempt, reported);
}

void TaskGraph::finish_attempt(NodeId id, AttemptState& attempt, Result reported) {
    auto& node = nodes.find(id)->second;
    assert(node.compiling && node.attempt.get() == &attempt && "attempt bookkeeping out of sync");

    // An overtaken attempt has no result about the current content,
    // whatever it reported: a stale Success must not clear dirty, and a
    // stale Failed must not stop waiters from retrying with the new
    // content.
    auto result = node.generation == attempt.generation ? reported : Result::Stale;

    // The waits' back-edges go with the attempt.
    for(auto dep: attempt.waits) {
        auto& backs = nodes.find(dep)->second.waiting_dependents;
        auto pos = ranges::find(backs, id);
        assert(pos != backs.end() && "wait back-edge lost");
        backs.erase(pos);
    }

    if(result == Result::Success) {
        node.dirty = false;
        replace_edges(id, &Node::waited, attempt.waits);
    }

    // Re-find: replace_edges may materialize nodes.
    nodes.find(id)->second.compiling = false;

    for(auto dep: attempt.waits) {
        release(dep);
    }

    // Publish last: resumes triggered by set() are deferred by the event
    // loop, so nothing re-enters the graph mid-commit.
    attempt.result = result;
    attempt.completion.set();
}

void TaskGraph::replace_edges(NodeId id,
                              llvm::SmallVector<NodeId> Node::* set,
                              llvm::ArrayRef<NodeId> deps) {
    for(auto dep: dependencies(id)) {
        auto& backs = nodes.find(dep)->second.dependents;
        auto pos = ranges::find(backs, id);
        assert(pos != backs.end() && "back-edge lost");
        backs.erase(pos);
    }
    auto& edges = nodes.find(id)->second.*set;
    edges.assign(deps.begin(), deps.end());
    for(auto dep: dependencies(id)) {
        nodes.find(dep)->second.dependents.push_back(id);
    }
}

void TaskGraph::set_edges(NodeId id, llvm::ArrayRef<NodeId> deps) {
    nodes.try_emplace(id);

    // Broken input can name a unit as its own import; a node never
    // depends on itself (the same rule depend() enforces).
    llvm::SmallVector<NodeId, 8> unique;
    for(auto dep: deps) {
        if(dep != id && !ranges::contains(unique, dep)) {
            unique.push_back(dep);
        }
    }
    for(auto dep: unique) {
        nodes.try_emplace(dep);
    }
    replace_edges(id, &Node::declared, unique);
}

void TaskGraph::record_wait(NodeId self, NodeId dep) {
    auto attempt = nodes.find(self)->second.attempt;
    if(ranges::contains(attempt->waits, dep)) {
        return;
    }
    attempt->waits.push_back(dep);
    acquire(dep);
    nodes.find(dep)->second.waiting_dependents.push_back(self);
    // Re-find: acquire may rehash the map.
    if(nodes.find(self)->second.foreground) {
        mark_foreground(dep);
    }
}

async::task<Result>
    TaskGraph::depend_from(NodeId self, async::cancellation_token token, NodeId dep, Work work) {
    // A node can never wait on itself.
    if(dep == self) {
        co_return Result::Failed;
    }

    // Synchronous phase: the edge takes effect for invalidation, interest
    // and foreground the moment it is recorded — no window between "the
    // attempt uses dep" and "invalidate(dep) reaches the attempt".
    record_wait(self, dep);

    // Race the dependency wait against this attempt's own advisory token:
    // a voided attempt must stop blocking on slow dependencies and wind
    // down, or it would delay the fresh attempt behind it.
    auto result = co_await async::with_token(join_node(dep, std::move(work), self), token);
    if(result.is_cancelled()) {
        co_return Result::Stale;
    }
    co_return *result;
}

async::task<std::optional<Result>> TaskGraph::await_attempt(NodeId id,
                                                            const Work& work,
                                                            std::optional<NodeId> waiter) {
    if(!nodes.find(id)->second.dirty) {
        switch(work.check ? work.check() : Check::Holds) {
            case Check::Holds: co_return Result::Success;
            // Only the node: what its dependents built from the product
            // stands.
            case Check::ProductGone: nodes.find(id)->second.dirty = true; break;
            case Check::InputsChanged: invalidate(id, waiter); break;
        }
    }
    if(!nodes.find(id)->second.compiling && !start_attempt(id, work)) {
        co_return std::nullopt;
    }

    // Re-find: start_attempt runs the work synchronously, which may rehash
    // the map (and may even finish the attempt outright).
    auto attempt = nodes.find(id)->second.attempt;

    // Blocking on a node whose live wait chain reaches back to the waiting
    // node would deadlock — fail as a dependency cycle instead.
    if(!attempt->completion.is_set() && waiter && has_wait_cycle(id, *waiter)) {
        co_return Result::Failed;
    }

    co_await attempt->completion.wait();
    co_return attempt->result;
}

async::task<Result> TaskGraph::join_node(NodeId id, Work work, NodeId waiter) {
    while(true) {
        // A stale attempt ended without a result; we still hold interest,
        // so drive a new one. Each retry consumes one staleness event —
        // this terminates once the events stop.
        auto result = co_await await_attempt(id, work, waiter);
        if(!result) {
            co_return Result::Stale;
        }
        if(*result != Result::Stale) {
            co_return *result;
        }
    }
}

async::task<Result> TaskGraph::join(NodeId id, Work work, JoinOptions options) {
    // Root reference, dropped when the waiter exits or its frame is
    // cancelled.
    RootGuard scope(*this, id);
    if(options.foreground) {
        mark_foreground(id);
    }

    while(true) {
        auto result = co_await await_attempt(id, work, std::nullopt);
        if(!result) {
            co_return Result::Stale;
        }
        if(*result != Result::Stale || !options.retry || !options.retry()) {
            co_return *result;
        }
    }
}

llvm::SmallVector<NodeId> TaskGraph::invalidate(NodeId id) {
    return invalidate(id, std::nullopt);
}

llvm::SmallVector<NodeId> TaskGraph::invalidate(NodeId id, std::optional<NodeId> spared) {
    llvm::SmallVector<NodeId> queue;
    llvm::SmallVector<NodeId> dirtied;
    llvm::DenseSet<NodeId> visited;
    queue.push_back(id);

    while(!queue.empty()) {
        auto current = queue.pop_back_val();
        if(!visited.insert(current).second) {
            continue;
        }

        auto it = nodes.find(current);
        if(it == nodes.end()) {
            continue;
        }
        auto& node = it->second;

        // The in-flight result (if any) is stale: fire the attempt's
        // token. Interest is untouched — waiters keep their references and
        // drive a fresh attempt once the overtaken one finishes.
        if(current != spared) {
            cancel_attempt(node);
            node.generation += 1;
        }
        node.dirty = true;
        dirtied.push_back(current);

        queue.append(node.dependents.begin(), node.dependents.end());
        queue.append(node.waiting_dependents.begin(), node.waiting_dependents.end());
    }

    return dirtied;
}

bool TaskGraph::has_wait_cycle(NodeId target, NodeId waiter) const {
    // BFS from the target through live attempts' waits — the exact set of
    // waits currently in force.
    llvm::SmallVector<NodeId> queue;
    llvm::DenseSet<NodeId> visited;
    queue.push_back(target);

    while(!queue.empty()) {
        auto current = queue.pop_back_val();
        if(!visited.insert(current).second) {
            continue;
        }

        auto it = nodes.find(current);
        if(it == nodes.end() || !it->second.compiling || !it->second.attempt) {
            continue;
        }

        for(auto dep: it->second.attempt->waits) {
            if(dep == waiter) {
                return true;
            }
            queue.push_back(dep);
        }
    }
    return false;
}

async::task<> TaskGraph::shutdown() {
    // Structured shutdown: refuse new attempts, fire every advisory token,
    // then wait for the attempt frames to unwind — each still reports a
    // real result on the way out.
    closed = true;
    for(auto& [_, node]: nodes) {
        cancel_attempt(node);
    }
    co_await tasks.join();
}

bool TaskGraph::has_node(NodeId id) const {
    return nodes.count(id);
}

bool TaskGraph::is_dirty(NodeId id) const {
    auto it = nodes.find(id);
    return it != nodes.end() && it->second.dirty;
}

bool TaskGraph::is_compiling(NodeId id) const {
    auto it = nodes.find(id);
    return it != nodes.end() && it->second.compiling;
}

std::uint32_t TaskGraph::refcount(NodeId id) const {
    auto it = nodes.find(id);
    return it != nodes.end() ? it->second.refcount : 0;
}

llvm::SmallVector<NodeId> TaskGraph::dependencies(NodeId id) const {
    llvm::SmallVector<NodeId> edges;
    auto it = nodes.find(id);
    if(it == nodes.end()) {
        return edges;
    }
    for(auto* set: {&it->second.declared, &it->second.waited}) {
        for(auto dep: *set) {
            if(!ranges::contains(edges, dep)) {
                edges.push_back(dep);
            }
        }
    }
    return edges;
}

bool TaskGraph::idle() const {
    return ranges::all_of(nodes, [](const auto& entry) {
        const auto& node = entry.second;
        bool attempt_done = !node.attempt || node.attempt->completion.is_set();
        return !node.compiling && node.refcount == 0 && attempt_done;
    });
}

llvm::SmallVector<NodeId> TaskGraph::compiling() const {
    llvm::SmallVector<NodeId> ids;
    for(auto& [id, node]: nodes) {
        if(node.compiling) {
            ids.push_back(id);
        }
    }
    return ids;
}

bool TaskGraph::consistent() const {
    return ranges::all_of(nodes, [](const auto& entry) {
        const auto& node = entry.second;
        return !node.compiling || (node.attempt && !node.attempt->completion.is_set());
    });
}

}  // namespace clice
