#include "test/cdb_helper.h"
#include "test/temp_dir.h"
#include "test/test.h"
#include "sched/context.h"
#include "sched/families/pcm.h"
#include "sched/families/turun.h"
#include "sched/graph.h"
#include "server/service/ast_family.h"
#include "server/service/context_service.h"
#include "server/state/invalidator.h"
#include "worker/pool.h"

namespace clice::testing {
namespace {

/// A loaded shard whose rows were built from `content`, for the
/// disk-vs-shard freshness comparisons below.
index::Shard shard_of(llvm::StringRef content) {
    std::string bytes;
    llvm::raw_string_ostream os(bytes);
    index::write_shard(
        {},
        [](index::SymbolHash) -> std::optional<index::SymbolIdentity> { return std::nullopt; },
        content,
        os);
    return index::Shard::from_buffer(llvm::MemoryBuffer::getMemBufferCopy(bytes));
}

/// Non-module fixtures need the invalidator's PCMFamily reference but
/// never drive it; this bundles the inert graph plumbing behind it.
struct PCMHarness {
    kota::event_loop loop;
    TaskGraph graph{loop};
    WorkerPool pool{loop};
    PCMFamily pcm;

    PCMHarness(Workspace& workspace, ContextResolver& resolver) :
        pcm(graph, workspace, resolver, pool) {}
};

/// The orphaned-choice tests exercise ContextService's session reset,
/// which goes through the AST family; this bundles its inert stack.
struct ASTHarness {
    kota::event_loop loop;
    TaskGraph graph{loop};
    WorkerPool pool{loop};
    PCMFamily pcm;
    PCHFamily pch;
    ASTFamily ast;

    ASTHarness(Workspace& workspace, ContextResolver& resolver, SessionStore& store) :
        pcm(graph, workspace, resolver, pool), pch(graph, workspace, resolver, pool),
        ast(workspace, resolver, graph, pcm, pch, pool, store, loop) {}
};

TEST_SUITE(Invalidator) {

TEST_CASE(EmptyBatchNoEffects) {
    Workspace workspace;
    SessionStore store;
    ContextResolver resolver(workspace);
    PCMHarness ph(workspace, resolver);
    Invalidator invalidator(workspace, store, resolver, ph.pcm);

    auto dirty = invalidator.apply({});

    ASSERT_TRUE(dirty.empty());
}

TEST_CASE(NewProviderDirtiesImporters) {
    // Consumers that scanned the name unresolved hold durable edges to
    // its sentinel node; the first provider cascades through them. A
    // closed TU reindexes as ContentChanged (its dep snapshot never
    // named the interface, so the hash gate cannot see the change); an
    // open document recompiles. Nothing is ever dropped — a consumer
    // that can no longer build keeps serving its last-known rows.
    TempDir tmp;
    tmp.touch("m.cppm", "export module m;\nexport int mv();\n");

    Workspace workspace;
    SessionStore store;
    auto iface = workspace.path_pool.intern(tmp.path("m.cppm"));
    auto closed = workspace.path_pool.intern("/proj/closed.cpp");
    auto open = workspace.path_pool.intern("/proj/open.cpp");
    store.open(open);

    ContextResolver resolver(workspace);
    PCMHarness ph(workspace, resolver);
    ph.graph.declare({turun_family, closed}, {PCMFamily::unresolved_node("m")});
    ph.graph.declare({ast_family, open}, {PCMFamily::unresolved_node("m")});
    Invalidator invalidator(workspace, store, resolver, ph.pcm);

    FileEvent events[] = {FileEvent::disk_changed(iface)};
    auto dirty = invalidator.apply(events);

    EXPECT_TRUE(llvm::is_contained(dirty.reindex_content_changed, closed));
    EXPECT_TRUE(llvm::is_contained(dirty.mark_ast_dirty, open));
    EXPECT_TRUE(dirty.drop_index.empty());

    // The same save again: the name already has its provider.
    auto again = invalidator.apply(events);
    EXPECT_FALSE(llvm::is_contained(again.reindex_content_changed, closed));
    EXPECT_FALSE(llvm::is_contained(again.mark_ast_dirty, open));
}

TEST_CASE(ReloadProviderCascades) {
    // The CDB-reload flavor of provider appearance: the provider-set diff
    // drives the same sentinel cascade, and a consumer retired by the
    // very same reload is enqueued harmlessly (its run falls to a skip)
    // rather than having its deliberately-kept index dropped.
    TempDir tmp;
    tmp.touch("m.cppm", "export module m;\nexport int mv();\n");

    Workspace workspace;
    SessionStore store;
    // The producer already reloaded the CDB: only the provider remains.
    write_cdb(tmp,
              workspace.cdb,
              build_cdb_json({
                  {tmp.root, tmp.path("m.cppm"), {}}
    }));
    auto iface = workspace.path_pool.intern(tmp.path("m.cppm"));
    auto retired = workspace.path_pool.intern(tmp.path("old.cpp"));

    ContextResolver resolver(workspace);
    PCMHarness ph(workspace, resolver);
    ph.graph.declare({turun_family, retired}, {PCMFamily::unresolved_node("m")});
    Invalidator invalidator(workspace, store, resolver, ph.pcm);

    FileEvent::CDBDelta delta;
    delta.added = {iface};
    delta.removed = {retired};
    FileEvent events[] = {FileEvent::cdb_changed(std::move(delta))};
    auto dirty = invalidator.apply(events);

    EXPECT_TRUE(llvm::is_contained(dirty.reindex_content_changed, retired));
    EXPECT_FALSE(llvm::is_contained(dirty.drop_index, retired));
}

TEST_CASE(DiskRemovedDropsProvider) {
    // Deleting a provider must leave the module map too: a later
    // replacement provider would otherwise sit behind the deleted one in
    // the candidate list and never be selected.
    Workspace workspace;
    SessionStore store;
    auto iface = workspace.path_pool.intern("/proj/m.cppm");
    workspace.dep_graph.add_module("m", iface);
    workspace.path_to_module[iface] = "m";

    ContextResolver resolver(workspace);
    PCMHarness ph(workspace, resolver);
    Invalidator invalidator(workspace, store, resolver, ph.pcm);

    FileEvent events[] = {FileEvent::disk_removed(iface)};
    invalidator.apply(events);

    EXPECT_TRUE(workspace.dep_graph.lookup_module("m").empty());
}

TEST_CASE(NoOpEventsNoEffects) {
    Workspace workspace;
    SessionStore store;
    auto file = workspace.path_pool.intern("/proj/a.cpp");
    store.open(file);

    ContextResolver resolver(workspace);
    PCMHarness ph(workspace, resolver);
    Invalidator invalidator(workspace, store, resolver, ph.pcm);
    // Buffer sync stays in SessionStore (exempt from the pipeline); these
    // events must produce no effects of their own.
    FileEvent events[] = {FileEvent::buffer_opened(file), FileEvent::buffer_edited(file)};
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
    PCMHarness ph(workspace, resolver);
    Invalidator invalidator(workspace, store, resolver, ph.pcm, [](llvm::StringRef) {
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
    Workspace workspace;
    SessionStore store;
    auto mod = workspace.path_pool.intern("/proj/m.cppm");
    auto open_user = workspace.path_pool.intern("/proj/open_user.cppm");
    auto closed_user = workspace.path_pool.intern("/proj/closed_user.cppm");

    ContextResolver resolver(workspace);
    PCMHarness ph(workspace, resolver);
    // The consumer edges build_deps declares in production — no rounds.
    auto node = [](std::uint32_t pid) {
        return NodeId{pcm_family, pid};
    };
    ph.graph.declare(node(open_user), {node(mod)});
    ph.graph.declare(node(closed_user), {node(mod)});

    store.open(open_user);
    Invalidator invalidator(workspace, store, resolver, ph.pcm);

    auto dirty = invalidator.apply(FileEvent::buffer_saved(mod));

    // Cascade-dirtied module units split by session state: open buffers
    // recompile, closed files go back to the background indexer.
    EXPECT_EQ(dirty.mark_ast_dirty, llvm::SmallVector<std::uint32_t>{open_user});
    llvm::SmallVector<std::uint32_t> reindexed{mod, closed_user};
    llvm::sort(reindexed);
    EXPECT_EQ(dirty.reindex_deps_only, reindexed);
    EXPECT_TRUE(dirty.reindex_content_changed.empty());
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
    PCMHarness ph(workspace, resolver);
    Invalidator invalidator(workspace, store, resolver, ph.pcm);
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
    // The closed header's own content did not change — only its chain did.
    ASSERT_EQ(dirty.reindex_deps_only, llvm::SmallVector<std::uint32_t>{closed});
    ASSERT_TRUE(dirty.reindex_content_changed.empty());
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
    PCMHarness ph(workspace, resolver);
    Invalidator invalidator(workspace, store, resolver, ph.pcm);
    auto dirty = invalidator.apply(FileEvent::buffer_saved(header));

    // Open dependents recompile, closed ones reindex; the old/new dependent
    // snapshots overlap fully here, so this also proves the dedup. A
    // dependent's own content did not change: deps-only.
    ASSERT_EQ(dirty.mark_ast_dirty, llvm::SmallVector<std::uint32_t>{open_tu});
    ASSERT_EQ(dirty.reindex_deps_only, llvm::SmallVector<std::uint32_t>{closed_tu});
    ASSERT_TRUE(dirty.reindex_content_changed.empty());
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
    PCMHarness ph(workspace, resolver);
    Invalidator invalidator(workspace, store, resolver, ph.pcm);
    auto dirty = invalidator.apply(FileEvent::buffer_saved(header));

    // Only root TUs own index shards; the intermediate header is not one.
    ASSERT_EQ(dirty.reindex_deps_only, llvm::SmallVector<std::uint32_t>{root});
    ASSERT_TRUE(dirty.reindex_content_changed.empty());
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
    PCMHarness ph(workspace, resolver);
    Invalidator invalidator(workspace, store, resolver, ph.pcm);
    auto dirty = invalidator.apply(FileEvent::buffer_saved(header));

    llvm::SmallVector<std::uint32_t> expected{known, unmapped};
    llvm::sort(expected);
    ASSERT_EQ(dirty.reindex_deps_only, expected);
    ASSERT_TRUE(dirty.reindex_content_changed.empty());
}

TEST_CASE(CloseWithoutShardReindexes) {
    Workspace workspace;
    SessionStore store;
    auto closed = workspace.path_pool.intern("/proj/a.cpp");

    ContextResolver resolver(workspace);
    // The file exists on disk (injected read), it just was never indexed.
    PCMHarness ph(workspace, resolver);
    Invalidator invalidator(workspace, store, resolver, ph.pcm, [](llvm::StringRef) {
        return std::optional<std::string>("int x;");
    });
    auto dirty = invalidator.apply(FileEvent::buffer_closed(closed));

    // No shard to compare against: nothing serves this file's rows anyway.
    ASSERT_EQ(dirty.reindex_content_changed, llvm::SmallVector<std::uint32_t>{closed});
    ASSERT_TRUE(dirty.reindex_deps_only.empty());
    ASSERT_TRUE(dirty.reschedule_indexing);
    ASSERT_TRUE(dirty.mark_ast_dirty.empty());
}

TEST_CASE(CloseCurrentShardDepsOnly) {
    Workspace workspace;
    SessionStore store;
    auto closed = workspace.path_pool.intern("/proj/a.cpp");
    workspace.shards[closed] = shard_of("int x;");

    ContextResolver resolver(workspace);
    // Disk matches the content the shard was built from: a browse-and-close
    // must not blank the file's rows for the reindex queue's latency.
    PCMHarness ph(workspace, resolver);
    Invalidator invalidator(workspace, store, resolver, ph.pcm, [](llvm::StringRef) {
        return std::optional<std::string>{"int x;"};
    });
    auto dirty = invalidator.apply(FileEvent::buffer_closed(closed));

    ASSERT_EQ(dirty.reindex_deps_only, llvm::SmallVector<std::uint32_t>{closed});
    ASSERT_TRUE(dirty.reindex_content_changed.empty());
}

TEST_CASE(CloseDivergentShardContentChanged) {
    Workspace workspace;
    SessionStore store;
    auto closed = workspace.path_pool.intern("/proj/a.cpp");
    workspace.shards[closed] = shard_of("int x;");

    ContextResolver resolver(workspace);
    // Disk holds edits the shard never saw (saved while open): the shard's
    // rows describe text that no longer exists.
    PCMHarness ph(workspace, resolver);
    Invalidator invalidator(workspace, store, resolver, ph.pcm, [](llvm::StringRef) {
        return std::optional<std::string>{"int edited;"};
    });
    auto dirty = invalidator.apply(FileEvent::buffer_closed(closed));

    ASSERT_EQ(dirty.reindex_content_changed, llvm::SmallVector<std::uint32_t>{closed});
    ASSERT_TRUE(dirty.reindex_deps_only.empty());
}

TEST_CASE(CloseStaleModuleCascades) {
    // An external rewrite consumed while the interface was open skipped the
    // module cascade (buffer authoritative) and the tracker will not refire:
    // the close must deliver it, or importers keep the pre-change PCM.
    Workspace workspace;
    SessionStore store;
    auto mod = workspace.path_pool.intern("/proj/m.cppm");
    auto user = workspace.path_pool.intern("/proj/user.cpp");
    workspace.shards[mod] = shard_of("export module m;\nexport int v1();\n");
    workspace.pcm_cache[mod] = {
        .path = "/cache/m.pcm",
        .key = "k",
        .deps = {.deps = {DepState{.path_id = mod, .missing = true}}},
    };

    ContextResolver resolver(workspace);
    PCMHarness ph(workspace, resolver);
    ph.graph.declare(
        {
            turun_family,
            user
    },
        {{pcm_family, mod}});
    Invalidator invalidator(workspace, store, resolver, ph.pcm, [](llvm::StringRef) {
        return std::optional<std::string>{"export module m;\nexport int v2();\n"};
    });
    auto dirty = invalidator.apply(FileEvent::buffer_closed(mod));

    EXPECT_TRUE(llvm::is_contained(dirty.reindex_content_changed, mod));
    EXPECT_TRUE(llvm::is_contained(dirty.reindex_deps_only, user));
    EXPECT_TRUE(workspace.pcm_cache.empty());
}

TEST_CASE(CloseRefreshesEdges) {
    // The shard can be current while the include edges are not (an
    // agent-mode reindex read the rewritten disk while the file stayed
    // open): the close refreshes the edges without a cascade.
    TempDir tmp;
    tmp.touch("new.h", "#pragma once\n");
    tmp.touch("a.cpp", "#include \"new.h\"\nint main() { return 0; }\n");

    Workspace workspace;
    SessionStore store;
    auto file = workspace.path_pool.intern(tmp.path("a.cpp"));
    auto old_header = workspace.path_pool.intern("/proj/old.h");
    auto new_header = workspace.path_pool.intern(tmp.path("new.h"));
    workspace.dep_graph.set_includes(file, 0, {old_header});
    workspace.dep_graph.build_reverse_map();

    auto disk = llvm::MemoryBuffer::getFile(tmp.path("a.cpp"));
    workspace.shards[file] = shard_of((*disk)->getBuffer());

    ContextResolver resolver(workspace);
    PCMHarness ph(workspace, resolver);
    Invalidator invalidator(workspace, store, resolver, ph.pcm);
    invalidator.apply(FileEvent::buffer_closed(file));

    auto includes = workspace.dep_graph.get_all_includes(file);
    EXPECT_TRUE(llvm::is_contained(includes, new_header));
    EXPECT_FALSE(llvm::is_contained(includes, old_header));
}

TEST_CASE(DeferredDiskChangeCascades) {
    // A DiskChanged consumed while the header was open defers the
    // dependent cascade to the close, and an agent-mode reindex can
    // refresh the shard from the rewritten disk before then: a current
    // shard must not hide the recorded debt from the close.
    Workspace workspace;
    SessionStore store;
    auto header = workspace.path_pool.intern("/proj/h.h");
    auto tu = workspace.path_pool.intern("/proj/a.cpp");
    workspace.dep_graph.set_includes(tu, 0, {header});
    workspace.dep_graph.build_reverse_map();
    workspace.shards[header] = shard_of("int rewritten;");
    store.open(header);

    ContextResolver resolver(workspace);
    PCMHarness ph(workspace, resolver);
    Invalidator invalidator(workspace, store, resolver, ph.pcm, [](llvm::StringRef) {
        return std::optional<std::string>{"int rewritten;"};
    });

    auto deferred = invalidator.apply(FileEvent::disk_changed(header));
    ASSERT_TRUE(deferred.reindex_deps_only.empty());

    store.close(header);
    auto dirty = invalidator.apply(FileEvent::buffer_closed(header));

    EXPECT_TRUE(llvm::is_contained(dirty.reindex_deps_only, tu));
    EXPECT_TRUE(llvm::is_contained(dirty.reindex_deps_only, header));
    EXPECT_TRUE(dirty.recheck_contexts);
}

TEST_CASE(CloseFirstProviderCascades) {
    // An external rewrite can make an open file a module's first provider
    // with the shard already current (an agent-mode reindex read the
    // rewritten disk): the close-time edge refresh must reach the name's
    // sentinel-edged consumers exactly as a save would.
    TempDir tmp;
    tmp.touch("m.cppm", "export module m;\n");

    Workspace workspace;
    SessionStore store;
    auto iface = workspace.path_pool.intern(tmp.path("m.cppm"));
    auto importer = workspace.path_pool.intern("/proj/use.cpp");

    auto disk = llvm::MemoryBuffer::getFile(tmp.path("m.cppm"));
    workspace.shards[iface] = shard_of((*disk)->getBuffer());

    ContextResolver resolver(workspace);
    PCMHarness ph(workspace, resolver);
    ph.graph.declare({turun_family, importer}, {PCMFamily::unresolved_node("m")});
    Invalidator invalidator(workspace, store, resolver, ph.pcm);

    auto dirty = invalidator.apply(FileEvent::buffer_closed(iface));
    EXPECT_TRUE(llvm::is_contained(dirty.reindex_content_changed, importer));
}

TEST_CASE(CloseProviderRenameCascades) {
    // An external rewrite can rename an open provider's module while an
    // agent-mode reindex keeps the shard current and an evicted PCM
    // leaves no cache entry for the close path's staleness probe: the
    // rescan's map delta is the only remaining signal, and it must
    // cascade the old name's consumers through the provider's node.
    TempDir tmp;
    tmp.touch("m.cppm", "export module b;\n");

    Workspace workspace;
    SessionStore store;
    auto iface = workspace.path_pool.intern(tmp.path("m.cppm"));
    auto importer = workspace.path_pool.intern("/proj/use.cpp");
    workspace.path_to_module[iface] = "a";
    workspace.dep_graph.update_module_decl(iface, "a");

    auto disk = llvm::MemoryBuffer::getFile(tmp.path("m.cppm"));
    workspace.shards[iface] = shard_of((*disk)->getBuffer());

    ContextResolver resolver(workspace);
    PCMHarness ph(workspace, resolver);
    ph.graph.declare(
        {
            turun_family,
            importer
    },
        {{pcm_family, iface}});
    Invalidator invalidator(workspace, store, resolver, ph.pcm);

    auto dirty = invalidator.apply(FileEvent::buffer_closed(iface));
    EXPECT_TRUE(llvm::is_contained(dirty.reindex_deps_only, importer));
}

TEST_CASE(CloseKeepsGuardedProvider) {
    // A close/save rescan meeting a module declaration inside a
    // preprocessor conditional must resolve it the way the startup scan
    // does (scan_quick alone leaves the name empty) instead of dropping
    // the provider and leaving its importers unresolved.
    TempDir tmp;
    tmp.touch("m.cpp", "#if 1\nexport module m;\n#endif\n");

    Workspace workspace;
    SessionStore store;
    auto iface = workspace.path_pool.intern(tmp.path("m.cpp"));
    workspace.path_to_module[iface] = "m";
    workspace.dep_graph.update_module_decl(iface, "m");

    auto disk = llvm::MemoryBuffer::getFile(tmp.path("m.cpp"));
    workspace.shards[iface] = shard_of((*disk)->getBuffer());

    ContextResolver resolver(workspace);
    PCMHarness ph(workspace, resolver);
    Invalidator invalidator(workspace, store, resolver, ph.pcm);

    invalidator.apply(FileEvent::buffer_closed(iface));

    EXPECT_EQ(workspace.path_to_module.lookup(iface), "m");
    EXPECT_TRUE(llvm::is_contained(workspace.dep_graph.lookup_module("m"), iface));
}

TEST_CASE(CloseStalePCMCascades) {
    // Agent-mode corner: a reindex read the rewritten disk while the file
    // was open, so the shard is current — but the PCM consumed bytes the
    // disk no longer holds. The artifact's deps snapshot is the judge, and
    // the current shard keeps serving (deps-only).
    Workspace workspace;
    SessionStore store;
    auto mod = workspace.path_pool.intern("/proj/m.cppm");
    auto user = workspace.path_pool.intern("/proj/user.cpp");
    workspace.shards[mod] = shard_of("export module m;\nexport int v2();\n");
    workspace.pcm_cache[mod] = {
        .path = "/cache/m.pcm",
        .key = "k",
        .deps = {.deps = {DepState{.path_id = mod, .hash = 1234}}},
    };

    ContextResolver resolver(workspace);
    PCMHarness ph(workspace, resolver);
    ph.graph.declare(
        {
            turun_family,
            user
    },
        {{pcm_family, mod}});
    Invalidator invalidator(workspace, store, resolver, ph.pcm, [](llvm::StringRef) {
        return std::optional<std::string>{"export module m;\nexport int v2();\n"};
    });
    auto dirty = invalidator.apply(FileEvent::buffer_closed(mod));

    llvm::SmallVector<std::uint32_t> reindexed{mod, user};
    llvm::sort(reindexed);
    EXPECT_EQ(dirty.reindex_deps_only, reindexed);
    EXPECT_TRUE(dirty.reindex_content_changed.empty());
    EXPECT_TRUE(workspace.pcm_cache.empty());
}

TEST_CASE(CrashMarksLostDirty) {
    Workspace workspace;
    SessionStore store;
    auto first = workspace.path_pool.intern("/proj/a.cpp");
    auto second = workspace.path_pool.intern("/proj/b.cpp");
    store.open(first);
    store.open(second);

    ContextResolver resolver(workspace);
    PCMHarness ph(workspace, resolver);
    Invalidator invalidator(workspace, store, resolver, ph.pcm);
    std::uint32_t lost[] = {first, second};
    auto dirty = invalidator.apply(FileEvent::worker_crashed(lost));

    llvm::SmallVector<std::uint32_t> expected{first, second};
    llvm::sort(expected);
    ASSERT_EQ(dirty.mark_lost, expected);
    // A crash loses build products, not compile inputs: no trial reset.
    ASSERT_TRUE(dirty.mark_ast_dirty.empty());
    ASSERT_TRUE(dirty.reset_trial.empty());
}

TEST_CASE(EvictionMarksLost) {
    Workspace workspace;
    SessionStore store;
    auto file = workspace.path_pool.intern("/proj/a.cpp");
    store.open(file);

    ContextResolver resolver(workspace);
    PCMHarness ph(workspace, resolver);
    Invalidator invalidator(workspace, store, resolver, ph.pcm);
    auto dirty = invalidator.apply(FileEvent::document_evicted(file));

    // Same loss as a crash, scoped to one document.
    ASSERT_EQ(dirty.mark_lost, llvm::SmallVector<std::uint32_t>{file});
    ASSERT_TRUE(dirty.mark_ast_dirty.empty());
    ASSERT_TRUE(dirty.reset_trial.empty());
}

TEST_CASE(BatchSavesDeduplicate) {
    Workspace workspace;
    SessionStore store;
    auto saved = workspace.path_pool.intern("/proj/a.h");
    store.open(saved);

    ContextResolver resolver(workspace);
    PCMHarness ph(workspace, resolver);
    Invalidator invalidator(workspace, store, resolver, ph.pcm);
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
    PCMHarness ph(workspace, resolver);
    Invalidator invalidator(workspace, store, resolver, ph.pcm, [](llvm::StringRef) {
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
    PCMHarness ph(workspace, resolver);
    Invalidator invalidator(workspace, store, resolver, ph.pcm, [](llvm::StringRef) {
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
    PCMHarness ph(workspace, resolver);
    Invalidator invalidator(workspace, store, resolver, ph.pcm);
    auto dirty = invalidator.apply(FileEvent::disk_changed(open_file));

    // The buffer is the truth for an open file: recompile so the next
    // compile's deps validation judges the disk change, but no rescan and
    // no cascade. The file's shard describes the old disk, so its reindex
    // queues alongside (skipped while open-file indexing is off).
    ASSERT_EQ(dirty.mark_ast_dirty, llvm::SmallVector<std::uint32_t>{open_file});
    ASSERT_EQ(dirty.reindex_content_changed, llvm::SmallVector<std::uint32_t>{open_file});
    ASSERT_TRUE(dirty.reindex_deps_only.empty());
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
    PCMHarness ph(workspace, resolver);
    Invalidator invalidator(workspace, store, resolver, ph.pcm);
    auto dirty = invalidator.apply(FileEvent::disk_changed(header));

    // A closed file's disk change cascades exactly like a save, plus the
    // file's own stale shard is refreshed. The changed file's own rows are
    // untrustworthy; its dependent only rebuilds semantics.
    ASSERT_EQ(dirty.mark_ast_dirty, llvm::SmallVector<std::uint32_t>{open_tu});
    ASSERT_EQ(dirty.reindex_content_changed, llvm::SmallVector<std::uint32_t>{header});
    ASSERT_EQ(dirty.reindex_deps_only, llvm::SmallVector<std::uint32_t>{closed_tu});
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
    PCMHarness ph(workspace, resolver);
    Invalidator invalidator(workspace, store, resolver, ph.pcm);
    auto dirty = invalidator.apply(FileEvent::disk_removed(removed_tu));

    // The removed file stops being an includer (and thus a host-source
    // candidate); surviving includers are untouched, shards are kept.
    ASSERT_EQ(workspace.dep_graph.get_includers(header), llvm::ArrayRef<std::uint32_t>{other_tu});
    ASSERT_TRUE(workspace.dep_graph.get_all_includes(removed_tu).empty());
    ASSERT_TRUE(dirty.recheck_contexts);
    ASSERT_TRUE(dirty.reindex_content_changed.empty());
    ASSERT_TRUE(dirty.reindex_deps_only.empty());
    ASSERT_TRUE(dirty.mark_ast_dirty.empty());
    ASSERT_EQ(workspace.context_epoch, epoch + 1);
    // The removal clears any pending-reindex state recorded earlier (e.g. a
    // DiskChanged observed just before deletion): the shard keeps serving
    // and nothing is left to reindex.
    ASSERT_EQ(dirty.clear_reindex, llvm::SmallVector<std::uint32_t>{removed_tu});
}

TEST_CASE(RemoveRecreateBatchOrder) {
    Workspace workspace;
    SessionStore store;
    auto file = workspace.path_pool.intern("/proj/a.cpp");
    workspace.dep_graph.set_includes(file, 0, {});
    workspace.dep_graph.build_reverse_map();
    ContextResolver resolver(workspace);
    PCMHarness ph(workspace, resolver);
    Invalidator invalidator(workspace, store, resolver, ph.pcm);

    // Change then delete: the removal is the later fact, the clear wins.
    {
        FileEvent events[] = {FileEvent::disk_changed(file), FileEvent::disk_removed(file)};
        auto dirty = invalidator.apply(events);
        ASSERT_TRUE(llvm::find(dirty.reindex_content_changed, file) ==
                    dirty.reindex_content_changed.end());
        ASSERT_EQ(dirty.clear_reindex, llvm::SmallVector<std::uint32_t>{file});
    }

    // Delete then recreate (an editor's atomic save): the later change must
    // survive — the recreated file needs its reindex.
    {
        workspace.dep_graph.set_includes(file, 0, {});
        workspace.dep_graph.build_reverse_map();
        FileEvent events[] = {FileEvent::disk_removed(file), FileEvent::disk_changed(file)};
        auto dirty = invalidator.apply(events);
        ASSERT_TRUE(dirty.clear_reindex.empty());
        ASSERT_TRUE(llvm::find(dirty.reindex_content_changed, file) !=
                    dirty.reindex_content_changed.end());
    }
}

TEST_CASE(EntryChangeThenRemoval) {
    TempDir tmp;
    tmp.touch("a.cpp", R"(int a;)");

    Workspace workspace;
    SessionStore store;
    auto json = build_cdb_json({
        {tmp.root, tmp.path("a.cpp"), {}}
    });
    write_cdb(tmp, workspace.cdb, json);
    auto file = workspace.path_pool.intern(tmp.path("a.cpp"));

    ContextResolver resolver(workspace);
    PCMHarness ph(workspace, resolver);
    Invalidator invalidator(workspace, store, resolver, ph.pcm);
    FileEvent::CDBDelta delta;
    delta.changed = {file};
    FileEvent events[] = {FileEvent::cdb_changed(std::move(delta)), FileEvent::disk_removed(file)};
    auto dirty = invalidator.apply(events);

    // The removal is the later fact: the file keeps its last-known index
    // serving, so the entry change's drop and enqueue must not survive — a
    // surviving drop would mask the shard and let the next save retire it.
    ASSERT_TRUE(dirty.drop_index.empty());
    ASSERT_TRUE(dirty.reindex_content_changed.empty());
    ASSERT_EQ(dirty.clear_reindex, llvm::SmallVector<std::uint32_t>{file});
}

TEST_CASE(CloseOfDeletedFile) {
    Workspace workspace;
    SessionStore store;
    auto file = workspace.path_pool.intern("/proj/gone.cpp");
    ContextResolver resolver(workspace);
    // Disk read fails: the file vanished while it was open.
    PCMHarness ph(workspace, resolver);
    Invalidator invalidator(workspace, store, resolver, ph.pcm, [](llvm::StringRef) {
        return std::optional<std::string>{};
    });

    auto dirty = invalidator.apply(FileEvent::buffer_closed(file));

    // The close is the first observation of the removal (the tracker skips
    // open files): keep any shard serving, do not record ContentChanged,
    // do not enqueue a nonexistent file.
    ASSERT_EQ(dirty.clear_reindex, llvm::SmallVector<std::uint32_t>{file});
    ASSERT_TRUE(dirty.reindex_content_changed.empty());
    ASSERT_TRUE(dirty.reindex_deps_only.empty());
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
    PCMHarness ph(workspace, resolver);
    Invalidator invalidator(workspace, store, resolver, ph.pcm);
    FileEvent::CDBDelta delta;
    delta.added = {main_id};
    auto dirty = invalidator.apply(FileEvent::cdb_changed(std::move(delta)));

    // The rescan resolved the new entry's includes; the new file reindexes.
    // A command change rewrites rows as thoroughly as an edit.
    ASSERT_EQ(workspace.dep_graph.get_includers(header_id), llvm::ArrayRef<std::uint32_t>{main_id});
    ASSERT_EQ(dirty.reindex_content_changed, llvm::SmallVector<std::uint32_t>{main_id});
    ASSERT_EQ(dirty.drop_index, llvm::SmallVector<std::uint32_t>{main_id});
    ASSERT_TRUE(dirty.reindex_deps_only.empty());
    ASSERT_TRUE(dirty.recheck_contexts);
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
    workspace.shards[open_id];
    workspace.shards[closed_id];

    ContextResolver resolver(workspace);
    PCMHarness ph(workspace, resolver);
    Invalidator invalidator(workspace, store, resolver, ph.pcm);
    FileEvent::CDBDelta delta;
    delta.changed = {open_id, closed_id};
    auto dirty = invalidator.apply(FileEvent::cdb_changed(std::move(delta)));

    // Flag changes recompile open files and reindex closed ones; the
    // pull-side cache keys (canonical flags) miss on their own.
    ASSERT_EQ(dirty.mark_ast_dirty, llvm::SmallVector<std::uint32_t>{open_id});
    llvm::SmallVector<std::uint32_t> reindexed{open_id, closed_id};
    llvm::sort(reindexed);
    auto content_changed = dirty.reindex_content_changed;
    llvm::sort(content_changed);
    ASSERT_EQ(content_changed, reindexed);
    ASSERT_TRUE(dirty.reindex_deps_only.empty());
    ASSERT_TRUE(dirty.recheck_contexts);

    // Both indexes were built under the old command and look fresh to
    // content-only validation: drop them so the queued reindexes are not
    // filtered out, here or after a restart. The shards themselves stay
    // with the indexer, which masks and retires them off the manifests.
    auto dropped = dirty.drop_index;
    llvm::sort(dropped);
    ASSERT_EQ(dropped, reindexed);
    ASSERT_EQ(workspace.shards.count(closed_id), 1u);
    ASSERT_EQ(workspace.shards.count(open_id), 1u);
}

TEST_CASE(CDBAddedOpenMarksDirty) {
    Workspace workspace;
    SessionStore store;
    auto file = workspace.path_pool.intern("/proj/a.cpp");
    store.open(file);

    ContextResolver resolver(workspace);
    PCMHarness ph(workspace, resolver);
    Invalidator invalidator(workspace, store, resolver, ph.pcm);
    FileEvent::CDBDelta delta;
    delta.added = {file};
    auto dirty = invalidator.apply(FileEvent::cdb_changed(std::move(delta)));

    // The open file gained its first real entry: drop the guessed command
    // it was compiled with, and queue the reindex that builds its shard
    // under the real command once open-file indexing is on.
    ASSERT_EQ(dirty.mark_ast_dirty, llvm::SmallVector<std::uint32_t>{file});
    ASSERT_EQ(dirty.reindex_content_changed, llvm::SmallVector<std::uint32_t>{file});
    ASSERT_EQ(dirty.drop_index, llvm::SmallVector<std::uint32_t>{file});
    ASSERT_TRUE(dirty.reindex_deps_only.empty());
}

TEST_CASE(CDBChangedDropsHostedContext) {
    Workspace workspace;
    SessionStore store;
    auto host = workspace.path_pool.intern("/proj/host.cpp");
    auto open_header = workspace.path_pool.intern("/proj/open.h");
    auto closed_header = workspace.path_pool.intern("/proj/closed.h");
    auto other_header = workspace.path_pool.intern("/proj/other.h");
    store.open(open_header);
    workspace.shards[closed_header];

    ContextResolver resolver(workspace);
    resolver.header_contexts[open_header].host_path_id = host;
    resolver.header_contexts[closed_header].host_path_id = host;
    resolver.header_contexts[other_header].host_path_id = no_path_id;
    PCMHarness ph(workspace, resolver);
    Invalidator invalidator(workspace, store, resolver, ph.pcm);
    FileEvent::CDBDelta delta;
    delta.changed = {host};
    auto dirty = invalidator.apply(FileEvent::cdb_changed(std::move(delta)));

    // Headers borrowing the changed entry re-resolve their context; the
    // open one recompiles, the closed one reindexes. Any standalone index
    // of theirs borrowed the changed command too, so it is dropped along
    // with the host's. Unrelated contexts are untouched.
    llvm::SmallVector<std::uint32_t> dropped{open_header, closed_header};
    llvm::sort(dropped);
    ASSERT_EQ(dirty.drop_context, dropped);
    llvm::SmallVector<std::uint32_t> evicted{host, open_header, closed_header};
    llvm::sort(evicted);
    auto drop = dirty.drop_index;
    llvm::sort(drop);
    ASSERT_EQ(drop, evicted);
    ASSERT_TRUE(llvm::is_contained(dirty.mark_ast_dirty, open_header));
    ASSERT_TRUE(llvm::is_contained(dirty.reindex_content_changed, closed_header));
    ASSERT_EQ(workspace.shards.count(closed_header), 1u);
}

TEST_CASE(CDBChangedCascadesModule) {
    Workspace workspace;
    SessionStore store;
    auto mod = workspace.path_pool.intern("/proj/m.cppm");
    auto open_user = workspace.path_pool.intern("/proj/open_user.cppm");
    auto closed_user = workspace.path_pool.intern("/proj/closed_user.cppm");

    ContextResolver resolver(workspace);
    PCMHarness ph(workspace, resolver);
    // The consumer edges build_deps declares in production — no rounds.
    auto node = [](std::uint32_t pid) {
        return NodeId{pcm_family, pid};
    };
    ph.graph.declare(node(open_user), {node(mod)});
    ph.graph.declare(node(closed_user), {node(mod)});

    store.open(open_user);
    Invalidator invalidator(workspace, store, resolver, ph.pcm);

    FileEvent::CDBDelta delta;
    delta.changed = {mod};
    auto dirty = invalidator.apply(FileEvent::cdb_changed(std::move(delta)));

    // A module unit's flag change cascades through the compile graph
    // exactly like a content change: importers' PCMs went stale. The
    // unit itself lands in both lists (its own entry changed AND the
    // cascade dirtied its PCM); the indexer's absorbing upgrade
    // resolves the overlap to ContentChanged.
    EXPECT_EQ(dirty.mark_ast_dirty, llvm::SmallVector<std::uint32_t>{open_user});
    EXPECT_EQ(dirty.reindex_content_changed, llvm::SmallVector<std::uint32_t>{mod});
    EXPECT_EQ(dirty.drop_index, llvm::SmallVector<std::uint32_t>{mod});
    llvm::SmallVector<std::uint32_t> deps{mod, closed_user};
    llvm::sort(deps);
    EXPECT_EQ(dirty.reindex_deps_only, deps);
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
    PCMHarness ph(workspace, resolver);
    Invalidator invalidator(workspace, store, resolver, ph.pcm);
    auto dirty = invalidator.apply(FileEvent::disk_removed(header));

    // Dependents now compile against a missing include: open ones
    // recompile, closed ones reindex.
    ASSERT_EQ(dirty.mark_ast_dirty, llvm::SmallVector<std::uint32_t>{open_tu});
    ASSERT_EQ(dirty.reindex_deps_only, llvm::SmallVector<std::uint32_t>{closed_tu});
    ASSERT_TRUE(dirty.reindex_content_changed.empty());
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
    PCMHarness ph(workspace, resolver);
    Invalidator invalidator(workspace, store, resolver, ph.pcm);
    FileEvent::CDBDelta delta;
    delta.removed = {gone_id};
    auto dirty = invalidator.apply(FileEvent::cdb_changed(std::move(delta)));

    // The rebuild resolves includes from the surviving entries only. A
    // removed entry keeps its index — the last-known rows still serve.
    ASSERT_TRUE(workspace.dep_graph.get_all_includes(gone_id).empty());
    ASSERT_EQ(workspace.dep_graph.get_includers(header_id), llvm::ArrayRef<std::uint32_t>{kept_id});
    ASSERT_TRUE(dirty.drop_index.empty());
    ASSERT_TRUE(dirty.recheck_contexts);
}

TEST_CASE(CDBEmptyDeltaNoEffects) {
    Workspace workspace;
    SessionStore store;

    ContextResolver resolver(workspace);
    PCMHarness ph(workspace, resolver);
    Invalidator invalidator(workspace, store, resolver, ph.pcm);
    auto dirty = invalidator.apply(FileEvent::cdb_changed({}));

    ASSERT_TRUE(dirty.empty());
}

TEST_CASE(BatchDiskEventsDeduplicate) {
    Workspace workspace;
    SessionStore store;
    auto first = workspace.path_pool.intern("/proj/a.h");
    auto second = workspace.path_pool.intern("/proj/b.h");

    ContextResolver resolver(workspace);
    PCMHarness ph(workspace, resolver);
    Invalidator invalidator(workspace, store, resolver, ph.pcm);
    FileEvent events[] = {FileEvent::disk_changed(first),
                          FileEvent::disk_changed(first),
                          FileEvent::disk_changed(second)};
    auto dirty = invalidator.apply(events);

    llvm::SmallVector<std::uint32_t> expected{first, second};
    llvm::sort(expected);
    ASSERT_EQ(dirty.reindex_content_changed, expected);
    ASSERT_TRUE(dirty.reindex_deps_only.empty());
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

    ASTHarness harness(workspace, resolver, store);
    ASSERT_FALSE(ContextService{workspace, resolver, harness.ast}.drop_orphaned_choices(store));
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

    ASTHarness harness(workspace, resolver, store);
    harness.ast.projections.entries[header].current = true;
    ASSERT_TRUE(ContextService{workspace, resolver, harness.ast}.drop_orphaned_choices(store));
    ASSERT_FALSE(resolver.header_contexts.contains(header));
    ASSERT_FALSE(harness.ast.projections.current(header));
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

    ASTHarness harness(workspace, resolver, store);
    ASSERT_TRUE(ContextService{workspace, resolver, harness.ast}.drop_orphaned_choices(store));
    ASSERT_FALSE(resolver.saved_contexts.contains(header));
}

};  // TEST_SUITE(DropOrphanedChoices)

}  // namespace
}  // namespace clice::testing
