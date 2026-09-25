#include "test/cdb_helper.h"
#include "test/temp_dir.h"
#include "test/test.h"
#include "sched/families/pcm.h"
#include "sched/families/turun.h"
#include "sched/graph.h"
#include "server/ast_family.h"
#include "server/context_service.h"
#include "server/editor_context.h"
#include "server/invalidator.h"
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

/// Non-module fixtures need the invalidator's PCMFamily and index store
/// references but never drive them; this bundles the inert plumbing
/// behind them.
struct PCMHarness {
    kota::event_loop loop;
    TaskGraph graph{loop};
    WorkerPool pool{loop};
    PCMFamily pcm;
    IndexStore index;
    ASTProjectionTable projections;

    PCMHarness(Project& project, EditorContext& resolver) :
        pcm(graph, project, resolver.commands, pool), index(loop, project, resolver.commands) {}
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

    ASTHarness(Project& project, EditorContext& resolver, SessionStore& store) :
        pcm(graph, project, resolver.commands, pool), pch(graph, project, pool),
        ast(project, resolver, graph, pcm, pch, pool, store, loop) {}
};

TEST_SUITE(Invalidator) {

TEST_CASE(EmptyBatchNoEffects) {
    FileTable files;
    Project project{files};
    SessionStore store;
    CommandResolver commands(project);
    ContextsBlob blob;
    EditorContext resolver(project, commands, blob);
    PCMHarness ph(project, resolver);
    Invalidator invalidator(project, store, resolver, ph.projections, ph.pcm, ph.index);

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

    FileTable files;

    Project project{files};
    SessionStore store;
    auto iface = project.file_table.intern(tmp.path("m.cppm"));
    auto closed = project.file_table.intern("/proj/closed.cpp");
    auto open = project.file_table.intern("/proj/open.cpp");
    store.open(open);

    CommandResolver commands(project);
    ContextsBlob blob;
    EditorContext resolver(project, commands, blob);
    PCMHarness ph(project, resolver);
    ph.graph.declare({Family::TURun, closed.raw}, {PCMFamily::unresolved_node("m")});
    ph.graph.declare({Family::AST, open.raw}, {PCMFamily::unresolved_node("m")});
    Invalidator invalidator(project, store, resolver, ph.projections, ph.pcm, ph.index);

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
    // very same reload leaves the index without a reindex being owed.
    TempDir tmp;
    tmp.touch("m.cppm", "export module m;\nexport int mv();\n");

    FileTable files;

    Project project{files};
    SessionStore store;
    // The producer already reloaded the CDB: only the provider remains.
    write_cdb(tmp,
              project.cdb,
              build_cdb_json({
                  {tmp.root, tmp.path("m.cppm"), {}}
    }));
    auto iface = project.file_table.intern(tmp.path("m.cppm"));
    auto retired = project.file_table.intern(tmp.path("old.cpp"));

    CommandResolver commands(project);
    ContextsBlob blob;
    EditorContext resolver(project, commands, blob);
    PCMHarness ph(project, resolver);
    ph.graph.declare({Family::TURun, retired.raw}, {PCMFamily::unresolved_node("m")});
    Invalidator invalidator(project, store, resolver, ph.projections, ph.pcm, ph.index);

    FileEvent::CDBDelta delta;
    delta.added = {iface};
    delta.removed = {retired};
    FileEvent events[] = {FileEvent::cdb_changed(std::move(delta))};
    auto dirty = invalidator.apply(events);

    EXPECT_FALSE(llvm::is_contained(dirty.reindex_content_changed, retired));
    EXPECT_TRUE(llvm::is_contained(dirty.drop_index, retired));
    EXPECT_TRUE(llvm::is_contained(dirty.clear_reindex, retired));
}

TEST_CASE(DiskRemovedDropsProvider) {
    // Deleting a provider must leave the module map too: a later
    // replacement provider would otherwise sit behind the deleted one in
    // the candidate list and never be selected.
    FileTable files;
    Project project{files};
    SessionStore store;
    auto iface = project.file_table.intern("/proj/m.cppm");
    project.dep_graph.add_module("m", iface);

    CommandResolver commands(project);
    ContextsBlob blob;
    EditorContext resolver(project, commands, blob);
    PCMHarness ph(project, resolver);
    Invalidator invalidator(project, store, resolver, ph.projections, ph.pcm, ph.index);

    FileEvent events[] = {FileEvent::disk_removed(iface)};
    invalidator.apply(events);

    EXPECT_TRUE(project.dep_graph.lookup_module("m").empty());
}

TEST_CASE(DiskChangeSparesOwnSession) {
    TempDir tmp;
    tmp.touch("a.h", "int x;");

    FileTable files;

    Project project{files};
    SessionStore store;
    auto saved = project.file_table.intern(tmp.path("a.h"));
    auto session = store.open(saved);
    store.apply_open(*session, "int x;", 1);

    CommandResolver commands(project);
    ContextsBlob blob;
    EditorContext resolver(project, commands, blob);
    PCMHarness ph(project, resolver);
    Invalidator invalidator(project, store, resolver, ph.projections, ph.pcm, ph.index);
    auto dirty = invalidator.apply(FileEvent::disk_changed(saved));

    // The open file's own compile reads its buffer, never its disk: it is
    // not stale — only its self-containment verdict needs re-evaluation —
    // while its disk rows are.
    ASSERT_EQ(dirty.reset_trial, llvm::SmallVector<Fid>{saved});
    ASSERT_EQ(dirty.reset_header_mode, llvm::SmallVector<Fid>{saved});
    ASSERT_TRUE(dirty.mark_ast_dirty.empty());
    ASSERT_EQ(dirty.reindex_content_changed, llvm::SmallVector<Fid>{saved});
    ASSERT_TRUE(dirty.drop_context.empty());
    ASSERT_TRUE(dirty.recheck_contexts);
    ASSERT_TRUE(dirty.reschedule_indexing);
}

TEST_CASE(CascadeSplitsOpenClosed) {
    FileTable files;
    Project project{files};
    SessionStore store;
    auto mod = project.file_table.intern("/proj/m.cppm");
    auto open_user = project.file_table.intern("/proj/open_user.cppm");
    auto closed_user = project.file_table.intern("/proj/closed_user.cppm");

    CommandResolver commands(project);
    ContextsBlob blob;
    EditorContext resolver(project, commands, blob);
    PCMHarness ph(project, resolver);
    // The consumer edges build_deps declares in production — no rounds.
    auto node = [](Fid pid) {
        return NodeId{Family::PCM, pid.raw};
    };
    ph.graph.declare(node(open_user), {node(mod)});
    ph.graph.declare(node(closed_user), {node(mod)});

    store.open(open_user);
    Invalidator invalidator(project, store, resolver, ph.projections, ph.pcm, ph.index);

    auto dirty = invalidator.apply(FileEvent::disk_changed(mod));

    // Cascade-dirtied module units split by session state: open buffers
    // recompile, closed files go back to the background indexer.
    EXPECT_EQ(dirty.mark_ast_dirty, llvm::SmallVector<Fid>{open_user});
    llvm::SmallVector<Fid> reindexed{mod, closed_user};
    llvm::sort(reindexed);
    EXPECT_EQ(dirty.reindex_deps_only, reindexed);
    EXPECT_EQ(dirty.reindex_content_changed, llvm::SmallVector<Fid>{mod});
}

TEST_CASE(ChainHitAndMiss) {
    FileTable files;
    Project project{files};
    SessionStore store;
    auto saved = project.file_table.intern("/proj/inner.h");
    auto other = project.file_table.intern("/proj/other.h");
    auto hit = project.file_table.intern("/proj/hit.h");
    auto miss = project.file_table.intern("/proj/miss.h");

    auto closed = project.file_table.intern("/proj/closed.h");
    store.open(hit);
    store.open(miss);

    CommandResolver commands(project);
    ContextsBlob blob;
    EditorContext resolver(project, commands, blob);
    resolver.header_contexts[hit].chain = {saved};
    resolver.header_contexts[miss].chain = {other};
    resolver.header_contexts[closed].chain = {saved};
    PCMHarness ph(project, resolver);
    Invalidator invalidator(project, store, resolver, ph.projections, ph.pcm, ph.index);
    auto dirty = invalidator.apply(FileEvent::disk_changed(saved));

    // Every context derived through the saved file resolves again and
    // drops its verdict; an open one recompiles, a closed one reindexes in
    // the background — its shard rows were built under the old chain.
    llvm::SmallVector<Fid> dropped{hit, closed};
    llvm::sort(dropped);
    ASSERT_EQ(dirty.drop_context, dropped);
    ASSERT_EQ(dirty.mark_ast_dirty, llvm::SmallVector<Fid>{hit});
    llvm::SmallVector<Fid> reset{saved, hit, closed};
    llvm::sort(reset);
    ASSERT_EQ(dirty.reset_header_mode, reset);
    // The closed header's own content did not change — only its chain did.
    ASSERT_EQ(dirty.reindex_deps_only, llvm::SmallVector<Fid>{closed});
    ASSERT_EQ(dirty.reindex_content_changed, llvm::SmallVector<Fid>{saved});
}

TEST_CASE(SaveMarksDependents) {
    FileTable files;
    Project project{files};
    SessionStore store;
    auto header = project.file_table.intern("/proj/h.h");
    auto open_tu = project.file_table.intern("/proj/a.cpp");
    auto closed_tu = project.file_table.intern("/proj/b.cpp");
    project.dep_graph.set_includes(open_tu, 0, {{header}});
    project.dep_graph.set_includes(closed_tu, 0, {{header}});
    project.dep_graph.build_reverse_map();
    store.open(open_tu);

    CommandResolver commands(project);
    ContextsBlob blob;
    EditorContext resolver(project, commands, blob);
    PCMHarness ph(project, resolver);
    Invalidator invalidator(project, store, resolver, ph.projections, ph.pcm, ph.index);
    auto dirty = invalidator.apply(FileEvent::disk_changed(header));

    // Open dependents recompile, closed ones reindex; the old/new dependent
    // snapshots overlap fully here, so this also proves the dedup. A
    // dependent's own content did not change: deps-only.
    ASSERT_EQ(dirty.mark_ast_dirty, llvm::SmallVector<Fid>{open_tu});
    ASSERT_EQ(dirty.reindex_deps_only, llvm::SmallVector<Fid>{closed_tu});
    ASSERT_EQ(dirty.reindex_content_changed, llvm::SmallVector<Fid>{header});
}

TEST_CASE(TransitiveDependentsEnqueue) {
    FileTable files;
    Project project{files};
    SessionStore store;
    auto header = project.file_table.intern("/proj/h.h");
    auto middle = project.file_table.intern("/proj/g.h");
    auto root = project.file_table.intern("/proj/c.cpp");
    project.dep_graph.set_includes(middle, 0, {{header}});
    project.dep_graph.set_includes(root, 0, {{middle}});
    project.dep_graph.build_reverse_map();

    CommandResolver commands(project);
    ContextsBlob blob;
    EditorContext resolver(project, commands, blob);
    PCMHarness ph(project, resolver);
    Invalidator invalidator(project, store, resolver, ph.projections, ph.pcm, ph.index);
    auto dirty = invalidator.apply(FileEvent::disk_changed(header));

    // Only root TUs own index shards; the intermediate header is not one.
    ASSERT_EQ(dirty.reindex_deps_only, llvm::SmallVector<Fid>{root});
    ASSERT_EQ(dirty.reindex_content_changed, llvm::SmallVector<Fid>{header});
    ASSERT_TRUE(dirty.mark_ast_dirty.empty());
}

TEST_CASE(BatchSeesEarlierEdges) {
    // An includer an earlier event of the batch adds is visible to a later
    // cascade: the reverse map follows every rescan.
    TempDir tmp;
    tmp.touch("h.h", "int h;");
    tmp.touch("a.cpp", R"(#include "h.h")");
    tmp.touch("b.cpp", "int b;");
    FileTable files;
    Project project{files};
    SessionStore store;
    auto header = project.file_table.intern(tmp.path("h.h"));
    auto known = project.file_table.intern(tmp.path("a.cpp"));
    auto added = project.file_table.intern(tmp.path("b.cpp"));
    project.dep_graph.set_includes(known, 0, {{header}});
    project.dep_graph.set_includes(added, 0, {});
    project.dep_graph.build_reverse_map();
    tmp.touch("b.cpp", R"(#include "h.h")");

    CommandResolver commands(project);
    ContextsBlob blob;
    EditorContext resolver(project, commands, blob);
    PCMHarness ph(project, resolver);
    Invalidator invalidator(project, store, resolver, ph.projections, ph.pcm, ph.index);
    auto dirty =
        invalidator.apply({FileEvent::disk_changed(added), FileEvent::disk_changed(header)});

    ASSERT_TRUE(llvm::is_contained(dirty.reindex_deps_only, known));
    ASSERT_TRUE(llvm::is_contained(dirty.reindex_deps_only, added));
    ASSERT_TRUE(llvm::is_contained(project.dep_graph.get_includers(header), added));
}

TEST_CASE(RemovalThenChangeKeepsClear) {
    // A unit removed earlier in the batch is no longer an includer when a
    // header it included changes: its clear survives, nothing requeues it.
    TempDir tmp;
    tmp.touch("h.h", "int changed;");
    FileTable files;
    Project project{files};
    SessionStore store;
    auto header = project.file_table.intern(tmp.path("h.h"));
    auto removed = project.file_table.intern(tmp.path("gone.cpp"));
    auto kept = project.file_table.intern(tmp.path("kept.cpp"));
    project.dep_graph.set_includes(removed, 0, {{header}});
    project.dep_graph.set_includes(kept, 0, {{header}});
    project.dep_graph.build_reverse_map();

    CommandResolver commands(project);
    ContextsBlob blob;
    EditorContext resolver(project, commands, blob);
    PCMHarness ph(project, resolver);
    Invalidator invalidator(project, store, resolver, ph.projections, ph.pcm, ph.index);
    auto dirty =
        invalidator.apply({FileEvent::disk_removed(removed), FileEvent::disk_changed(header)});

    ASSERT_TRUE(llvm::is_contained(dirty.clear_reindex, removed));
    ASSERT_FALSE(llvm::is_contained(dirty.reindex_deps_only, removed));
    ASSERT_TRUE(llvm::is_contained(dirty.reindex_deps_only, kept));
}

TEST_CASE(RescanKeepsGuardedProvider) {
    // A disk-change rescan meeting a module declaration inside a
    // preprocessor conditional must resolve it the way the startup scan
    // does (scan_quick alone leaves the name empty) instead of dropping
    // the provider and leaving its importers unresolved.
    TempDir tmp;
    tmp.touch("m.cpp", "#if 1\nexport module m;\n#endif\n");

    FileTable files;

    Project project{files};
    SessionStore store;
    auto iface = project.file_table.intern(tmp.path("m.cpp"));
    project.dep_graph.update_module_decl(iface, "m");

    auto disk = llvm::MemoryBuffer::getFile(tmp.path("m.cpp"));
    project.project_index.shards[iface] = shard_of((*disk)->getBuffer());

    CommandResolver commands(project);
    ContextsBlob blob;
    EditorContext resolver(project, commands, blob);
    PCMHarness ph(project, resolver);
    Invalidator invalidator(project, store, resolver, ph.projections, ph.pcm, ph.index);

    invalidator.apply(FileEvent::disk_changed(iface));

    EXPECT_EQ(project.dep_graph.module_of(iface), "m");
    EXPECT_TRUE(llvm::is_contained(project.dep_graph.lookup_module("m"), iface));
}

TEST_CASE(CrashMarksLostDirty) {
    FileTable files;
    Project project{files};
    SessionStore store;
    auto first = project.file_table.intern("/proj/a.cpp");
    auto second = project.file_table.intern("/proj/b.cpp");
    store.open(first);
    store.open(second);

    CommandResolver commands(project);
    ContextsBlob blob;
    EditorContext resolver(project, commands, blob);
    PCMHarness ph(project, resolver);
    Invalidator invalidator(project, store, resolver, ph.projections, ph.pcm, ph.index);
    Fid lost[] = {first, second};
    auto dirty = invalidator.apply(FileEvent::worker_crashed(lost));

    llvm::SmallVector<Fid> expected{first, second};
    llvm::sort(expected);
    ASSERT_EQ(dirty.mark_lost, expected);
    // A crash loses build products, not compile inputs: no trial reset.
    ASSERT_TRUE(dirty.mark_ast_dirty.empty());
    ASSERT_TRUE(dirty.reset_trial.empty());
}

TEST_CASE(EvictionMarksLost) {
    FileTable files;
    Project project{files};
    SessionStore store;
    auto file = project.file_table.intern("/proj/a.cpp");
    store.open(file);

    CommandResolver commands(project);
    ContextsBlob blob;
    EditorContext resolver(project, commands, blob);
    PCMHarness ph(project, resolver);
    Invalidator invalidator(project, store, resolver, ph.projections, ph.pcm, ph.index);
    auto dirty = invalidator.apply(FileEvent::document_evicted(file));

    // Same loss as a crash, scoped to one document.
    ASSERT_EQ(dirty.mark_lost, llvm::SmallVector<Fid>{file});
    ASSERT_TRUE(dirty.mark_ast_dirty.empty());
    ASSERT_TRUE(dirty.reset_trial.empty());
}

TEST_CASE(BatchChangesDeduplicate) {
    FileTable files;
    Project project{files};
    SessionStore store;
    auto saved = project.file_table.intern("/proj/a.h");
    store.open(saved);

    CommandResolver commands(project);
    ContextsBlob blob;
    EditorContext resolver(project, commands, blob);
    PCMHarness ph(project, resolver);
    Invalidator invalidator(project, store, resolver, ph.projections, ph.pcm, ph.index);
    FileEvent events[] = {FileEvent::disk_changed(saved), FileEvent::disk_changed(saved)};
    auto dirty = invalidator.apply(events);

    ASSERT_EQ(dirty.reset_trial, llvm::SmallVector<Fid>{saved});
}

TEST_CASE(DiskChangeClosedCascades) {
    FileTable files;
    Project project{files};
    SessionStore store;
    auto header = project.file_table.intern("/proj/h.h");
    auto open_tu = project.file_table.intern("/proj/a.cpp");
    auto closed_tu = project.file_table.intern("/proj/b.cpp");
    project.dep_graph.set_includes(open_tu, 0, {{header}});
    project.dep_graph.set_includes(closed_tu, 0, {{header}});
    project.dep_graph.build_reverse_map();
    store.open(open_tu);

    CommandResolver commands(project);
    ContextsBlob blob;
    EditorContext resolver(project, commands, blob);
    PCMHarness ph(project, resolver);
    Invalidator invalidator(project, store, resolver, ph.projections, ph.pcm, ph.index);
    auto dirty = invalidator.apply(FileEvent::disk_changed(header));

    // A closed file's disk change cascades exactly like a save, plus the
    // file's own stale shard is refreshed. The changed file's own rows are
    // untrustworthy; its dependent only rebuilds semantics.
    ASSERT_EQ(dirty.mark_ast_dirty, llvm::SmallVector<Fid>{open_tu});
    ASSERT_EQ(dirty.reindex_content_changed, llvm::SmallVector<Fid>{header});
    ASSERT_EQ(dirty.reindex_deps_only, llvm::SmallVector<Fid>{closed_tu});
    ASSERT_EQ(dirty.reset_trial, llvm::SmallVector<Fid>{header});
    ASSERT_TRUE(dirty.recheck_contexts);
    ASSERT_TRUE(dirty.reschedule_indexing);
}

TEST_CASE(DiskChangeOpenCascades) {
    // Open or not, a disk change reaches every file reading the disk: the
    // open header's includers cascade now, not at its close, and only the
    // header's own session — whose compile reads its buffer — is spared.
    FileTable files;
    Project project{files};
    SessionStore store;
    auto header = project.file_table.intern("/proj/h.h");
    auto open_tu = project.file_table.intern("/proj/a.cpp");
    auto closed_tu = project.file_table.intern("/proj/b.cpp");
    project.dep_graph.set_includes(open_tu, 0, {{header}});
    project.dep_graph.set_includes(closed_tu, 0, {{header}});
    project.dep_graph.build_reverse_map();
    store.open(header);
    store.open(open_tu);

    CommandResolver commands(project);
    ContextsBlob blob;
    EditorContext resolver(project, commands, blob);
    PCMHarness ph(project, resolver);
    Invalidator invalidator(project, store, resolver, ph.projections, ph.pcm, ph.index);
    auto dirty = invalidator.apply(FileEvent::disk_changed(header));

    ASSERT_EQ(dirty.mark_ast_dirty, llvm::SmallVector<Fid>{open_tu});
    ASSERT_EQ(dirty.reindex_content_changed, llvm::SmallVector<Fid>{header});
    ASSERT_EQ(dirty.reindex_deps_only, llvm::SmallVector<Fid>{closed_tu});
}

TEST_CASE(CompiledIncluderCascades) {
    // An includer the lexical scan never saw (a macro include) but whose
    // indexed compile read the file is a dependent all the same.
    FileTable files;
    Project project{files};
    SessionStore store;
    auto header = project.file_table.intern("/proj/m.h");
    auto scanned = project.file_table.intern("/proj/a.cpp");
    auto compiled = project.file_table.intern("/proj/b.cpp");
    project.dep_graph.set_includes(scanned, 0, {{header}});
    project.dep_graph.set_includes(compiled, 0, {});
    project.dep_graph.build_reverse_map();
    project.project_index.contributions[header][compiled] = 1;

    CommandResolver commands(project);
    ContextsBlob blob;
    EditorContext resolver(project, commands, blob);
    PCMHarness ph(project, resolver);
    Invalidator invalidator(project, store, resolver, ph.projections, ph.pcm, ph.index);
    auto dirty = invalidator.apply(FileEvent::disk_changed(header));

    llvm::SmallVector<Fid> reindexed{scanned, compiled};
    llvm::sort(reindexed);
    ASSERT_EQ(dirty.reindex_deps_only, reindexed);
}

TEST_CASE(AppearedHeaderCascades) {
    // A header appearing where compiles looked for it: the closed TU whose
    // indexed compile looked reindexes, the open document whose AST looked
    // recompiles.
    FileTable files;
    Project project{files};
    SessionStore store;
    auto header = project.file_table.intern("/proj/gen.h");
    auto closed = project.file_table.intern("/proj/b.cpp");
    auto open = project.file_table.intern("/proj/a.cpp");
    project.project_index.probed[header].insert(closed);
    store.open(open);

    CommandResolver commands(project);
    ContextsBlob blob;
    EditorContext resolver(project, commands, blob);
    PCMHarness ph(project, resolver);
    ph.projections.entries[open].deps = DepsSnapshot{
        DepState{.path_id = header, .missing = true}
    };
    Invalidator invalidator(project, store, resolver, ph.projections, ph.pcm, ph.index);
    auto dirty = invalidator.apply(FileEvent::disk_changed(header));

    ASSERT_EQ(dirty.reindex_deps_only, llvm::SmallVector<Fid>{closed});
    ASSERT_EQ(dirty.mark_ast_dirty, llvm::SmallVector<Fid>{open});
}

TEST_CASE(DiskRemovedScrubsSourceRole) {
    FileTable files;
    Project project{files};
    SessionStore store;
    auto header = project.file_table.intern("/proj/h.h");
    auto removed_tu = project.file_table.intern("/proj/gone.cpp");
    auto other_tu = project.file_table.intern("/proj/kept.cpp");
    project.dep_graph.set_includes(removed_tu, 0, {{header}});
    project.dep_graph.set_includes(other_tu, 0, {{header}});
    project.dep_graph.build_reverse_map();
    auto epoch = project.context_epoch;

    CommandResolver commands(project);
    ContextsBlob blob;
    EditorContext resolver(project, commands, blob);
    PCMHarness ph(project, resolver);
    Invalidator invalidator(project, store, resolver, ph.projections, ph.pcm, ph.index);
    auto dirty = invalidator.apply(FileEvent::disk_removed(removed_tu));

    // The removed file stops being an includer (and thus a host-source
    // candidate); surviving includers are untouched, shards are kept.
    ASSERT_EQ(project.dep_graph.get_includers(header), llvm::ArrayRef<Fid>{other_tu});
    ASSERT_TRUE(project.dep_graph.get_all_includes(removed_tu).empty());
    ASSERT_TRUE(dirty.recheck_contexts);
    ASSERT_TRUE(dirty.reindex_content_changed.empty());
    ASSERT_TRUE(dirty.reindex_deps_only.empty());
    ASSERT_TRUE(dirty.mark_ast_dirty.empty());
    ASSERT_EQ(project.context_epoch, epoch + 1);
    // The removal clears any pending-reindex state recorded earlier (e.g. a
    // DiskChanged observed just before deletion): the shard keeps serving
    // and nothing is left to reindex.
    ASSERT_EQ(dirty.clear_reindex, llvm::SmallVector<Fid>{removed_tu});
}

TEST_CASE(RemoveRecreateBatchOrder) {
    FileTable files;
    Project project{files};
    SessionStore store;
    auto file = project.file_table.intern("/proj/a.cpp");
    project.dep_graph.set_includes(file, 0, {});
    project.dep_graph.build_reverse_map();
    CommandResolver commands(project);
    ContextsBlob blob;
    EditorContext resolver(project, commands, blob);
    PCMHarness ph(project, resolver);
    Invalidator invalidator(project, store, resolver, ph.projections, ph.pcm, ph.index);

    // Change then delete: the removal is the later fact, the clear wins.
    {
        FileEvent events[] = {FileEvent::disk_changed(file), FileEvent::disk_removed(file)};
        auto dirty = invalidator.apply(events);
        ASSERT_TRUE(llvm::find(dirty.reindex_content_changed, file) ==
                    dirty.reindex_content_changed.end());
        ASSERT_EQ(dirty.clear_reindex, llvm::SmallVector<Fid>{file});
    }

    // Delete then recreate (an editor's atomic save): the later change must
    // survive — the recreated file needs its reindex.
    {
        project.dep_graph.set_includes(file, 0, {});
        project.dep_graph.build_reverse_map();
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

    FileTable files;

    Project project{files};
    SessionStore store;
    auto json = build_cdb_json({
        {tmp.root, tmp.path("a.cpp"), {}}
    });
    write_cdb(tmp, project.cdb, json);
    auto file = project.file_table.intern(tmp.path("a.cpp"));

    CommandResolver commands(project);
    ContextsBlob blob;
    EditorContext resolver(project, commands, blob);
    PCMHarness ph(project, resolver);
    Invalidator invalidator(project, store, resolver, ph.projections, ph.pcm, ph.index);
    FileEvent::CDBDelta delta;
    delta.changed = {file};
    FileEvent events[] = {FileEvent::cdb_changed(std::move(delta)), FileEvent::disk_removed(file)};
    auto dirty = invalidator.apply(events);

    // The removal is the later fact: the file keeps its last-known index
    // serving, so the entry change's drop and enqueue must not survive — a
    // surviving drop would mask the shard and let the next save retire it.
    ASSERT_TRUE(dirty.drop_index.empty());
    ASSERT_TRUE(dirty.reindex_content_changed.empty());
    ASSERT_EQ(dirty.clear_reindex, llvm::SmallVector<Fid>{file});
}

TEST_CASE(CDBAddedScansAndEnqueues) {
    TempDir tmp;
    tmp.touch("inc/header.h", R"(int x = 1;)");
    tmp.touch("src/main.cpp", R"(#include "header.h")");

    FileTable files;

    Project project{files};
    SessionStore store;
    auto json = build_cdb_json({
        {tmp.root, tmp.path("src/main.cpp"), {"-I", tmp.path("inc")}}
    });
    write_cdb(tmp, project.cdb, json);
    auto main_id = project.file_table.intern(tmp.path("src/main.cpp"));
    auto header_id = project.file_table.intern(tmp.path("inc/header.h"));

    CommandResolver commands(project);
    ContextsBlob blob;
    EditorContext resolver(project, commands, blob);
    PCMHarness ph(project, resolver);
    Invalidator invalidator(project, store, resolver, ph.projections, ph.pcm, ph.index);
    FileEvent::CDBDelta delta;
    delta.added = {main_id};
    auto dirty = invalidator.apply(FileEvent::cdb_changed(std::move(delta)));

    // The rescan resolved the new entry's includes; the new file reindexes.
    // A command change rewrites rows as thoroughly as an edit.
    ASSERT_EQ(project.dep_graph.get_includers(header_id), llvm::ArrayRef<Fid>{main_id});
    ASSERT_EQ(dirty.reindex_content_changed, llvm::SmallVector<Fid>{main_id});
    ASSERT_EQ(dirty.drop_index, llvm::SmallVector<Fid>{main_id});
    ASSERT_TRUE(dirty.reindex_deps_only.empty());
    ASSERT_TRUE(dirty.recheck_contexts);
}

TEST_CASE(CDBChangedSplitsOpenClosed) {
    TempDir tmp;
    tmp.touch("a.cpp", R"(int a;)");
    tmp.touch("b.cpp", R"(int b;)");

    FileTable files;

    Project project{files};
    SessionStore store;
    auto json = build_cdb_json({
        {tmp.root, tmp.path("a.cpp"), {}},
        {tmp.root, tmp.path("b.cpp"), {}}
    });
    write_cdb(tmp, project.cdb, json);
    auto open_id = project.file_table.intern(tmp.path("a.cpp"));
    auto closed_id = project.file_table.intern(tmp.path("b.cpp"));
    store.open(open_id);
    project.project_index.shards[open_id];
    project.project_index.shards[closed_id];

    CommandResolver commands(project);
    ContextsBlob blob;
    EditorContext resolver(project, commands, blob);
    PCMHarness ph(project, resolver);
    Invalidator invalidator(project, store, resolver, ph.projections, ph.pcm, ph.index);
    FileEvent::CDBDelta delta;
    delta.changed = {open_id, closed_id};
    auto dirty = invalidator.apply(FileEvent::cdb_changed(std::move(delta)));

    // Flag changes recompile open files and reindex closed ones; the
    // pull-side cache keys (canonical flags) miss on their own.
    ASSERT_EQ(dirty.mark_ast_dirty, llvm::SmallVector<Fid>{open_id});
    llvm::SmallVector<Fid> reindexed{open_id, closed_id};
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
    ASSERT_EQ(project.project_index.shards.count(closed_id), 1u);
    ASSERT_EQ(project.project_index.shards.count(open_id), 1u);
}

TEST_CASE(CDBAddedOpenMarksDirty) {
    FileTable files;
    Project project{files};
    SessionStore store;
    auto file = project.file_table.intern("/proj/a.cpp");
    store.open(file);

    CommandResolver commands(project);
    ContextsBlob blob;
    EditorContext resolver(project, commands, blob);
    PCMHarness ph(project, resolver);
    Invalidator invalidator(project, store, resolver, ph.projections, ph.pcm, ph.index);
    FileEvent::CDBDelta delta;
    delta.added = {file};
    auto dirty = invalidator.apply(FileEvent::cdb_changed(std::move(delta)));

    // The open file gained its first real entry: drop the guessed command
    // it was compiled with, and queue the reindex that builds its shard
    // under the real command once open-file indexing is on.
    ASSERT_EQ(dirty.mark_ast_dirty, llvm::SmallVector<Fid>{file});
    ASSERT_EQ(dirty.reindex_content_changed, llvm::SmallVector<Fid>{file});
    ASSERT_EQ(dirty.drop_index, llvm::SmallVector<Fid>{file});
    ASSERT_TRUE(dirty.reindex_deps_only.empty());
}

TEST_CASE(CDBChangedDropsHostedContext) {
    FileTable files;
    Project project{files};
    SessionStore store;
    auto host = project.file_table.intern("/proj/host.cpp");
    auto open_header = project.file_table.intern("/proj/open.h");
    auto closed_header = project.file_table.intern("/proj/closed.h");
    auto other_header = project.file_table.intern("/proj/other.h");
    store.open(open_header);
    project.project_index.shards[closed_header];

    CommandResolver commands(project);
    ContextsBlob blob;
    EditorContext resolver(project, commands, blob);
    resolver.header_contexts[open_header].host_path_id = host;
    resolver.header_contexts[closed_header].host_path_id = host;
    resolver.header_contexts[other_header].host_path_id = Fid{};
    PCMHarness ph(project, resolver);
    Invalidator invalidator(project, store, resolver, ph.projections, ph.pcm, ph.index);
    FileEvent::CDBDelta delta;
    delta.changed = {host};
    auto dirty = invalidator.apply(FileEvent::cdb_changed(std::move(delta)));

    // Headers borrowing the changed entry re-resolve their context; the
    // open one recompiles, the closed one reindexes. Any standalone index
    // of theirs borrowed the changed command too, so it is dropped along
    // with the host's. Unrelated contexts are untouched.
    llvm::SmallVector<Fid> dropped{open_header, closed_header};
    llvm::sort(dropped);
    ASSERT_EQ(dirty.drop_context, dropped);
    llvm::SmallVector<Fid> evicted{host, open_header, closed_header};
    llvm::sort(evicted);
    auto drop = dirty.drop_index;
    llvm::sort(drop);
    ASSERT_EQ(drop, evicted);
    ASSERT_TRUE(llvm::is_contained(dirty.mark_ast_dirty, open_header));
    ASSERT_TRUE(llvm::is_contained(dirty.reindex_content_changed, closed_header));
    ASSERT_EQ(project.project_index.shards.count(closed_header), 1u);
}

TEST_CASE(CDBDropsBorrowedIndex) {
    // A header indexed standalone in an earlier session borrowed its
    // host's command without ever being opened in this one: the host's
    // command change drops and rebuilds its rows all the same.
    FileTable files;
    Project project{files};
    SessionStore store;
    auto host = project.file_table.intern("/proj/host.cpp");
    auto header = project.file_table.intern("/proj/header.h");

    CommandResolver commands(project);
    ContextsBlob blob;
    EditorContext resolver(project, commands, blob);
    PCMHarness ph(project, resolver);
    ph.index.record_header_host(header, host);
    Invalidator invalidator(project, store, resolver, ph.projections, ph.pcm, ph.index);
    FileEvent::CDBDelta delta;
    delta.changed = {host};
    auto dirty = invalidator.apply(FileEvent::cdb_changed(std::move(delta)));

    ASSERT_TRUE(dirty.drop_context.empty());
    ASSERT_TRUE(llvm::is_contained(dirty.drop_index, header));
    ASSERT_TRUE(llvm::is_contained(dirty.reindex_content_changed, header));
    ASSERT_FALSE(llvm::is_contained(dirty.mark_ast_dirty, header));
}

TEST_CASE(CDBBorrowersByServing) {
    // Open borrowers the index store names: an index-only session loses
    // its serving rows with the drop and reindexes now; a compiling one
    // reindexes when it closes. Neither recompiles — no context borrows.
    FileTable files;
    Project project{files};
    SessionStore store;
    auto host = project.file_table.intern("/proj/host.cpp");
    auto served = project.file_table.intern("/proj/served.h");
    auto compiled = project.file_table.intern("/proj/compiled.h");
    store.open(served)->serving = ServingMode::IndexOnly;
    store.open(compiled);

    CommandResolver commands(project);
    ContextsBlob blob;
    EditorContext resolver(project, commands, blob);
    PCMHarness ph(project, resolver);
    ph.index.record_header_host(served, host);
    ph.index.record_header_host(compiled, host);
    Invalidator invalidator(project, store, resolver, ph.projections, ph.pcm, ph.index);
    FileEvent::CDBDelta delta;
    delta.changed = {host};
    auto dirty = invalidator.apply(FileEvent::cdb_changed(std::move(delta)));

    ASSERT_TRUE(llvm::is_contained(dirty.drop_index, served));
    ASSERT_TRUE(llvm::is_contained(dirty.drop_index, compiled));
    ASSERT_TRUE(llvm::is_contained(dirty.reindex_content_changed, served));
    ASSERT_FALSE(llvm::is_contained(dirty.reindex_content_changed, compiled));
    ASSERT_FALSE(llvm::is_contained(dirty.mark_ast_dirty, served));
    ASSERT_FALSE(llvm::is_contained(dirty.mark_ast_dirty, compiled));
}

TEST_CASE(CDBChangedCascadesModule) {
    FileTable files;
    Project project{files};
    SessionStore store;
    auto mod = project.file_table.intern("/proj/m.cppm");
    auto open_user = project.file_table.intern("/proj/open_user.cppm");
    auto closed_user = project.file_table.intern("/proj/closed_user.cppm");

    CommandResolver commands(project);
    ContextsBlob blob;
    EditorContext resolver(project, commands, blob);
    PCMHarness ph(project, resolver);
    // The consumer edges build_deps declares in production — no rounds.
    auto node = [](Fid pid) {
        return NodeId{Family::PCM, pid.raw};
    };
    ph.graph.declare(node(open_user), {node(mod)});
    ph.graph.declare(node(closed_user), {node(mod)});

    store.open(open_user);
    Invalidator invalidator(project, store, resolver, ph.projections, ph.pcm, ph.index);

    FileEvent::CDBDelta delta;
    delta.changed = {mod};
    auto dirty = invalidator.apply(FileEvent::cdb_changed(std::move(delta)));

    // A module unit's flag change cascades through the compile graph
    // exactly like a content change: importers' PCMs went stale. The
    // unit itself lands in both lists (its own entry changed AND the
    // cascade dirtied its PCM); the indexer's absorbing upgrade
    // resolves the overlap to ContentChanged.
    EXPECT_EQ(dirty.mark_ast_dirty, llvm::SmallVector<Fid>{open_user});
    EXPECT_EQ(dirty.reindex_content_changed, llvm::SmallVector<Fid>{mod});
    EXPECT_EQ(dirty.drop_index, llvm::SmallVector<Fid>{mod});
    llvm::SmallVector<Fid> deps{mod, closed_user};
    llvm::sort(deps);
    EXPECT_EQ(dirty.reindex_deps_only, deps);
}

TEST_CASE(DiskRemovedReindexesIncluders) {
    FileTable files;
    Project project{files};
    SessionStore store;
    auto header = project.file_table.intern("/proj/h.h");
    auto open_tu = project.file_table.intern("/proj/a.cpp");
    auto closed_tu = project.file_table.intern("/proj/b.cpp");
    project.dep_graph.set_includes(open_tu, 0, {{header}});
    project.dep_graph.set_includes(closed_tu, 0, {{header}});
    project.dep_graph.build_reverse_map();
    store.open(open_tu);

    CommandResolver commands(project);
    ContextsBlob blob;
    EditorContext resolver(project, commands, blob);
    PCMHarness ph(project, resolver);
    Invalidator invalidator(project, store, resolver, ph.projections, ph.pcm, ph.index);
    auto dirty = invalidator.apply(FileEvent::disk_removed(header));

    // Dependents now compile against a missing include: open ones
    // recompile, closed ones reindex.
    ASSERT_EQ(dirty.mark_ast_dirty, llvm::SmallVector<Fid>{open_tu});
    ASSERT_EQ(dirty.reindex_deps_only, llvm::SmallVector<Fid>{closed_tu});
    ASSERT_TRUE(dirty.reindex_content_changed.empty());
    ASSERT_TRUE(dirty.recheck_contexts);
}

TEST_CASE(CDBRemovedDropsSourceRole) {
    TempDir tmp;
    tmp.touch("inc/h.h", R"(int x;)");
    tmp.touch("kept.cpp", R"(#include "inc/h.h")");

    FileTable files;

    Project project{files};
    SessionStore store;
    // The pre-reload graph still shows gone.cpp as an includer; the CDB has
    // already been reloaded without it.
    auto gone_id = project.file_table.intern(tmp.path("gone.cpp"));
    auto header_id = project.file_table.intern(tmp.path("inc/h.h"));
    project.dep_graph.set_includes(gone_id, 0, {{header_id}});
    project.dep_graph.build_reverse_map();
    auto json = build_cdb_json({
        {tmp.root, tmp.path("kept.cpp"), {}}
    });
    write_cdb(tmp, project.cdb, json);
    auto kept_id = project.file_table.intern(tmp.path("kept.cpp"));

    CommandResolver commands(project);
    ContextsBlob blob;
    EditorContext resolver(project, commands, blob);
    PCMHarness ph(project, resolver);
    Invalidator invalidator(project, store, resolver, ph.projections, ph.pcm, ph.index);
    FileEvent::CDBDelta delta;
    delta.removed = {gone_id};
    auto dirty = invalidator.apply(FileEvent::cdb_changed(std::move(delta)));

    // The rebuild resolves includes from the surviving entries only. The
    // removed entry's rows leave the index: its database still loads and
    // simply stopped compiling the file.
    ASSERT_TRUE(project.dep_graph.get_all_includes(gone_id).empty());
    ASSERT_EQ(project.dep_graph.get_includers(header_id), llvm::ArrayRef<Fid>{kept_id});
    ASSERT_EQ(dirty.drop_index, llvm::SmallVector<Fid>{gone_id});
    ASSERT_TRUE(dirty.reindex_content_changed.empty());
    ASSERT_TRUE(dirty.recheck_contexts);
}

TEST_CASE(CDBRemovedStillClaimed) {
    /// The entry left the database but a rule's default command still
    /// claims the file: a command change, not a retirement — the rows are
    /// rebuilt under the default command instead of leaving.
    TempDir tmp;
    tmp.touch("gone.cpp", R"(int x;)");
    tmp.touch("kept.cpp", R"(int y;)");

    FileTable files;

    Project project{files};
    SessionStore store;
    project.config.rules.push_back(ConfigRule{.default_command = std::string("clang++")});
    project.config.finalize(tmp.root.str());
    project.build.reset_active("");
    auto gone_id = project.file_table.intern(tmp.path("gone.cpp"));
    auto json = build_cdb_json({
        {tmp.root, tmp.path("kept.cpp"), {}}
    });
    write_cdb(tmp, project.cdb, json);

    CommandResolver commands(project);
    ContextsBlob blob;
    EditorContext resolver(project, commands, blob);
    PCMHarness ph(project, resolver);
    Invalidator invalidator(project, store, resolver, ph.projections, ph.pcm, ph.index);
    FileEvent::CDBDelta delta;
    delta.removed = {gone_id};
    auto dirty = invalidator.apply(FileEvent::cdb_changed(std::move(delta)));

    ASSERT_EQ(dirty.drop_index, llvm::SmallVector<Fid>{gone_id});
    ASSERT_EQ(dirty.reindex_content_changed, llvm::SmallVector<Fid>{gone_id});
    ASSERT_TRUE(dirty.clear_reindex.empty());
}

TEST_CASE(CDBEmptyDeltaNoEffects) {
    FileTable files;
    Project project{files};
    SessionStore store;

    CommandResolver commands(project);
    ContextsBlob blob;
    EditorContext resolver(project, commands, blob);
    PCMHarness ph(project, resolver);
    Invalidator invalidator(project, store, resolver, ph.projections, ph.pcm, ph.index);
    auto dirty = invalidator.apply(FileEvent::cdb_changed({}));

    ASSERT_TRUE(dirty.empty());
}

TEST_CASE(BatchDiskEventsDeduplicate) {
    FileTable files;
    Project project{files};
    SessionStore store;
    auto first = project.file_table.intern("/proj/a.h");
    auto second = project.file_table.intern("/proj/b.h");

    CommandResolver commands(project);
    ContextsBlob blob;
    EditorContext resolver(project, commands, blob);
    PCMHarness ph(project, resolver);
    Invalidator invalidator(project, store, resolver, ph.projections, ph.pcm, ph.index);
    FileEvent events[] = {FileEvent::disk_changed(first),
                          FileEvent::disk_changed(first),
                          FileEvent::disk_changed(second)};
    auto dirty = invalidator.apply(events);

    llvm::SmallVector<Fid> expected{first, second};
    llvm::sort(expected);
    ASSERT_EQ(dirty.reindex_content_changed, expected);
    ASSERT_TRUE(dirty.reindex_deps_only.empty());
}

};  // TEST_SUITE(Invalidator)

TEST_SUITE(DropOrphanedChoices) {

TEST_CASE(SurvivingEdgeKeepsChoice) {
    TempDir tmp;
    tmp.touch("host.cpp", R"(#include "h.h")");
    tmp.touch("h.h");
    FileTable files;
    Project project{files};
    SessionStore store;
    write_cdb(tmp,
              project.cdb,
              build_cdb_json({
                  {tmp.root, tmp.path("host.cpp"), {}}
    }));
    CommandResolver commands(project);
    ContextsBlob blob;
    EditorContext resolver(project, commands, blob);
    auto host = project.file_table.intern(tmp.path("host.cpp"));
    auto header = project.file_table.intern(tmp.path("h.h"));
    project.dep_graph.set_includes(host, 0, {{header}});
    project.dep_graph.build_reverse_map();

    auto session = store.open(header);
    resolver.selections[header] = Selection{host, std::nullopt, ""};

    ASTHarness harness(project, resolver, store);
    ASSERT_FALSE(ContextService{project, resolver, harness.ast}.drop_orphaned_choices(store));
    ASSERT_TRUE(resolver.selections.contains(header));
}

TEST_CASE(RemovedEdgeDropsChoice) {
    FileTable files;
    Project project{files};
    SessionStore store;
    CommandResolver commands(project);
    ContextsBlob blob;
    EditorContext resolver(project, commands, blob);
    auto host = project.file_table.intern("/proj/host.cpp");
    auto header = project.file_table.intern("/proj/h.h");
    project.dep_graph.build_reverse_map();

    auto session = store.open(header);
    session->trial_done = true;
    resolver.header_contexts[header] = HeaderContext{};
    resolver.selections[header] = Selection{host, std::nullopt, ""};
    auto generation = session->generation;

    ASTHarness harness(project, resolver, store);
    harness.ast.projections.entries[header].current = true;
    ASSERT_TRUE(ContextService{project, resolver, harness.ast}.drop_orphaned_choices(store));
    ASSERT_FALSE(resolver.header_contexts.contains(header));
    ASSERT_FALSE(harness.ast.projections.current(header));
    ASSERT_FALSE(session->trial_done);
    ASSERT_EQ(session->generation, generation + 1);
    ASSERT_FALSE(resolver.selections.contains(header));
}

TEST_CASE(VanishedOccurrenceDropsChoice) {
    TempDir tmp;
    FileTable files;
    Project project{files};
    SessionStore store;
    CommandResolver commands(project);
    ContextsBlob blob;
    EditorContext resolver(project, commands, blob);
    // The host still includes the header, but only once — the pinned
    // occurrence #1 no longer exists.
    tmp.touch("host.cpp", R"(#include "h.h")");
    tmp.touch("h.h");
    auto host = project.file_table.intern(tmp.path("host.cpp"));
    auto header = project.file_table.intern(tmp.path("h.h"));
    project.dep_graph.set_includes(host, 0, {{header}});
    project.dep_graph.build_reverse_map();

    store.open(header);
    resolver.selections[header] = Selection{host, 1, ""};

    ASTHarness harness(project, resolver, store);
    ASSERT_TRUE(ContextService{project, resolver, harness.ast}.drop_orphaned_choices(store));
    ASSERT_FALSE(resolver.selections.contains(header));
}

};  // TEST_SUITE(DropOrphanedChoices)

}  // namespace
}  // namespace clice::testing
