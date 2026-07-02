#include <optional>
#include <random>

#include "test/test.h"
#include "server/compiler/compile_graph.h"

#include "llvm/ADT/DenseSet.h"

namespace clice::testing {
namespace {

namespace ranges = std::ranges;

/// A resolve_fn that always returns no dependencies.
CompileGraph::resolve_fn no_deps() {
    return [](std::uint32_t) -> llvm::SmallVector<std::uint32_t> {
        return {};
    };
}

/// A resolve_fn backed by a static adjacency map.
CompileGraph::resolve_fn
    static_resolver(llvm::DenseMap<std::uint32_t, llvm::SmallVector<std::uint32_t>> adj) {
    return [adj = std::move(adj)](std::uint32_t path_id) -> llvm::SmallVector<std::uint32_t> {
        auto it = adj.find(path_id);
        if(it != adj.end()) {
            return it->second;
        }
        return {};
    };
}

CompileGraph::dispatch_fn instant_dispatch() {
    return [](std::uint32_t) -> kota::task<bool> {
        co_return true;
    };
}

CompileGraph::dispatch_fn tracking_dispatch(std::vector<std::uint32_t>& compiled) {
    return [&compiled](std::uint32_t path_id) -> kota::task<bool> {
        compiled.push_back(path_id);
        co_return true;
    };
}

CompileGraph::dispatch_fn failing_dispatch() {
    return [](std::uint32_t) -> kota::task<bool> {
        co_return false;
    };
}

/// Dispatch that fails only for specific path_ids.
CompileGraph::dispatch_fn selective_dispatch(llvm::DenseSet<std::uint32_t> fail_ids) {
    return [fail_ids = std::move(fail_ids)](std::uint32_t path_id) -> kota::task<bool> {
        co_return !fail_ids.contains(path_id);
    };
}

/// Dispatch driven manually by per-unit events: the test observes when a
/// unit enters dispatch (started) and decides when and how it completes
/// (proceed/result). Cancellation can thus be injected at every suspension
/// point with deterministic timing.
struct ManualDispatch {
    struct Gate {
        kota::event started;
        kota::event proceed;
        bool result = true;
        int calls = 0;
    };

    llvm::DenseMap<std::uint32_t, std::unique_ptr<Gate>> gates;

    Gate& gate(std::uint32_t path_id) {
        auto& slot = gates[path_id];
        if(!slot) {
            slot = std::make_unique<Gate>();
        }
        return *slot;
    }

    void open(std::initializer_list<std::uint32_t> path_ids) {
        for(auto id: path_ids) {
            gate(id).proceed.set();
        }
    }

    CompileGraph::dispatch_fn fn() {
        return [this](std::uint32_t path_id) -> kota::task<bool> {
            auto& g = gate(path_id);
            g.calls += 1;
            g.started.set();
            co_await g.proceed.wait();
            co_return g.result;
        };
    }
};

/// A cancellable compile request and its observed result.
/// result is empty while running and after cancellation.
struct Request {
    kota::cancellation_source source;
    std::optional<bool> result;
    bool done = false;
};

TEST_SUITE(CompileGraph) {

std::vector<std::uint32_t> compiled;
std::optional<kota::event_loop> loop;
std::optional<CompileGraph> graph;

void make_graph(CompileGraph::dispatch_fn dispatch, CompileGraph::resolve_fn resolve) {
    loop.emplace();
    graph.emplace(*loop, std::move(dispatch), std::move(resolve));
}

/// Run the test body, then verify the shutdown protocol: cancel + join must
/// exit cleanly and leave the graph fully quiesced (no compiling residue, no
/// held interest, every completion fired).
template <typename F>
void execute(F&& fn) {
    auto wrapper = [&]() -> kota::task<> {
        co_await fn();
        co_await graph->shutdown();
        EXPECT_TRUE(graph->idle());
    };
    auto t = wrapper();
    loop->schedule(t);
    loop->run();
}

kota::task<> run_request(std::uint32_t path_id, Request& req) {
    auto result = co_await kota::with_token(graph->compile(path_id), req.source.token());
    req.done = true;
    if(result.has_value()) {
        req.result = *result;
    }
}

kota::task<> run_deps_request(std::uint32_t path_id, Request& req) {
    auto result = co_await kota::with_token(graph->compile_deps(path_id), req.source.token());
    req.done = true;
    if(result.has_value()) {
        req.result = *result;
    }
}

/// Zero-interest cancellation is deferred by one event-loop tick per cascade
/// level; wait (bounded) until `pred` holds before asserting settled state.
template <typename Pred>
kota::task<> settle(Pred pred) {
    for(int i = 0; i < 100 && !pred(); i++) {
        co_await kota::sleep(1);
    }
    EXPECT_TRUE(pred());
}

/// ============================================================================
///                              Basic compilation
/// ============================================================================
///
/// A request compiles the unit and its transitive dependencies,
/// dependencies first, each dirty unit exactly once.

TEST_CASE(compile_no_deps) {
    // A unit without dependencies is dispatched once and becomes clean.
    make_graph(tracking_dispatch(compiled), no_deps());

    execute([&]() -> kota::task<> {
        auto result = co_await graph->compile(1).catch_cancel();
        EXPECT_TRUE(result.has_value());
        EXPECT_TRUE(*result);
        EXPECT_EQ(compiled.size(), 1u);
        EXPECT_EQ(compiled[0], 1u);
        EXPECT_FALSE(graph->is_dirty(1));
    });
}

TEST_CASE(compile_single_dep) {
    // 1 -> 2: the dependency is dispatched before the dependent.
    make_graph(tracking_dispatch(compiled),
               static_resolver({
                   {1, {2}}
    }));

    execute([&]() -> kota::task<> {
        auto result = co_await graph->compile(1).catch_cancel();
        EXPECT_TRUE(result.has_value());
        EXPECT_TRUE(*result);
        EXPECT_EQ(compiled.size(), 2u);
        auto pos2 = ranges::find(compiled, 2u);
        auto pos1 = ranges::find(compiled, 1u);
        EXPECT_TRUE(pos2 < pos1);
        EXPECT_FALSE(graph->is_dirty(1));
        EXPECT_FALSE(graph->is_dirty(2));
    });
}

TEST_CASE(compile_chain) {
    // 1 -> 2 -> 3: dispatch order follows the dependency chain bottom-up.
    make_graph(tracking_dispatch(compiled),
               static_resolver({
                   {1, {2}},
                   {2, {3}}
    }));

    execute([&]() -> kota::task<> {
        auto result = co_await graph->compile(1).catch_cancel();
        EXPECT_TRUE(result.has_value());
        EXPECT_TRUE(*result);
        EXPECT_EQ(compiled.size(), 3u);
        auto pos3 = ranges::find(compiled, 3u);
        auto pos2 = ranges::find(compiled, 2u);
        auto pos1 = ranges::find(compiled, 1u);
        EXPECT_TRUE(pos3 < pos2);
        EXPECT_TRUE(pos2 < pos1);
    });
}

TEST_CASE(compile_diamond_dedup) {
    // Diamond 1 -> {2, 3}, 2 -> 4, 3 -> 4: the shared dependency 4 is
    // reached through two branches but dispatched exactly once.
    make_graph(tracking_dispatch(compiled),
               static_resolver({
                   {1, {2, 3}},
                   {2, {4}   },
                   {3, {4}   }
    }));

    execute([&]() -> kota::task<> {
        auto result = co_await graph->compile(1).catch_cancel();
        EXPECT_TRUE(result.has_value());
        EXPECT_TRUE(*result);
        auto count4 = ranges::count(compiled, 4u);
        EXPECT_EQ(count4, 1);
        EXPECT_FALSE(graph->is_dirty(2));
        EXPECT_FALSE(graph->is_dirty(3));
        EXPECT_FALSE(graph->is_dirty(4));
    });
}

TEST_CASE(second_compile_skips) {
    // A clean unit is not redispatched by a later request.
    make_graph(tracking_dispatch(compiled), no_deps());

    execute([&]() -> kota::task<> {
        co_await graph->compile(1).catch_cancel();
        EXPECT_EQ(compiled.size(), 1u);
        co_await graph->compile(1).catch_cancel();
        EXPECT_EQ(compiled.size(), 1u);
    });
}

TEST_CASE(state_queries) {
    // has_unit/is_compiling reflect the unit's lifecycle: absent before the
    // first request, present and not compiling after completion.
    make_graph(instant_dispatch(), no_deps());

    execute([&]() -> kota::task<> {
        EXPECT_FALSE(graph->has_unit(1));
        EXPECT_FALSE(graph->is_compiling(1));

        co_await graph->compile(1).catch_cancel();
        EXPECT_TRUE(graph->has_unit(1));
        EXPECT_FALSE(graph->is_compiling(1));
    });
}

TEST_CASE(concurrent_requests_share_round) {
    // Two concurrent requests for the same unit join one round: a single
    // dispatch serves both.
    ManualDispatch md;
    make_graph(md.fn(), no_deps());

    Request ra, rc;
    execute([&]() -> kota::task<> {
        auto driver = [&]() -> kota::task<> {
            co_await md.gate(1).started.wait();
            EXPECT_EQ(graph->refcount(1), 2u);
            md.gate(1).proceed.set();
            co_return;
        };

        co_await kota::when_all(run_request(1, ra), run_request(1, rc), driver());

        EXPECT_TRUE(ra.result == true);
        EXPECT_TRUE(rc.result == true);
        EXPECT_EQ(md.gate(1).calls, 1);
    });
}

/// ============================================================================
///                                 compile_deps
/// ============================================================================
///
/// Compiles a unit's transitive dependencies but never the unit itself —
/// used for plain .cpp files that import modules.

TEST_CASE(compile_deps_empty) {
    // No dependencies: nothing is dispatched, the request succeeds.
    make_graph(tracking_dispatch(compiled), no_deps());

    execute([&]() -> kota::task<> {
        auto result = co_await graph->compile_deps(1).catch_cancel();
        EXPECT_TRUE(result.has_value());
        EXPECT_TRUE(*result);
        EXPECT_EQ(compiled.size(), 0u);
    });
}

TEST_CASE(compile_deps_single) {
    // 1 -> 2: only the dependency is dispatched, never unit 1 itself.
    make_graph(tracking_dispatch(compiled),
               static_resolver({
                   {1, {2}}
    }));

    execute([&]() -> kota::task<> {
        auto result = co_await graph->compile_deps(1).catch_cancel();
        EXPECT_TRUE(result.has_value());
        EXPECT_TRUE(*result);
        EXPECT_EQ(compiled.size(), 1u);
        EXPECT_EQ(compiled[0], 2u);
        EXPECT_TRUE(ranges::find(compiled, 1u) == compiled.end());
    });
}

TEST_CASE(compile_deps_chain) {
    // 1 -> 2 -> 3: transitive dependencies are compiled, the root is not.
    make_graph(tracking_dispatch(compiled),
               static_resolver({
                   {1, {2}},
                   {2, {3}}
    }));

    execute([&]() -> kota::task<> {
        auto result = co_await graph->compile_deps(1).catch_cancel();
        EXPECT_TRUE(result.has_value());
        EXPECT_TRUE(*result);
        EXPECT_EQ(compiled.size(), 2u);
        EXPECT_TRUE(ranges::find(compiled, 3u) != compiled.end());
        EXPECT_TRUE(ranges::find(compiled, 2u) != compiled.end());
        EXPECT_TRUE(ranges::find(compiled, 1u) == compiled.end());
    });
}

TEST_CASE(compile_deps_diamond) {
    // Diamond below the root: every dependency once, the root never.
    make_graph(tracking_dispatch(compiled),
               static_resolver({
                   {1, {2, 3}},
                   {2, {4}   },
                   {3, {4}   }
    }));

    execute([&]() -> kota::task<> {
        auto result = co_await graph->compile_deps(1).catch_cancel();
        EXPECT_TRUE(result.has_value());
        EXPECT_TRUE(*result);
        EXPECT_TRUE(ranges::find(compiled, 1u) == compiled.end());
        EXPECT_TRUE(ranges::find(compiled, 2u) != compiled.end());
        EXPECT_TRUE(ranges::find(compiled, 3u) != compiled.end());
        EXPECT_TRUE(ranges::find(compiled, 4u) != compiled.end());
        auto count4 = ranges::count(compiled, 4u);
        EXPECT_EQ(count4, 1);
    });
}

TEST_CASE(compile_deps_plain_cpp) {
    // The .cpp file (10) importing a module (20) gets its module built
    // without ever being treated as a module unit itself.
    make_graph(tracking_dispatch(compiled),
               static_resolver({
                   {10, {20}}
    }));

    execute([&]() -> kota::task<> {
        auto result = co_await graph->compile_deps(10).catch_cancel();
        EXPECT_TRUE(result.has_value());
        EXPECT_TRUE(*result);
        EXPECT_EQ(compiled.size(), 1u);
        EXPECT_EQ(compiled[0], 20u);
    });
}

TEST_CASE(compile_deps_concurrent_dedup) {
    // Two concurrent requests with overlapping dependency sets ({3,4} and
    // {3,5}): the shared dependency 3 is dispatched exactly once.
    make_graph(tracking_dispatch(compiled),
               static_resolver({
                   {1, {3, 4}},
                   {2, {3, 5}},
    }));

    execute([&]() -> kota::task<> {
        auto t1 = graph->compile_deps(1);
        auto t2 = graph->compile_deps(2);
        auto results = co_await kota::when_all(std::move(t1), std::move(t2));

        auto [r1, r2] = results;
        EXPECT_TRUE(r1);
        EXPECT_TRUE(r2);

        ranges::sort(compiled);
        EXPECT_EQ(compiled.size(), 3u);
        EXPECT_EQ(compiled[0], 3u);
        EXPECT_EQ(compiled[1], 4u);
        EXPECT_EQ(compiled[2], 5u);
    });
}

TEST_CASE(compile_deps_resolve_once) {
    // resolve_fn is expensive (full module scan): it runs at most once per
    // unit even when concurrent requests touch the same dependency.
    int resolve_count = 0;

    auto resolve = [&resolve_count](std::uint32_t path_id) -> llvm::SmallVector<std::uint32_t> {
        resolve_count++;
        if(path_id == 1 || path_id == 2)
            return {3};
        return {};
    };

    make_graph(tracking_dispatch(compiled), std::move(resolve));

    execute([&]() -> kota::task<> {
        auto t1 = graph->compile_deps(1);
        auto t2 = graph->compile_deps(2);
        auto results = co_await kota::when_all(std::move(t1), std::move(t2));

        auto [r1, r2] = results;
        EXPECT_TRUE(r1);
        EXPECT_TRUE(r2);

        EXPECT_EQ(compiled.size(), 1u);
        EXPECT_EQ(compiled[0], 3u);
        EXPECT_EQ(resolve_count, 3);
    });
}

TEST_CASE(compile_deps_failure) {
    // A failing dependency fails the request; the root unit is never
    // dispatched on a failed preparation.
    auto fail_and_track = [&](std::uint32_t path_id) -> kota::task<bool> {
        compiled.push_back(path_id);
        co_return false;
    };

    make_graph(std::move(fail_and_track),
               static_resolver({
                   {1, {2}}
    }));

    execute([&]() -> kota::task<> {
        auto result = co_await graph->compile_deps(1).catch_cancel();
        EXPECT_TRUE(result.has_value());
        EXPECT_FALSE(*result);
        EXPECT_TRUE(ranges::find(compiled, 1u) == compiled.end());
    });
}

/// ============================================================================
///                              Staleness marking
/// ============================================================================
///
/// update() marks a changed file and everything that (transitively)
/// depends on it dirty, following reverse edges. Marking alone never
/// recompiles — the next request does.

TEST_CASE(update_invalidates) {
    // Updating a dependency dirties it and its dependent.
    make_graph(instant_dispatch(),
               static_resolver({
                   {1, {2}}
    }));

    execute([&]() -> kota::task<> {
        co_await graph->compile(1).catch_cancel();
        EXPECT_FALSE(graph->is_dirty(2));
        EXPECT_FALSE(graph->is_dirty(1));

        graph->update(2);
        EXPECT_TRUE(graph->is_dirty(2));
        EXPECT_TRUE(graph->is_dirty(1));
    });
}

TEST_CASE(update_cascade) {
    // 1 -> 2 -> 3: updating the leaf dirties the whole dependent chain.
    make_graph(instant_dispatch(),
               static_resolver({
                   {1, {2}},
                   {2, {3}}
    }));

    execute([&]() -> kota::task<> {
        co_await graph->compile(1).catch_cancel();
        EXPECT_FALSE(graph->is_dirty(2));
        EXPECT_FALSE(graph->is_dirty(3));

        graph->update(3);
        EXPECT_TRUE(graph->is_dirty(3));
        EXPECT_TRUE(graph->is_dirty(2));
        EXPECT_TRUE(graph->is_dirty(1));
    });
}

TEST_CASE(cascade_through_dirty) {
    // The cascade must not stop at an already-dirty intermediate node:
    // after update(2) dirtied {2, 1}, update(3) still has to reach 1.
    make_graph(instant_dispatch(),
               static_resolver({
                   {1, {2}},
                   {2, {3}}
    }));

    execute([&]() -> kota::task<> {
        co_await graph->compile(1).catch_cancel();

        graph->update(2);
        EXPECT_TRUE(graph->is_dirty(1));
        EXPECT_TRUE(graph->is_dirty(2));
        EXPECT_FALSE(graph->is_dirty(3));

        graph->update(3);
        EXPECT_TRUE(graph->is_dirty(3));
        EXPECT_TRUE(graph->is_dirty(2));
        EXPECT_TRUE(graph->is_dirty(1));
    });
}

TEST_CASE(diamond_update_cascade) {
    // Updating the bottom of a diamond dirties both branches and the top;
    // the recompile still dedups the shared node.
    make_graph(tracking_dispatch(compiled),
               static_resolver({
                   {1, {2, 3}},
                   {2, {4}   },
                   {3, {4}   }
    }));

    execute([&]() -> kota::task<> {
        co_await graph->compile(1).catch_cancel();
        EXPECT_FALSE(graph->is_dirty(1));
        EXPECT_FALSE(graph->is_dirty(4));

        graph->update(4);
        EXPECT_TRUE(graph->is_dirty(4));
        EXPECT_TRUE(graph->is_dirty(2));
        EXPECT_TRUE(graph->is_dirty(3));
        EXPECT_TRUE(graph->is_dirty(1));

        compiled.clear();
        auto result = co_await graph->compile(1).catch_cancel();
        EXPECT_TRUE(result.has_value() && *result);
        auto count4 = ranges::count(compiled, 4u);
        EXPECT_EQ(count4, 1);
    });
}

TEST_CASE(update_returns_dirtied) {
    // The caller (PCM cache eviction) gets the full set of dirtied units.
    make_graph(instant_dispatch(),
               static_resolver({
                   {1, {2}},
                   {2, {3}}
    }));

    execute([&]() -> kota::task<> {
        co_await graph->compile(1).catch_cancel();

        auto dirtied = graph->update(3);
        EXPECT_EQ(dirtied.size(), 3u);
        EXPECT_TRUE(llvm::find(dirtied, 1u) != dirtied.end());
        EXPECT_TRUE(llvm::find(dirtied, 2u) != dirtied.end());
        EXPECT_TRUE(llvm::find(dirtied, 3u) != dirtied.end());
    });
}

TEST_CASE(compile_after_update) {
    // A request after an update recompiles exactly the dirtied units.
    make_graph(tracking_dispatch(compiled),
               static_resolver({
                   {1, {2}}
    }));

    execute([&]() -> kota::task<> {
        co_await graph->compile(1).catch_cancel();
        EXPECT_EQ(compiled.size(), 2u);

        graph->update(2);
        co_await graph->compile(1).catch_cancel();
        EXPECT_EQ(compiled.size(), 4u);
    });
}

TEST_CASE(update_resets_resolved) {
    // The updated file may have added/removed imports: its dependencies are
    // re-resolved on the next compile and the new set takes effect.
    int resolve_count = 0;
    bool updated = false;
    auto resolver = [&](std::uint32_t path_id) -> llvm::SmallVector<std::uint32_t> {
        if(path_id == 1) {
            resolve_count++;
            return updated ? llvm::SmallVector<std::uint32_t>{3}
                           : llvm::SmallVector<std::uint32_t>{2};
        }
        return {};
    };

    make_graph(tracking_dispatch(compiled), std::move(resolver));

    execute([&]() -> kota::task<> {
        co_await graph->compile(1).catch_cancel();
        EXPECT_EQ(resolve_count, 1);
        EXPECT_EQ(compiled.size(), 2u);  // 2, then 1

        updated = true;
        graph->update(1);

        co_await graph->compile(1).catch_cancel();
        EXPECT_EQ(resolve_count, 2);
        auto tail = compiled | std::views::drop(2);
        EXPECT_TRUE(ranges::find(tail, 3u) != tail.end());
    });
}

TEST_CASE(update_cleans_back_edges) {
    // When a re-resolve drops a dependency, the reverse edge goes with it:
    // updating the ex-dependency no longer cascades to the ex-dependent.
    bool updated = false;
    auto resolver = [&](std::uint32_t path_id) -> llvm::SmallVector<std::uint32_t> {
        if(path_id == 1) {
            return updated ? llvm::SmallVector<std::uint32_t>{}
                           : llvm::SmallVector<std::uint32_t>{2};
        }
        return {};
    };

    make_graph(tracking_dispatch(compiled), std::move(resolver));

    execute([&]() -> kota::task<> {
        co_await graph->compile(1).catch_cancel();
        EXPECT_FALSE(graph->is_dirty(1));

        updated = true;
        graph->update(1);

        co_await graph->compile(1).catch_cancel();
        EXPECT_FALSE(graph->is_dirty(1));

        graph->update(2);
        EXPECT_TRUE(graph->is_dirty(2));
        EXPECT_FALSE(graph->is_dirty(1));
    });
}

TEST_CASE(update_unknown_id) {
    // Updating a file the graph has never seen is a harmless no-op.
    make_graph(instant_dispatch(), no_deps());

    auto dirtied = graph->update(999);
    EXPECT_EQ(dirtied.size(), 0u);
    EXPECT_FALSE(graph->has_unit(999));
}

/// ============================================================================
///                          Update vs in-flight rounds
/// ============================================================================
///
/// The stale round is cancelled unconditionally (its result is garbage),
/// interest is untouched, and the waiters drive a fresh round with the
/// new content. Dependencies that are NOT stale themselves must keep
/// compiling across the retry.

TEST_CASE(update_during_dispatch) {
    // The unit is updated mid-dispatch: that round's result is discarded,
    // the waiter respawns the unit and succeeds. Exactly one retry — each
    // retry consumes one staleness event, so there is no retry storm.
    ManualDispatch md;
    make_graph(md.fn(), no_deps());

    execute([&]() -> kota::task<> {
        bool done = false;
        std::optional<bool> result;

        auto compiler = [&]() -> kota::task<> {
            auto r = co_await graph->compile(1).catch_cancel();
            done = true;
            if(r.has_value()) {
                result = *r;
            }
        };

        auto driver = [&]() -> kota::task<> {
            co_await md.gate(1).started.wait();
            md.gate(1).started.reset();
            graph->update(1);
            co_await md.gate(1).started.wait();
            EXPECT_EQ(md.gate(1).calls, 2);
            md.gate(1).proceed.set();
            co_return;
        };

        co_await kota::when_all(compiler(), driver());

        EXPECT_TRUE(done);
        EXPECT_TRUE(result == true);
        EXPECT_FALSE(graph->is_dirty(1));
        EXPECT_EQ(md.gate(1).calls, 2);
    });
}

TEST_CASE(update_keeps_waited_dep) {
    // Unit 1 is updated while waiting on its (unchanged) dependency 2. The
    // retry re-acquires 2 within the same drain cycle, so 2's in-flight
    // round is handed over — neither cancelled nor restarted.
    ManualDispatch md;
    make_graph(md.fn(),
               static_resolver({
                   {1, {2}}
    }));
    md.open({1});

    execute([&]() -> kota::task<> {
        bool done = false;
        std::optional<bool> result;

        auto compiler = [&]() -> kota::task<> {
            auto r = co_await graph->compile(1).catch_cancel();
            done = true;
            if(r.has_value()) {
                result = *r;
            }
        };

        auto driver = [&]() -> kota::task<> {
            co_await md.gate(2).started.wait();
            EXPECT_TRUE(graph->is_compiling(1));

            graph->update(1);
            co_await kota::sleep(1);

            // Unit 1's round was respawned; 2 kept compiling throughout.
            EXPECT_TRUE(graph->is_compiling(2));
            EXPECT_EQ(md.gate(2).calls, 1);
            EXPECT_EQ(graph->refcount(2), 1u);

            md.gate(2).proceed.set();
            co_return;
        };

        co_await kota::when_all(compiler(), driver());

        EXPECT_TRUE(done);
        EXPECT_TRUE(result == true);
        EXPECT_FALSE(graph->is_dirty(1));
        EXPECT_FALSE(graph->is_dirty(2));
        EXPECT_EQ(md.gate(2).calls, 1);
    });
}

TEST_CASE(update_keeps_retained_dep) {
    // Unit 1 waits on {2, 3} when an update changes its imports to {2, 4}:
    // the orphaned 3 is cancelled, while the retained 2 is handed over to
    // the retry — not cancelled and restarted, which would waste the work
    // done so far.
    bool flipped = false;
    auto resolver = [&](std::uint32_t path_id) -> llvm::SmallVector<std::uint32_t> {
        if(path_id == 1) {
            return flipped ? llvm::SmallVector<std::uint32_t>{2, 4}
                           : llvm::SmallVector<std::uint32_t>{2, 3};
        }
        return {};
    };

    ManualDispatch md;
    make_graph(md.fn(), std::move(resolver));
    md.open({1, 4});

    execute([&]() -> kota::task<> {
        bool done = false;
        std::optional<bool> result;

        auto compiler = [&]() -> kota::task<> {
            auto r = co_await graph->compile(1).catch_cancel();
            done = true;
            if(r.has_value()) {
                result = *r;
            }
        };

        auto driver = [&]() -> kota::task<> {
            co_await md.gate(2).started.wait();
            co_await md.gate(3).started.wait();
            EXPECT_EQ(graph->refcount(2), 1u);
            EXPECT_EQ(graph->refcount(3), 1u);

            flipped = true;
            graph->update(1);
            co_await settle([&] { return !graph->is_compiling(3); });

            // 3 lost its last interest and was cancelled; 2 kept compiling.
            EXPECT_TRUE(graph->is_dirty(3));
            EXPECT_TRUE(graph->is_compiling(2));
            EXPECT_EQ(md.gate(2).calls, 1);
            EXPECT_EQ(md.gate(3).calls, 1);
            EXPECT_EQ(graph->refcount(2), 1u);

            md.gate(2).proceed.set();
            co_return;
        };

        co_await kota::when_all(compiler(), driver());

        EXPECT_TRUE(done);
        EXPECT_TRUE(result == true);
        EXPECT_FALSE(graph->is_dirty(1));
        EXPECT_FALSE(graph->is_dirty(2));
        EXPECT_FALSE(graph->is_dirty(4));
        // The retained dependency was dispatched exactly once overall.
        EXPECT_EQ(md.gate(2).calls, 1);
        EXPECT_EQ(md.gate(3).calls, 1);
    });
}

TEST_CASE(update_swaps_deps) {
    // Unit 1's import set changes from {2} to {3} mid-flight: the retry
    // re-resolves, the orphaned 2 is released (its now-unwanted round
    // cancelled, never restarted) and its reverse edge fully detached.
    bool flipped = false;
    auto resolver = [&](std::uint32_t path_id) -> llvm::SmallVector<std::uint32_t> {
        if(path_id == 1) {
            return flipped ? llvm::SmallVector<std::uint32_t>{3}
                           : llvm::SmallVector<std::uint32_t>{2};
        }
        return {};
    };

    ManualDispatch md;
    make_graph(md.fn(), std::move(resolver));
    md.open({1, 3});

    execute([&]() -> kota::task<> {
        bool done = false;
        std::optional<bool> result;

        auto compiler = [&]() -> kota::task<> {
            auto r = co_await graph->compile(1).catch_cancel();
            done = true;
            if(r.has_value()) {
                result = *r;
            }
        };

        auto driver = [&]() -> kota::task<> {
            co_await md.gate(2).started.wait();
            EXPECT_TRUE(graph->is_compiling(1));

            flipped = true;
            graph->update(1);

            co_await md.gate(3).started.wait();
            EXPECT_EQ(md.gate(2).calls, 1);
            EXPECT_EQ(md.gate(3).calls, 1);
            co_return;
        };

        co_await kota::when_all(compiler(), driver());

        EXPECT_TRUE(done);
        EXPECT_TRUE(result == true);
        EXPECT_FALSE(graph->is_dirty(1));
        EXPECT_FALSE(graph->is_dirty(3));
        // The orphan stays dirty and detached: updating it no longer
        // cascades to 1.
        EXPECT_TRUE(graph->is_dirty(2));
        auto dirtied = graph->update(2);
        EXPECT_EQ(dirtied.size(), 1u);
        EXPECT_FALSE(graph->is_dirty(1));
    });
}

TEST_CASE(update_cascades_cancel) {
    // Updating a dependency cancels the in-flight rounds of its dependents
    // too (their results would embed the stale dependency); the surviving
    // request retries the whole chain with the new content.
    ManualDispatch md;
    make_graph(md.fn(),
               static_resolver({
                   {1, {2}},
                   {2, {3}}
    }));
    md.open({1, 2});

    execute([&]() -> kota::task<> {
        bool done = false;
        std::optional<bool> result;

        auto compiler = [&]() -> kota::task<> {
            auto r = co_await graph->compile(1).catch_cancel();
            done = true;
            if(r.has_value()) {
                result = *r;
            }
        };

        auto driver = [&]() -> kota::task<> {
            co_await md.gate(3).started.wait();
            EXPECT_TRUE(graph->is_compiling(1));
            EXPECT_TRUE(graph->is_compiling(2));

            md.gate(3).started.reset();
            graph->update(3);
            EXPECT_TRUE(graph->is_dirty(1));
            EXPECT_TRUE(graph->is_dirty(2));

            // The updated unit itself is stale, so its dispatch restarts.
            co_await md.gate(3).started.wait();
            EXPECT_EQ(md.gate(3).calls, 2);
            EXPECT_TRUE(graph->is_compiling(1));
            EXPECT_TRUE(graph->is_compiling(2));
            md.gate(3).proceed.set();
            co_return;
        };

        co_await kota::when_all(compiler(), driver());

        EXPECT_TRUE(done);
        EXPECT_TRUE(result == true);
        EXPECT_FALSE(graph->is_dirty(1));
        EXPECT_FALSE(graph->is_dirty(2));
        EXPECT_FALSE(graph->is_dirty(3));
        // No further updates arrived — exactly one retry, no storm.
        EXPECT_EQ(md.gate(3).calls, 2);
    });
}

TEST_CASE(shared_dep_update_retries) {
    // The shared dependency 5 of two waiting chains is updated mid-dispatch:
    // both chains observe the stale round and retry, sharing a single fresh
    // round — 5 is dispatched exactly twice overall, not three times.
    ManualDispatch md;
    make_graph(md.fn(),
               static_resolver({
                   {1, {5}},
                   {3, {5}}
    }));
    md.open({1, 3});

    Request ra, rc;
    execute([&]() -> kota::task<> {
        auto driver = [&]() -> kota::task<> {
            co_await md.gate(5).started.wait();
            EXPECT_EQ(graph->refcount(5), 2u);

            md.gate(5).started.reset();
            graph->update(5);

            co_await md.gate(5).started.wait();
            EXPECT_EQ(md.gate(5).calls, 2);
            md.gate(5).proceed.set();
            co_return;
        };

        co_await kota::when_all(run_request(1, ra), run_request(3, rc), driver());

        EXPECT_TRUE(ra.result == true);
        EXPECT_TRUE(rc.result == true);
        EXPECT_EQ(md.gate(5).calls, 2);
        EXPECT_FALSE(graph->is_dirty(5));
    });
}

/// ============================================================================
///                              Failure semantics
/// ============================================================================
///
/// A failed round (compile error, cycle) propagates to every waiter
/// without retry — retrying failures would turn a syntax error into a
/// storm. Failure is not sticky: the next explicit request tries again.

TEST_CASE(failure_leaves_dirty) {
    // A failing dependency fails the request; neither the failed dependency
    // nor the never-dispatched dependent is marked clean.
    make_graph(failing_dispatch(),
               static_resolver({
                   {1, {2}}
    }));

    execute([&]() -> kota::task<> {
        auto result = co_await graph->compile(1).catch_cancel();
        EXPECT_TRUE(result.has_value());
        EXPECT_FALSE(*result);
        EXPECT_TRUE(graph->is_dirty(2));
        EXPECT_TRUE(graph->is_dirty(1));
    });
}

TEST_CASE(partial_dep_failure) {
    // 1 -> {2, 3}, only 3 fails: 2's success is kept, 3 stays dirty, and 1
    // fails without being dispatched.
    make_graph(selective_dispatch({
                   3
    }),
               static_resolver({{1, {2, 3}}}));

    execute([&]() -> kota::task<> {
        auto result = co_await graph->compile(1).catch_cancel();
        EXPECT_TRUE(result.has_value());
        EXPECT_FALSE(*result);
        EXPECT_FALSE(graph->is_dirty(2));
        EXPECT_TRUE(graph->is_dirty(3));
        EXPECT_TRUE(graph->is_dirty(1));
    });
}

TEST_CASE(failed_dep_no_retry) {
    // The dependency fails once: the failure propagates (1 never
    // dispatched, no retry) and the failed round releases its interest.
    ManualDispatch md;
    make_graph(md.fn(),
               static_resolver({
                   {1, {2}}
    }));
    md.gate(2).result = false;
    md.open({1, 2});

    execute([&]() -> kota::task<> {
        auto result = co_await graph->compile(1).catch_cancel();
        EXPECT_TRUE(result.has_value());
        EXPECT_FALSE(*result);
        EXPECT_EQ(md.gate(2).calls, 1);
        EXPECT_EQ(md.gate(1).calls, 0);
        EXPECT_EQ(graph->refcount(2), 0u);
        EXPECT_EQ(graph->refcount(1), 0u);
    });
}

TEST_CASE(recompile_after_failure) {
    // Failure is not sticky: a new request drives a fresh attempt, which
    // succeeds once the dependency compiles.
    ManualDispatch md;
    make_graph(md.fn(),
               static_resolver({
                   {1, {2}}
    }));
    md.gate(2).result = false;
    md.open({1, 2});

    execute([&]() -> kota::task<> {
        auto r1 = co_await graph->compile(1).catch_cancel();
        EXPECT_TRUE(r1.has_value());
        EXPECT_FALSE(*r1);

        md.gate(2).result = true;
        auto r2 = co_await graph->compile(1).catch_cancel();
        EXPECT_TRUE(r2.has_value());
        EXPECT_TRUE(*r2);
        EXPECT_EQ(md.gate(2).calls, 2);
        EXPECT_EQ(md.gate(1).calls, 1);
        EXPECT_FALSE(graph->is_dirty(1));
        EXPECT_FALSE(graph->is_dirty(2));
    });
}

TEST_CASE(shared_dep_failure_propagates) {
    // One failing round of a shared dependency fails every consumer waiting
    // on it; the failing dispatch runs only once.
    ManualDispatch md;
    make_graph(md.fn(),
               static_resolver({
                   {1, {5}},
                   {3, {5}}
    }));
    md.gate(5).result = false;

    Request ra, rc;
    execute([&]() -> kota::task<> {
        // Hold 5's gate until both chains wait on the same round.
        auto driver = [&]() -> kota::task<> {
            co_await md.gate(5).started.wait();
            EXPECT_EQ(graph->refcount(5), 2u);
            md.gate(5).proceed.set();
            co_return;
        };

        co_await kota::when_all(run_request(1, ra), run_request(3, rc), driver());

        EXPECT_TRUE(ra.result == false);
        EXPECT_TRUE(rc.result == false);
        EXPECT_EQ(md.gate(5).calls, 1);
        EXPECT_EQ(md.gate(1).calls, 0);
        EXPECT_EQ(md.gate(3).calls, 0);
        EXPECT_TRUE(graph->is_dirty(5));
    });
}

/// ============================================================================
///                                Cycle handling
/// ============================================================================
///
/// Every shape of dependency cycle terminates with failure — never a
/// deadlock, never unbounded retry.

TEST_CASE(self_loop) {
    // A unit importing itself fails immediately after resolve.
    make_graph(instant_dispatch(),
               static_resolver({
                   {1, {1}}
    }));

    execute([&]() -> kota::task<> {
        auto result = co_await graph->compile(1).catch_cancel();
        EXPECT_TRUE(result.has_value());
        EXPECT_FALSE(*result);
    });
}

TEST_CASE(circular_dependency) {
    // 1 -> 2 -> 1: the waiter that would close the wait loop detects it
    // and fails the unit instead of blocking.
    make_graph(instant_dispatch(),
               static_resolver({
                   {1, {2}},
                   {2, {1}}
    }));

    execute([&]() -> kota::task<> {
        auto result = co_await graph->compile(1).catch_cancel();
        EXPECT_TRUE(result.has_value());
        EXPECT_FALSE(*result);
    });
}

TEST_CASE(cross_branch_cycle) {
    // 1 -> {2, 3}, 2 -> 3, 3 -> 2: sibling unit tasks would deadlock on
    // each other's completion without wait-cycle detection.
    make_graph(instant_dispatch(),
               static_resolver({
                   {1, {2, 3}},
                   {2, {3}   },
                   {3, {2}   }
    }));

    execute([&]() -> kota::task<> {
        auto result = co_await graph->compile(1).catch_cancel();
        EXPECT_TRUE(result.has_value());
        EXPECT_FALSE(*result);
    });
}

TEST_CASE(partitioned_cycle) {
    // The cycle (2 <-> 3) does not involve the requested root; the failure
    // still propagates up to it.
    make_graph(instant_dispatch(),
               static_resolver({
                   {1, {2}},
                   {2, {3}},
                   {3, {2}}
    }));

    execute([&]() -> kota::task<> {
        auto result = co_await graph->compile(1).catch_cancel();
        EXPECT_TRUE(result.has_value());
        EXPECT_FALSE(*result);
    });
}

TEST_CASE(update_introduced_cycle) {
    // The cycle only appears after update() forces a re-resolve of unit 2;
    // the retry detects it and fails instead of hanging.
    bool flipped = false;
    auto resolver = [&](std::uint32_t path_id) -> llvm::SmallVector<std::uint32_t> {
        if(path_id == 1) {
            return {2};
        }
        if(path_id == 2 && flipped) {
            return {1};
        }
        return {};
    };

    make_graph(instant_dispatch(), std::move(resolver));

    execute([&]() -> kota::task<> {
        auto r1 = co_await graph->compile(1).catch_cancel();
        EXPECT_TRUE(r1.has_value() && *r1);

        flipped = true;
        graph->update(2);

        auto r2 = co_await graph->compile(1).catch_cancel();
        EXPECT_TRUE(r2.has_value());
        EXPECT_FALSE(*r2);
    });
}

/// ============================================================================
///                      Shared dependencies & cancellation
/// ============================================================================
///
/// Topology unless noted: A(1) -> B(2) -> E(5) and C(3) -> D(4) -> E(5),
/// E shared by both chains. Cancelling a request must only kill the parts
/// of its chain no other consumer holds interest in.

TEST_CASE(shared_dep_survives_cancel) {
    // Cancel request A while E dispatches: the A-chain dies, but D still
    // holds interest in E — E keeps compiling and serves C unscathed.
    ManualDispatch md;
    make_graph(md.fn(),
               static_resolver({
                   {1, {2}},
                   {2, {5}},
                   {3, {4}},
                   {4, {5}}
    }));
    md.open({1, 2, 3, 4});

    Request ra, rc;
    execute([&]() -> kota::task<> {
        auto driver = [&]() -> kota::task<> {
            co_await md.gate(5).started.wait();
            EXPECT_EQ(graph->refcount(5), 2u);
            EXPECT_EQ(md.gate(5).calls, 1);

            ra.source.cancel();
            co_await settle([&] { return !graph->is_compiling(2); });

            EXPECT_TRUE(ra.done);
            EXPECT_TRUE(graph->is_compiling(5));
            EXPECT_EQ(graph->refcount(5), 1u);
            EXPECT_EQ(md.gate(5).calls, 1);

            md.gate(5).proceed.set();
            co_return;
        };

        co_await kota::when_all(run_request(1, ra), run_request(3, rc), driver());

        EXPECT_FALSE(ra.result.has_value());
        EXPECT_TRUE(rc.result == true);
        EXPECT_FALSE(graph->is_dirty(5));
        EXPECT_FALSE(graph->is_dirty(3));
        EXPECT_TRUE(graph->is_dirty(1));
    });
}

TEST_CASE(shared_dep_sequential_cancel) {
    // Both directions, stepwise: cancel C first (E survives via the
    // A-chain, refcount 2 -> 1), then cancel A too (last interest gone,
    // refcount 1 -> 0, E dies). E is never restarted along the way.
    ManualDispatch md;
    make_graph(md.fn(),
               static_resolver({
                   {1, {2}},
                   {2, {5}},
                   {3, {4}},
                   {4, {5}}
    }));
    md.open({1, 2, 3, 4});

    Request ra, rc;
    execute([&]() -> kota::task<> {
        auto driver = [&]() -> kota::task<> {
            co_await md.gate(5).started.wait();
            EXPECT_EQ(graph->refcount(5), 2u);

            rc.source.cancel();
            co_await settle([&] { return !graph->is_compiling(4); });

            EXPECT_TRUE(graph->is_compiling(5));
            EXPECT_EQ(graph->refcount(5), 1u);
            EXPECT_EQ(md.gate(5).calls, 1);

            ra.source.cancel();
            co_await settle([&] { return !graph->is_compiling(5); });

            EXPECT_TRUE(graph->is_dirty(5));
            EXPECT_EQ(graph->refcount(5), 0u);
            EXPECT_EQ(md.gate(5).calls, 1);
            co_return;
        };

        co_await kota::when_all(run_request(1, ra), run_request(3, rc), driver());

        EXPECT_FALSE(ra.result.has_value());
        EXPECT_FALSE(rc.result.has_value());
    });
}

TEST_CASE(shared_dep_both_cancelled) {
    // E shared at different depths (A -> B -> E, C -> E); cancelling both
    // requests in the same tick drops E to zero and cancels it.
    ManualDispatch md;
    make_graph(md.fn(),
               static_resolver({
                   {1, {2}},
                   {2, {5}},
                   {3, {5}}
    }));
    md.open({1, 2, 3});

    Request ra, rc;
    execute([&]() -> kota::task<> {
        auto driver = [&]() -> kota::task<> {
            co_await md.gate(5).started.wait();
            EXPECT_EQ(graph->refcount(5), 2u);

            ra.source.cancel();
            rc.source.cancel();
            co_await settle([&] { return !graph->is_compiling(5); });

            EXPECT_TRUE(graph->is_dirty(5));
            EXPECT_EQ(graph->refcount(5), 0u);
            EXPECT_EQ(md.gate(5).calls, 1);
            co_return;
        };

        co_await kota::when_all(run_request(1, ra), run_request(3, rc), driver());

        EXPECT_FALSE(ra.result.has_value());
        EXPECT_FALSE(rc.result.has_value());
    });
}

TEST_CASE(shared_dep_queued_cancel) {
    // E is still queued behind its own dependency F(9) (not yet
    // dispatching) when A is cancelled: E's task survives and keeps
    // waiting, F is untouched.
    ManualDispatch md;
    make_graph(md.fn(),
               static_resolver({
                   {1, {2}},
                   {2, {5}},
                   {3, {4}},
                   {4, {5}},
                   {5, {9}}
    }));
    md.open({1, 2, 3, 4, 5});

    Request ra, rc;
    execute([&]() -> kota::task<> {
        auto driver = [&]() -> kota::task<> {
            co_await md.gate(9).started.wait();
            EXPECT_TRUE(graph->is_compiling(5));
            EXPECT_EQ(graph->refcount(5), 2u);

            ra.source.cancel();
            co_await settle([&] { return !graph->is_compiling(2); });

            EXPECT_TRUE(graph->is_compiling(5));
            EXPECT_TRUE(graph->is_compiling(9));
            EXPECT_EQ(graph->refcount(5), 1u);
            EXPECT_EQ(md.gate(9).calls, 1);

            md.gate(9).proceed.set();
            co_return;
        };

        co_await kota::when_all(run_request(1, ra), run_request(3, rc), driver());

        EXPECT_FALSE(ra.result.has_value());
        EXPECT_TRUE(rc.result == true);
        EXPECT_FALSE(graph->is_dirty(5));
        EXPECT_FALSE(graph->is_dirty(9));
    });
}

TEST_CASE(shared_dep_already_compiled) {
    // E was already built by the A-chain; later requests reuse it without
    // recompiling.
    ManualDispatch md;
    make_graph(md.fn(),
               static_resolver({
                   {1, {2}},
                   {2, {5}},
                   {3, {4}},
                   {4, {5}}
    }));
    md.open({1, 2, 3, 4, 5});

    execute([&]() -> kota::task<> {
        auto r1 = co_await graph->compile(1).catch_cancel();
        EXPECT_TRUE(r1.has_value() && *r1);
        EXPECT_EQ(md.gate(5).calls, 1);

        auto r2 = co_await graph->compile(3).catch_cancel();
        EXPECT_TRUE(r2.has_value() && *r2);
        EXPECT_EQ(md.gate(5).calls, 1);
    });
}

TEST_CASE(compile_deps_cancel_releases) {
    // A plain .cpp (10) holds root references on its direct deps;
    // cancelling the compile_deps request releases them without killing a
    // dependency that another consumer still needs.
    ManualDispatch md;
    make_graph(md.fn(),
               static_resolver({
                   {10, {5}},
                   {3,  {4}},
                   {4,  {5}}
    }));
    md.open({3, 4});

    Request ra, rc;
    execute([&]() -> kota::task<> {
        auto driver = [&]() -> kota::task<> {
            co_await md.gate(5).started.wait();
            // Root reference from the compile_deps request + edge from 4.
            EXPECT_EQ(graph->refcount(5), 2u);

            ra.source.cancel();
            co_await kota::sleep(1);

            EXPECT_TRUE(ra.done);
            EXPECT_TRUE(graph->is_compiling(5));
            EXPECT_EQ(graph->refcount(5), 1u);
            EXPECT_EQ(md.gate(5).calls, 1);

            md.gate(5).proceed.set();
            co_return;
        };

        co_await kota::when_all(run_deps_request(10, ra), run_request(3, rc), driver());

        EXPECT_FALSE(ra.result.has_value());
        EXPECT_TRUE(rc.result == true);
        EXPECT_FALSE(graph->is_dirty(5));
    });
}

TEST_CASE(duplicate_requests_cancel_one) {
    // Two requests on the same unit; cancelling one must not disturb the
    // round the other is waiting on.
    ManualDispatch md;
    make_graph(md.fn(), no_deps());

    Request ra, rc;
    execute([&]() -> kota::task<> {
        auto driver = [&]() -> kota::task<> {
            co_await md.gate(1).started.wait();
            EXPECT_EQ(graph->refcount(1), 2u);

            ra.source.cancel();
            co_await kota::sleep(1);

            EXPECT_TRUE(ra.done);
            EXPECT_TRUE(graph->is_compiling(1));
            EXPECT_EQ(graph->refcount(1), 1u);
            EXPECT_EQ(md.gate(1).calls, 1);

            md.gate(1).proceed.set();
            co_return;
        };

        co_await kota::when_all(run_request(1, ra), run_request(1, rc), driver());

        EXPECT_FALSE(ra.result.has_value());
        EXPECT_TRUE(rc.result == true);
        EXPECT_EQ(md.gate(1).calls, 1);
    });
}

TEST_CASE(rerequest_within_grace) {
    // The only request is cancelled and a new one arrives within the same
    // drain cycle — release first, re-acquire second, strictly worse than
    // the supersede handoff ordering. The deferred zero-interest check
    // bridges the transient zero: the in-flight round survives instead of
    // being killed and restarted.
    ManualDispatch md;
    make_graph(md.fn(), no_deps());

    kota::event start_c;
    Request ra, rc;
    execute([&]() -> kota::task<> {
        auto delayed_request = [&]() -> kota::task<> {
            co_await start_c.wait();
            co_await run_request(1, rc);
        };

        auto driver = [&]() -> kota::task<> {
            co_await md.gate(1).started.wait();
            EXPECT_EQ(graph->refcount(1), 1u);

            ra.source.cancel();
            start_c.set();
            // Two sleeps: the second is armed after the zero-interest check,
            // so the assertions observe its (skipped) outcome.
            co_await kota::sleep(1);
            co_await kota::sleep(1);

            EXPECT_TRUE(graph->is_compiling(1));
            EXPECT_EQ(graph->refcount(1), 1u);
            EXPECT_EQ(md.gate(1).calls, 1);

            md.gate(1).proceed.set();
            co_return;
        };

        co_await kota::when_all(run_request(1, ra), delayed_request(), driver());

        EXPECT_FALSE(ra.result.has_value());
        EXPECT_TRUE(rc.result == true);
        EXPECT_EQ(md.gate(1).calls, 1);
    });
}

/// ============================================================================
///                                  Lifecycle
/// ============================================================================
///
/// cancel_all restarts in-flight work without dropping waiters; shutdown
/// (cancel + join) quiesces the graph with tasks still in flight.

TEST_CASE(empty_graph) {
    // A graph that never compiled anything: cancel_all is a no-op and
    // teardown is clean.
    make_graph(instant_dispatch(), no_deps());
    EXPECT_FALSE(graph->has_unit(1));
    graph->cancel_all();
}

TEST_CASE(cancel_all_recompile) {
    // The graph stays fully usable after cancel_all: a later update +
    // compile recompiles everything as usual.
    make_graph(tracking_dispatch(compiled),
               static_resolver({
                   {1, {2}}
    }));

    execute([&]() -> kota::task<> {
        co_await graph->compile(1).catch_cancel();
        EXPECT_EQ(compiled.size(), 2u);
        EXPECT_FALSE(graph->is_dirty(1));
        EXPECT_FALSE(graph->is_dirty(2));

        graph->cancel_all();
        graph->update(2);
        EXPECT_TRUE(graph->is_dirty(2));
        EXPECT_TRUE(graph->is_dirty(1));

        auto result = co_await graph->compile(1).catch_cancel();
        EXPECT_TRUE(result.has_value());
        EXPECT_TRUE(*result);
        EXPECT_EQ(compiled.size(), 4u);
        EXPECT_FALSE(graph->is_dirty(1));
        EXPECT_FALSE(graph->is_dirty(2));
    });
}

TEST_CASE(cancel_all_respawns) {
    // cancel_all kills the in-flight round; the waiter still holds its
    // interest, so it respawns the unit and the request succeeds.
    ManualDispatch md;
    make_graph(md.fn(), no_deps());

    execute([&]() -> kota::task<> {
        bool done = false;
        std::optional<bool> result;

        auto compiler = [&]() -> kota::task<> {
            auto r = co_await graph->compile(1).catch_cancel();
            done = true;
            if(r.has_value()) {
                result = *r;
            }
        };

        auto driver = [&]() -> kota::task<> {
            co_await md.gate(1).started.wait();
            md.gate(1).started.reset();
            graph->cancel_all();
            co_await md.gate(1).started.wait();
            EXPECT_EQ(md.gate(1).calls, 2);
            md.gate(1).proceed.set();
            co_return;
        };

        co_await kota::when_all(compiler(), driver());

        EXPECT_TRUE(done);
        EXPECT_TRUE(result == true);
        EXPECT_FALSE(graph->is_dirty(1));
    });
}

TEST_CASE(shutdown_with_inflight) {
    // shutdown() with several unit tasks in flight: pending requests
    // resolve with failure (the graph refuses to respawn), every frame
    // unwinds, and the destructor protocol holds (idle, no asserts).
    ManualDispatch md;
    make_graph(md.fn(),
               static_resolver({
                   {1, {2, 3}},
                   {2, {4}   },
                   {3, {4}   }
    }));

    Request req;
    auto driver = [&]() -> kota::task<> {
        co_await md.gate(4).started.wait();
        co_await graph->shutdown();
        co_return;
    };

    auto t1 = run_request(1, req);
    auto t2 = driver();
    loop->schedule(t1);
    loop->schedule(t2);
    loop->run();

    EXPECT_TRUE(req.done);
    EXPECT_TRUE(req.result == false);
    EXPECT_TRUE(graph->idle());
}

/// ============================================================================
///                              Randomized stress
/// ============================================================================
///
/// A fixed-seed, single-threaded interleaving of requests, cancellations,
/// updates and dispatch completions. Verifies the structural invariants
/// at every step and full quiescence at teardown.

TEST_CASE(randomized_stress) {
    kota::semaphore permits{0};
    auto dispatch = [&](std::uint32_t) -> kota::task<bool> {
        co_await permits.acquire();
        co_return true;
    };

    make_graph(std::move(dispatch),
               static_resolver({
                   {1, {2, 3}},
                   {2, {4}   },
                   {3, {4}   },
                   {4, {5}   },
                   {6, {4, 7}},
                   {7, {5}   },
                   {8, {6}   }
    }));

    execute([&]() -> kota::task<> {
        std::mt19937 rng(20260612u);
        std::vector<std::unique_ptr<Request>> requests;
        kota::task_group<> inflight(*loop);

        constexpr std::uint32_t roots[] = {1, 6, 8};
        constexpr std::uint32_t nodes[] = {1, 2, 3, 4, 5, 6, 7, 8};

        for(int step = 0; step < 200; ++step) {
            switch(rng() % 4) {
                case 0: {
                    auto& req = requests.emplace_back(std::make_unique<Request>());
                    inflight.spawn(run_request(roots[rng() % 3], *req));
                    break;
                }
                case 1: {
                    if(!requests.empty()) {
                        requests[rng() % requests.size()]->source.cancel();
                    }
                    break;
                }
                case 2: {
                    graph->update(nodes[rng() % 8]);
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
            EXPECT_TRUE(graph->consistent());
        }

        // Drain: cancel every outstanding request and wait for them all.
        for(auto& req: requests) {
            req->source.cancel();
        }
        co_await inflight.join();
    });
}

};  // TEST_SUITE(CompileGraph)

}  // namespace
}  // namespace clice::testing
