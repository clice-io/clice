#include "test/test.h"
#include "server/compile_graph.h"

namespace clice::testing {
namespace {

namespace et = eventide;

/// Create a dispatch function that always succeeds instantly.
inline CompileGraph::dispatch_fn instant_dispatch() {
    return [](std::uint32_t) -> et::task<bool> {
        co_return true;
    };
}

/// Create a dispatch function that records which units were compiled.
inline CompileGraph::dispatch_fn tracking_dispatch(std::vector<std::uint32_t>& compiled) {
    return [&compiled](std::uint32_t path_id) -> et::task<bool> {
        compiled.push_back(path_id);
        co_return true;
    };
}

/// Create a dispatch function that always fails.
inline CompileGraph::dispatch_fn failing_dispatch() {
    return [](std::uint32_t) -> et::task<bool> {
        co_return false;
    };
}

/// Helper: runs an async test. Creates event_loop + CompileGraph,
/// passes them to the test body, then the loop drains naturally.
template <typename F>
void run_test(CompileGraph::dispatch_fn dispatch, F&& body) {
    et::event_loop loop;
    CompileGraph graph(loop, std::move(dispatch));

    auto wrapper = [&]() -> et::task<> {
        co_await body(graph);
    };

    auto t = wrapper();
    loop.schedule(t);
    loop.run();
}

TEST_SUITE(CompileGraph) {

TEST_CASE(RegisterUnit) {
    run_test(instant_dispatch(), [this](CompileGraph& graph) -> et::task<> {
        graph.register_unit(1, {2, 3});

        EXPECT_TRUE(graph.has_unit(1));
        EXPECT_TRUE(graph.has_unit(2));
        EXPECT_TRUE(graph.has_unit(3));
        co_return;
    });
}

TEST_CASE(CompileNoDeps) {
    std::vector<std::uint32_t> compiled;
    run_test(tracking_dispatch(compiled), [this, &compiled](CompileGraph& graph) -> et::task<> {
        graph.register_unit(1, {});

        auto result = co_await graph.compile(1);
        EXPECT_TRUE(result);
        // compile() only ensures deps are ready; unit 1 has no deps,
        // so dispatch is never called (source files are compiled by Workers).
        EXPECT_TRUE(compiled.empty());
    });
}

TEST_CASE(CompileWithDependency) {
    std::vector<std::uint32_t> compiled;
    run_test(tracking_dispatch(compiled), [this, &compiled](CompileGraph& graph) -> et::task<> {
        // Unit 1 depends on unit 2.
        graph.register_unit(1, {2});

        auto result = co_await graph.compile(1);
        EXPECT_TRUE(result);
        EXPECT_EQ(compiled.size(), 1u);
        EXPECT_EQ(compiled[0], 2u);
        EXPECT_FALSE(graph.is_dirty(2));
    });
}

TEST_CASE(CompileChain) {
    std::vector<std::uint32_t> compiled;
    run_test(tracking_dispatch(compiled), [this, &compiled](CompileGraph& graph) -> et::task<> {
        // Chain: 1 -> 2 -> 3.
        graph.register_unit(3, {});
        graph.register_unit(2, {3});
        graph.register_unit(1, {2});

        auto result = co_await graph.compile(1);
        EXPECT_TRUE(result);

        // Both 2 and 3 should have been compiled (3 first, then 2).
        EXPECT_EQ(compiled.size(), 2u);
        // 3 must be compiled before 2.
        auto pos3 = std::find(compiled.begin(), compiled.end(), 3u);
        auto pos2 = std::find(compiled.begin(), compiled.end(), 2u);
        EXPECT_TRUE(pos3 < pos2);
    });
}

TEST_CASE(DiamondDependency) {
    std::vector<std::uint32_t> compiled;
    run_test(tracking_dispatch(compiled), [this, &compiled](CompileGraph& graph) -> et::task<> {
        // Diamond: 1 -> {2, 3}, 2 -> 4, 3 -> 4.
        graph.register_unit(4, {});
        graph.register_unit(3, {4});
        graph.register_unit(2, {4});
        graph.register_unit(1, {2, 3});

        auto result = co_await graph.compile(1);
        EXPECT_TRUE(result);

        // Unit 4 should be compiled exactly once (dedup).
        auto count4 = std::count(compiled.begin(), compiled.end(), 4u);
        EXPECT_EQ(count4, 1);

        // All deps should be clean.
        EXPECT_FALSE(graph.is_dirty(2));
        EXPECT_FALSE(graph.is_dirty(3));
        EXPECT_FALSE(graph.is_dirty(4));
    });
}

TEST_CASE(UpdateInvalidates) {
    run_test(instant_dispatch(), [this](CompileGraph& graph) -> et::task<> {
        graph.register_unit(1, {2});

        // Compile everything.
        co_await graph.compile(1);
        EXPECT_FALSE(graph.is_dirty(2));

        // Update unit 2 — should become dirty again.
        graph.update(2);
        EXPECT_TRUE(graph.is_dirty(2));
    });
}

TEST_CASE(UpdateCascade) {
    run_test(instant_dispatch(), [this](CompileGraph& graph) -> et::task<> {
        // Chain: 1 -> 2 -> 3.
        graph.register_unit(3, {});
        graph.register_unit(2, {3});
        graph.register_unit(1, {2});

        // Compile everything.
        co_await graph.compile(1);
        EXPECT_FALSE(graph.is_dirty(2));
        EXPECT_FALSE(graph.is_dirty(3));

        // Update leaf (3) — should cascade to 2 and 1.
        graph.update(3);
        EXPECT_TRUE(graph.is_dirty(3));
        EXPECT_TRUE(graph.is_dirty(2));
        EXPECT_TRUE(graph.is_dirty(1));
    });
}

TEST_CASE(CompileAfterUpdate) {
    std::vector<std::uint32_t> compiled;
    run_test(tracking_dispatch(compiled), [this, &compiled](CompileGraph& graph) -> et::task<> {
        graph.register_unit(1, {2});

        // First compile.
        co_await graph.compile(1);
        EXPECT_EQ(compiled.size(), 1u);

        // Update and recompile.
        graph.update(2);
        co_await graph.compile(1);

        // Unit 2 should have been compiled a second time.
        EXPECT_EQ(compiled.size(), 2u);
        EXPECT_EQ(compiled[1], 2u);
    });
}

TEST_CASE(CompileUnregistered) {
    run_test(instant_dispatch(), [this](CompileGraph& graph) -> et::task<> {
        // Compiling an unregistered unit should succeed (no deps to wait for).
        auto result = co_await graph.compile(999);
        EXPECT_TRUE(result);
    });
}

TEST_CASE(DispatchFailure) {
    run_test(failing_dispatch(), [this](CompileGraph& graph) -> et::task<> {
        graph.register_unit(1, {2});

        auto result = co_await graph.compile(1);
        // Dependency compilation failed → compile returns false.
        EXPECT_FALSE(result);
        // Unit 2 should still be dirty (compilation failed).
        EXPECT_TRUE(graph.is_dirty(2));
    });
}

TEST_CASE(CancelAll) {
    run_test(instant_dispatch(), [this](CompileGraph& graph) -> et::task<> {
        graph.register_unit(1, {});
        graph.register_unit(2, {});

        // cancel_all should not crash.
        graph.cancel_all();
        co_return;
    });
}

};  // TEST_SUITE(CompileGraph)

}  // namespace
}  // namespace clice::testing
