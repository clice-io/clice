#include <optional>

#include "test/test.h"
#include "server/compile_graph.h"

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

TEST_SUITE(CompileGraph) {

std::vector<std::uint32_t> compiled;
std::optional<CompileGraph> graph;

template <typename F>
void execute(F&& fn) {
    kota::event_loop loop;
    auto t = fn();
    loop.schedule(t);
    loop.run();
}

TEST_CASE(CompileNoDeps) {
    graph.emplace(tracking_dispatch(compiled), no_deps());

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
    graph.emplace(tracking_dispatch(compiled),
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
    graph.emplace(tracking_dispatch(compiled),
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
    graph.emplace(tracking_dispatch(compiled),
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
    graph.emplace(instant_dispatch(),
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
    graph.emplace(instant_dispatch(),
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
    graph.emplace(tracking_dispatch(compiled),
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
    graph.emplace(failing_dispatch(),
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
    graph.emplace(instant_dispatch(), no_deps());
    // Just verify it doesn't crash.
    graph->cancel_all();
}

TEST_CASE(SecondCompileSkips) {
    graph.emplace(tracking_dispatch(compiled), no_deps());

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
    graph.emplace(instant_dispatch(),
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
    graph.emplace(instant_dispatch(),
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
    // With when_all, sibling branches could deadlock on each other's
    // completion.wait() without proper deadlock detection.
    graph.emplace(instant_dispatch(),
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

    graph.emplace(tracking_dispatch(compiled), std::move(resolver));

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

    graph.emplace(tracking_dispatch(compiled), std::move(resolver));

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
    graph.emplace(tracking_dispatch(compiled),
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
    graph.emplace(instant_dispatch(),
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
    graph.emplace(instant_dispatch(), no_deps());

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
    graph.emplace(failing_dispatch(),
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
    graph.emplace(instant_dispatch(),
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
    graph.emplace(tracking_dispatch(compiled),
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
    kota::event_loop loop;
    kota::event gate;

    auto gated_dispatch = [&gate](std::uint32_t) -> kota::task<bool> {
        co_await gate.wait();
        co_return true;
    };

    graph.emplace(std::move(gated_dispatch), no_deps());

    bool compile_done = false;
    bool was_cancelled = false;

    // Coroutine 1: compile(1), will suspend inside dispatch waiting on gate.
    auto compiler = [&]() -> kota::task<> {
        auto result = co_await graph->compile(1).catch_cancel();
        compile_done = true;
        was_cancelled = !result.has_value();
    };

    // Coroutine 2: update(1) while dispatch is in flight, then unblock gate.
    auto updater = [&]() -> kota::task<> {
        graph->update(1);
        gate.set();
        co_return;
    };

    auto t1 = compiler();
    auto t2 = updater();
    loop.schedule(t1);
    loop.schedule(t2);
    loop.run();

    // update() cancelled the source, so compile should have been cancelled.
    EXPECT_TRUE(compile_done);
    EXPECT_TRUE(was_cancelled);
    EXPECT_TRUE(graph->is_dirty(1));
}

TEST_CASE(WhenAllPartialFailure) {
    // 1 -> {2, 3}. Only unit 3 fails.
    graph.emplace(selective_dispatch({
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
    graph.emplace(instant_dispatch(), no_deps());

    // update on a path_id that was never compiled should not crash.
    auto dirtied = graph->update(999);
    EXPECT_EQ(dirtied.size(), 0u);
    EXPECT_FALSE(graph->has_unit(999));
}

TEST_CASE(EmptyGraphNoCompile) {
    // Construct and destroy without any compile calls.
    graph.emplace(instant_dispatch(), no_deps());
    EXPECT_FALSE(graph->has_unit(1));
    graph->cancel_all();  // Should not crash on empty graph.
}

TEST_CASE(CompileDepsNoDeps) {
    graph.emplace(tracking_dispatch(compiled), no_deps());

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
    graph.emplace(tracking_dispatch(compiled),
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
    graph.emplace(tracking_dispatch(compiled),
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
    graph.emplace(tracking_dispatch(compiled),
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

    graph.emplace(std::move(fail_and_track),
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
    graph.emplace(tracking_dispatch(compiled),
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
    graph.emplace(tracking_dispatch(compiled),
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

    graph.emplace(tracking_dispatch(compiled), std::move(resolve));

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

};  // TEST_SUITE(CompileGraph)

}  // namespace
}  // namespace clice::testing
