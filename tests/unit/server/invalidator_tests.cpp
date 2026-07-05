#include "test/temp_dir.h"
#include "test/test.h"
#include "server/compiler/compile_graph.h"
#include "server/context/context_resolver.h"
#include "server/workspace/invalidator.h"

namespace clice::testing {
namespace {

TEST_SUITE(Invalidator) {

TEST_CASE(EmptyBatchNoEffects) {
    Workspace workspace;
    SessionStore store;
    ContextResolver resolver(workspace);
    Invalidator invalidator(workspace, store, resolver);

    auto dirty = invalidator.apply({});

    ASSERT_TRUE(dirty.empty());
}

TEST_CASE(NoOpEventsNoEffects) {
    Workspace workspace;
    SessionStore store;
    auto file = workspace.path_pool.intern("/proj/a.cpp");
    store.open(file);

    ContextResolver resolver(workspace);
    Invalidator invalidator(workspace, store, resolver);
    // Buffer sync stays in SessionStore and context switching in
    // ContextResolver; these events must produce no effects of their own.
    FileEvent events[] = {FileEvent::buffer_opened(file),
                          FileEvent::buffer_edited(file),
                          FileEvent::context_changed(file)};
    auto dirty = invalidator.apply(events);

    ASSERT_TRUE(dirty.empty());
}

TEST_CASE(SaveResetsTrialOnly) {
    Workspace workspace;
    SessionStore store;
    auto saved = workspace.path_pool.intern("/proj/a.h");
    store.open(saved);

    ContextResolver resolver(workspace);
    Invalidator invalidator(workspace, store, resolver);
    auto dirty = invalidator.apply(FileEvent::buffer_saved(saved));

    // The saved file itself is not stale — its buffer was already current —
    // only its self-containment verdict needs re-evaluation.
    ASSERT_EQ(dirty.reset_trial, llvm::SmallVector<std::uint32_t>{saved});
    ASSERT_EQ(dirty.reset_header_mode, llvm::SmallVector<std::uint32_t>{saved});
    ASSERT_TRUE(dirty.mark_ast_dirty.empty());
    ASSERT_TRUE(dirty.force_revalidate.empty());
    ASSERT_TRUE(dirty.recheck_contexts);
    ASSERT_TRUE(dirty.reschedule_indexing);
}

TEST_CASE(CascadeSplitsOpenClosed) {
    kota::event_loop loop;
    Workspace workspace;
    SessionStore store;
    auto mod = workspace.path_pool.intern("/proj/m.cppm");
    auto open_user = workspace.path_pool.intern("/proj/open_user.cppm");
    auto closed_user = workspace.path_pool.intern("/proj/closed_user.cppm");

    llvm::DenseMap<std::uint32_t, llvm::SmallVector<std::uint32_t>> deps;
    deps[open_user] = {mod};
    deps[closed_user] = {mod};
    workspace.compile_graph = std::make_unique<CompileGraph>(
        loop,
        [](std::uint32_t) -> kota::task<bool> { co_return true; },
        [deps = std::move(deps)](std::uint32_t id) -> llvm::SmallVector<std::uint32_t> {
            auto it = deps.find(id);
            return it != deps.end() ? it->second : llvm::SmallVector<std::uint32_t>{};
        });

    store.open(open_user);
    ContextResolver resolver(workspace);
    Invalidator invalidator(workspace, store, resolver);

    auto body = [&]() -> kota::task<> {
        co_await workspace.compile_graph->compile(open_user);
        co_await workspace.compile_graph->compile(closed_user);

        auto dirty = invalidator.apply(FileEvent::buffer_saved(mod));

        // Cascade-dirtied module units split by session state: open buffers
        // recompile, closed files go back to the background indexer.
        EXPECT_EQ(dirty.mark_ast_dirty, llvm::SmallVector<std::uint32_t>{open_user});
        llvm::SmallVector<std::uint32_t> reindexed{mod, closed_user};
        llvm::sort(reindexed);
        EXPECT_EQ(dirty.enqueue_reindex, reindexed);

        co_await workspace.compile_graph->shutdown();
    };
    auto task = body();
    loop.schedule(task);
    loop.run();
}

TEST_CASE(ChainHitAndMiss) {
    Workspace workspace;
    SessionStore store;
    auto saved = workspace.path_pool.intern("/proj/inner.h");
    auto other = workspace.path_pool.intern("/proj/other.h");
    auto hit = workspace.path_pool.intern("/proj/hit.h");
    auto miss = workspace.path_pool.intern("/proj/miss.h");

    auto closed = workspace.path_pool.intern("/proj/closed.h");
    store.open(hit);
    store.open(miss);

    ContextResolver resolver(workspace);
    resolver.header_contexts[hit].chain = {saved};
    resolver.header_contexts[miss].chain = {other};
    resolver.header_contexts[closed].chain = {saved};
    Invalidator invalidator(workspace, store, resolver);
    auto dirty = invalidator.apply(FileEvent::buffer_saved(saved));

    // Every context embedding the saved file re-validates and drops its
    // verdict; a closed one additionally reindexes in the background — its
    // shard rows were built under the old chain.
    llvm::SmallVector<std::uint32_t> revalidated{hit, closed};
    llvm::sort(revalidated);
    ASSERT_EQ(dirty.force_revalidate, revalidated);
    llvm::SmallVector<std::uint32_t> reset{saved, hit, closed};
    llvm::sort(reset);
    ASSERT_EQ(dirty.reset_header_mode, reset);
    ASSERT_EQ(dirty.enqueue_reindex, llvm::SmallVector<std::uint32_t>{closed});
}

TEST_CASE(SaveMarksDependents) {
    Workspace workspace;
    SessionStore store;
    auto header = workspace.path_pool.intern("/proj/h.h");
    auto open_tu = workspace.path_pool.intern("/proj/a.cpp");
    auto closed_tu = workspace.path_pool.intern("/proj/b.cpp");
    workspace.dep_graph.set_includes(open_tu, 0, {header});
    workspace.dep_graph.set_includes(closed_tu, 0, {header});
    workspace.dep_graph.build_reverse_map();
    store.open(open_tu);

    ContextResolver resolver(workspace);
    Invalidator invalidator(workspace, store, resolver);
    auto dirty = invalidator.apply(FileEvent::buffer_saved(header));

    // Open dependents recompile, closed ones reindex; the old/new dependent
    // snapshots overlap fully here, so this also proves the dedup.
    ASSERT_EQ(dirty.mark_ast_dirty, llvm::SmallVector<std::uint32_t>{open_tu});
    ASSERT_EQ(dirty.enqueue_reindex, llvm::SmallVector<std::uint32_t>{closed_tu});
}

TEST_CASE(TransitiveDependentsEnqueue) {
    Workspace workspace;
    SessionStore store;
    auto header = workspace.path_pool.intern("/proj/h.h");
    auto middle = workspace.path_pool.intern("/proj/g.h");
    auto root = workspace.path_pool.intern("/proj/c.cpp");
    workspace.dep_graph.set_includes(middle, 0, {header});
    workspace.dep_graph.set_includes(root, 0, {middle});
    workspace.dep_graph.build_reverse_map();

    ContextResolver resolver(workspace);
    Invalidator invalidator(workspace, store, resolver);
    auto dirty = invalidator.apply(FileEvent::buffer_saved(header));

    // Only root TUs own index shards; the intermediate header is not one.
    ASSERT_EQ(dirty.enqueue_reindex, llvm::SmallVector<std::uint32_t>{root});
    ASSERT_TRUE(dirty.mark_ast_dirty.empty());
}

TEST_CASE(StaleReverseMapUnion) {
    Workspace workspace;
    SessionStore store;
    auto header = workspace.path_pool.intern("/proj/h.h");
    auto known = workspace.path_pool.intern("/proj/a.cpp");
    auto unmapped = workspace.path_pool.intern("/proj/b.cpp");
    workspace.dep_graph.set_includes(known, 0, {header});
    workspace.dep_graph.build_reverse_map();
    // Edge added without rebuilding the reverse map: visible only after the
    // save's rescan rebuilds it. Both snapshots must contribute.
    workspace.dep_graph.set_includes(unmapped, 0, {header});

    ContextResolver resolver(workspace);
    Invalidator invalidator(workspace, store, resolver);
    auto dirty = invalidator.apply(FileEvent::buffer_saved(header));

    llvm::SmallVector<std::uint32_t> expected{known, unmapped};
    llvm::sort(expected);
    ASSERT_EQ(dirty.enqueue_reindex, expected);
}

TEST_CASE(CloseEnqueuesReindex) {
    Workspace workspace;
    SessionStore store;
    auto closed = workspace.path_pool.intern("/proj/a.cpp");

    ContextResolver resolver(workspace);
    Invalidator invalidator(workspace, store, resolver);
    auto dirty = invalidator.apply(FileEvent::buffer_closed(closed));

    ASSERT_EQ(dirty.enqueue_reindex, llvm::SmallVector<std::uint32_t>{closed});
    ASSERT_TRUE(dirty.reschedule_indexing);
    ASSERT_TRUE(dirty.mark_ast_dirty.empty());
}

TEST_CASE(CrashMarksLostDirty) {
    Workspace workspace;
    SessionStore store;
    auto first = workspace.path_pool.intern("/proj/a.cpp");
    auto second = workspace.path_pool.intern("/proj/b.cpp");
    store.open(first);
    store.open(second);

    ContextResolver resolver(workspace);
    Invalidator invalidator(workspace, store, resolver);
    std::uint32_t lost[] = {first, second};
    auto dirty = invalidator.apply(FileEvent::worker_crashed(lost));

    llvm::SmallVector<std::uint32_t> expected{first, second};
    llvm::sort(expected);
    ASSERT_EQ(dirty.mark_lost, expected);
    // A crash loses build products, not compile inputs: no trial reset.
    ASSERT_TRUE(dirty.mark_ast_dirty.empty());
    ASSERT_TRUE(dirty.reset_trial.empty());
}

TEST_CASE(BatchSavesDeduplicate) {
    Workspace workspace;
    SessionStore store;
    auto saved = workspace.path_pool.intern("/proj/a.h");
    store.open(saved);

    ContextResolver resolver(workspace);
    Invalidator invalidator(workspace, store, resolver);
    FileEvent events[] = {FileEvent::buffer_saved(saved), FileEvent::buffer_saved(saved)};
    auto dirty = invalidator.apply(events);

    ASSERT_EQ(dirty.reset_trial, llvm::SmallVector<std::uint32_t>{saved});
}

};  // TEST_SUITE(Invalidator)

TEST_SUITE(DropOrphanedChoices) {

TEST_CASE(SurvivingEdgeKeepsChoice) {
    Workspace workspace;
    SessionStore store;
    ContextResolver resolver(workspace);
    auto host = workspace.path_pool.intern("/proj/host.cpp");
    auto header = workspace.path_pool.intern("/proj/h.h");
    workspace.dep_graph.set_includes(host, 0, {header});
    workspace.dep_graph.build_reverse_map();

    auto session = store.open(header);
    resolver.saved_contexts[header] = SavedContext{host, std::nullopt, ""};

    ASSERT_FALSE(resolver.drop_orphaned_choices(store));
    ASSERT_TRUE(resolver.saved_contexts.contains(header));
}

TEST_CASE(RemovedEdgeDropsChoice) {
    Workspace workspace;
    SessionStore store;
    ContextResolver resolver(workspace);
    auto host = workspace.path_pool.intern("/proj/host.cpp");
    auto header = workspace.path_pool.intern("/proj/h.h");
    workspace.dep_graph.build_reverse_map();

    auto session = store.open(header);
    session->trial_done = true;
    resolver.header_contexts[header] = HeaderContext{};
    resolver.saved_contexts[header] = SavedContext{host, std::nullopt, ""};
    auto generation = session->generation;

    ASSERT_TRUE(resolver.drop_orphaned_choices(store));
    ASSERT_FALSE(resolver.header_contexts.contains(header));
    ASSERT_TRUE(session->ast_dirty);
    ASSERT_FALSE(session->trial_done);
    ASSERT_EQ(session->generation, generation + 1);
    ASSERT_FALSE(resolver.saved_contexts.contains(header));
}

TEST_CASE(VanishedOccurrenceDropsChoice) {
    TempDir tmp;
    Workspace workspace;
    SessionStore store;
    ContextResolver resolver(workspace);
    // The host still includes the header, but only once — the pinned
    // occurrence #1 no longer exists.
    tmp.touch("host.cpp", R"(#include "h.h")");
    tmp.touch("h.h");
    auto host = workspace.path_pool.intern(tmp.path("host.cpp"));
    auto header = workspace.path_pool.intern(tmp.path("h.h"));
    workspace.dep_graph.set_includes(host, 0, {header});
    workspace.dep_graph.build_reverse_map();

    store.open(header);
    resolver.saved_contexts[header] = SavedContext{host, 1, ""};

    ASSERT_TRUE(resolver.drop_orphaned_choices(store));
    ASSERT_FALSE(resolver.saved_contexts.contains(header));
}

};  // TEST_SUITE(DropOrphanedChoices)

}  // namespace
}  // namespace clice::testing
