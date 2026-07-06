#include "test/cdb_helper.h"
#include "test/temp_dir.h"
#include "test/test.h"
#include "server/compiler/compile_graph.h"
#include "server/compiler/context_resolver.h"
#include "server/state/invalidator.h"

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
    auto session = store.open(saved);
    store.apply_open(*session, "int x;", 1);

    ContextResolver resolver(workspace);
    // A plain save: the disk holds exactly what the buffer holds.
    Invalidator invalidator(workspace, store, resolver, [](llvm::StringRef) {
        return std::optional<std::string>{"int x;"};
    });
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

TEST_CASE(SaveDivergentDiskDirties) {
    Workspace workspace;
    SessionStore store;
    auto saved = workspace.path_pool.intern("/proj/a.h");
    auto session = store.open(saved);
    store.apply_open(*session, "int buffer;", 1);

    ContextResolver resolver(workspace);
    // A save hook rewrote the file as it landed: disk != buffer.
    Invalidator invalidator(workspace, store, resolver, [](llvm::StringRef) {
        return std::optional<std::string>{"int disk;"};
    });
    auto dirty = invalidator.apply(FileEvent::buffer_saved(saved));

    // The session recompiles so its deps snapshot re-validates against the
    // rewritten disk instead of describing a state that no longer exists.
    ASSERT_EQ(dirty.mark_ast_dirty, llvm::SmallVector<std::uint32_t>{saved});
}

TEST_CASE(SaveUnreadableDiskDirties) {
    Workspace workspace;
    SessionStore store;
    auto saved = workspace.path_pool.intern("/proj/a.h");
    auto session = store.open(saved);
    store.apply_open(*session, "int buffer;", 1);

    ContextResolver resolver(workspace);
    // The file cannot be read back after the save: the disk state is
    // unknown, which is treated as divergent (conservative).
    Invalidator invalidator(workspace, store, resolver, [](llvm::StringRef) {
        return std::optional<std::string>{};
    });
    auto dirty = invalidator.apply(FileEvent::buffer_saved(saved));

    ASSERT_EQ(dirty.mark_ast_dirty, llvm::SmallVector<std::uint32_t>{saved});
}

TEST_CASE(DiskChangeOpenMarksDirty) {
    Workspace workspace;
    SessionStore store;
    auto open_file = workspace.path_pool.intern("/proj/a.cpp");
    store.open(open_file);

    ContextResolver resolver(workspace);
    Invalidator invalidator(workspace, store, resolver);
    auto dirty = invalidator.apply(FileEvent::disk_changed(open_file));

    // The buffer is the truth for an open file: recompile so the next
    // compile's deps validation judges the disk change, but no rescan and
    // no cascade.
    ASSERT_EQ(dirty.mark_ast_dirty, llvm::SmallVector<std::uint32_t>{open_file});
    ASSERT_TRUE(dirty.enqueue_reindex.empty());
    ASSERT_TRUE(dirty.reset_trial.empty());
    ASSERT_FALSE(dirty.recheck_contexts);
}

TEST_CASE(DiskChangeClosedCascades) {
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
    auto dirty = invalidator.apply(FileEvent::disk_changed(header));

    // A closed file's disk change cascades exactly like a save, plus the
    // file's own stale shard is refreshed.
    ASSERT_EQ(dirty.mark_ast_dirty, llvm::SmallVector<std::uint32_t>{open_tu});
    llvm::SmallVector<std::uint32_t> reindexed{header, closed_tu};
    llvm::sort(reindexed);
    ASSERT_EQ(dirty.enqueue_reindex, reindexed);
    ASSERT_EQ(dirty.reset_trial, llvm::SmallVector<std::uint32_t>{header});
    ASSERT_TRUE(dirty.recheck_contexts);
    ASSERT_TRUE(dirty.reschedule_indexing);
}

TEST_CASE(DiskRemovedScrubsSourceRole) {
    Workspace workspace;
    SessionStore store;
    auto header = workspace.path_pool.intern("/proj/h.h");
    auto removed_tu = workspace.path_pool.intern("/proj/gone.cpp");
    auto other_tu = workspace.path_pool.intern("/proj/kept.cpp");
    workspace.dep_graph.set_includes(removed_tu, 0, {header});
    workspace.dep_graph.set_includes(other_tu, 0, {header});
    workspace.dep_graph.build_reverse_map();
    auto epoch = workspace.context_epoch;

    ContextResolver resolver(workspace);
    Invalidator invalidator(workspace, store, resolver);
    auto dirty = invalidator.apply(FileEvent::disk_removed(removed_tu));

    // The removed file stops being an includer (and thus a host-source
    // candidate); surviving includers are untouched, shards are kept.
    ASSERT_EQ(workspace.dep_graph.get_includers(header), llvm::ArrayRef<std::uint32_t>{other_tu});
    ASSERT_TRUE(workspace.dep_graph.get_all_includes(removed_tu).empty());
    ASSERT_TRUE(dirty.recheck_contexts);
    ASSERT_TRUE(dirty.enqueue_reindex.empty());
    ASSERT_TRUE(dirty.mark_ast_dirty.empty());
    ASSERT_EQ(workspace.context_epoch, epoch + 1);
}

TEST_CASE(CDBAddedScansAndEnqueues) {
    TempDir tmp;
    tmp.touch("inc/header.h", R"(int x = 1;)");
    tmp.touch("src/main.cpp", R"(#include "header.h")");

    Workspace workspace;
    SessionStore store;
    auto json = build_cdb_json({
        {tmp.root, tmp.path("src/main.cpp"), {"-I", tmp.path("inc")}}
    });
    write_cdb(tmp, workspace.cdb, json);
    auto main_id = workspace.path_pool.intern(tmp.path("src/main.cpp"));
    auto header_id = workspace.path_pool.intern(tmp.path("inc/header.h"));

    ContextResolver resolver(workspace);
    Invalidator invalidator(workspace, store, resolver);
    FileEvent::CDBDelta delta;
    delta.added = {main_id};
    auto dirty = invalidator.apply(FileEvent::cdb_changed(std::move(delta)));

    // The rescan resolved the new entry's includes; the new file reindexes.
    ASSERT_EQ(workspace.dep_graph.get_includers(header_id), llvm::ArrayRef<std::uint32_t>{main_id});
    ASSERT_EQ(dirty.enqueue_reindex, llvm::SmallVector<std::uint32_t>{main_id});
    ASSERT_TRUE(dirty.recheck_contexts);
    ASSERT_TRUE(dirty.ensure_compile_graph);
}

TEST_CASE(CDBChangedSplitsOpenClosed) {
    TempDir tmp;
    tmp.touch("a.cpp", R"(int a;)");
    tmp.touch("b.cpp", R"(int b;)");

    Workspace workspace;
    SessionStore store;
    auto json = build_cdb_json({
        {tmp.root, tmp.path("a.cpp"), {}},
        {tmp.root, tmp.path("b.cpp"), {}}
    });
    write_cdb(tmp, workspace.cdb, json);
    auto open_id = workspace.path_pool.intern(tmp.path("a.cpp"));
    auto closed_id = workspace.path_pool.intern(tmp.path("b.cpp"));
    store.open(open_id);
    workspace.merged_indices[open_id];
    workspace.merged_indices[closed_id];

    ContextResolver resolver(workspace);
    Invalidator invalidator(workspace, store, resolver);
    FileEvent::CDBDelta delta;
    delta.changed = {open_id, closed_id};
    auto dirty = invalidator.apply(FileEvent::cdb_changed(std::move(delta)));

    // Flag changes recompile open files and reindex closed ones; the
    // pull-side cache keys (canonical flags) miss on their own.
    ASSERT_EQ(dirty.mark_ast_dirty, llvm::SmallVector<std::uint32_t>{open_id});
    ASSERT_EQ(dirty.enqueue_reindex, llvm::SmallVector<std::uint32_t>{closed_id});
    ASSERT_TRUE(dirty.recheck_contexts);

    // The closed file's shard was built under the old command and looks
    // fresh to content-only validation: it must be evicted so the queued
    // reindex is not filtered out. The open file's shard stays (its next
    // compile owns the refresh).
    ASSERT_EQ(workspace.merged_indices.count(closed_id), 0u);
    ASSERT_EQ(workspace.merged_indices.count(open_id), 1u);
}

TEST_CASE(CDBAddedOpenMarksDirty) {
    Workspace workspace;
    SessionStore store;
    auto file = workspace.path_pool.intern("/proj/a.cpp");
    store.open(file);

    ContextResolver resolver(workspace);
    Invalidator invalidator(workspace, store, resolver);
    FileEvent::CDBDelta delta;
    delta.added = {file};
    auto dirty = invalidator.apply(FileEvent::cdb_changed(std::move(delta)));

    // The open file gained its first real entry: drop the guessed command
    // it was compiled with instead of queueing a background reindex.
    ASSERT_EQ(dirty.mark_ast_dirty, llvm::SmallVector<std::uint32_t>{file});
    ASSERT_TRUE(dirty.enqueue_reindex.empty());
}

TEST_CASE(CDBChangedDropsHostedContext) {
    Workspace workspace;
    SessionStore store;
    auto host = workspace.path_pool.intern("/proj/host.cpp");
    auto open_header = workspace.path_pool.intern("/proj/open.h");
    auto closed_header = workspace.path_pool.intern("/proj/closed.h");
    auto other_header = workspace.path_pool.intern("/proj/other.h");
    store.open(open_header);
    workspace.merged_indices[closed_header];

    ContextResolver resolver(workspace);
    resolver.header_contexts[open_header].host_path_id = host;
    resolver.header_contexts[closed_header].host_path_id = host;
    resolver.header_contexts[other_header].host_path_id = no_path_id;
    Invalidator invalidator(workspace, store, resolver);
    FileEvent::CDBDelta delta;
    delta.changed = {host};
    auto dirty = invalidator.apply(FileEvent::cdb_changed(std::move(delta)));

    // Headers borrowing the changed entry re-resolve their context; the
    // open one recompiles, the closed one loses its stale shard and
    // reindexes. Unrelated contexts are untouched.
    llvm::SmallVector<std::uint32_t> dropped{open_header, closed_header};
    llvm::sort(dropped);
    ASSERT_EQ(dirty.drop_context, dropped);
    ASSERT_TRUE(llvm::is_contained(dirty.mark_ast_dirty, open_header));
    ASSERT_TRUE(llvm::is_contained(dirty.enqueue_reindex, closed_header));
    ASSERT_EQ(workspace.merged_indices.count(closed_header), 0u);
}

TEST_CASE(CDBChangedCascadesModule) {
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

        FileEvent::CDBDelta delta;
        delta.changed = {mod};
        auto dirty = invalidator.apply(FileEvent::cdb_changed(std::move(delta)));

        // A module unit's flag change cascades through the compile graph
        // exactly like a content change: importers' PCMs went stale.
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

TEST_CASE(DiskRemovedReindexesIncluders) {
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
    auto dirty = invalidator.apply(FileEvent::disk_removed(header));

    // Dependents now compile against a missing include: open ones
    // recompile, closed ones reindex.
    ASSERT_EQ(dirty.mark_ast_dirty, llvm::SmallVector<std::uint32_t>{open_tu});
    ASSERT_EQ(dirty.enqueue_reindex, llvm::SmallVector<std::uint32_t>{closed_tu});
    ASSERT_TRUE(dirty.recheck_contexts);
}

TEST_CASE(CDBRemovedDropsSourceRole) {
    TempDir tmp;
    tmp.touch("inc/h.h", R"(int x;)");
    tmp.touch("kept.cpp", R"(#include "inc/h.h")");

    Workspace workspace;
    SessionStore store;
    // The pre-reload graph still shows gone.cpp as an includer; the CDB has
    // already been reloaded without it.
    auto gone_id = workspace.path_pool.intern(tmp.path("gone.cpp"));
    auto header_id = workspace.path_pool.intern(tmp.path("inc/h.h"));
    workspace.dep_graph.set_includes(gone_id, 0, {header_id});
    workspace.dep_graph.build_reverse_map();
    auto json = build_cdb_json({
        {tmp.root, tmp.path("kept.cpp"), {}}
    });
    write_cdb(tmp, workspace.cdb, json);
    auto kept_id = workspace.path_pool.intern(tmp.path("kept.cpp"));

    ContextResolver resolver(workspace);
    Invalidator invalidator(workspace, store, resolver);
    FileEvent::CDBDelta delta;
    delta.removed = {gone_id};
    auto dirty = invalidator.apply(FileEvent::cdb_changed(std::move(delta)));

    // The rebuild resolves includes from the surviving entries only.
    ASSERT_TRUE(workspace.dep_graph.get_all_includes(gone_id).empty());
    ASSERT_EQ(workspace.dep_graph.get_includers(header_id), llvm::ArrayRef<std::uint32_t>{kept_id});
    ASSERT_TRUE(dirty.recheck_contexts);
}

TEST_CASE(CDBEmptyDeltaNoEffects) {
    Workspace workspace;
    SessionStore store;

    ContextResolver resolver(workspace);
    Invalidator invalidator(workspace, store, resolver);
    auto dirty = invalidator.apply(FileEvent::cdb_changed({}));

    ASSERT_TRUE(dirty.empty());
}

TEST_CASE(BatchDiskEventsDeduplicate) {
    Workspace workspace;
    SessionStore store;
    auto first = workspace.path_pool.intern("/proj/a.h");
    auto second = workspace.path_pool.intern("/proj/b.h");

    ContextResolver resolver(workspace);
    Invalidator invalidator(workspace, store, resolver);
    FileEvent events[] = {FileEvent::disk_changed(first),
                          FileEvent::disk_changed(first),
                          FileEvent::disk_changed(second)};
    auto dirty = invalidator.apply(events);

    llvm::SmallVector<std::uint32_t> expected{first, second};
    llvm::sort(expected);
    ASSERT_EQ(dirty.enqueue_reindex, expected);
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
