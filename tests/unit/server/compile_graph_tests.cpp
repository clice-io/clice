#include "test/test.h"
#include "server/compile_graph.h"

namespace clice::testing {
namespace {

namespace et = eventide;

/// A resolve_fn that always returns no dependencies.
inline CompileGraph::resolve_fn no_deps() {
    return [](std::uint32_t) -> llvm::SmallVector<std::uint32_t> {
        return {};
    };
}

/// A resolve_fn backed by a static adjacency map.
inline CompileGraph::resolve_fn
    static_resolver(llvm::DenseMap<std::uint32_t, llvm::SmallVector<std::uint32_t>> adj) {
    return [adj = std::move(adj)](std::uint32_t path_id) -> llvm::SmallVector<std::uint32_t> {
        auto it = adj.find(path_id);
        if(it != adj.end()) {
            return it->second;
        }
        return {};
    };
}

inline CompileGraph::dispatch_fn instant_dispatch() {
    return [](std::uint32_t) -> et::task<bool> {
        co_return true;
    };
}

inline CompileGraph::dispatch_fn tracking_dispatch(std::vector<std::uint32_t>& compiled) {
    return [&compiled](std::uint32_t path_id) -> et::task<bool> {
        compiled.push_back(path_id);
        co_return true;
    };
}

inline CompileGraph::dispatch_fn failing_dispatch() {
    return [](std::uint32_t) -> et::task<bool> {
        co_return false;
    };
}

TEST_SUITE(CompileGraph) {

TEST_CASE(CompileNoDeps) {
    et::event_loop loop;
    std::vector<std::uint32_t> compiled;
    CompileGraph graph(tracking_dispatch(compiled), no_deps());

    auto test = [this, &graph, &compiled]() -> et::task<> {
        auto result = co_await graph.compile(1).catch_cancel();
        EXPECT_TRUE(result.has_value());
        EXPECT_TRUE(*result);
        EXPECT_EQ(compiled.size(), 1u);
        EXPECT_EQ(compiled[0], 1u);
        EXPECT_FALSE(graph.is_dirty(1));
    };

    auto t = test();
    loop.schedule(t);
    loop.run();
}

TEST_CASE(CompileWithDependency) {
    et::event_loop loop;
    std::vector<std::uint32_t> compiled;
    // Unit 1 depends on unit 2.
    CompileGraph graph(tracking_dispatch(compiled),
                       static_resolver({
                           {1, {2}}
    }));

    auto test = [this, &graph, &compiled]() -> et::task<> {
        auto result = co_await graph.compile(1).catch_cancel();
        EXPECT_TRUE(result.has_value());
        EXPECT_TRUE(*result);
        // Both 2 (dep) and 1 (self) should be compiled, in that order.
        EXPECT_EQ(compiled.size(), 2u);
        auto pos2 = std::find(compiled.begin(), compiled.end(), 2u);
        auto pos1 = std::find(compiled.begin(), compiled.end(), 1u);
        EXPECT_TRUE(pos2 < pos1);
        EXPECT_FALSE(graph.is_dirty(1));
        EXPECT_FALSE(graph.is_dirty(2));
    };

    auto t = test();
    loop.schedule(t);
    loop.run();
}

TEST_CASE(CompileChain) {
    et::event_loop loop;
    std::vector<std::uint32_t> compiled;
    // Chain: 1 -> 2 -> 3.
    CompileGraph graph(tracking_dispatch(compiled),
                       static_resolver({
                           {1, {2}},
                           {2, {3}}
    }));

    auto test = [this, &graph, &compiled]() -> et::task<> {
        auto result = co_await graph.compile(1).catch_cancel();
        EXPECT_TRUE(result.has_value());
        EXPECT_TRUE(*result);
        EXPECT_EQ(compiled.size(), 3u);
        // 3 before 2 before 1.
        auto pos3 = std::find(compiled.begin(), compiled.end(), 3u);
        auto pos2 = std::find(compiled.begin(), compiled.end(), 2u);
        auto pos1 = std::find(compiled.begin(), compiled.end(), 1u);
        EXPECT_TRUE(pos3 < pos2);
        EXPECT_TRUE(pos2 < pos1);
    };

    auto t = test();
    loop.schedule(t);
    loop.run();
}

TEST_CASE(DiamondDependency) {
    et::event_loop loop;
    std::vector<std::uint32_t> compiled;
    // Diamond: 1 -> {2, 3}, 2 -> 4, 3 -> 4.
    CompileGraph graph(tracking_dispatch(compiled),
                       static_resolver({
                           {1, {2, 3}},
                           {2, {4}   },
                           {3, {4}   }
    }));

    auto test = [this, &graph, &compiled]() -> et::task<> {
        auto result = co_await graph.compile(1).catch_cancel();
        EXPECT_TRUE(result.has_value());
        EXPECT_TRUE(*result);
        // Unit 4 should be compiled exactly once (dedup).
        auto count4 = std::count(compiled.begin(), compiled.end(), 4u);
        EXPECT_EQ(count4, 1);
        EXPECT_FALSE(graph.is_dirty(2));
        EXPECT_FALSE(graph.is_dirty(3));
        EXPECT_FALSE(graph.is_dirty(4));
    };

    auto t = test();
    loop.schedule(t);
    loop.run();
}

TEST_CASE(UpdateInvalidates) {
    et::event_loop loop;
    // 1 -> 2.
    CompileGraph graph(instant_dispatch(),
                       static_resolver({
                           {1, {2}}
    }));

    auto test = [this, &graph]() -> et::task<> {
        co_await graph.compile(1).catch_cancel();
        EXPECT_FALSE(graph.is_dirty(2));
        EXPECT_FALSE(graph.is_dirty(1));

        graph.update(2);
        EXPECT_TRUE(graph.is_dirty(2));
        // Cascade: 1 depends on 2, so 1 should also be dirty.
        EXPECT_TRUE(graph.is_dirty(1));
    };

    auto t = test();
    loop.schedule(t);
    loop.run();
}

TEST_CASE(UpdateCascade) {
    et::event_loop loop;
    // Chain: 1 -> 2 -> 3.
    CompileGraph graph(instant_dispatch(),
                       static_resolver({
                           {1, {2}},
                           {2, {3}}
    }));

    auto test = [this, &graph]() -> et::task<> {
        co_await graph.compile(1).catch_cancel();
        EXPECT_FALSE(graph.is_dirty(2));
        EXPECT_FALSE(graph.is_dirty(3));

        // Update leaf (3) — should cascade to 2 and 1.
        graph.update(3);
        EXPECT_TRUE(graph.is_dirty(3));
        EXPECT_TRUE(graph.is_dirty(2));
        EXPECT_TRUE(graph.is_dirty(1));
    };

    auto t = test();
    loop.schedule(t);
    loop.run();
}

TEST_CASE(CompileAfterUpdate) {
    et::event_loop loop;
    std::vector<std::uint32_t> compiled;
    // 1 -> 2.
    CompileGraph graph(tracking_dispatch(compiled),
                       static_resolver({
                           {1, {2}}
    }));

    auto test = [this, &graph, &compiled]() -> et::task<> {
        co_await graph.compile(1).catch_cancel();
        EXPECT_EQ(compiled.size(), 2u);

        graph.update(2);
        co_await graph.compile(1).catch_cancel();
        // 2 and 1 should be recompiled.
        EXPECT_EQ(compiled.size(), 4u);
    };

    auto t = test();
    loop.schedule(t);
    loop.run();
}

TEST_CASE(DispatchFailure) {
    et::event_loop loop;
    // 1 -> 2. Dispatch always fails.
    CompileGraph graph(failing_dispatch(),
                       static_resolver({
                           {1, {2}}
    }));

    auto test = [this, &graph]() -> et::task<> {
        auto result = co_await graph.compile(1).catch_cancel();
        EXPECT_TRUE(result.has_value());
        EXPECT_FALSE(*result);
        // Dep 2 failed, so it stays dirty.
        EXPECT_TRUE(graph.is_dirty(2));
    };

    auto t = test();
    loop.schedule(t);
    loop.run();
}

TEST_CASE(CancelAll) {
    CompileGraph graph(instant_dispatch(), no_deps());
    // Just verify it doesn't crash.
    graph.cancel_all();
}

TEST_CASE(SecondCompileSkips) {
    et::event_loop loop;
    std::vector<std::uint32_t> compiled;
    CompileGraph graph(tracking_dispatch(compiled), no_deps());

    auto test = [this, &graph, &compiled]() -> et::task<> {
        co_await graph.compile(1).catch_cancel();
        EXPECT_EQ(compiled.size(), 1u);
        // Second compile should skip (already clean).
        co_await graph.compile(1).catch_cancel();
        EXPECT_EQ(compiled.size(), 1u);
    };

    auto t = test();
    loop.schedule(t);
    loop.run();
}

TEST_CASE(CascadeThroughAlreadyDirty) {
    et::event_loop loop;
    // Chain: 1 -> 2 -> 3.
    CompileGraph graph(instant_dispatch(),
                       static_resolver({
                           {1, {2}},
                           {2, {3}}
    }));

    auto test = [this, &graph]() -> et::task<> {
        co_await graph.compile(1).catch_cancel();

        // Update node 2: marks 2 and 1 dirty.
        graph.update(2);
        EXPECT_TRUE(graph.is_dirty(1));
        EXPECT_TRUE(graph.is_dirty(2));
        EXPECT_FALSE(graph.is_dirty(3));

        // Now update node 3: must cascade through already-dirty 2 to reach 1.
        graph.update(3);
        EXPECT_TRUE(graph.is_dirty(3));
        EXPECT_TRUE(graph.is_dirty(2));
        EXPECT_TRUE(graph.is_dirty(1));
    };

    auto t = test();
    loop.schedule(t);
    loop.run();
}

TEST_CASE(CircularDependencyDetection) {
    et::event_loop loop;
    // Cycle: 1 -> 2 -> 1.
    CompileGraph graph(instant_dispatch(),
                       static_resolver({
                           {1, {2}},
                           {2, {1}}
    }));

    auto test = [this, &graph]() -> et::task<> {
        auto result = co_await graph.compile(1).catch_cancel();
        // Should return false (cycle detected), not deadlock.
        EXPECT_TRUE(result.has_value());
        EXPECT_FALSE(*result);
    };

    auto t = test();
    loop.schedule(t);
    loop.run();
}

TEST_CASE(CrossBranchCycleDetection) {
    et::event_loop loop;
    // Cross-branch cycle: 1 -> {2, 3}, 2 -> 3, 3 -> 2.
    // With when_all, sibling branches could deadlock on each other's
    // completion.wait() without proper deadlock detection.
    CompileGraph graph(instant_dispatch(),
                       static_resolver({
                           {1, {2, 3}},
                           {2, {3}   },
                           {3, {2}   }
    }));

    auto test = [this, &graph]() -> et::task<> {
        auto result = co_await graph.compile(1).catch_cancel();
        // Should return false (cycle detected), not deadlock.
        EXPECT_TRUE(result.has_value());
        EXPECT_FALSE(*result);
    };

    auto t = test();
    loop.schedule(t);
    loop.run();
}

TEST_CASE(UpdateResetsResolved) {
    et::event_loop loop;
    std::vector<std::uint32_t> compiled;
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

    CompileGraph graph(tracking_dispatch(compiled), std::move(resolver));

    auto test = [this, &graph, &compiled, &resolve_count, &updated]() -> et::task<> {
        // First compile: resolves 1 -> {2}.
        co_await graph.compile(1).catch_cancel();
        EXPECT_EQ(resolve_count, 1);
        EXPECT_EQ(compiled.size(), 2u);  // 2, then 1

        // Update node 1: resets resolved, changes deps.
        updated = true;
        graph.update(1);

        // Recompile: should re-resolve 1 -> {3}.
        co_await graph.compile(1).catch_cancel();
        EXPECT_EQ(resolve_count, 2);
        // New dep 3 should be compiled, then 1 recompiled.
        EXPECT_TRUE(std::find(compiled.begin() + 2, compiled.end(), 3u) != compiled.end());
    };

    auto t = test();
    loop.schedule(t);
    loop.run();
}

TEST_CASE(UpdateCleansStaleBackEdges) {
    et::event_loop loop;
    std::vector<std::uint32_t> compiled;
    bool updated = false;
    auto resolver = [&](std::uint32_t path_id) -> llvm::SmallVector<std::uint32_t> {
        if(path_id == 1) {
            // Initially depends on 2; after update, no deps.
            return updated ? llvm::SmallVector<std::uint32_t>{}
                           : llvm::SmallVector<std::uint32_t>{2};
        }
        return {};
    };

    CompileGraph graph(tracking_dispatch(compiled), std::move(resolver));

    auto test = [this, &graph, &compiled, &updated]() -> et::task<> {
        // First compile: 1 -> {2}.
        co_await graph.compile(1).catch_cancel();
        EXPECT_FALSE(graph.is_dirty(1));

        // Update 1: resets resolved, removes dep on 2.
        updated = true;
        graph.update(1);

        // Recompile: 1 has no deps now.
        co_await graph.compile(1).catch_cancel();
        EXPECT_FALSE(graph.is_dirty(1));

        // Now update 2: should NOT cascade to 1 (back-edge was removed).
        graph.update(2);
        EXPECT_TRUE(graph.is_dirty(2));
        EXPECT_FALSE(graph.is_dirty(1));
    };

    auto t = test();
    loop.schedule(t);
    loop.run();
}

TEST_CASE(DiamondUpdateCascade) {
    et::event_loop loop;
    std::vector<std::uint32_t> compiled;
    // Diamond: 1 -> {2, 3}, 2 -> 4, 3 -> 4.
    CompileGraph graph(tracking_dispatch(compiled),
                       static_resolver({
                           {1, {2, 3}},
                           {2, {4}   },
                           {3, {4}   }
    }));

    auto test = [this, &graph, &compiled]() -> et::task<> {
        co_await graph.compile(1).catch_cancel();
        EXPECT_FALSE(graph.is_dirty(1));
        EXPECT_FALSE(graph.is_dirty(4));

        // Update leaf 4: should cascade to 2, 3, and 1.
        graph.update(4);
        EXPECT_TRUE(graph.is_dirty(4));
        EXPECT_TRUE(graph.is_dirty(2));
        EXPECT_TRUE(graph.is_dirty(3));
        EXPECT_TRUE(graph.is_dirty(1));

        compiled.clear();
        auto result = co_await graph.compile(1).catch_cancel();
        EXPECT_TRUE(result.has_value() && *result);
        // Unit 4 should still be compiled exactly once (dedup on recompile).
        auto count4 = std::count(compiled.begin(), compiled.end(), 4u);
        EXPECT_EQ(count4, 1);
    };

    auto t = test();
    loop.schedule(t);
    loop.run();
}

TEST_CASE(UpdateReturnsAllDirtied) {
    et::event_loop loop;
    // Chain: 1 -> 2 -> 3.
    CompileGraph graph(instant_dispatch(),
                       static_resolver({
                           {1, {2}},
                           {2, {3}}
    }));

    auto test = [this, &graph]() -> et::task<> {
        co_await graph.compile(1).catch_cancel();

        auto dirtied = graph.update(3);
        // Should return 3, 2, 1 (all dirtied nodes).
        EXPECT_EQ(dirtied.size(), 3u);
        EXPECT_TRUE(llvm::find(dirtied, 1u) != dirtied.end());
        EXPECT_TRUE(llvm::find(dirtied, 2u) != dirtied.end());
        EXPECT_TRUE(llvm::find(dirtied, 3u) != dirtied.end());
    };

    auto t = test();
    loop.schedule(t);
    loop.run();
}

TEST_CASE(HasUnitAndIsCompiling) {
    et::event_loop loop;
    CompileGraph graph(instant_dispatch(), no_deps());

    auto test = [this, &graph]() -> et::task<> {
        EXPECT_FALSE(graph.has_unit(1));
        EXPECT_FALSE(graph.is_compiling(1));

        co_await graph.compile(1).catch_cancel();
        EXPECT_TRUE(graph.has_unit(1));
        EXPECT_FALSE(graph.is_compiling(1));
    };

    auto t = test();
    loop.schedule(t);
    loop.run();
}

TEST_CASE(DispatchFailureLeavesDepDirty) {
    et::event_loop loop;
    // 1 -> 2. Dispatch always fails.
    CompileGraph graph(failing_dispatch(),
                       static_resolver({
                           {1, {2}}
    }));

    auto test = [this, &graph]() -> et::task<> {
        auto result = co_await graph.compile(1).catch_cancel();
        EXPECT_TRUE(result.has_value());
        EXPECT_FALSE(*result);
        // Both dep and self should stay dirty.
        EXPECT_TRUE(graph.is_dirty(2));
        EXPECT_TRUE(graph.is_dirty(1));
    };

    auto t = test();
    loop.schedule(t);
    loop.run();
}

TEST_CASE(SelfLoop) {
    et::event_loop loop;
    // Unit 1 depends on itself.
    CompileGraph graph(instant_dispatch(),
                       static_resolver({
                           {1, {1}}
    }));

    auto test = [this, &graph]() -> et::task<> {
        auto result = co_await graph.compile(1).catch_cancel();
        // Should detect cycle and return false, not deadlock.
        EXPECT_TRUE(result.has_value());
        EXPECT_FALSE(*result);
    };

    auto t = test();
    loop.schedule(t);
    loop.run();
}

TEST_CASE(CancelAllAndRecompile) {
    et::event_loop loop;
    std::vector<std::uint32_t> compiled;
    CompileGraph graph(tracking_dispatch(compiled),
                       static_resolver({
                           {1, {2}}
    }));

    auto test = [this, &graph, &compiled]() -> et::task<> {
        co_await graph.compile(1).catch_cancel();
        EXPECT_EQ(compiled.size(), 2u);
        EXPECT_FALSE(graph.is_dirty(1));
        EXPECT_FALSE(graph.is_dirty(2));

        // cancel_all + update to mark dirty again.
        graph.cancel_all();
        graph.update(2);
        EXPECT_TRUE(graph.is_dirty(2));
        EXPECT_TRUE(graph.is_dirty(1));

        // Recompile should succeed normally.
        auto result = co_await graph.compile(1).catch_cancel();
        EXPECT_TRUE(result.has_value());
        EXPECT_TRUE(*result);
        EXPECT_EQ(compiled.size(), 4u);
        EXPECT_FALSE(graph.is_dirty(1));
        EXPECT_FALSE(graph.is_dirty(2));
    };

    auto t = test();
    loop.schedule(t);
    loop.run();
}

TEST_CASE(UpdateDuringCompile) {
    et::event_loop loop;
    et::event gate;

    auto gated_dispatch = [&gate](std::uint32_t) -> et::task<bool> {
        co_await gate.wait();
        co_return true;
    };

    CompileGraph graph(std::move(gated_dispatch), no_deps());

    bool compile_done = false;
    bool was_cancelled = false;

    // Coroutine 1: compile(1), will suspend inside dispatch waiting on gate.
    auto compiler = [&graph, &compile_done, &was_cancelled]() -> et::task<> {
        auto result = co_await graph.compile(1).catch_cancel();
        compile_done = true;
        was_cancelled = !result.has_value();
    };

    // Coroutine 2: update(1) while dispatch is in flight, then unblock gate.
    auto updater = [&graph, &gate]() -> et::task<> {
        graph.update(1);
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
    EXPECT_TRUE(graph.is_dirty(1));
}

};  // TEST_SUITE(CompileGraph)

}  // namespace
}  // namespace clice::testing
