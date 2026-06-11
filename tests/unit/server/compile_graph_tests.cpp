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
/// exit cleanly and leave the graph fully quiesced.
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

TEST_CASE(CompileNoDeps) {
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

TEST_CASE(CompileWithDependency) {
    // Unit 1 depends on unit 2.
    make_graph(tracking_dispatch(compiled),
               static_resolver({
                   {1, {2}}
    }));

    execute([&]() -> kota::task<> {
        auto result = co_await graph->compile(1).catch_cancel();
        EXPECT_TRUE(result.has_value());
        EXPECT_TRUE(*result);
        // Both 2 (dep) and 1 (self) should be compiled, in that order.
        EXPECT_EQ(compiled.size(), 2u);
        auto pos2 = ranges::find(compiled, 2u);
        auto pos1 = ranges::find(compiled, 1u);
        EXPECT_TRUE(pos2 < pos1);
        EXPECT_FALSE(graph->is_dirty(1));
        EXPECT_FALSE(graph->is_dirty(2));
    });
}

TEST_CASE(CompileChain) {
    // Chain: 1 -> 2 -> 3.
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
        // 3 before 2 before 1.
        auto pos3 = ranges::find(compiled, 3u);
        auto pos2 = ranges::find(compiled, 2u);
        auto pos1 = ranges::find(compiled, 1u);
        EXPECT_TRUE(pos3 < pos2);
        EXPECT_TRUE(pos2 < pos1);
    });
}

TEST_CASE(DiamondDependency) {
    // Diamond: 1 -> {2, 3}, 2 -> 4, 3 -> 4.
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
        // Unit 4 should be compiled exactly once (dedup).
        auto count4 = ranges::count(compiled, 4u);
        EXPECT_EQ(count4, 1);
        EXPECT_FALSE(graph->is_dirty(2));
        EXPECT_FALSE(graph->is_dirty(3));
        EXPECT_FALSE(graph->is_dirty(4));
    });
}

TEST_CASE(UpdateInvalidates) {
    // 1 -> 2.
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
        // Cascade: 1 depends on 2, so 1 should also be dirty.
        EXPECT_TRUE(graph->is_dirty(1));
    });
}

TEST_CASE(UpdateCascade) {
    // Chain: 1 -> 2 -> 3.
    make_graph(instant_dispatch(),
               static_resolver({
                   {1, {2}},
                   {2, {3}}
    }));

    execute([&]() -> kota::task<> {
        co_await graph->compile(1).catch_cancel();
        EXPECT_FALSE(graph->is_dirty(2));
        EXPECT_FALSE(graph->is_dirty(3));

        // Update leaf (3) — should cascade to 2 and 1.
        graph->update(3);
        EXPECT_TRUE(graph->is_dirty(3));
        EXPECT_TRUE(graph->is_dirty(2));
        EXPECT_TRUE(graph->is_dirty(1));
    });
}

TEST_CASE(CompileAfterUpdate) {
    // 1 -> 2.
    make_graph(tracking_dispatch(compiled),
               static_resolver({
                   {1, {2}}
    }));

    execute([&]() -> kota::task<> {
        co_await graph->compile(1).catch_cancel();
        EXPECT_EQ(compiled.size(), 2u);

        graph->update(2);
        co_await graph->compile(1).catch_cancel();
        // 2 and 1 should be recompiled.
        EXPECT_EQ(compiled.size(), 4u);
    });
}

TEST_CASE(DispatchFailure) {
    // 1 -> 2. Dispatch always fails.
    make_graph(failing_dispatch(),
               static_resolver({
                   {1, {2}}
    }));

    execute([&]() -> kota::task<> {
        auto result = co_await graph->compile(1).catch_cancel();
        EXPECT_TRUE(result.has_value());
        EXPECT_FALSE(*result);
        // Dep 2 failed, so it stays dirty.
        EXPECT_TRUE(graph->is_dirty(2));
    });
}

TEST_CASE(CancelAll) {
    make_graph(instant_dispatch(), no_deps());
    // Just verify it doesn't crash.
    graph->cancel_all();
}

TEST_CASE(SecondCompileSkips) {
    make_graph(tracking_dispatch(compiled), no_deps());

    execute([&]() -> kota::task<> {
        co_await graph->compile(1).catch_cancel();
        EXPECT_EQ(compiled.size(), 1u);
        // Second compile should skip (already clean).
        co_await graph->compile(1).catch_cancel();
        EXPECT_EQ(compiled.size(), 1u);
    });
}

TEST_CASE(CascadeThroughAlreadyDirty) {
    // Chain: 1 -> 2 -> 3.
    make_graph(instant_dispatch(),
               static_resolver({
                   {1, {2}},
                   {2, {3}}
    }));

    execute([&]() -> kota::task<> {
        co_await graph->compile(1).catch_cancel();

        // Update node 2: marks 2 and 1 dirty.
        graph->update(2);
        EXPECT_TRUE(graph->is_dirty(1));
        EXPECT_TRUE(graph->is_dirty(2));
        EXPECT_FALSE(graph->is_dirty(3));

        // Now update node 3: must cascade through already-dirty 2 to reach 1.
        graph->update(3);
        EXPECT_TRUE(graph->is_dirty(3));
        EXPECT_TRUE(graph->is_dirty(2));
        EXPECT_TRUE(graph->is_dirty(1));
    });
}

TEST_CASE(CircularDependencyDetection) {
    // Cycle: 1 -> 2 -> 1.
    make_graph(instant_dispatch(),
               static_resolver({
                   {1, {2}},
                   {2, {1}}
    }));

    execute([&]() -> kota::task<> {
        auto result = co_await graph->compile(1).catch_cancel();
        // Should return false (cycle detected), not deadlock.
        EXPECT_TRUE(result.has_value());
        EXPECT_FALSE(*result);
    });
}

TEST_CASE(CrossBranchCycleDetection) {
    // Cross-branch cycle: 1 -> {2, 3}, 2 -> 3, 3 -> 2.
    // Sibling unit tasks could deadlock on each other's completion
    // without proper wait-cycle detection.
    make_graph(instant_dispatch(),
               static_resolver({
                   {1, {2, 3}},
                   {2, {3}   },
                   {3, {2}   }
    }));

    execute([&]() -> kota::task<> {
        auto result = co_await graph->compile(1).catch_cancel();
        // Should return false (cycle detected), not deadlock.
        EXPECT_TRUE(result.has_value());
        EXPECT_FALSE(*result);
    });
}

TEST_CASE(UpdateResetsResolved) {
    int resolve_count = 0;
    // 1 depends on {2} initially; after update, depends on {3}.
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
        // First compile: resolves 1 -> {2}.
        co_await graph->compile(1).catch_cancel();
        EXPECT_EQ(resolve_count, 1);
        EXPECT_EQ(compiled.size(), 2u);  // 2, then 1

        // Update node 1: resets resolved, changes deps.
        updated = true;
        graph->update(1);

        // Recompile: should re-resolve 1 -> {3}.
        co_await graph->compile(1).catch_cancel();
        EXPECT_EQ(resolve_count, 2);
        // New dep 3 should be compiled, then 1 recompiled.
        auto tail = compiled | std::views::drop(2);
        EXPECT_TRUE(ranges::find(tail, 3u) != tail.end());
    });
}

TEST_CASE(UpdateCleansBackEdges) {
    bool updated = false;
    auto resolver = [&](std::uint32_t path_id) -> llvm::SmallVector<std::uint32_t> {
        if(path_id == 1) {
            // Initially depends on 2; after update, no deps.
            return updated ? llvm::SmallVector<std::uint32_t>{}
                           : llvm::SmallVector<std::uint32_t>{2};
        }
        return {};
    };

    make_graph(tracking_dispatch(compiled), std::move(resolver));

    execute([&]() -> kota::task<> {
        // First compile: 1 -> {2}.
        co_await graph->compile(1).catch_cancel();
        EXPECT_FALSE(graph->is_dirty(1));

        // Update 1: resets resolved, removes dep on 2.
        updated = true;
        graph->update(1);

        // Recompile: 1 has no deps now.
        co_await graph->compile(1).catch_cancel();
        EXPECT_FALSE(graph->is_dirty(1));

        // Now update 2: should NOT cascade to 1 (back-edge was removed).
        graph->update(2);
        EXPECT_TRUE(graph->is_dirty(2));
        EXPECT_FALSE(graph->is_dirty(1));
    });
}

TEST_CASE(DiamondUpdateCascade) {
    // Diamond: 1 -> {2, 3}, 2 -> 4, 3 -> 4.
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

        // Update leaf 4: should cascade to 2, 3, and 1.
        graph->update(4);
        EXPECT_TRUE(graph->is_dirty(4));
        EXPECT_TRUE(graph->is_dirty(2));
        EXPECT_TRUE(graph->is_dirty(3));
        EXPECT_TRUE(graph->is_dirty(1));

        compiled.clear();
        auto result = co_await graph->compile(1).catch_cancel();
        EXPECT_TRUE(result.has_value() && *result);
        // Unit 4 should still be compiled exactly once (dedup on recompile).
        auto count4 = ranges::count(compiled, 4u);
        EXPECT_EQ(count4, 1);
    });
}

TEST_CASE(UpdateReturnsAllDirtied) {
    // Chain: 1 -> 2 -> 3.
    make_graph(instant_dispatch(),
               static_resolver({
                   {1, {2}},
                   {2, {3}}
    }));

    execute([&]() -> kota::task<> {
        co_await graph->compile(1).catch_cancel();

        auto dirtied = graph->update(3);
        // Should return 3, 2, 1 (all dirtied nodes).
        EXPECT_EQ(dirtied.size(), 3u);
        EXPECT_TRUE(llvm::find(dirtied, 1u) != dirtied.end());
        EXPECT_TRUE(llvm::find(dirtied, 2u) != dirtied.end());
        EXPECT_TRUE(llvm::find(dirtied, 3u) != dirtied.end());
    });
}

TEST_CASE(HasUnitAndIsCompiling) {
    make_graph(instant_dispatch(), no_deps());

    execute([&]() -> kota::task<> {
        EXPECT_FALSE(graph->has_unit(1));
        EXPECT_FALSE(graph->is_compiling(1));

        co_await graph->compile(1).catch_cancel();
        EXPECT_TRUE(graph->has_unit(1));
        EXPECT_FALSE(graph->is_compiling(1));
    });
}

TEST_CASE(FailureLeavesDepsDirty) {
    // 1 -> 2. Dispatch always fails.
    make_graph(failing_dispatch(),
               static_resolver({
                   {1, {2}}
    }));

    execute([&]() -> kota::task<> {
        auto result = co_await graph->compile(1).catch_cancel();
        EXPECT_TRUE(result.has_value());
        EXPECT_FALSE(*result);
        // Both dep and self should stay dirty.
        EXPECT_TRUE(graph->is_dirty(2));
        EXPECT_TRUE(graph->is_dirty(1));
    });
}

TEST_CASE(SelfLoop) {
    // Unit 1 depends on itself.
    make_graph(instant_dispatch(),
               static_resolver({
                   {1, {1}}
    }));

    execute([&]() -> kota::task<> {
        auto result = co_await graph->compile(1).catch_cancel();
        // Should detect cycle and return false, not deadlock.
        EXPECT_TRUE(result.has_value());
        EXPECT_FALSE(*result);
    });
}

TEST_CASE(CancelAllAndRecompile) {
    make_graph(tracking_dispatch(compiled),
               static_resolver({
                   {1, {2}}
    }));

    execute([&]() -> kota::task<> {
        co_await graph->compile(1).catch_cancel();
        EXPECT_EQ(compiled.size(), 2u);
        EXPECT_FALSE(graph->is_dirty(1));
        EXPECT_FALSE(graph->is_dirty(2));

        // cancel_all + update to mark dirty again.
        graph->cancel_all();
        graph->update(2);
        EXPECT_TRUE(graph->is_dirty(2));
        EXPECT_TRUE(graph->is_dirty(1));

        // Recompile should succeed normally.
        auto result = co_await graph->compile(1).catch_cancel();
        EXPECT_TRUE(result.has_value());
        EXPECT_TRUE(*result);
        EXPECT_EQ(compiled.size(), 4u);
        EXPECT_FALSE(graph->is_dirty(1));
        EXPECT_FALSE(graph->is_dirty(2));
    });
}

TEST_CASE(UpdateDuringCompile) {
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

        // Update while dispatch is in flight: the round is cancelled, the
        // waiter retries with the new content, and — with no further
        // updates — exactly one retry happens.
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

TEST_CASE(WhenAllPartialFailure) {
    // 1 -> {2, 3}. Only unit 3 fails.
    make_graph(selective_dispatch({
                   3
    }),
               static_resolver({{1, {2, 3}}}));

    execute([&]() -> kota::task<> {
        auto result = co_await graph->compile(1).catch_cancel();
        EXPECT_TRUE(result.has_value());
        EXPECT_FALSE(*result);
        // Unit 2 succeeded — should be clean.
        EXPECT_FALSE(graph->is_dirty(2));
        // Unit 3 failed — stays dirty.
        EXPECT_TRUE(graph->is_dirty(3));
        // Unit 1 was not dispatched — stays dirty.
        EXPECT_TRUE(graph->is_dirty(1));
    });
}

TEST_CASE(UpdateUnknownPathId) {
    make_graph(instant_dispatch(), no_deps());

    // update on a path_id that was never compiled should not crash.
    auto dirtied = graph->update(999);
    EXPECT_EQ(dirtied.size(), 0u);
    EXPECT_FALSE(graph->has_unit(999));
}

TEST_CASE(EmptyGraphNoCompile) {
    // Construct and destroy without any compile calls.
    make_graph(instant_dispatch(), no_deps());
    EXPECT_FALSE(graph->has_unit(1));
    graph->cancel_all();  // Should not crash on empty graph.
}

TEST_CASE(CompileDepsNoDeps) {
    make_graph(tracking_dispatch(compiled), no_deps());

    execute([&]() -> kota::task<> {
        auto result = co_await graph->compile_deps(1).catch_cancel();
        EXPECT_TRUE(result.has_value());
        EXPECT_TRUE(*result);
        // No dependencies, so nothing should be dispatched.
        EXPECT_EQ(compiled.size(), 0u);
    });
}

TEST_CASE(CompileDepsWithDependency) {
    // Unit 1 depends on unit 2.
    make_graph(tracking_dispatch(compiled),
               static_resolver({
                   {1, {2}}
    }));

    execute([&]() -> kota::task<> {
        auto result = co_await graph->compile_deps(1).catch_cancel();
        EXPECT_TRUE(result.has_value());
        EXPECT_TRUE(*result);
        // Only dep 2 should be compiled, NOT unit 1 itself.
        EXPECT_EQ(compiled.size(), 1u);
        EXPECT_EQ(compiled[0], 2u);
        auto pos1 = ranges::find(compiled, 1u);
        EXPECT_TRUE(pos1 == compiled.end());
    });
}

TEST_CASE(CompileDepsChain) {
    // Chain: 1 -> 2 -> 3.
    make_graph(tracking_dispatch(compiled),
               static_resolver({
                   {1, {2}},
                   {2, {3}}
    }));

    execute([&]() -> kota::task<> {
        auto result = co_await graph->compile_deps(1).catch_cancel();
        EXPECT_TRUE(result.has_value());
        EXPECT_TRUE(*result);
        // Deps 2 and 3 should be compiled, but NOT unit 1.
        EXPECT_EQ(compiled.size(), 2u);
        EXPECT_TRUE(ranges::find(compiled, 3u) != compiled.end());
        EXPECT_TRUE(ranges::find(compiled, 2u) != compiled.end());
        EXPECT_TRUE(ranges::find(compiled, 1u) == compiled.end());
    });
}

TEST_CASE(CompileDepsDiamond) {
    // Diamond: 1 -> {2, 3}, 2 -> 4, 3 -> 4.
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
        // Deps 2, 3, 4 should be compiled, but NOT unit 1.
        EXPECT_TRUE(ranges::find(compiled, 1u) == compiled.end());
        EXPECT_TRUE(ranges::find(compiled, 2u) != compiled.end());
        EXPECT_TRUE(ranges::find(compiled, 3u) != compiled.end());
        EXPECT_TRUE(ranges::find(compiled, 4u) != compiled.end());
        // Unit 4 should be compiled exactly once (dedup).
        auto count4 = ranges::count(compiled, 4u);
        EXPECT_EQ(count4, 1);
    });
}

TEST_CASE(CompileDepsFailure) {
    // 1 -> 2. Dispatch fails for unit 2.
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
        // Unit 1 should NOT be dispatched at all.
        EXPECT_TRUE(ranges::find(compiled, 1u) == compiled.end());
    });
}

TEST_CASE(CompileDepsPlainCpp) {
    // Simulates a plain .cpp file (unit 10) that imports a module (unit 20).
    make_graph(tracking_dispatch(compiled),
               static_resolver({
                   {10, {20}}
    }));

    execute([&]() -> kota::task<> {
        auto result = co_await graph->compile_deps(10).catch_cancel();
        EXPECT_TRUE(result.has_value());
        EXPECT_TRUE(*result);
        // Only dep 20 should be compiled, NOT the .cpp file itself.
        EXPECT_EQ(compiled.size(), 1u);
        EXPECT_EQ(compiled[0], 20u);
        EXPECT_TRUE(ranges::find(compiled, 10u) == compiled.end());
    });
}

TEST_CASE(CompileDepsConcurrentDedup) {
    // Two concurrent compile_deps calls with overlapping dependencies.
    // Each dep should be dispatched exactly once (no duplicate compilation).
    // Unit 1 depends on {3, 4}, unit 2 depends on {3, 5}.
    // Dep 3 is shared — must be compiled only once.
    make_graph(tracking_dispatch(compiled),
               static_resolver({
                   {1, {3, 4}},
                   {2, {3, 5}},
    }));

    execute([&]() -> kota::task<> {
        // Launch both compile_deps concurrently.
        auto t1 = graph->compile_deps(1);
        auto t2 = graph->compile_deps(2);
        auto results = co_await kota::when_all(std::move(t1), std::move(t2));

        auto [r1, r2] = results;
        EXPECT_TRUE(r1);
        EXPECT_TRUE(r2);

        // Deps 3, 4, 5 should each be compiled exactly once.
        // Unit 1 and 2 should NOT be compiled.
        ranges::sort(compiled);
        EXPECT_EQ(compiled.size(), 3u);
        EXPECT_EQ(compiled[0], 3u);
        EXPECT_EQ(compiled[1], 4u);
        EXPECT_EQ(compiled[2], 5u);
    });
}

TEST_CASE(CompileDepsResolveOnce) {
    // Verify that resolve_fn is called at most once per unit,
    // even when multiple compile_deps requests touch the same dependency.
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

        // Dep 3 compiled exactly once.
        EXPECT_EQ(compiled.size(), 1u);
        EXPECT_EQ(compiled[0], 3u);

        // resolve_fn called for units 1, 2, 3 — each at most once (3 total).
        EXPECT_EQ(resolve_count, 3);
    });
}

// Shared dependency matrix: A(1) -> B(2) -> E(5), C(3) -> D(4) -> E(5).

TEST_CASE(SharedDepSurvivesCancel) {
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
            // E is dispatching, with interest from both chains.
            co_await md.gate(5).started.wait();
            EXPECT_EQ(graph->refcount(5), 2u);
            EXPECT_EQ(md.gate(5).calls, 1);

            // Cancel request A: B's task exits and drops its edge on E,
            // but D's task still holds interest — E must keep compiling.
            ra.source.cancel();
            co_await kota::sleep(1);

            EXPECT_TRUE(ra.done);
            EXPECT_FALSE(graph->is_compiling(2));
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

TEST_CASE(SharedDepBothCancelled) {
    // E is shared at different depths: depth 2 from A, depth 1 from C.
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

            // Cancel both requests: E's interest drops to zero and its
            // compilation is cancelled too.
            ra.source.cancel();
            rc.source.cancel();
            co_await kota::sleep(1);

            EXPECT_FALSE(graph->is_compiling(5));
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

TEST_CASE(SharedDepQueuedCancel) {
    // E(5) itself depends on F(9); cancel A while E is still waiting for F,
    // i.e. E is queued behind its own dependency rather than dispatching.
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
            co_await kota::sleep(1);

            // E keeps waiting for F; only the A-chain died.
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

TEST_CASE(CompileDepsCancelReleases) {
    // A plain .cpp (10) holds root references on its direct deps; cancelling
    // the compile_deps request releases them without killing a dependency
    // that another consumer still needs.
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

TEST_CASE(SharedDepAlreadyCompiled) {
    // E was already built by the A-chain; cancelling/finishing A afterwards
    // must not invalidate it for the C-chain.
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
        // E was clean — not recompiled.
        EXPECT_EQ(md.gate(5).calls, 1);
    });
}

// Update semantics: staleness-driven cancellation, waiters retry.

TEST_CASE(UpdateWhileWaitingDeps) {
    // Update hits unit 1 while it waits for its dependency 2.
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

            // Only unit 1 is updated; its round is cancelled while the
            // waiter retries. Releasing its edge on 2 momentarily drops 2's
            // interest to zero, so 2 restarts when the retry re-acquires it.
            md.gate(2).started.reset();
            graph->update(1);

            co_await md.gate(2).started.wait();
            EXPECT_EQ(md.gate(2).calls, 2);
            md.gate(2).proceed.set();
            co_return;
        };

        co_await kota::when_all(compiler(), driver());

        EXPECT_TRUE(done);
        EXPECT_TRUE(result == true);
        EXPECT_FALSE(graph->is_dirty(1));
        EXPECT_FALSE(graph->is_dirty(2));
    });
}

TEST_CASE(UpdateDepCascadesCancel) {
    // Updating a dependency cancels the in-flight rounds of its dependents;
    // the request retries the whole chain and succeeds.
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

            // The cascade cancelled the in-flight rounds of 3 and its
            // dependents; the surviving waiter drives a fresh chain, which
            // restarts 3's dispatch.
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

// Failure semantics: no retry, edge references released.

TEST_CASE(FailedDepNoRetry) {
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
        // Failure propagates without retry, and 1 is never dispatched.
        EXPECT_EQ(md.gate(2).calls, 1);
        EXPECT_EQ(md.gate(1).calls, 0);
        // The failed round released its edge references.
        EXPECT_EQ(graph->refcount(2), 0u);
        EXPECT_EQ(graph->refcount(1), 0u);
    });
}

TEST_CASE(RecompileAfterFailure) {
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

        // Failure is not sticky: an explicit recompile tries again.
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

TEST_CASE(CancelAllRespawns) {
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

        // cancel_all kills the in-flight round; the waiter still holds its
        // interest, so it respawns the unit and succeeds.
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

// Cycles: all terminate with failure, never deadlock.

TEST_CASE(UpdateIntroducedCycle) {
    // The cycle only appears after update() forces a re-resolve of unit 2.
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

TEST_CASE(PartitionedCycle) {
    // The cycle (2 <-> 3) does not involve the requested root.
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

// Lifecycle: shutdown with tasks in flight.

TEST_CASE(ShutdownWithInflight) {
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
        // Cancel + join with several unit tasks in flight.
        co_await graph->shutdown();
        co_return;
    };

    auto t1 = run_request(1, req);
    auto t2 = driver();
    loop->schedule(t1);
    loop->schedule(t2);
    loop->run();

    // The pending request resolves with failure once the graph refuses to
    // respawn, and all bookkeeping is quiesced.
    EXPECT_TRUE(req.done);
    EXPECT_TRUE(req.result == false);
    EXPECT_TRUE(graph->idle());
}

// Randomized stress: seeded, single-threaded, deterministic.

TEST_CASE(RandomizedStress) {
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
            co_await kota::sleep(0);
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
