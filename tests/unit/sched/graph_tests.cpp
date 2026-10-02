#include <optional>
#include <random>

#include "test/async.h"
#include "test/test.h"
#include "sched/graph.h"

namespace clice::testing {
namespace {

namespace ranges = std::ranges;

/// Two arbitrary families exercising the multi-family surface.
constexpr Family FamA = Family{1};
constexpr Family FamB = Family{2};

NodeId a(std::uint64_t key) {
    return {FamA, key};
}

NodeId b(std::uint64_t key) {
    return {FamB, key};
}

using Adjacency = llvm::DenseMap<NodeId, llvm::SmallVector<NodeId>>;

/// A cancellable request join and its observed terminal state.
/// outcome stays empty while running and after requester cancellation.
struct Probe {
    kota::cancellation_source source;
    std::optional<JoinOutcome> outcome;
    bool done = false;
};

ZEST_SUITE(TaskGraph) {

std::vector<NodeId> ran;
std::optional<kota::event_loop> loop;
std::optional<TaskGraph> graph;

void make_graph() {
    ran.clear();
    loop.emplace();
    graph.emplace();
}

/// Runner that resolves dependencies from a shared adjacency, then
/// records the run and succeeds.
TaskGraph::RoundRunner instant(const Adjacency& adj) {
    return [this, &adj](RoundContext& ctx, NodeId id) -> kota::task<RoundOutcome> {
        if(auto it = adj.find(id); it != adj.end()) {
            for(auto dep: it->second) {
                switch(co_await ctx.depend(dep)) {
                    case DependResult::Ready: break;
                    case DependResult::Failed: co_return RoundOutcome::Failed;
                    case DependResult::Cancelled: co_return RoundOutcome::Stale;
                }
            }
        }
        ran.push_back(id);
        co_return RoundOutcome::Success;
    };
}

/// Runner driven manually by per-node gates: the test observes when a
/// round reaches its dispatch (started) and decides when and how it
/// completes. The dispatch races its gate against the round's advisory
/// token — the run-to-reply discipline every real family follows.
struct ManualFamily {
    struct Gate {
        kota::event started;
        kota::event proceed;
        RoundOutcome result = RoundOutcome::Success;
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

    TaskGraph::RoundRunner runner() {
        return [this](RoundContext& ctx, NodeId id) -> kota::task<RoundOutcome> {
            if(auto it = adj.find(id); it != adj.end()) {
                for(auto dep: it->second) {
                    switch(co_await ctx.depend(dep)) {
                        case DependResult::Ready: break;
                        case DependResult::Failed: co_return RoundOutcome::Failed;
                        case DependResult::Cancelled: co_return RoundOutcome::Stale;
                    }
                }
            }

            auto& g = gate(id);
            g.calls += 1;
            g.classes.push_back(ctx.foreground());
            g.started.set();

            if(stubborn) {
                co_await g.proceed.wait();
                co_return g.result;
            }

            auto proceed = [&]() -> kota::task<> {
                co_await g.proceed.wait();
            };
            auto waited = co_await kota::with_token(proceed(), ctx.token());
            if(waited.is_cancelled()) {
                co_return RoundOutcome::Stale;
            }
            co_return g.result;
        };
    }
};

/// Run the test body, then verify the shutdown protocol: firing every
/// advisory token and joining must exit cleanly and leave the graph fully
/// quiesced (no compiling residue, no held interest, every completion
/// fired).
template <typename F>
void execute(F&& fn) {
    auto wrapper = [&]() -> kota::task<> {
        co_await fn();
        co_await graph->shutdown();
        EXPECT(graph->idle());
    };
    auto t = wrapper();
    loop->schedule(t);
    loop->run();
}

kota::task<> run_request(NodeId id, Probe& probe, JoinOptions options = {}) {
    auto result =
        co_await kota::with_token(graph->request(id, std::move(options)), probe.source.token());
    probe.done = true;
    if(result.has_value()) {
        probe.outcome = *result;
    }
}

/// ============================================================================
///                           Basic rounds & joins
/// ============================================================================

ZEST_CASE(request_no_deps) {
    // A node without dependencies runs one round and becomes clean.
    make_graph();
    Adjacency adj;
    graph->register_family(FamA, instant(adj));

    execute([&]() -> kota::task<> {
        auto outcome = co_await graph->request(a(1));
        EXPECT(outcome == JoinOutcome::Success);
        EXPECT(ran.size() == 1u);
        EXPECT(!graph->is_dirty(a(1)));
    });
}

ZEST_CASE(request_dep_chain) {
    // 1 -> 2 -> 3: rounds land bottom-up along declared edges, and the
    // durable edge sets reflect the declarations.
    make_graph();
    Adjacency adj;
    adj[a(1)] = {a(2)};
    adj[a(2)] = {a(3)};
    graph->register_family(FamA, instant(adj));

    execute([&]() -> kota::task<> {
        auto outcome = co_await graph->request(a(1));
        EXPECT(outcome == JoinOutcome::Success);
        EXPECT(ran.size() == 3u);
        EXPECT(ranges::find(ran, a(3)) < ranges::find(ran, a(2)));
        EXPECT(ranges::find(ran, a(2)) < ranges::find(ran, a(1)));
        EXPECT(graph->dependencies(a(1)).size() == 1u);
        EXPECT(graph->dependencies(a(2)).size() == 1u);
    });
}

ZEST_CASE(diamond_dedup) {
    // Diamond 1 -> {2, 3} -> 4: the shared dependency is reached through
    // two branches but runs exactly once.
    make_graph();
    Adjacency adj;
    adj[a(1)] = {a(2), a(3)};
    adj[a(2)] = {a(4)};
    adj[a(3)] = {a(4)};
    graph->register_family(FamA, instant(adj));

    execute([&]() -> kota::task<> {
        auto outcome = co_await graph->request(a(1));
        EXPECT(outcome == JoinOutcome::Success);
        EXPECT(ranges::count(ran, a(4)) == 1);
        EXPECT(!graph->is_dirty(a(4)));
    });
}

ZEST_CASE(second_request_skips) {
    // A clean node is not re-run by a later request.
    make_graph();
    Adjacency adj;
    graph->register_family(FamA, instant(adj));

    execute([&]() -> kota::task<> {
        co_await graph->request(a(1));
        EXPECT(ran.size() == 1u);
        co_await graph->request(a(1));
        EXPECT(ran.size() == 1u);
    });
}

ZEST_CASE(reference_survives_landing) {
    // reference() records a candidate edge to a node that never runs: it
    // must survive the successful landing (candidates replace the durable
    // set) so a later update on the referenced node re-dirties this one.
    make_graph();
    NodeId sentinel{FamB, (1ull << 63) | 42};
    graph->register_family(FamA, [&](RoundContext& ctx, NodeId) -> kota::task<RoundOutcome> {
        ctx.reference(sentinel);
        ran.push_back(a(1));
        co_return RoundOutcome::Success;
    });

    Probe probe;
    execute([&]() -> kota::task<> {
        co_await run_request(a(1), probe);
        CO_ASSERT(probe.outcome == JoinOutcome::Success);

        auto dirtied = graph->update(sentinel);
        EXPECT(std::ranges::find(dirtied, a(1)) != dirtied.end());
        EXPECT(graph->is_dirty(a(1)));
    });
}

ZEST_CASE(artifact_dirty_no_cascade) {
    // The eviction tier: the marked node alone rebuilds on next demand.
    // Dependents stay clean — they consumed the content, which did not
    // change — so a later request through them touches nothing.
    make_graph();
    Adjacency adj;
    adj[a(1)] = {a(2)};
    graph->register_family(FamA, instant(adj));

    Probe warm, hit, rebuild;
    execute([&]() -> kota::task<> {
        co_await run_request(a(1), warm);
        CO_ASSERT(ran.size() == std::size_t(2));

        graph->mark_dirty(a(2));
        CO_ASSERT(graph->is_dirty(a(2)));
        CO_ASSERT(!graph->is_dirty(a(1)));

        co_await run_request(a(1), hit);
        CO_ASSERT(ran.size() == std::size_t(2));

        co_await run_request(a(2), rebuild);
        CO_ASSERT(ran.size() == std::size_t(3));
        EXPECT(ran.back() == a(2));
        EXPECT(!graph->is_dirty(a(2)));
    });
    EXPECT(warm.outcome == JoinOutcome::Success);
    EXPECT(hit.outcome == JoinOutcome::Success);
    EXPECT(rebuild.outcome == JoinOutcome::Success);
}

ZEST_CASE(artifact_dirty_inflight_lands) {
    // Marking while a round is in flight neither voids nor re-runs it:
    // the round is already producing the fresh artifact, and its landing
    // clears the flag it finds set.
    make_graph();
    ManualFamily mf;
    graph->register_family(FamA, mf.runner());

    Probe probe;
    execute([&]() -> kota::task<> {
        auto driver = [&]() -> kota::task<> {
            co_await mf.gate(a(1)).started.wait();
            graph->mark_dirty(a(1));
            mf.open({a(1)});
            co_return;
        };

        co_await kota::when_all(run_request(a(1), probe), driver());

        EXPECT(probe.outcome == JoinOutcome::Success);
        EXPECT(mf.gate(a(1)).calls == 1);
        EXPECT(!graph->is_dirty(a(1)));
    });
}

ZEST_CASE(concurrent_requests_share) {
    // Two concurrent requests for the same node join one round: a single
    // dispatch serves both.
    make_graph();
    ManualFamily mf;
    graph->register_family(FamA, mf.runner());

    Probe p1, p2;
    execute([&]() -> kota::task<> {
        auto driver = [&]() -> kota::task<> {
            co_await mf.gate(a(1)).started.wait();
            EXPECT(graph->refcount(a(1)) == 2u);
            mf.open({a(1)});
            co_return;
        };

        co_await kota::when_all(run_request(a(1), p1), run_request(a(1), p2), driver());

        EXPECT(p1.outcome == JoinOutcome::Success);
        EXPECT(p2.outcome == JoinOutcome::Success);
        EXPECT(mf.gate(a(1)).calls == 1);
    });
}

ZEST_CASE(cross_family_edge) {
    // A FamA node depending on a FamB node: the edge crosses families and
    // update() cascades across it.
    make_graph();
    Adjacency adj;
    adj[a(1)] = {b(7)};
    graph->register_family(FamA, instant(adj));
    graph->register_family(FamB, instant(adj));

    execute([&]() -> kota::task<> {
        auto outcome = co_await graph->request(a(1));
        EXPECT(outcome == JoinOutcome::Success);
        EXPECT(ran.size() == 2u);
        EXPECT(graph->dependencies(a(1))[0] == b(7));

        auto dirtied = graph->update(b(7));
        EXPECT(ranges::contains(dirtied, a(1)));
        EXPECT(graph->is_dirty(a(1)));
    });
}

/// ============================================================================
///                     Edge publication (contract 14)
/// ============================================================================
///
/// An edge takes effect for cascade and interest the moment depend()
/// records it; only a current successful round replaces the durable set,
/// and overtaken rounds' candidates are discarded. declare() commits
/// facade-known topology into the same durable set without a round.

ZEST_CASE(edge_live_before_landing) {
    // The dependent's round is still in flight when its dependency is
    // updated: the cascade must reach the dependent through the candidate
    // edge — the round never landed, so no durable edge exists yet.
    make_graph();
    ManualFamily mf;
    mf.adj[a(1)] = {b(5)};
    graph->register_family(FamA, mf.runner());
    graph->register_family(FamB, mf.runner());
    mf.open({a(1)});

    Probe probe;
    execute([&]() -> kota::task<> {
        auto driver = [&]() -> kota::task<> {
            co_await mf.gate(b(5)).started.wait();
            mf.gate(b(5)).started.reset();

            auto dirtied = graph->update(b(5));
            EXPECT(ranges::contains(dirtied, a(1)));

            co_await mf.gate(b(5)).started.wait();
            EXPECT(mf.gate(b(5)).calls == 2);
            mf.open({b(5)});
            co_return;
        };

        co_await kota::when_all(run_request(a(1), probe), driver());

        EXPECT(probe.outcome == JoinOutcome::Success);
        EXPECT(!graph->is_dirty(a(1)));
        EXPECT(!graph->is_dirty(b(5)));
    });
}

ZEST_CASE(success_replaces_edges) {
    // A re-resolve that drops a dependency detaches its reverse edge once
    // the new round lands: updating the ex-dependency no longer cascades.
    make_graph();
    Adjacency adj;
    adj[a(1)] = {b(2)};
    graph->register_family(FamA, instant(adj));
    graph->register_family(FamB, instant(adj));

    execute([&]() -> kota::task<> {
        co_await graph->request(a(1));
        EXPECT(graph->dependencies(a(1))[0] == b(2));

        adj[a(1)] = {b(3)};
        graph->update(a(1));
        co_await graph->request(a(1));

        EXPECT(graph->dependencies(a(1))[0] == b(3));
        EXPECT(!ranges::contains(graph->update(b(2)), a(1)));
        EXPECT(ranges::contains(graph->update(b(3)), a(1)));
    });
}

ZEST_CASE(voided_candidates_discarded) {
    // An overtaken round's candidate edges are discarded at landing: the
    // durable set still reflects the last successful round, not the
    // declarations of the round that update() voided.
    make_graph();
    ManualFamily mf;
    mf.adj[a(1)] = {b(2)};
    graph->register_family(FamA, mf.runner());
    graph->register_family(FamB, mf.runner());
    mf.open({a(1), b(2), b(3)});

    Probe probe;
    execute([&]() -> kota::task<> {
        co_await graph->request(a(1));
        EXPECT(graph->dependencies(a(1))[0] == b(2));

        // Flip the imports and drive a new round parked in its dispatch:
        // it has declared the candidate edge to b(3) but not landed. The
        // observer joins OneAttempt so the voided round's landing leaves a
        // quiescent graph instead of respawning against the closed gate.
        mf.adj[a(1)] = {b(3)};
        mf.gate(a(1)).proceed.reset();
        mf.gate(a(1)).started.reset();
        graph->update(a(1));

        auto driver = [&]() -> kota::task<> {
            co_await mf.gate(a(1)).started.wait();

            // The candidate edge is already cascade-visible, and the
            // durable edge from the last success still cascades too.
            EXPECT(ranges::contains(graph->update(b(3)), a(1)));
            EXPECT(ranges::contains(graph->update(b(2)), a(1)));
            co_return;
        };

        co_await kota::when_all(run_request(a(1), probe, {.flavor = JoinFlavor::OneAttempt}),
                                driver());

        // The void discarded the candidates: the durable set still points
        // at b(2), and only it cascades.
        EXPECT(probe.outcome == JoinOutcome::Stale);
        EXPECT(!graph->is_compiling(a(1)));
        EXPECT(graph->dependencies(a(1))[0] == b(2));
        EXPECT(!ranges::contains(graph->update(b(3)), a(1)));
        EXPECT(ranges::contains(graph->update(b(2)), a(1)));

        // A fresh terminal join lands the flipped imports.
        mf.open({a(1)});
        auto outcome = co_await graph->request(a(1));
        EXPECT(outcome == JoinOutcome::Success);
        EXPECT(graph->dependencies(a(1))[0] == b(3));
    });
}

ZEST_CASE(declare_no_round) {
    // declare() commits durable edges without rounds or interest: the
    // nodes exist, the cascade reaches the declared consumer, and nothing
    // ever compiles.
    make_graph();

    execute([&]() -> kota::task<> {
        graph->declare(a(2), {a(1)});
        EXPECT(graph->has_node(a(1)));
        EXPECT(graph->has_node(a(2)));
        EXPECT(graph->refcount(a(1)) == 0u);
        EXPECT(graph->refcount(a(2)) == 0u);
        EXPECT(!graph->is_compiling(a(2)));

        auto dirtied = graph->update(a(1));
        EXPECT(ranges::contains(dirtied, a(1)));
        EXPECT(ranges::contains(dirtied, a(2)));
        co_return;
    });
}

ZEST_CASE(declare_replaces) {
    // A later declare replaces the edge set — the no-round analogue of a
    // successful round's promotion. An empty replacement clears the edges
    // but keeps the node: a consumer whose last import was removed stops
    // cascading yet stays tracked.
    make_graph();

    execute([&]() -> kota::task<> {
        graph->declare(a(3), {a(1)});
        graph->declare(a(3), {a(2)});
        EXPECT(!ranges::contains(graph->update(a(1)), a(3)));
        EXPECT(ranges::contains(graph->update(a(2)), a(3)));

        graph->declare(a(3), {});
        EXPECT(!ranges::contains(graph->update(a(2)), a(3)));
        EXPECT(graph->has_node(a(3)));
        co_return;
    });
}

ZEST_CASE(round_replaces_declared) {
    // A declared node that later runs a real round: the current
    // successful round's candidates replace the declared edges.
    make_graph();
    Adjacency adj;
    adj[a(2)] = {a(3)};
    graph->register_family(FamA, instant(adj));

    execute([&]() -> kota::task<> {
        graph->declare(a(2), {a(1)});
        auto outcome = co_await graph->request(a(2));
        EXPECT(outcome == JoinOutcome::Success);

        EXPECT(!ranges::contains(graph->update(a(1)), a(2)));
        EXPECT(ranges::contains(graph->update(a(3)), a(2)));
    });
}

ZEST_CASE(failed_keeps_declared) {
    // A failed round discards its candidates and leaves the declared
    // topology standing: a unit whose build breaks must stay
    // cascade-reachable from its declared imports, or fixing an import
    // could never re-dirty it.
    make_graph();
    ManualFamily mf;
    mf.adj[a(2)] = {a(3)};
    graph->register_family(FamA, mf.runner());
    mf.gate(a(2)).result = RoundOutcome::Failed;
    mf.open({a(2), a(3)});

    execute([&]() -> kota::task<> {
        graph->declare(a(2), {a(1)});
        auto outcome = co_await graph->request(a(2));
        EXPECT(outcome == JoinOutcome::Failed);

        EXPECT(ranges::contains(graph->update(a(1)), a(2)));
        EXPECT(!ranges::contains(graph->update(a(3)), a(2)));
    });
}

ZEST_CASE(landing_overwrites_declare) {
    // A declare against an in-flight round is answered at landing: a
    // current Success promotes its candidates over the interim
    // declaration. Benign by the topology invariant — had the content
    // changed between the two resolves, update() would have voided the
    // round; unchanged content resolves to the same set.
    make_graph();
    ManualFamily mf;
    mf.adj[a(2)] = {a(3)};
    graph->register_family(FamA, mf.runner());
    mf.open({a(3)});

    Probe probe;
    execute([&]() -> kota::task<> {
        auto driver = [&]() -> kota::task<> {
            co_await mf.gate(a(2)).started.wait();
            graph->declare(a(2), {a(1)});
            mf.open({a(2)});
            co_return;
        };

        co_await kota::when_all(run_request(a(2), probe), driver());

        EXPECT(probe.outcome == JoinOutcome::Success);
        EXPECT(!ranges::contains(graph->update(a(1)), a(2)));
        EXPECT(ranges::contains(graph->update(a(3)), a(2)));
    });
}

/// ============================================================================
///                  Round identity filter & run-to-reply
/// ============================================================================
///
/// The graph never destroys a running round; an overtaken round runs to
/// its real reply, and whatever it reports lands as Stale.

ZEST_CASE(stale_success_discarded) {
    // The closure ignores its token and reports Success after update()
    // overtook the round: the result must not count — the node stays
    // dirty and the waiter drives a fresh round.
    make_graph();
    ManualFamily mf;
    mf.stubborn = true;
    graph->register_family(FamA, mf.runner());

    Probe probe;
    execute([&]() -> kota::task<> {
        auto driver = [&]() -> kota::task<> {
            co_await mf.gate(a(1)).started.wait();
            mf.gate(a(1)).started.reset();

            graph->update(a(1));

            // Advisory cancellation: the round is signalled, not killed.
            EXPECT(graph->is_compiling(a(1)));

            mf.open({a(1)});
            co_await mf.gate(a(1)).started.wait();
            EXPECT(mf.gate(a(1)).calls == 2);
            co_return;
        };

        co_await kota::when_all(run_request(a(1), probe), driver());

        EXPECT(probe.outcome == JoinOutcome::Success);
        EXPECT(mf.gate(a(1)).calls == 2);
        EXPECT(!graph->is_dirty(a(1)));
    });
}

ZEST_CASE(stale_failure_retries) {
    // A Failed reply from an overtaken round is no verdict about the new
    // content: waiters retry instead of propagating the failure.
    make_graph();
    ManualFamily mf;
    mf.stubborn = true;
    mf.gate(a(1)).result = RoundOutcome::Failed;
    graph->register_family(FamA, mf.runner());

    Probe probe;
    execute([&]() -> kota::task<> {
        auto driver = [&]() -> kota::task<> {
            co_await mf.gate(a(1)).started.wait();
            mf.gate(a(1)).started.reset();

            graph->update(a(1));
            mf.gate(a(1)).result = RoundOutcome::Success;
            mf.open({a(1)});

            co_await mf.gate(a(1)).started.wait();
            co_return;
        };

        co_await kota::when_all(run_request(a(1), probe), driver());

        // The stale Failed never reached the waiter.
        EXPECT(probe.outcome == JoinOutcome::Success);
        EXPECT(mf.gate(a(1)).calls == 2);
    });
}

ZEST_CASE(salvage_before_stale) {
    // A round may publish side effects and then report Stale (bounded-
    // stale publication): the graph has no say over what happened before
    // the report — the side effect stands, and the join retries.
    make_graph();
    int calls = 0;
    int salvaged = 0;
    graph->register_family(FamA, [&](RoundContext&, NodeId) -> kota::task<RoundOutcome> {
        calls += 1;
        if(calls == 1) {
            salvaged += 1;
            co_return RoundOutcome::Stale;
        }
        co_return RoundOutcome::Success;
    });

    execute([&]() -> kota::task<> {
        auto outcome = co_await graph->request(a(1));
        EXPECT(outcome == JoinOutcome::Success);
        EXPECT(calls == 2);
        EXPECT(salvaged == 1);
    });
}

/// ============================================================================
///                              Join flavors
/// ============================================================================

ZEST_CASE(one_attempt_stale) {
    // OneAttempt observes exactly one attempt: an overtaken round returns
    // Stale to the caller instead of retrying.
    make_graph();
    ManualFamily mf;
    graph->register_family(FamA, mf.runner());

    Probe probe;
    execute([&]() -> kota::task<> {
        auto driver = [&]() -> kota::task<> {
            co_await mf.gate(a(1)).started.wait();
            graph->update(a(1));
            co_return;
        };

        co_await kota::when_all(run_request(a(1), probe, {.flavor = JoinFlavor::OneAttempt}),
                                driver());

        EXPECT(probe.outcome == JoinOutcome::Stale);
        EXPECT(mf.gate(a(1)).calls == 1);
        EXPECT(graph->is_dirty(a(1)));
    });
}

ZEST_CASE(one_attempt_clean) {
    // OneAttempt on a clean node succeeds without a round.
    make_graph();
    Adjacency adj;
    graph->register_family(FamA, instant(adj));

    execute([&]() -> kota::task<> {
        co_await graph->request(a(1));
        EXPECT(ran.size() == 1u);

        auto outcome = co_await graph->request(a(1), {.flavor = JoinFlavor::OneAttempt});
        EXPECT(outcome == JoinOutcome::Success);
        EXPECT(ran.size() == 1u);
    });
}

ZEST_CASE(abandoned_validity) {
    // The validity predicate goes false while the join waits: the next
    // continuation point abandons instead of retrying.
    make_graph();
    ManualFamily mf;
    graph->register_family(FamA, mf.runner());

    bool valid = true;
    Probe probe;
    execute([&]() -> kota::task<> {
        auto driver = [&]() -> kota::task<> {
            co_await mf.gate(a(1)).started.wait();
            valid = false;
            graph->update(a(1));
            co_return;
        };

        co_await kota::when_all(run_request(a(1), probe, {.validity = [&] { return valid; }}),
                                driver());

        EXPECT(probe.outcome == JoinOutcome::Abandoned);
        EXPECT(mf.gate(a(1)).calls == 1);
    });
}

/// ============================================================================
///                            Failure semantics
/// ============================================================================

ZEST_CASE(failed_propagates) {
    // A failing dependency fails the depender without dispatching it;
    // both stay dirty.
    make_graph();
    ManualFamily mf;
    mf.adj[a(1)] = {b(2)};
    mf.gate(b(2)).result = RoundOutcome::Failed;
    graph->register_family(FamA, mf.runner());
    graph->register_family(FamB, mf.runner());
    mf.open({a(1), b(2)});

    execute([&]() -> kota::task<> {
        auto outcome = co_await graph->request(a(1));
        EXPECT(outcome == JoinOutcome::Failed);
        EXPECT(mf.gate(b(2)).calls == 1);
        EXPECT(mf.gate(a(1)).calls == 0);
        EXPECT(graph->is_dirty(a(1)));
        EXPECT(graph->is_dirty(b(2)));
    });
}

ZEST_CASE(failure_not_sticky) {
    // Failure propagates without retry, but a new request tries again and
    // succeeds once the dependency compiles.
    make_graph();
    ManualFamily mf;
    mf.adj[a(1)] = {b(2)};
    mf.gate(b(2)).result = RoundOutcome::Failed;
    graph->register_family(FamA, mf.runner());
    graph->register_family(FamB, mf.runner());
    mf.open({a(1), b(2)});

    execute([&]() -> kota::task<> {
        auto first = co_await graph->request(a(1));
        EXPECT(first == JoinOutcome::Failed);

        mf.gate(b(2)).result = RoundOutcome::Success;
        auto second = co_await graph->request(a(1));
        EXPECT(second == JoinOutcome::Success);
        EXPECT(mf.gate(b(2)).calls == 2);
        EXPECT(mf.gate(a(1)).calls == 1);
    });
}

/// ============================================================================
///                        Interest & cancellation
/// ============================================================================

ZEST_CASE(requester_cancel_releases) {
    // The requester's frame unwinds mid-round: interest drops to zero, the
    // advisory token fires after one tick, the closure winds down and the
    // graph quiesces with the node still dirty.
    make_graph();
    ManualFamily mf;
    graph->register_family(FamA, mf.runner());

    Probe probe;
    execute([&]() -> kota::task<> {
        auto driver = [&]() -> kota::task<> {
            co_await mf.gate(a(1)).started.wait();
            probe.source.cancel();
            co_await settle([&] { return !graph->is_compiling(a(1)); });

            EXPECT(probe.done);
            EXPECT(probe.outcome == std::nullopt);
            EXPECT(graph->is_dirty(a(1)));
            EXPECT(graph->refcount(a(1)) == 0u);
            EXPECT(mf.gate(a(1)).calls == 1);
            co_return;
        };

        co_await kota::when_all(run_request(a(1), probe), driver());
    });
}

ZEST_CASE(shared_dep_survives_cancel) {
    // Two chains share one dependency; cancelling one requester must only
    // wind down its own chain — the shared dependency keeps compiling for
    // the survivor.
    make_graph();
    ManualFamily mf;
    mf.adj[a(1)] = {b(5)};
    mf.adj[a(3)] = {b(5)};
    graph->register_family(FamA, mf.runner());
    graph->register_family(FamB, mf.runner());
    mf.open({a(1), a(3)});

    Probe p1, p3;
    execute([&]() -> kota::task<> {
        auto driver = [&]() -> kota::task<> {
            co_await mf.gate(b(5)).started.wait();
            EXPECT(graph->refcount(b(5)) == 2u);

            p1.source.cancel();
            co_await settle([&] { return !graph->is_compiling(a(1)); });

            EXPECT(graph->is_compiling(b(5)));
            EXPECT(mf.gate(b(5)).calls == 1);
            EXPECT(graph->refcount(b(5)) == 1u);

            mf.open({b(5)});
            co_return;
        };

        co_await kota::when_all(run_request(a(1), p1), run_request(a(3), p3), driver());

        EXPECT(p1.outcome == std::nullopt);
        EXPECT(p3.outcome == JoinOutcome::Success);
        EXPECT(mf.gate(b(5)).calls == 1);
    });
}

ZEST_CASE(transient_drop_handover) {
    // The depender is updated while waiting on its unchanged dependency:
    // the retry re-acquires the dependency within the same drain cycle, so
    // its in-flight round is handed over — neither cancelled nor
    // restarted.
    make_graph();
    ManualFamily mf;
    mf.adj[a(1)] = {b(2)};
    graph->register_family(FamA, mf.runner());
    graph->register_family(FamB, mf.runner());
    mf.open({a(1)});

    Probe probe;
    execute([&]() -> kota::task<> {
        auto driver = [&]() -> kota::task<> {
            co_await mf.gate(b(2)).started.wait();
            EXPECT(graph->is_compiling(a(1)));

            graph->update(a(1));
            co_await kota::sleep(1);

            // The depender's round was respawned; the dependency kept
            // compiling throughout.
            EXPECT(graph->is_compiling(b(2)));
            EXPECT(mf.gate(b(2)).calls == 1);
            EXPECT(graph->refcount(b(2)) == 1u);

            mf.open({b(2)});
            co_return;
        };

        co_await kota::when_all(run_request(a(1), probe), driver());

        EXPECT(probe.outcome == JoinOutcome::Success);
        EXPECT(mf.gate(b(2)).calls == 1);
        EXPECT(!graph->is_dirty(a(1)));
        EXPECT(!graph->is_dirty(b(2)));
    });
}

/// ============================================================================
///                               Foreground
/// ============================================================================

ZEST_CASE(foreground_late_join) {
    // A foreground requester joins a Low round that then reports Stale:
    // the respawn re-reads the interest class, so the retry dispatches
    // foreground instead of staying cancellable Low.
    make_graph();
    kota::event started;
    kota::event proceed;
    int calls = 0;
    std::vector<bool> classes;
    graph->register_family(FamA, [&](RoundContext& ctx, NodeId) -> kota::task<RoundOutcome> {
        calls += 1;
        classes.push_back(ctx.foreground());
        if(calls == 1) {
            started.set();
            co_await proceed.wait();
            co_return RoundOutcome::Stale;
        }
        co_return RoundOutcome::Success;
    });

    Probe background, foreground;
    execute([&]() -> kota::task<> {
        auto driver = [&]() -> kota::task<> {
            co_await started.wait();
            proceed.set();
            co_return;
        };

        // The foreground request joins while the first round is parked
        // inside its dispatch.
        co_await kota::when_all(run_request(a(1), background),
                                run_request(a(1), foreground, {.foreground = true}),
                                driver());

        EXPECT(background.outcome == JoinOutcome::Success);
        EXPECT(foreground.outcome == JoinOutcome::Success);
        CO_ASSERT(calls == 2);
        EXPECT(!classes[0]);
        EXPECT(classes[1]);
    });
}

ZEST_CASE(foreground_spreads_edges) {
    // Foreground must travel the live candidate edges: a foreground join
    // at the root upgrades a deep dependency already parked in a Low
    // round, so its preempted retry dispatches foreground.
    make_graph();
    kota::event started;
    kota::event proceed;
    int deep_calls = 0;
    std::vector<bool> deep_classes;
    Adjacency adj;
    adj[a(1)] = {a(2)};
    adj[a(2)] = {a(3)};
    graph->register_family(FamA, [&](RoundContext& ctx, NodeId id) -> kota::task<RoundOutcome> {
        if(auto it = adj.find(id); it != adj.end()) {
            for(auto dep: it->second) {
                switch(co_await ctx.depend(dep)) {
                    case DependResult::Ready: break;
                    case DependResult::Failed: co_return RoundOutcome::Failed;
                    case DependResult::Cancelled: co_return RoundOutcome::Stale;
                }
            }
        }
        if(id == a(3)) {
            deep_calls += 1;
            deep_classes.push_back(ctx.foreground());
            if(deep_calls == 1) {
                started.set();
                co_await proceed.wait();
                co_return RoundOutcome::Stale;
            }
        }
        co_return RoundOutcome::Success;
    });

    Probe background, foreground;
    execute([&]() -> kota::task<> {
        auto driver = [&]() -> kota::task<> {
            co_await started.wait();
            proceed.set();
            co_return;
        };

        // The background request parks a(3); the foreground request then
        // joins at the root and must upgrade through two candidate edges.
        co_await kota::when_all(run_request(a(2), background),
                                run_request(a(1), foreground, {.foreground = true}),
                                driver());

        EXPECT(background.outcome == JoinOutcome::Success);
        EXPECT(foreground.outcome == JoinOutcome::Success);
        CO_ASSERT(deep_calls == 2);
        EXPECT(!deep_classes[0]);
        EXPECT(deep_classes[1]);
    });
}

ZEST_CASE(foreground_skips_clean_deps) {
    // A foreground request answered by a clean cached chain must not tag
    // the chain's durable dependencies: no request ever acquires a clean
    // dependency, so nothing would reset the mark, and an unrelated
    // background rebuild much later would dispatch at foreground class.
    make_graph();
    std::vector<bool> dep_classes;
    Adjacency adj;
    adj[a(1)] = {a(2)};
    graph->register_family(FamA, [&](RoundContext& ctx, NodeId id) -> kota::task<RoundOutcome> {
        if(auto it = adj.find(id); it != adj.end()) {
            for(auto dep: it->second) {
                if(co_await ctx.depend(dep) != DependResult::Ready) {
                    co_return RoundOutcome::Failed;
                }
            }
        }
        if(id == a(2)) {
            dep_classes.push_back(ctx.foreground());
        }
        co_return RoundOutcome::Success;
    });

    Probe warm, hit, rebuild;
    execute([&]() -> kota::task<> {
        // Build the chain clean at Low, then answer a foreground request
        // from the clean root outright.
        co_await run_request(a(1), warm);
        co_await run_request(a(1), hit, {.foreground = true});

        graph->update(a(2));
        co_await run_request(a(2), rebuild);

        EXPECT(rebuild.outcome == JoinOutcome::Success);
        CO_ASSERT(dep_classes.size() == std::size_t(2));
        EXPECT(!dep_classes[0]);
        EXPECT(!dep_classes[1]);
    });
}

ZEST_CASE(foreground_resets_zero) {
    // The foreground flag is sticky while any interest remains and reset
    // when the count returns to zero: a later background request runs Low.
    make_graph();
    ManualFamily mf;
    graph->register_family(FamA, mf.runner());
    mf.open({a(1)});

    execute([&]() -> kota::task<> {
        co_await graph->request(a(1), {.foreground = true});
        EXPECT(mf.gate(a(1)).classes[0]);

        graph->update(a(1));
        co_await graph->request(a(1));
        CO_ASSERT(mf.gate(a(1)).calls == 2);
        EXPECT(!mf.gate(a(1)).classes[1]);
    });
}

/// ============================================================================
///                             Cycle handling
/// ============================================================================

ZEST_CASE(self_depend_fails) {
    // A node depending on itself fails immediately.
    make_graph();
    Adjacency adj;
    adj[a(1)] = {a(1)};
    graph->register_family(FamA, instant(adj));

    execute([&]() -> kota::task<> {
        auto outcome = co_await graph->request(a(1));
        EXPECT(outcome == JoinOutcome::Failed);
    });
}

ZEST_CASE(depend_cycle_fails) {
    // 1 -> 2 -> 1: the depend that would close the wait loop detects it
    // and fails the round instead of deadlocking.
    make_graph();
    Adjacency adj;
    adj[a(1)] = {a(2)};
    adj[a(2)] = {a(1)};
    graph->register_family(FamA, instant(adj));

    execute([&]() -> kota::task<> {
        auto outcome = co_await graph->request(a(1));
        EXPECT(outcome == JoinOutcome::Failed);
    });
}

ZEST_CASE(update_introduces_cycle) {
    // The cycle only appears after an update changes the declared imports;
    // the retry detects it and fails instead of hanging.
    make_graph();
    Adjacency adj;
    adj[a(1)] = {a(2)};
    graph->register_family(FamA, instant(adj));

    execute([&]() -> kota::task<> {
        auto first = co_await graph->request(a(1));
        EXPECT(first == JoinOutcome::Success);

        adj[a(2)] = {a(1)};
        graph->update(a(2));

        auto second = co_await graph->request(a(1));
        EXPECT(second == JoinOutcome::Failed);
    });
}

/// ============================================================================
///                                Shutdown
/// ============================================================================

ZEST_CASE(shutdown_with_inflight) {
    // shutdown() with rounds in flight: every advisory token fires, the
    // closures wind down with real replies, pending joins resolve with
    // Shutdown, and the graph quiesces.
    make_graph();
    ManualFamily mf;
    mf.adj[a(1)] = {b(4)};
    graph->register_family(FamA, mf.runner());
    graph->register_family(FamB, mf.runner());

    Probe probe;
    auto driver = [&]() -> kota::task<> {
        co_await mf.gate(b(4)).started.wait();
        co_await graph->shutdown();
        co_return;
    };

    auto t1 = run_request(a(1), probe);
    auto t2 = driver();
    loop->schedule(t1);
    loop->schedule(t2);
    loop->run();

    EXPECT(probe.done);
    EXPECT(probe.outcome == JoinOutcome::Shutdown);
    EXPECT(graph->idle());
}

/// ============================================================================
///                            Randomized stress
/// ============================================================================

ZEST_CASE(randomized_stress) {
    // A fixed-seed, single-threaded interleaving of requests,
    // cancellations, updates and dispatch completions. Every dispatch
    // races a semaphore against its advisory token — the run-to-reply
    // discipline — and the structural invariants hold at every step.
    make_graph();
    kota::semaphore permits{0};
    Adjacency adj;
    adj[a(1)] = {a(2), a(3)};
    adj[a(2)] = {b(4)};
    adj[a(3)] = {b(4)};
    adj[b(4)] = {b(5)};
    adj[a(6)] = {b(4), a(7)};
    adj[a(7)] = {b(5)};
    adj[a(8)] = {a(6)};

    auto runner = [&](RoundContext& ctx, NodeId id) -> kota::task<RoundOutcome> {
        if(auto it = adj.find(id); it != adj.end()) {
            for(auto dep: it->second) {
                switch(co_await ctx.depend(dep)) {
                    case DependResult::Ready: break;
                    case DependResult::Failed: co_return RoundOutcome::Failed;
                    case DependResult::Cancelled: co_return RoundOutcome::Stale;
                }
            }
        }
        auto acquire = [&]() -> kota::task<> {
            co_await permits.acquire();
        };
        auto waited = co_await kota::with_token(acquire(), ctx.token());
        if(waited.is_cancelled()) {
            co_return RoundOutcome::Stale;
        }
        co_return RoundOutcome::Success;
    };
    graph->register_family(FamA, runner);
    graph->register_family(FamB, runner);

    execute([&]() -> kota::task<> {
        std::mt19937 rng(20260826u);
        std::vector<std::unique_ptr<Probe>> probes;
        kota::task_group<> inflight;

        const NodeId roots[] = {a(1), a(6), a(8)};
        const NodeId all[] = {a(1), a(2), a(3), b(4), b(5), a(6), a(7), a(8)};

        for(int step = 0; step < 200; step += 1) {
            switch(rng() % 4) {
                case 0: {
                    auto& probe = probes.emplace_back(std::make_unique<Probe>());
                    inflight.spawn(run_request(roots[rng() % 3], *probe));
                    break;
                }
                case 1: {
                    if(!probes.empty()) {
                        probes[rng() % probes.size()]->source.cancel();
                    }
                    break;
                }
                case 2: {
                    graph->update(all[rng() % 8]);
                    break;
                }
                case 3: {
                    // Let one pending dispatch finish.
                    permits.release();
                    break;
                }
            }

            // Let deferred unwinds land, then check structural sanity.
            co_await kota::yield();
            EXPECT(graph->consistent());
        }

        // Drain: cancel every outstanding request and wait for them all.
        for(auto& probe: probes) {
            probe->source.cancel();
        }
        co_await inflight.join();
    });
}

};  // ZEST_SUITE(TaskGraph)

}  // namespace
}  // namespace clice::testing
