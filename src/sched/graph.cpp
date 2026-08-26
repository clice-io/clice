#include "sched/graph.h"

#include <algorithm>
#include <cassert>

#include "llvm/ADT/DenseSet.h"

namespace clice {

namespace ranges = std::ranges;

/// Request scope: holds the root reference for one request() join. The
/// destructor runs on every exit path, including cancellation unwind of
/// the requester's frame.
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

kota::task<DependResult> RoundContext::depend(NodeId dep) {
    return graph.depend_from(self, tok, dep);
}

void RoundContext::reference(NodeId dep) {
    graph.reference_from(self, dep);
}

bool RoundContext::current() const {
    auto& node = graph.nodes.find(self)->second;
    return node.round->generation == node.generation;
}

bool RoundContext::foreground() const {
    return graph.nodes.find(self)->second.foreground;
}

TaskGraph::TaskGraph(kota::event_loop& loop) : tasks(loop) {}

void TaskGraph::register_family(std::uint8_t family, RoundRunner run) {
    assert(!families.contains(family) && "family registered twice");
    families[family] = std::move(run);
}

void TaskGraph::acquire(NodeId id) {
    auto& node = nodes[id];
    node.id = id;
    node.refcount += 1;
}

void TaskGraph::release(NodeId id) {
    auto& node = nodes.find(id)->second;
    assert(node.refcount > 0 && "released more interest than acquired");
    node.refcount -= 1;
    if(node.refcount == 0) {
        node.foreground = false;
    }

    // No interest left in an in-flight round. The drop is often transient —
    // a stale round's edges being re-acquired by the retry that replaces
    // it — so don't cancel right away: defer the decision by one event-loop
    // tick. Synchronous re-acquisition happens within the current drain
    // cycle, strictly before the check fires, so retained dependencies are
    // handed over to the new round instead of being killed and restarted;
    // only a sustained zero cancels.
    if(node.refcount == 0 && node.compiling && !node.zero_check_pending) {
        node.zero_check_pending = true;
        if(!tasks.spawn(zero_interest_check(id))) {
            // Graph is shutting down; every round token fires anyway.
            nodes.find(id)->second.zero_check_pending = false;
        }
    }
}

kota::task<> TaskGraph::zero_interest_check(NodeId id) {
    // Resumes on the next event-loop iteration, strictly after every
    // deferred resume of the current one — i.e. after the release/
    // re-acquire cascade that scheduled this check has fully settled.
    co_await kota::yield();

    auto& node = nodes.find(id)->second;
    node.zero_check_pending = false;
    if(node.refcount == 0 && node.compiling) {
        // Advisory: the closure winds down and reports, and the finished
        // round releases its own edge references in turn (cascading the
        // loss of interest).
        cancel_round(node);
    }
}

void TaskGraph::cancel_round(Node& node) {
    node.source->cancel();
    node.source = std::make_unique<kota::cancellation_source>();
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
    // preempted round retries, and without the closure mark that retry
    // would stay Low — cancellable by the very foreground waiting on it.
    // Only live rounds' candidate edges propagate: they hold interest, so
    // the flag is guaranteed its refcount-zero reset. Durable edges would
    // tag clean cached dependencies no request ever acquires — the mark
    // could never clear, and an unrelated background rebuild much later
    // would run at foreground class. Edges the round declares later
    // inherit through depend().
    if(node.compiling && node.round) {
        for(auto dep: node.round->candidates) {
            mark_foreground(dep);
        }
    }
}

bool TaskGraph::spawn_round(NodeId id) {
    if(closed) {
        return false;
    }

    auto& node = nodes.find(id)->second;
    assert(!node.compiling && "spawn requested while a round is in flight");
    assert(families.contains(id.family) && "request for an unregistered family");
    node.compiling = true;
    node.round = std::make_shared<Round>();
    node.round->generation = node.generation;
    auto round = node.round;
    auto token = node.source->token();

    // spawn resumes the round synchronously up to its first suspension,
    // which may insert nodes and invalidate references — don't touch
    // `node` below.
    [[maybe_unused]] bool spawned = tasks.spawn(round_task(id, round, std::move(token)));
    assert(spawned && "task group refused a spawn before shutdown");
    return true;
}

kota::task<> TaskGraph::round_task(NodeId id,
                                   std::shared_ptr<Round> round,
                                   kota::cancellation_token token) {
    RoundContext ctx(*this, id, std::move(token));
    auto reported = co_await families.find(id.family)->second(ctx, id);
    finish_round(id, *round, reported);
}

void TaskGraph::finish_round(NodeId id, Round& round, RoundOutcome reported) {
    auto& node = nodes.find(id)->second;
    assert(node.compiling && node.round.get() == &round && "round bookkeeping out of sync");

    // An overtaken round has no verdict about the current content,
    // whatever it reported: a stale Success must not clear dirty, and a
    // stale Failed must not stop waiters from retrying with the new
    // content. Outcome always passes through round identity.
    auto outcome = node.generation == round.generation ? reported : RoundOutcome::Stale;

    // Candidate back-edges retire with the round.
    for(auto dep: round.candidates) {
        auto& backs = nodes.find(dep)->second.candidate_dependents;
        auto pos = ranges::find(backs, id);
        assert(pos != backs.end() && "candidate back-edge lost");
        backs.erase(pos);
    }

    if(outcome == RoundOutcome::Success) {
        node.dirty = false;

        // Only a current successful round may replace the durable edge
        // set; stale and failed candidate sets are discarded above.
        set_durable_edges(id, round.candidates);
    }

    node.compiling = false;

    for(auto dep: round.candidates) {
        release(dep);
    }

    // Publish last: resumes triggered by set() are deferred by the event
    // loop, so nothing re-enters the graph mid-landing.
    round.outcome = outcome;
    round.completion.set();
}

void TaskGraph::set_durable_edges(NodeId id, llvm::ArrayRef<NodeId> deps) {
    auto& node = nodes.find(id)->second;
    for(auto dep: node.deps) {
        auto& backs = nodes.find(dep)->second.dependents;
        auto pos = ranges::find(backs, id);
        assert(pos != backs.end() && "durable back-edge lost");
        backs.erase(pos);
    }
    node.deps.assign(deps.begin(), deps.end());
    for(auto dep: deps) {
        nodes.find(dep)->second.dependents.push_back(id);
    }
}

void TaskGraph::declare(NodeId id, llvm::ArrayRef<NodeId> deps) {
    nodes[id].id = id;

    // Broken input can name a unit as its own import; a node never
    // depends on itself (the same rule depend() enforces).
    llvm::SmallVector<NodeId, 8> unique;
    for(auto dep: deps) {
        if(dep != id && !ranges::contains(unique, dep)) {
            unique.push_back(dep);
        }
    }
    for(auto dep: unique) {
        nodes[dep].id = dep;
    }
    set_durable_edges(id, unique);
}

void TaskGraph::reference_from(NodeId self, NodeId dep) {
    if(dep == self) {
        return;
    }
    auto round = nodes.find(self)->second.round;
    if(!ranges::contains(round->candidates, dep)) {
        round->candidates.push_back(dep);
        acquire(dep);
        nodes.find(dep)->second.candidate_dependents.push_back(self);
        if(nodes.find(self)->second.foreground) {
            mark_foreground(dep);
        }
    }
}

kota::task<DependResult> TaskGraph::depend_from(NodeId self,
                                                kota::cancellation_token token,
                                                NodeId dep) {
    // A node can never wait on itself.
    if(dep == self) {
        co_return DependResult::Failed;
    }

    // Synchronous phase: the edge takes effect for cascade, interest and
    // foreground the moment it is declared — no window between "the round
    // uses dep" and "update(dep) reaches the round".
    auto round = nodes.find(self)->second.round;
    if(!ranges::contains(round->candidates, dep)) {
        round->candidates.push_back(dep);
        acquire(dep);
        nodes.find(dep)->second.candidate_dependents.push_back(self);
        // Re-find: acquire may rehash the map.
        if(nodes.find(self)->second.foreground) {
            mark_foreground(dep);
        }
    }

    // Race the dependency wait against this round's own advisory token: a
    // voided round must stop blocking on slow dependencies and wind down,
    // or it would delay the fresh round behind it.
    auto result = co_await kota::with_token(join_node(dep, self), token);
    if(result.is_cancelled()) {
        co_return DependResult::Cancelled;
    }
    co_return *result ? DependResult::Ready : DependResult::Failed;
}

kota::task<bool> TaskGraph::join_node(NodeId id, std::optional<NodeId> waiter) {
    while(true) {
        auto& node = nodes.find(id)->second;
        if(!node.dirty) {
            co_return true;
        }

        if(!node.compiling && !spawn_round(id)) {
            co_return false;  // graph is shutting down
        }

        // Re-find: spawn_round runs the closure synchronously, which may
        // rehash the map (and may even land the round outright).
        auto round = nodes.find(id)->second.round;

        // Blocking on a node whose live wait chain reaches back to the
        // waiting node would deadlock — fail as a dependency cycle instead.
        if(!round->completion.is_set() && waiter && has_wait_cycle(id, *waiter)) {
            co_return false;
        }

        co_await round->completion.wait();

        switch(round->outcome) {
            case RoundOutcome::Success: co_return true;
            case RoundOutcome::Failed: co_return false;
            // The round ended without a verdict; we still hold interest,
            // so drive a new one. Each retry consumes one staleness event
            // — this terminates once the events stop.
            case RoundOutcome::Stale: break;
        }
    }
}

kota::task<JoinOutcome> TaskGraph::request(NodeId id, JoinOptions options) {
    // Root reference, dropped when the requester exits or its frame is
    // cancelled.
    RootGuard scope(*this, id);
    if(options.foreground) {
        mark_foreground(id);
    }

    while(true) {
        if(options.validity && !options.validity()) {
            co_return JoinOutcome::Abandoned;
        }

        auto& node = nodes.find(id)->second;
        if(!node.dirty) {
            co_return JoinOutcome::Success;
        }

        if(!node.compiling && !spawn_round(id)) {
            co_return JoinOutcome::Shutdown;
        }

        auto round = nodes.find(id)->second.round;
        co_await round->completion.wait();

        switch(round->outcome) {
            case RoundOutcome::Success: co_return JoinOutcome::Success;
            case RoundOutcome::Failed: co_return JoinOutcome::Failed;
            case RoundOutcome::Stale:
                if(options.flavor == JoinFlavor::OneAttempt) {
                    co_return JoinOutcome::Stale;
                }
                break;
        }
    }
}

llvm::SmallVector<NodeId> TaskGraph::update(NodeId id) {
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

        // The in-flight result (if any) is stale: fire the round's token.
        // Interest is untouched — waiters keep their references and drive
        // a fresh round once the overtaken one lands.
        cancel_round(node);
        node.dirty = true;
        node.generation += 1;
        dirtied.push_back(current);

        queue.append(node.dependents.begin(), node.dependents.end());
        queue.append(node.candidate_dependents.begin(), node.candidate_dependents.end());
    }

    return dirtied;
}

void TaskGraph::mark_dirty(NodeId id) {
    auto it = nodes.find(id);
    if(it == nodes.end()) {
        return;
    }
    // No generation bump and no token: an in-flight round is already
    // producing the fresh artifact this mark asks for, and its landing
    // clears the flag it would find set.
    it->second.dirty = true;
}

bool TaskGraph::has_wait_cycle(NodeId target, NodeId waiter) const {
    // BFS from the target through live rounds' candidate edges — the
    // exact set of waits currently in force.
    llvm::SmallVector<NodeId> queue;
    llvm::DenseSet<NodeId> visited;
    queue.push_back(target);

    while(!queue.empty()) {
        auto current = queue.pop_back_val();
        if(!visited.insert(current).second) {
            continue;
        }

        auto it = nodes.find(current);
        if(it == nodes.end() || !it->second.compiling || !it->second.round) {
            continue;
        }

        for(auto dep: it->second.round->candidates) {
            if(dep == waiter) {
                return true;
            }
            queue.push_back(dep);
        }
    }
    return false;
}

kota::task<> TaskGraph::shutdown() {
    // Structured shutdown: refuse new rounds, fire every advisory token,
    // then wait for the round frames to unwind — each closure still
    // reports a real outcome on the way out.
    closed = true;
    for(auto& [_, node]: nodes) {
        cancel_round(node);
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

llvm::ArrayRef<NodeId> TaskGraph::dependencies(NodeId id) const {
    auto it = nodes.find(id);
    return it != nodes.end() ? llvm::ArrayRef<NodeId>(it->second.deps) : llvm::ArrayRef<NodeId>();
}

bool TaskGraph::idle() const {
    return ranges::all_of(nodes, [](const auto& entry) {
        const auto& node = entry.second;
        bool round_done = !node.round || node.round->completion.is_set();
        return !node.compiling && node.refcount == 0 && round_done;
    });
}

bool TaskGraph::consistent() const {
    return ranges::all_of(nodes, [](const auto& entry) {
        const auto& node = entry.second;
        return !node.compiling || (node.round && !node.round->completion.is_set());
    });
}

}  // namespace clice
