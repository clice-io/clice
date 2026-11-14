module clice;

import :project.graph.task_graph;
import :support.signal;
import :unittests.utils;

namespace clice::testing {
namespace {

/// Two arbitrary kinds exercising the multi-kind surface.
constexpr NodeKind KindA = NodeKind{1};
constexpr NodeKind KindB = NodeKind{2};

NodeId a(std::uint64_t key) {
    return {KindA, key};
}

NodeId b(std::uint64_t key) {
    return {KindB, key};
}

using Adjacency = llvm::DenseMap<NodeId, llvm::SmallVector<NodeId>>;

/// What an attempt does once its node's dependencies hold.
using Body = std::function<async::task<Result>(Attempt&)>;

/// The retry of a join that waits out every stale attempt.
constexpr auto always = [] {
    return true;
};

async::task<Result> reply(Result result) {
    co_return result;
}

/// Reply once `event` is set, ignoring the advisory token.
async::task<Result> reply_after(async::event& event, Result result) {
    co_await event.wait();
    co_return result;
}

async::task<> acquire_permit(async::semaphore& permits) {
    co_await permits.acquire();
}

/// Succeed once a permit is free, or end Stale when the attempt's advisory
/// token fires first.
async::task<Result> take_permit(async::semaphore& permits, Attempt& attempt) {
    auto waited = co_await async::with_token(acquire_permit(permits), attempt.token());
    co_return waited.is_cancelled() ? Result::Stale : Result::Success;
}

Work after_deps(const Adjacency& adj, Body body);

async::task<Result> run_after_deps(const Adjacency& adj, Body body, Attempt& attempt) {
    if(auto it = adj.find(attempt.node()); it != adj.end()) {
        for(auto dep: it->second) {
            auto result = co_await attempt.depend(dep, after_deps(adj, body));
            if(result != Result::Success) {
                co_return result;
            }
        }
    }
    co_return co_await body(attempt);
}

/// Work that waits on the node's dependencies in `adj`, each built with this
/// same work, then runs `body`; a dependency that does not succeed ends the
/// attempt with its result.
Work after_deps(const Adjacency& adj, Body body) {
    return {.run = [&adj, body](Attempt& attempt) { return run_after_deps(adj, body, attempt); }};
}

/// A cancellable join and its observed result: result stays empty while the
/// join runs and after the waiter's cancellation.
struct Probe {
    async::cancellation_source source;
    std::optional<Result> result;
    bool done = false;
};

ZEST_SUITE(TaskGraph, zest::LoopFixture) {

std::vector<NodeId> ran;
TaskGraph graph;

/// Work that resolves dependencies from a shared adjacency, then records the
/// run and succeeds.
Work instant(const Adjacency& adj) {
    return after_deps(adj, [this](Attempt& attempt) {
        ran.push_back(attempt.node());
        return reply(Result::Success);
    });
}

/// Wait on `dep` with `work`, then record the run.
async::task<Result> wait_on(Attempt& attempt, NodeId dep, Work work) {
    auto result = co_await attempt.depend(dep, std::move(work));
    if(result == Result::Success) {
        ran.push_back(attempt.node());
    }
    co_return result;
}

/// Work driven manually by per-node gates: the test observes when an attempt
/// reaches its dispatch (started) and decides when and how it completes. The
/// dispatch races its gate against the attempt's advisory token — the
/// run-to-reply discipline every real attempt follows.
struct ManualRunner {
    struct Gate {
        async::event started;
        async::event proceed;
        Result result = Result::Success;
        int calls = 0;
        std::vector<bool> classes;
    };

    llvm::DenseMap<NodeId, std::unique_ptr<Gate>> gates;
    Adjacency adj;

    /// Ignore the advisory token: reply only when the gate opens.
    bool stubborn = false;

    Gate& gate(NodeId id) {
        auto& slot = gates[id];
        if(!slot) {
            slot = std::make_unique<Gate>();
        }
        return *slot;
    }

    void open(std::initializer_list<NodeId> ids) {
        for(auto id: ids) {
            gate(id).proceed.set();
        }
    }

    Work work() {
        return after_deps(adj, [this](Attempt& attempt) { return dispatch(attempt); });
    }

    async::task<Result> dispatch(Attempt& attempt) {
        auto& g = gate(attempt.node());
        g.calls += 1;
        g.classes.push_back(attempt.foreground());
        g.started.set();

        if(stubborn) {
            co_await g.proceed.wait();
            co_return g.result;
        }

        auto waited = co_await async::with_token(opened(g), attempt.token());
        if(waited.is_cancelled()) {
            co_return Result::Stale;
        }
        co_return g.result;
    }

    static async::task<> opened(Gate& g) {
        co_await g.proceed.wait();
    }
};

/// Run the test body, then verify the shutdown protocol: firing every
/// advisory token and joining must exit cleanly and leave the graph fully
/// quiesced (no compiling residue, no held interest, every completion
/// fired).
template <typename F>
void execute(F&& fn) {
    auto wrapper = [&]() -> async::task<> {
        co_await fn();
        co_await graph.shutdown();
        ZEXPECT(graph.idle());
    };
    run(wrapper());
}

/// Wait on `read` with `work`, whatever it gives, then on `waited`.
async::task<Result> read_then_wait(Attempt& attempt, NodeId read, NodeId waited, Work work) {
    co_await attempt.depend(read, work);
    co_return co_await attempt.depend(waited, std::move(work));
}

/// Declare an edge to `absent`, a node no attempt builds, then wait on
/// `waited`.
async::task<Result> declare_then_wait(Attempt& attempt, NodeId absent, NodeId waited, Work work) {
    graph.set_edges(attempt.node(), {absent});
    co_return co_await attempt.depend(waited, std::move(work));
}

async::task<> run_join(NodeId id, Work work, Probe& probe, JoinOptions options = {}) {
    auto result = co_await async::with_token(graph.join(id, std::move(work), std::move(options)),
                                             probe.source.token());
    probe.done = true;
    if(result.has_value()) {
        probe.result = *result;
    }
}

/// ============================================================================
///                          Basic attempts & joins
/// ============================================================================

ZEST_CASE(JoinWithoutDependencies) {
    // A node without dependencies runs one attempt and becomes clean.
    Adjacency adj;

    execute([&]() -> async::task<> {
        auto result = co_await graph.join(a(1), instant(adj), {.retry = always});
        ZEXPECT(result == Result::Success);
        ZEXPECT(ran.size() == 1u);
        ZEXPECT(!graph.is_dirty(a(1)));
    });
}

ZEST_CASE(JoinDependencyChain) {
    // 1 -> 2 -> 3: attempts commit bottom-up along their waits, and each
    // node's edges are the waits of its successful attempt.
    Adjacency adj;
    adj[a(1)] = {a(2)};
    adj[a(2)] = {a(3)};

    execute([&]() -> async::task<> {
        auto result = co_await graph.join(a(1), instant(adj), {.retry = always});
        ZEXPECT(result == Result::Success);
        ZEXPECT(ran.size() == 3u);
        ZEXPECT(ranges::find(ran, a(3)) < ranges::find(ran, a(2)));
        ZEXPECT(ranges::find(ran, a(2)) < ranges::find(ran, a(1)));
        ZEXPECT(graph.dependencies(a(1)).size() == 1u);
        ZEXPECT(graph.dependencies(a(2)).size() == 1u);
    });
}

ZEST_CASE(DiamondDeduplicates) {
    // Diamond 1 -> {2, 3} -> 4: the shared dependency is reached through
    // two branches but runs exactly once.
    Adjacency adj;
    adj[a(1)] = {a(2), a(3)};
    adj[a(2)] = {a(4)};
    adj[a(3)] = {a(4)};

    execute([&]() -> async::task<> {
        auto result = co_await graph.join(a(1), instant(adj), {.retry = always});
        ZEXPECT(result == Result::Success);
        ZEXPECT(ranges::count(ran, a(4)) == 1);
        ZEXPECT(!graph.is_dirty(a(4)));
    });
}

ZEST_CASE(SecondJoinSkips) {
    // A clean node is not re-run by a later join.
    Adjacency adj;

    execute([&]() -> async::task<> {
        co_await graph.join(a(1), instant(adj), {.retry = always});
        ZEXPECT(ran.size() == 1u);
        co_await graph.join(a(1), instant(adj), {.retry = always});
        ZEXPECT(ran.size() == 1u);
    });
}

ZEST_CASE(DeclaredSurvivesCommit) {
    // An edge set_edges() declares to a node that never runs survives a
    // successful attempt that waits on nothing, so a later invalidate() on
    // that node re-dirties this one.
    NodeId unresolved{KindB, (1ull << 63) | 42};
    Adjacency adj;

    execute([&]() -> async::task<> {
        graph.set_edges(a(1), {unresolved});
        auto result = co_await graph.join(a(1), instant(adj), {.retry = always});
        ZASSERT(result == Result::Success);

        auto dirtied = graph.invalidate(unresolved);
        ZEXPECT(ranges::find(dirtied, a(1)) != dirtied.end());
        ZEXPECT(graph.is_dirty(a(1)));
    });
}

ZEST_CASE(ConcurrentJoinsShareAttempt) {
    // Two concurrent joins of the same node share one attempt: a single
    // dispatch serves both.
    ManualRunner manual;

    Probe p1, p2;
    execute([&]() -> async::task<> {
        auto driver = [&]() -> async::task<> {
            co_await manual.gate(a(1)).started.wait();
            ZEXPECT(graph.refcount(a(1)) == 2u);
            manual.open({a(1)});
            co_return;
        };

        co_await async::when_all(run_join(a(1), manual.work(), p1, {.retry = always}),
                                 run_join(a(1), manual.work(), p2, {.retry = always}),
                                 driver());

        ZEXPECT(p1.result == Result::Success);
        ZEXPECT(p2.result == Result::Success);
        ZEXPECT(manual.gate(a(1)).calls == 1);
    });
}

ZEST_CASE(WaiterWorkRunsOnce) {
    // A second waiter of a node mid-attempt waits on that attempt: its own
    // work never runs, and it gets the attempt's result.
    ManualRunner manual;
    manual.gate(a(1)).result = Result::Failed;
    bool other_ran = false;
    Work other{.run = [&other_ran](Attempt&) {
        other_ran = true;
        return reply(Result::Success);
    }};

    Probe p1, p2;
    execute([&]() -> async::task<> {
        auto driver = [&]() -> async::task<> {
            co_await manual.gate(a(1)).started.wait();
            manual.open({a(1)});
            co_return;
        };

        co_await async::when_all(run_join(a(1), manual.work(), p1),
                                 run_join(a(1), other, p2),
                                 driver());

        ZEXPECT(p1.result == Result::Failed);
        ZEXPECT(p2.result == Result::Failed);
        ZEXPECT(!other_ran);
        ZEXPECT(manual.gate(a(1)).calls == 1);
    });
}

ZEST_CASE(CrossKindEdge) {
    // A KindA node depending on a KindB node: the edge crosses kinds and
    // invalidate() cascades across it.
    Adjacency adj;
    adj[a(1)] = {b(7)};

    execute([&]() -> async::task<> {
        auto result = co_await graph.join(a(1), instant(adj), {.retry = always});
        ZEXPECT(result == Result::Success);
        ZEXPECT(ran.size() == 2u);
        ZEXPECT(graph.dependencies(a(1))[0] == b(7));

        auto dirtied = graph.invalidate(b(7));
        ZEXPECT(ranges::contains(dirtied, a(1)));
        ZEXPECT(graph.is_dirty(a(1)));
    });
}

/// ============================================================================
///                             Product checks
/// ============================================================================

ZEST_CASE(CheckHoldsSkipsAttempt) {
    // The check is asked only of a clean node, and Holds answers Success
    // without an attempt.
    Adjacency adj;
    int checks = 0;
    auto work = instant(adj);
    work.check = [&checks] {
        checks += 1;
        return Check::Holds;
    };

    execute([&]() -> async::task<> {
        co_await graph.join(a(1), work);
        ZEXPECT(checks == 0);
        ZEXPECT(ran.size() == 1u);

        auto result = co_await graph.join(a(1), work);
        ZEXPECT(result == Result::Success);
        ZEXPECT(checks == 1);
        ZEXPECT(ran.size() == 1u);
    });
}

ZEST_CASE(ProductGoneNoCascade) {
    // 1 -> 2: a waiter of 2 finding its product gone rebuilds 2 alone. Node
    // 1 consumed content that did not change: it stays clean, and a later
    // join of it runs nothing.
    Adjacency adj;
    adj[a(1)] = {a(2)};
    auto gone = instant(adj);
    gone.check = [] {
        return Check::ProductGone;
    };

    execute([&]() -> async::task<> {
        co_await graph.join(a(1), instant(adj));
        ZASSERT(ran.size() == std::size_t(2));

        auto rebuild = co_await graph.join(a(2), gone);
        ZEXPECT(rebuild == Result::Success);
        ZASSERT(ran.size() == std::size_t(3));
        ZEXPECT(ran.back() == a(2));
        ZEXPECT(!graph.is_dirty(a(2)));
        ZEXPECT(!graph.is_dirty(a(1)));

        auto hit = co_await graph.join(a(1), instant(adj));
        ZEXPECT(hit == Result::Success);
        ZEXPECT(ran.size() == std::size_t(3));
    });
}

ZEST_CASE(GenerationCountsInvalidations) {
    // 1 -> 2: every invalidation that reaches a node moves its generation
    // on, through a cascade too; a product found gone rebuilds it at the
    // generation it had.
    Adjacency adj;
    adj[a(1)] = {a(2)};
    auto gone = instant(adj);
    gone.check = [] {
        return Check::ProductGone;
    };

    execute([&]() -> async::task<> {
        co_await graph.join(a(1), instant(adj));
        ZEXPECT(graph.generation(a(1)) == 0u);

        graph.invalidate(a(1));
        ZEXPECT(graph.generation(a(1)) == 1u);
        ZEXPECT(graph.generation(a(2)) == 0u);

        graph.invalidate(a(2));
        ZEXPECT(graph.generation(a(2)) == 1u);
        ZEXPECT(graph.generation(a(1)) == 2u);

        co_await graph.join(a(1), instant(adj));
        co_await graph.join(a(1), gone);
        ZEXPECT(!graph.is_dirty(a(1)));
        ZEXPECT(graph.generation(a(1)) == 2u);
    });
}

ZEST_CASE(AddedNodeCountsInvalidations) {
    // A node added before any attempt counts the invalidations that reach
    // it, as one that never existed does not, and an attempt records the
    // generation it started at.
    graph.invalidate(a(1));
    ZEXPECT(graph.generation(a(1)) == 0u);
    graph.add(a(1));
    graph.invalidate(a(1));
    ZEXPECT(graph.generation(a(1)) == 1u);

    std::uint64_t started = 0;
    Work work{.run = [&](Attempt& attempt) -> async::task<Result> {
        started = attempt.generation();
        co_return Result::Success;
    }};
    execute([&]() -> async::task<> {
        co_await graph.join(a(1), work);
        ZEXPECT(started == 1u);
    });
}

ZEST_CASE(CheckInputsChangedPropagates) {
    // 1 -> 2: a waiter of 2 finding its inputs changed invalidates 2 with its
    // dependents — 2 rebuilds at once, 1 on its next join.
    Adjacency adj;
    adj[a(1)] = {a(2)};
    auto changed = instant(adj);
    changed.check = [] {
        return Check::InputsChanged;
    };

    execute([&]() -> async::task<> {
        co_await graph.join(a(1), instant(adj));
        ZASSERT(ran.size() == std::size_t(2));

        auto rebuild = co_await graph.join(a(2), changed);
        ZEXPECT(rebuild == Result::Success);
        ZASSERT(ran.size() == std::size_t(3));
        ZEXPECT(ran.back() == a(2));
        ZEXPECT(graph.is_dirty(a(1)));

        auto rerun = co_await graph.join(a(1), instant(adj));
        ZEXPECT(rerun == Result::Success);
        ZASSERT(ran.size() == std::size_t(4));
        ZEXPECT(ran.back() == a(1));
        ZEXPECT(ranges::count(ran, a(2)) == 2);
    });
}

ZEST_CASE(CheckSparesFinder) {
    // An attempt whose wait finds its dependency's inputs changed waits for
    // the fresh product: the invalidation overtakes what was built from the
    // old one, not that attempt.
    Adjacency none;
    auto changed = instant(none);
    changed.check = [] {
        return Check::InputsChanged;
    };
    Work first{
        .run = [this, changed](Attempt& attempt) { return wait_on(attempt, a(2), changed); }};

    execute([&]() -> async::task<> {
        ZASSERT(co_await graph.join(a(2), instant(none)) == Result::Success);
        auto result = co_await graph.join(a(1), first);
        ZEXPECT(result == Result::Success);
        ZEXPECT(ran == (std::vector<NodeId>{a(2), a(2), a(1)}));
        ZEXPECT(!graph.is_dirty(a(1)));
    });
}

ZEST_CASE(InvalidationJudgesEveryPath) {
    // An invalidation reaching an attempt first through a product it still
    // waits for, then through one it read, overtakes it.
    ManualRunner manual;
    manual.adj[a(1)] = {b(1), b(5)};
    manual.open({a(1), b(1)});

    Probe probe;
    execute([&]() -> async::task<> {
        ZASSERT(co_await graph.join(b(1), manual.work()) == Result::Success);
        graph.set_edges(b(1), {b(5)});

        auto driver = [&]() -> async::task<> {
            co_await manual.gate(b(5)).started.wait();
            graph.invalidate(b(5));
            manual.open({b(5)});
        };
        co_await async::when_all(run_join(a(1), manual.work(), probe), driver());
        ZEXPECT(probe.result == Result::Stale);
    });
}

ZEST_CASE(FailureReadOvertaken) {
    // A waiter read a failure as it reads a product: an invalidation of the
    // failed node overtakes it.
    ManualRunner manual;
    manual.gate(b(1)).result = Result::Failed;
    manual.open({b(1)});
    Work reader{.run = [this, &manual](Attempt& attempt) {
        return read_then_wait(attempt, b(1), b(5), manual.work());
    }};

    Probe probe;
    execute([&]() -> async::task<> {
        auto driver = [&]() -> async::task<> {
            co_await manual.gate(b(5)).started.wait();
            graph.invalidate(b(1));
            manual.open({b(5)});
        };
        co_await async::when_all(run_join(a(1), reader, probe), driver());
        ZEXPECT(probe.result == Result::Stale);
    });
}

ZEST_CASE(AbsentNodeOvertakes) {
    // An attempt that declared an edge to a node no attempt built read its
    // absence: an invalidation of that node overtakes it.
    ManualRunner manual;
    Work declarer{.run = [this, &manual](Attempt& attempt) {
        return declare_then_wait(attempt, b(9), b(5), manual.work());
    }};

    Probe probe;
    execute([&]() -> async::task<> {
        auto driver = [&]() -> async::task<> {
            co_await manual.gate(b(5)).started.wait();
            graph.invalidate(b(9));
            manual.open({b(5)});
        };
        co_await async::when_all(run_join(a(1), declarer, probe), driver());
        ZEXPECT(probe.result == Result::Stale);
    });
}

ZEST_CASE(InflightSkipsCheck) {
    // A waiter reaching a node mid-attempt joins that attempt: it is already
    // producing a fresh product, so the waiter's check is never asked and
    // nothing re-runs.
    ManualRunner manual;
    int checks = 0;
    auto gone = manual.work();
    gone.check = [&checks] {
        checks += 1;
        return Check::ProductGone;
    };

    Probe first, second;
    execute([&]() -> async::task<> {
        auto driver = [&]() -> async::task<> {
            co_await manual.gate(a(1)).started.wait();
            manual.open({a(1)});
            co_return;
        };

        co_await async::when_all(run_join(a(1), manual.work(), first),
                                 run_join(a(1), gone, second),
                                 driver());

        ZEXPECT(first.result == Result::Success);
        ZEXPECT(second.result == Result::Success);
        ZEXPECT(checks == 0);
        ZEXPECT(manual.gate(a(1)).calls == 1);
        ZEXPECT(!graph.is_dirty(a(1)));
    });
}

/// ============================================================================
///                     Edge publication (contract 4)
/// ============================================================================
///
/// An edge takes effect for invalidation and interest the moment depend()
/// records it; only a current successful attempt replaces the node's waited
/// edges, and an overtaken or failed attempt's waits are discarded.
/// set_edges() declares edges beside them without an attempt, and
/// invalidation follows both.

ZEST_CASE(EdgeLiveBeforeCommit) {
    // The dependent's attempt is still in flight when its dependency is
    // invalidated: the cascade must reach the dependent through the live
    // wait — the attempt never committed, so no waited edge exists yet.
    ManualRunner manual;
    manual.adj[a(1)] = {b(5)};
    manual.open({a(1)});

    Probe probe;
    execute([&]() -> async::task<> {
        auto driver = [&]() -> async::task<> {
            co_await manual.gate(b(5)).started.wait();
            manual.gate(b(5)).started.reset();

            auto dirtied = graph.invalidate(b(5));
            ZEXPECT(ranges::contains(dirtied, a(1)));

            co_await manual.gate(b(5)).started.wait();
            ZEXPECT(manual.gate(b(5)).calls == 2);
            manual.open({b(5)});
            co_return;
        };

        co_await async::when_all(run_join(a(1), manual.work(), probe, {.retry = always}), driver());

        ZEXPECT(probe.result == Result::Success);
        ZEXPECT(!graph.is_dirty(a(1)));
        ZEXPECT(!graph.is_dirty(b(5)));
    });
}

ZEST_CASE(SuccessReplacesEdges) {
    // A re-resolve that drops a dependency detaches its reverse edge once
    // the new attempt commits: invalidating the ex-dependency no longer
    // cascades.
    Adjacency adj;
    adj[a(1)] = {b(2)};

    execute([&]() -> async::task<> {
        co_await graph.join(a(1), instant(adj), {.retry = always});
        ZEXPECT(graph.dependencies(a(1))[0] == b(2));

        adj[a(1)] = {b(3)};
        graph.invalidate(a(1));
        co_await graph.join(a(1), instant(adj), {.retry = always});

        ZEXPECT(graph.dependencies(a(1))[0] == b(3));
        ZEXPECT(!ranges::contains(graph.invalidate(b(2)), a(1)));
        ZEXPECT(ranges::contains(graph.invalidate(b(3)), a(1)));
    });
}

ZEST_CASE(OvertakenWaitsDiscarded) {
    // An overtaken attempt's waits are discarded at commit: the edges still
    // reflect the last successful attempt, not the waits of the attempt
    // invalidate() overtook.
    ManualRunner manual;
    manual.adj[a(1)] = {b(2)};
    manual.open({a(1), b(2), b(3)});

    Probe probe;
    execute([&]() -> async::task<> {
        co_await graph.join(a(1), manual.work(), {.retry = always});
        ZEXPECT(graph.dependencies(a(1))[0] == b(2));

        // Flip the imports and drive a new attempt parked in its dispatch:
        // it has waited on b(3) but not committed. The observer joins
        // without retry so the overtaken attempt finishing leaves a
        // quiescent graph instead of starting another against the closed
        // gate.
        manual.adj[a(1)] = {b(3)};
        manual.gate(a(1)).proceed.reset();
        manual.gate(a(1)).started.reset();
        graph.invalidate(a(1));

        auto driver = [&]() -> async::task<> {
            co_await manual.gate(a(1)).started.wait();

            // The live wait is already cascade-visible, and the waited edge
            // from the last success still cascades too.
            ZEXPECT(ranges::contains(graph.invalidate(b(3)), a(1)));
            ZEXPECT(ranges::contains(graph.invalidate(b(2)), a(1)));
            co_return;
        };

        co_await async::when_all(run_join(a(1), manual.work(), probe), driver());

        // The overtaken attempt's waits were discarded: the edges still
        // point at b(2), and only it cascades.
        ZEXPECT(probe.result == Result::Stale);
        ZEXPECT(!graph.is_compiling(a(1)));
        ZEXPECT(graph.dependencies(a(1))[0] == b(2));
        ZEXPECT(!ranges::contains(graph.invalidate(b(3)), a(1)));
        ZEXPECT(ranges::contains(graph.invalidate(b(2)), a(1)));

        // A fresh terminal join commits the flipped imports.
        manual.open({a(1)});
        auto result = co_await graph.join(a(1), manual.work(), {.retry = always});
        ZEXPECT(result == Result::Success);
        ZEXPECT(graph.dependencies(a(1))[0] == b(3));
    });
}

ZEST_CASE(SetEdgesNoAttempt) {
    // set_edges() records edges without attempts or interest: the nodes
    // exist, the cascade reaches the declaring node, and nothing ever
    // compiles.
    execute([&]() -> async::task<> {
        graph.set_edges(a(2), {a(1)});
        ZEXPECT(graph.has_node(a(1)));
        ZEXPECT(graph.has_node(a(2)));
        ZEXPECT(graph.refcount(a(1)) == 0u);
        ZEXPECT(graph.refcount(a(2)) == 0u);
        ZEXPECT(!graph.is_compiling(a(2)));

        auto dirtied = graph.invalidate(a(1));
        ZEXPECT(ranges::contains(dirtied, a(1)));
        ZEXPECT(ranges::contains(dirtied, a(2)));
        co_return;
    });
}

ZEST_CASE(SetEdgesReplaces) {
    // A later set_edges() replaces the declared edges. An empty replacement
    // clears them but keeps the node: a consumer whose last import was
    // removed stops cascading yet stays tracked.
    execute([&]() -> async::task<> {
        graph.set_edges(a(3), {a(1)});
        graph.set_edges(a(3), {a(2)});
        ZEXPECT(!ranges::contains(graph.invalidate(a(1)), a(3)));
        ZEXPECT(ranges::contains(graph.invalidate(a(2)), a(3)));

        graph.set_edges(a(3), {});
        ZEXPECT(!ranges::contains(graph.invalidate(a(2)), a(3)));
        ZEXPECT(graph.has_node(a(3)));
        co_return;
    });
}

ZEST_CASE(SetEdgesDropsDuplicates) {
    // Broken input can name a dependency twice or the node itself: the
    // declared edges keep each dependency once and never the node.
    execute([&]() -> async::task<> {
        graph.set_edges(a(1), {a(2), a(2), a(1), a(3)});
        ZEXPECT(graph.dependencies(a(1)) == (llvm::SmallVector<NodeId>{a(2), a(3)}));
        co_return;
    });
}

ZEST_CASE(SuccessKeepsDeclared) {
    // A node with declared edges that then runs a successful attempt keeps
    // them: its waits stand beside the declaration, and invalidation
    // follows both.
    Adjacency adj;
    adj[a(2)] = {a(3)};

    execute([&]() -> async::task<> {
        graph.set_edges(a(2), {a(1)});
        auto result = co_await graph.join(a(2), instant(adj), {.retry = always});
        ZEXPECT(result == Result::Success);

        ZEXPECT(ranges::contains(graph.invalidate(a(1)), a(2)));
        ZEXPECT(ranges::contains(graph.invalidate(a(3)), a(2)));
    });
}

ZEST_CASE(FailedKeepsDeclared) {
    // A failed attempt discards its waits and leaves the declared edges
    // standing: a unit whose build breaks must stay cascade-reachable from
    // its declared imports, or fixing an import could never re-dirty it.
    ManualRunner manual;
    manual.adj[a(2)] = {a(3)};
    manual.gate(a(2)).result = Result::Failed;
    manual.open({a(2), a(3)});

    execute([&]() -> async::task<> {
        graph.set_edges(a(2), {a(1)});
        auto result = co_await graph.join(a(2), manual.work(), {.retry = always});
        ZEXPECT(result == Result::Failed);

        ZEXPECT(ranges::contains(graph.invalidate(a(1)), a(2)));
        ZEXPECT(!ranges::contains(graph.invalidate(a(3)), a(2)));
    });
}

ZEST_CASE(SetEdgesMidAttempt) {
    // set_edges() against an in-flight attempt stands through its commit: a
    // current Success adds its waits beside the declaration instead of
    // replacing it.
    ManualRunner manual;
    manual.adj[a(2)] = {a(3)};
    manual.open({a(3)});

    Probe probe;
    execute([&]() -> async::task<> {
        auto driver = [&]() -> async::task<> {
            co_await manual.gate(a(2)).started.wait();
            graph.set_edges(a(2), {a(1)});
            manual.open({a(2)});
            co_return;
        };

        co_await async::when_all(run_join(a(2), manual.work(), probe, {.retry = always}), driver());

        ZEXPECT(probe.result == Result::Success);
        ZEXPECT(ranges::contains(graph.invalidate(a(1)), a(2)));
        ZEXPECT(ranges::contains(graph.invalidate(a(3)), a(2)));
    });
}

ZEST_CASE(DependenciesUnion) {
    // A node's edges are its declared ones plus the waits of its last
    // successful attempt, each once: a failed attempt leaves the waited set
    // alone, a later success replaces it.
    ManualRunner manual;
    manual.adj[a(1)] = {a(2), a(3)};
    manual.open({a(1), a(2), a(3), a(4)});

    execute([&]() -> async::task<> {
        graph.set_edges(a(1), {a(2)});
        co_await graph.join(a(1), manual.work());
        ZEXPECT(graph.dependencies(a(1)) == (llvm::SmallVector<NodeId>{a(2), a(3)}));

        manual.adj[a(1)] = {a(4)};
        manual.gate(a(1)).result = Result::Failed;
        graph.invalidate(a(1));
        auto failed = co_await graph.join(a(1), manual.work());
        ZEXPECT(failed == Result::Failed);
        ZEXPECT(graph.dependencies(a(1)) == (llvm::SmallVector<NodeId>{a(2), a(3)}));

        manual.adj[a(1)].clear();
        manual.gate(a(1)).result = Result::Success;
        auto succeeded = co_await graph.join(a(1), manual.work());
        ZEXPECT(succeeded == Result::Success);
        ZEXPECT(graph.dependencies(a(1)) == (llvm::SmallVector<NodeId>{a(2)}));
    });
}

/// ============================================================================
///                 Attempt identity filter & run-to-reply
/// ============================================================================
///
/// The graph never destroys a running attempt; an overtaken attempt runs to
/// its real reply, and whatever it reports arrives as Stale.

ZEST_CASE(StaleSuccessDiscarded) {
    // The work ignores its token and reports Success after invalidate()
    // overtook the attempt: the result must not count — the node stays
    // dirty and the waiter drives a fresh attempt.
    ManualRunner manual;
    manual.stubborn = true;

    Probe probe;
    execute([&]() -> async::task<> {
        auto driver = [&]() -> async::task<> {
            co_await manual.gate(a(1)).started.wait();
            manual.gate(a(1)).started.reset();

            graph.invalidate(a(1));

            // Advisory cancellation: the attempt is signalled, not killed.
            ZEXPECT(graph.is_compiling(a(1)));

            manual.open({a(1)});
            co_await manual.gate(a(1)).started.wait();
            ZEXPECT(manual.gate(a(1)).calls == 2);
            co_return;
        };

        co_await async::when_all(run_join(a(1), manual.work(), probe, {.retry = always}), driver());

        ZEXPECT(probe.result == Result::Success);
        ZEXPECT(manual.gate(a(1)).calls == 2);
        ZEXPECT(!graph.is_dirty(a(1)));
    });
}

ZEST_CASE(StaleFailureRetries) {
    // A Failed reply from an overtaken attempt is no decision about the new
    // content: waiters retry instead of propagating the failure.
    ManualRunner manual;
    manual.stubborn = true;
    manual.gate(a(1)).result = Result::Failed;

    Probe probe;
    execute([&]() -> async::task<> {
        auto driver = [&]() -> async::task<> {
            co_await manual.gate(a(1)).started.wait();
            manual.gate(a(1)).started.reset();

            graph.invalidate(a(1));
            manual.gate(a(1)).result = Result::Success;
            manual.open({a(1)});

            co_await manual.gate(a(1)).started.wait();
            co_return;
        };

        co_await async::when_all(run_join(a(1), manual.work(), probe, {.retry = always}), driver());

        // The stale Failed never reached the waiter.
        ZEXPECT(probe.result == Result::Success);
        ZEXPECT(manual.gate(a(1)).calls == 2);
    });
}

ZEST_CASE(SalvageBeforeStale) {
    // An attempt may publish side effects and then report Stale (bounded-
    // stale publication): the graph has no say over what happened before
    // the report — the side effect stands, and the join retries.
    int calls = 0;
    int salvaged = 0;
    Work work{.run = [&](Attempt&) {
        calls += 1;
        if(calls == 1) {
            salvaged += 1;
            return reply(Result::Stale);
        }
        return reply(Result::Success);
    }};

    execute([&]() -> async::task<> {
        auto result = co_await graph.join(a(1), work, {.retry = always});
        ZEXPECT(result == Result::Success);
        ZEXPECT(calls == 2);
        ZEXPECT(salvaged == 1);
    });
}

/// ============================================================================
///                               Join retry
/// ============================================================================

ZEST_CASE(OneAttemptStale) {
    // A join without retry observes exactly one attempt: an overtaken
    // attempt returns Stale to the caller instead of retrying.
    ManualRunner manual;

    Probe probe;
    execute([&]() -> async::task<> {
        auto driver = [&]() -> async::task<> {
            co_await manual.gate(a(1)).started.wait();
            graph.invalidate(a(1));
            co_return;
        };

        co_await async::when_all(run_join(a(1), manual.work(), probe), driver());

        ZEXPECT(probe.result == Result::Stale);
        ZEXPECT(manual.gate(a(1)).calls == 1);
        ZEXPECT(graph.is_dirty(a(1)));
    });
}

ZEST_CASE(OneAttemptClean) {
    // A join without retry on a clean node succeeds without an attempt.
    Adjacency adj;

    execute([&]() -> async::task<> {
        co_await graph.join(a(1), instant(adj), {.retry = always});
        ZEXPECT(ran.size() == 1u);

        auto result = co_await graph.join(a(1), instant(adj));
        ZEXPECT(result == Result::Success);
        ZEXPECT(ran.size() == 1u);
    });
}

ZEST_CASE(RetryPredicate) {
    // The predicate is asked after each Stale attempt, not when the join
    // starts: one that turned false while its join waited ends it Stale,
    // while a join whose predicate holds drives a second attempt.
    ManualRunner manual;
    bool valid = true;

    Probe declined, retried;
    execute([&]() -> async::task<> {
        auto driver = [&]() -> async::task<> {
            co_await manual.gate(a(1)).started.wait();
            manual.gate(a(1)).started.reset();
            valid = false;
            graph.invalidate(a(1));

            co_await manual.gate(a(1)).started.wait();
            manual.open({a(1)});
            co_return;
        };

        co_await async::when_all(
            run_join(a(1), manual.work(), declined, {.retry = [&] { return valid; }}),
            run_join(a(1), manual.work(), retried, {.retry = always}),
            driver());

        ZEXPECT(declined.result == Result::Stale);
        ZEXPECT(retried.result == Result::Success);
        ZEXPECT(manual.gate(a(1)).calls == 2);
    });
}

/// ============================================================================
///                            Failure semantics
/// ============================================================================

ZEST_CASE(FailedPropagates) {
    // A failing dependency fails the depender without dispatching it; both
    // stay dirty.
    ManualRunner manual;
    manual.adj[a(1)] = {b(2)};
    manual.gate(b(2)).result = Result::Failed;
    manual.open({a(1), b(2)});

    execute([&]() -> async::task<> {
        auto result = co_await graph.join(a(1), manual.work(), {.retry = always});
        ZEXPECT(result == Result::Failed);
        ZEXPECT(manual.gate(b(2)).calls == 1);
        ZEXPECT(manual.gate(a(1)).calls == 0);
        ZEXPECT(graph.is_dirty(a(1)));
        ZEXPECT(graph.is_dirty(b(2)));
    });
}

ZEST_CASE(FailureNotSticky) {
    // Failure propagates without retry, but a new join tries again and
    // succeeds once the dependency compiles.
    ManualRunner manual;
    manual.adj[a(1)] = {b(2)};
    manual.gate(b(2)).result = Result::Failed;
    manual.open({a(1), b(2)});

    execute([&]() -> async::task<> {
        auto first = co_await graph.join(a(1), manual.work(), {.retry = always});
        ZEXPECT(first == Result::Failed);

        manual.gate(b(2)).result = Result::Success;
        auto second = co_await graph.join(a(1), manual.work(), {.retry = always});
        ZEXPECT(second == Result::Success);
        ZEXPECT(manual.gate(b(2)).calls == 2);
        ZEXPECT(manual.gate(a(1)).calls == 1);
    });
}

/// ============================================================================
///                        Interest & cancellation
/// ============================================================================

ZEST_CASE(WaiterCancelReleases) {
    // The waiter's frame unwinds mid-attempt: interest drops to zero, the
    // advisory token fires after one tick, the work winds down and the
    // graph quiesces with the node still dirty.
    ManualRunner manual;

    Probe probe;
    execute([&]() -> async::task<> {
        auto driver = [&]() -> async::task<> {
            co_await manual.gate(a(1)).started.wait();
            probe.source.cancel();
            co_await graph.attempt_finished(a(1));

            ZEXPECT(probe.done);
            ZEXPECT(probe.result == std::nullopt);
            ZEXPECT(graph.is_dirty(a(1)));
            ZEXPECT(graph.refcount(a(1)) == 0u);
            ZEXPECT(manual.gate(a(1)).calls == 1);
            co_return;
        };

        co_await async::when_all(run_join(a(1), manual.work(), probe, {.retry = always}), driver());
    });
}

ZEST_CASE(SharedDependencySurvivesCancel) {
    // Two chains share one dependency; cancelling one waiter must only wind
    // down its own chain — the shared dependency keeps compiling for the
    // survivor.
    ManualRunner manual;
    manual.adj[a(1)] = {b(5)};
    manual.adj[a(3)] = {b(5)};
    manual.open({a(1), a(3)});

    Probe p1, p3;
    execute([&]() -> async::task<> {
        auto driver = [&]() -> async::task<> {
            co_await manual.gate(b(5)).started.wait();
            ZEXPECT(graph.refcount(b(5)) == 2u);

            p1.source.cancel();
            co_await graph.attempt_finished(a(1));

            ZEXPECT(graph.is_compiling(b(5)));
            ZEXPECT(manual.gate(b(5)).calls == 1);
            ZEXPECT(graph.refcount(b(5)) == 1u);

            manual.open({b(5)});
            co_return;
        };

        co_await async::when_all(run_join(a(1), manual.work(), p1, {.retry = always}),
                                 run_join(a(3), manual.work(), p3, {.retry = always}),
                                 driver());

        ZEXPECT(p1.result == std::nullopt);
        ZEXPECT(p3.result == Result::Success);
        ZEXPECT(manual.gate(b(5)).calls == 1);
    });
}

ZEST_CASE(TransientDropHandover) {
    // The depender is invalidated while waiting on its unchanged dependency:
    // the retry re-acquires the dependency within the same drain cycle, so
    // its in-flight attempt is handed over — neither cancelled nor
    // restarted.
    ManualRunner manual;
    manual.adj[a(1)] = {b(2)};
    manual.open({a(1)});

    Probe probe;
    execute([&]() -> async::task<> {
        auto driver = [&]() -> async::task<> {
            co_await manual.gate(b(2)).started.wait();
            ZEXPECT(graph.is_compiling(a(1)));

            graph.invalidate(a(1));
            co_await async::yield();

            // The depender's attempt was restarted; the dependency kept
            // compiling throughout.
            ZEXPECT(graph.is_compiling(b(2)));
            ZEXPECT(manual.gate(b(2)).calls == 1);
            ZEXPECT(graph.refcount(b(2)) == 1u);

            manual.open({b(2)});
            co_return;
        };

        co_await async::when_all(run_join(a(1), manual.work(), probe, {.retry = always}), driver());

        ZEXPECT(probe.result == Result::Success);
        ZEXPECT(manual.gate(b(2)).calls == 1);
        ZEXPECT(!graph.is_dirty(a(1)));
        ZEXPECT(!graph.is_dirty(b(2)));
    });
}

/// ============================================================================
///                               Foreground
/// ============================================================================

ZEST_CASE(ForegroundLateJoin) {
    // A foreground waiter joins a Low attempt that then reports Stale: the
    // retry re-reads the interest class, so it dispatches foreground instead
    // of staying cancellable Low.
    async::event started;
    async::event proceed;
    int calls = 0;
    std::vector<bool> classes;
    Work work{.run = [&](Attempt& attempt) {
        calls += 1;
        classes.push_back(attempt.foreground());
        if(calls == 1) {
            started.set();
            return reply_after(proceed, Result::Stale);
        }
        return reply(Result::Success);
    }};

    Probe background, foreground;
    execute([&]() -> async::task<> {
        auto driver = [&]() -> async::task<> {
            co_await started.wait();
            proceed.set();
            co_return;
        };

        // The foreground join arrives while the first attempt is parked
        // inside its dispatch.
        co_await async::when_all(
            run_join(a(1), work, background, {.retry = always}),
            run_join(a(1), work, foreground, {.foreground = true, .retry = always}),
            driver());

        ZEXPECT(background.result == Result::Success);
        ZEXPECT(foreground.result == Result::Success);
        ZASSERT(calls == 2);
        ZEXPECT(!classes[0]);
        ZEXPECT(classes[1]);
    });
}

ZEST_CASE(ForegroundRaisesRunning) {
    // Foreground interest joining an attempt already running at Low tells
    // it once, however many join: its request is raised where it is.
    async::event started;
    async::event proceed;
    Signal<>::Connection connection;
    int raised = 0;
    Work work{.run = [&](Attempt& attempt) {
        connection = attempt.on_foreground().connect([&] { raised += 1; });
        started.set();
        return reply_after(proceed, Result::Success);
    }};

    Probe background, first, second;
    execute([&]() -> async::task<> {
        auto driver = [&]() -> async::task<> {
            co_await started.wait();
            proceed.set();
        };

        co_await async::when_all(run_join(a(1), work, background),
                                 run_join(a(1), work, first, {.foreground = true}),
                                 run_join(a(1), work, second, {.foreground = true}),
                                 driver());

        ZEXPECT(background.result == Result::Success);
        ZEXPECT(first.result == Result::Success);
        ZEXPECT(second.result == Result::Success);
        ZEXPECT(raised == 1);
    });
}

ZEST_CASE(ForegroundSpreadsEdges) {
    // Foreground must travel the live waits: a foreground join at the root
    // upgrades a deep dependency already parked in a Low attempt, so its
    // preempted retry dispatches foreground.
    async::event started;
    async::event proceed;
    int deep_calls = 0;
    std::vector<bool> deep_classes;
    Adjacency adj;
    adj[a(1)] = {a(2)};
    adj[a(2)] = {a(3)};
    auto work = after_deps(adj, [&](Attempt& attempt) {
        if(attempt.node() != a(3)) {
            return reply(Result::Success);
        }
        deep_calls += 1;
        deep_classes.push_back(attempt.foreground());
        if(deep_calls == 1) {
            started.set();
            return reply_after(proceed, Result::Stale);
        }
        return reply(Result::Success);
    });

    Probe background, foreground;
    execute([&]() -> async::task<> {
        auto driver = [&]() -> async::task<> {
            co_await started.wait();
            proceed.set();
            co_return;
        };

        // The background join parks a(3); the foreground join then arrives
        // at the root and must upgrade through two live waits.
        co_await async::when_all(
            run_join(a(2), work, background, {.retry = always}),
            run_join(a(1), work, foreground, {.foreground = true, .retry = always}),
            driver());

        ZEXPECT(background.result == Result::Success);
        ZEXPECT(foreground.result == Result::Success);
        ZASSERT(deep_calls == 2);
        ZEXPECT(!deep_classes[0]);
        ZEXPECT(deep_classes[1]);
    });
}

ZEST_CASE(ForegroundSkipsCleanDependencies) {
    // A foreground join answered by a clean cached chain must not tag the
    // chain's settled edges: no join ever acquires a clean dependency, so
    // nothing would reset the mark, and an unrelated background rebuild
    // much later would dispatch at foreground class.
    std::vector<bool> dep_classes;
    Adjacency adj;
    adj[a(1)] = {a(2)};
    auto work = after_deps(adj, [&](Attempt& attempt) {
        if(attempt.node() == a(2)) {
            dep_classes.push_back(attempt.foreground());
        }
        return reply(Result::Success);
    });

    Probe warm, hit, rebuild;
    execute([&]() -> async::task<> {
        // Build the chain clean at Low, then answer a foreground join from
        // the clean root outright.
        co_await run_join(a(1), work, warm, {.retry = always});
        co_await run_join(a(1), work, hit, {.foreground = true, .retry = always});

        graph.invalidate(a(2));
        co_await run_join(a(2), work, rebuild, {.retry = always});

        ZEXPECT(rebuild.result == Result::Success);
        ZASSERT(dep_classes.size() == std::size_t(2));
        ZEXPECT(!dep_classes[0]);
        ZEXPECT(!dep_classes[1]);
    });
}

ZEST_CASE(ForegroundResetsZero) {
    // The foreground flag is sticky while any interest remains and reset
    // when the count returns to zero: a later background join runs Low.
    ManualRunner manual;
    manual.open({a(1)});

    execute([&]() -> async::task<> {
        co_await graph.join(a(1), manual.work(), {.foreground = true, .retry = always});
        ZEXPECT(manual.gate(a(1)).classes[0]);

        graph.invalidate(a(1));
        co_await graph.join(a(1), manual.work(), {.retry = always});
        ZASSERT(manual.gate(a(1)).calls == 2);
        ZEXPECT(!manual.gate(a(1)).classes[1]);
    });
}

/// ============================================================================
///                             Cycle handling
/// ============================================================================

ZEST_CASE(SelfDependFails) {
    // A node depending on itself fails immediately.
    Adjacency adj;
    adj[a(1)] = {a(1)};

    execute([&]() -> async::task<> {
        auto result = co_await graph.join(a(1), instant(adj), {.retry = always});
        ZEXPECT(result == Result::Failed);
    });
}

ZEST_CASE(DependCycleFails) {
    // 1 -> 2 -> 1: the depend that would close the wait loop detects it and
    // fails the attempt instead of deadlocking.
    Adjacency adj;
    adj[a(1)] = {a(2)};
    adj[a(2)] = {a(1)};

    execute([&]() -> async::task<> {
        auto result = co_await graph.join(a(1), instant(adj), {.retry = always});
        ZEXPECT(result == Result::Failed);
    });
}

ZEST_CASE(InvalidateIntroducesCycle) {
    // The cycle only appears after an invalidate() follows a change of the
    // imports; the retry detects it and fails instead of hanging.
    Adjacency adj;
    adj[a(1)] = {a(2)};

    execute([&]() -> async::task<> {
        auto first = co_await graph.join(a(1), instant(adj), {.retry = always});
        ZEXPECT(first == Result::Success);

        adj[a(2)] = {a(1)};
        graph.invalidate(a(2));

        auto second = co_await graph.join(a(1), instant(adj), {.retry = always});
        ZEXPECT(second == Result::Failed);
    });
}

/// ============================================================================
///                                Shutdown
/// ============================================================================

ZEST_CASE(ShutdownWithInflight) {
    // shutdown() with attempts in flight: every advisory token fires, the
    // work winds down with real replies, pending joins end Stale even when
    // they would retry, and the graph quiesces.
    ManualRunner manual;
    manual.adj[a(1)] = {b(4)};

    Probe probe;
    auto driver = [&]() -> async::task<> {
        co_await manual.gate(b(4)).started.wait();
        co_await graph.shutdown();
        co_return;
    };

    run(run_join(a(1), manual.work(), probe, {.retry = always}), driver());

    ZEXPECT(probe.done);
    ZEXPECT(probe.result == Result::Stale);
    ZEXPECT(graph.idle());
}

/// ============================================================================
///                            Randomized stress
/// ============================================================================

ZEST_CASE(RandomizedStress) {
    // A fixed-seed, single-threaded interleaving of joins, cancellations,
    // invalidations and dispatch completions. Every dispatch races a
    // semaphore against its advisory token — the run-to-reply discipline.
    // The graph's own assertions, built into Debug, hold at every step.
    async::semaphore permits{0};
    Adjacency adj;
    adj[a(1)] = {a(2), a(3)};
    adj[a(2)] = {b(4)};
    adj[a(3)] = {b(4)};
    adj[b(4)] = {b(5)};
    adj[a(6)] = {b(4), a(7)};
    adj[a(7)] = {b(5)};
    adj[a(8)] = {a(6)};
    auto work =
        after_deps(adj, [&permits](Attempt& attempt) { return take_permit(permits, attempt); });

    execute([&]() -> async::task<> {
        std::mt19937 rng(20260826u);
        std::vector<std::unique_ptr<Probe>> probes;
        async::task_group<> inflight;

        const NodeId roots[] = {a(1), a(6), a(8)};
        const NodeId all[] = {a(1), a(2), a(3), b(4), b(5), a(6), a(7), a(8)};

        for(int step = 0; step < 200; step += 1) {
            switch(rng() % 4) {
                case 0: {
                    auto& probe = probes.emplace_back(std::make_unique<Probe>());
                    inflight.spawn(run_join(roots[rng() % 3], work, *probe, {.retry = always}));
                    break;
                }
                case 1: {
                    if(!probes.empty()) {
                        probes[rng() % probes.size()]->source.cancel();
                    }
                    break;
                }
                case 2: {
                    graph.invalidate(all[rng() % 8]);
                    break;
                }
                case 3: {
                    // Let one pending dispatch finish.
                    permits.release();
                    break;
                }
            }

            // Let deferred unwinds finish.
            co_await async::yield();
        }

        // Drain: cancel every outstanding join and wait for them all.
        for(auto& probe: probes) {
            probe->source.cancel();
        }
        co_await inflight.join();

        // With no interest left, every attempt winds down on its own, before
        // the shutdown would make it.
        for(auto id: graph.compiling()) {
            co_await graph.attempt_finished(id);
        }
        ZEXPECT(graph.idle());
    });
}

};  // ZEST_SUITE(TaskGraph)

}  // namespace
}  // namespace clice::testing
