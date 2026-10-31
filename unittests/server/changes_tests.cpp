module clice;

import :feature.diagnostics;
import :index.shard;
import :index.symbol;
import :index.tu_manifest;
import :project.hosting;
import :project.project;
import :sched.builders.pcm;
import :sched.graph;
import :sched.index.pending_table;
import :server.served_project;
import :server.session;
import :settings.settings;
import :support.bytes;
import :support.failure_record;
import :unittests.server.served_project_fixture;
import :unittests.utils.cdb_helper;
import :unittests.utils.index_internals;
import :unittests.utils.temp_dir;
import :unittests.utils;
import :vfs.file_table;
import :vfs.ids;
import :vfs.path;

namespace clice::testing {
namespace {

/// A loaded shard whose entries were built from `content`, for the
/// disk-vs-shard freshness comparisons below.
index::Shard shard_of(llvm::StringRef content) {
    auto bytes = index::write_shard(
        {},
        [](index::SymbolHash) -> std::optional<index::SymbolView> { return std::nullopt; },
        content);
    return index::Shard::from_buffer(llvm::MemoryBuffer::getMemBufferCopy(as_chars(bytes)));
}

/// The served project the changes are handed to. Its open documents'
/// compiles parse the bodies of their headers and serve their index, so
/// that a dependency change splits open files from closed ones
/// (SkippedBodiesReindexOpen covers the default).
struct Harness {
    ServedProjectFixture fixture;
    ServedProject& served = *fixture.served;
    Project& project = served.project;

    Harness() {
        project.settings.project.skip_header_bodies = false;
    }

    /// Whether a change invalidated the open document's compile: every
    /// invalidation moves its epoch on.
    bool invalidated(Fid path_id) {
        return fixture.entry(path_id).epoch != 0;
    }

    /// The files the background index owes a reindex for `reason`, sorted.
    llvm::SmallVector<Fid> queued(ReindexReason reason) {
        auto& pump = served.sched.pump;
        auto debt = pump.save_debt();
        auto files = ranges::to<llvm::SmallVector<Fid>>(
            views::filter(debt, [&](Fid file) { return pump.pending_reason(file) == reason; }));
        ranges::sort(files);
        return files;
    }

    /// Give the unit an index of its own, which a change of its command
    /// drops.
    void index_unit(Fid tu) {
        index::TUManifest manifest;
        manifest.built_at = 1;
        manifest.tu_version = project.file_table.intern_version(tu, 1);
        project.index.apply_manifest(tu, std::move(manifest));
    }
};

ZEST_SUITE(Changes, Harness) {

ZEST_CASE(EmptyBatchNoEffects) {
    served.disk_changed({});

    ZASSERT(served.sched.pump.save_debt().empty());
}

ZEST_CASE(NewProviderDirtiesImporters) {
    // Consumers that scanned the name unresolved hold durable edges to
    // its sentinel node; the first provider cascades through them. A
    // closed TU reindexes as ContentChanged (its dep snapshot never
    // named the interface, so the hash gate cannot see the change); an
    // open document recompiles. Nothing is ever dropped — a consumer
    // that can no longer build keeps serving its last-known index.
    TempDir tmp;
    tmp.touch("m.cppm", "int mv();\n");
    write_cdb(tmp,
              project.cdb,
              build_cdb_json({
                  {tmp.root, tmp.path("m.cppm"), {}}
    }));
    scan_all(project.cdb, project.toolchain, project.dep_graph);
    project.dep_graph.build_reverse_map();
    tmp.touch("m.cppm", "export module m;\nexport int mv();\n");
    auto iface = fixture.intern(tmp.path("m.cppm"));
    auto closed = fixture.intern("/proj/closed.cpp");
    auto open = fixture.open("/proj/open.cpp", "")->path_id;
    index_unit(closed);
    served.sched.graph.declare({Family::TURun, closed.raw}, {PCMBuilder::unresolved_node("m")});
    served.sched.graph.declare({Family::AST, open.raw}, {PCMBuilder::unresolved_node("m")});

    served.disk_changed({iface});

    ZEXPECT(served.sched.pump.pending_reason(closed) == ReindexReason::ContentChanged);
    ZEXPECT(invalidated(open));
    ZEXPECT(project.index.indexed(closed));

    // The same save again: the name already has its provider.
    served.sched.pump.clear_pending(closed);
    auto epoch = fixture.entry(open).epoch;
    served.disk_changed({iface});
    ZEXPECT(!served.sched.pump.pending_reason(closed).has_value());
    ZEXPECT(fixture.entry(open).epoch == epoch);
}

ZEST_CASE(ReloadProviderCascades) {
    // The database-reload variant of provider appearance: the provider-set
    // diff drives the same sentinel cascade, and a consumer retired by the
    // very same reload leaves the index without a reindex being owed.
    TempDir tmp;
    tmp.touch("m.cppm", "export module m;\nexport int mv();\n");
    // The database already reloaded: only the provider remains.
    write_cdb(tmp,
              project.cdb,
              build_cdb_json({
                  {tmp.root, tmp.path("m.cppm"), {}}
    }));
    auto iface = fixture.intern(tmp.path("m.cppm"));
    auto retired = fixture.intern(tmp.path("old.cpp"));
    index_unit(retired);
    served.sched.graph.declare({Family::TURun, retired.raw}, {PCMBuilder::unresolved_node("m")});

    fixture.build_changed({.added = {iface}, .removed = {retired}});

    ZEXPECT(!served.sched.pump.pending_reason(retired).has_value());
    ZEXPECT(!project.index.indexed(retired));
}

ZEST_CASE(DiskRemovedDropsProvider) {
    // Deleting a provider must leave the module map too: a later
    // replacement provider would otherwise sit behind the deleted one in
    // the candidate list and never be selected.
    auto iface = fixture.intern("/proj/m.cppm");
    project.dep_graph.add_module("m", iface);
    project.file_table.disk.saw_missing(iface);

    served.disk_changed({iface});

    ZEXPECT(project.dep_graph.lookup_module("m").empty());
}

ZEST_CASE(DiskChangeSparesSession) {
    TempDir tmp;
    tmp.touch("a.h", "int x;");
    auto saved = fixture.open(tmp.path("a.h"), "int x;", 1)->path_id;
    project.file_table.disk.read(saved);

    served.disk_changed({saved});

    // The open file's own compile reads its buffer, never its disk: it is
    // not stale, while its disk index is.
    ZASSERT(!invalidated(saved));
    ZASSERT(queued(ReindexReason::ContentChanged) == llvm::SmallVector<Fid>{saved});
}

ZEST_CASE(CascadeSplitsOpenClosed) {
    auto mod = fixture.intern("/proj/m.cppm");
    auto open_user = fixture.open("/proj/open_user.cppm", "")->path_id;
    auto closed_user = fixture.intern("/proj/closed_user.cppm");
    // The consumer edges PCMBuilder::declare_deps declares in production —
    // no rounds.
    auto node = [](Fid pid) {
        return NodeId{Family::PCM, pid.raw};
    };
    served.sched.graph.declare(node(open_user), {node(mod)});
    served.sched.graph.declare(node(closed_user), {node(mod)});

    served.disk_changed({mod});

    // Cascade-dirtied module units split by document state: open buffers
    // recompile, closed files go back to the background indexer. The
    // changed unit, dirtied too, is owed the reindex of its own content.
    ZEXPECT(invalidated(open_user));
    ZEXPECT(queued(ReindexReason::DepsOnly) == llvm::SmallVector<Fid>{closed_user});
    ZEXPECT(queued(ReindexReason::ContentChanged) == llvm::SmallVector<Fid>{mod});
}

ZEST_CASE(ChainHitAndMiss) {
    auto saved = fixture.intern("/proj/inner.h");
    auto other = fixture.intern("/proj/other.h");
    auto hit = fixture.open("/proj/hit.h", "")->path_id;
    auto miss = fixture.open("/proj/miss.h", "")->path_id;
    // The chain files a header's context embeds are inputs of its compile.
    fixture.entry(hit).deps = DepsSnapshot{DepState{.path_id = saved}};
    fixture.entry(miss).deps = DepsSnapshot{DepState{.path_id = other}};

    served.disk_changed({saved});

    // The header whose context embeds the saved file recompiles; no header
    // has an index of its own to rebuild.
    ZASSERT(invalidated(hit));
    ZASSERT(!invalidated(miss));
    ZASSERT(queued(ReindexReason::DepsOnly).empty());
    ZASSERT(queued(ReindexReason::ContentChanged) == llvm::SmallVector<Fid>{saved});
}

ZEST_CASE(SaveMarksDependents) {
    auto header = fixture.intern("/proj/h.h");
    auto open_tu = fixture.open("/proj/a.cpp", "")->path_id;
    auto closed_tu = fixture.intern("/proj/b.cpp");
    project.dep_graph.set_includes(open_tu, 0, {{header}});
    project.dep_graph.set_includes(closed_tu, 0, {{header}});
    project.dep_graph.build_reverse_map();

    served.disk_changed({header});

    // Open dependents recompile, closed ones reindex; the old/new dependent
    // snapshots overlap fully here, so this also proves the dedup. A
    // dependent's own content did not change: deps-only.
    ZASSERT(invalidated(open_tu));
    ZASSERT(queued(ReindexReason::DepsOnly) == llvm::SmallVector<Fid>{closed_tu});
    ZASSERT(queued(ReindexReason::ContentChanged) == llvm::SmallVector<Fid>{header});
}

ZEST_CASE(SkippedBodiesReindexOpen) {
    auto header = fixture.intern("/proj/h.h");
    auto open_tu = fixture.open("/proj/a.cpp", "")->path_id;
    auto closed_tu = fixture.intern("/proj/b.cpp");
    project.dep_graph.set_includes(open_tu, 0, {{header}});
    project.dep_graph.set_includes(closed_tu, 0, {{header}});
    project.dep_graph.build_reverse_map();
    project.settings.project.skip_header_bodies = true;

    served.disk_changed({header});

    // An open file's compile skipping the bodies of its headers serves no
    // index of what they hold: the open dependent reindexes too.
    ZASSERT(invalidated(open_tu));
    llvm::SmallVector<Fid> reindexed{open_tu, closed_tu};
    ranges::sort(reindexed);
    ZASSERT(queued(ReindexReason::DepsOnly) == reindexed);
    ZASSERT(!served.compiled_by_session(open_tu));
}

ZEST_CASE(TransitiveDependentsEnqueue) {
    auto header = fixture.intern("/proj/h.h");
    auto middle = fixture.intern("/proj/g.h");
    auto root = fixture.intern("/proj/c.cpp");
    project.dep_graph.set_includes(middle, 0, {{header}});
    project.dep_graph.set_includes(root, 0, {{middle}});
    project.dep_graph.build_reverse_map();

    served.disk_changed({header});

    // Only root TUs own index shards; the intermediate header is not one.
    ZASSERT(queued(ReindexReason::DepsOnly) == llvm::SmallVector<Fid>{root});
    ZASSERT(queued(ReindexReason::ContentChanged) == llvm::SmallVector<Fid>{header});
}

ZEST_CASE(ForcedHeaderReachesUnits) {
    // A header reached only through a command's forced include: its
    // change reaches the units forcing that header in, open or closed.
    auto header = fixture.intern("/proj/h.h");
    auto forced = fixture.intern("/proj/force.h");
    auto open = fixture.open("/proj/a.cpp", "")->path_id;
    auto closed = fixture.intern("/proj/b.cpp");
    project.dep_graph.set_includes(forced, 0, {{header}});
    project.dep_graph.add_forced_include(open, forced);
    project.dep_graph.add_forced_include(closed, forced);
    project.dep_graph.build_reverse_map();

    served.disk_changed({header});

    ZASSERT(invalidated(open));
    ZASSERT(queued(ReindexReason::DepsOnly) == llvm::SmallVector<Fid>{closed});
}

ZEST_CASE(BatchSeesEarlierEdges) {
    // An includer an earlier file of the batch adds is visible to a later
    // cascade: the reverse map follows every rescan.
    TempDir tmp;
    tmp.touch("h.h", "int h;");
    tmp.touch("a.cpp", R"(#include "h.h")");
    tmp.touch("b.cpp", "int b;");
    write_cdb(tmp,
              project.cdb,
              build_cdb_json({
                  {tmp.root, tmp.path("a.cpp"), {}},
                  {tmp.root, tmp.path("b.cpp"), {}},
    }));
    scan_all(project.cdb, project.toolchain, project.dep_graph);
    project.dep_graph.build_reverse_map();
    auto header = fixture.intern(tmp.path("h.h"));
    auto known = fixture.intern(tmp.path("a.cpp"));
    // Open, so that its own change spares its compile: only the header's
    // cascade recompiles it.
    auto added = fixture.open(tmp.path("b.cpp"), "")->path_id;
    tmp.touch("b.cpp", R"(#include "h.h")");

    served.disk_changed({added, header});

    ZASSERT(ranges::contains(queued(ReindexReason::DepsOnly), known));
    ZASSERT(invalidated(added));
    ZASSERT(ranges::contains(project.dep_graph.get_includers(header), added));
}

ZEST_CASE(RemovalThenChangeKeepsClear) {
    // A unit removed earlier in the batch is no longer an includer when a
    // header it included changes: its clear survives, nothing requeues it.
    TempDir tmp;
    tmp.touch("h.h", "int changed;");
    auto header = fixture.intern(tmp.path("h.h"));
    auto removed = fixture.intern(tmp.path("gone.cpp"));
    auto kept = fixture.intern(tmp.path("kept.cpp"));
    project.dep_graph.set_includes(removed, 0, {{header}});
    project.dep_graph.set_includes(kept, 0, {{header}});
    project.dep_graph.build_reverse_map();
    // A change observed just before the deletion.
    served.sched.pump.enqueue(removed, ReindexReason::ContentChanged);
    project.file_table.disk.saw_missing(removed);

    served.disk_changed({removed, header});

    ZASSERT(!served.sched.pump.pending_reason(removed).has_value());
    ZASSERT(served.sched.pump.pending_reason(kept) == ReindexReason::DepsOnly);
}

ZEST_CASE(RescanKeepsGuardedProvider) {
    // A disk-change rescan meeting a module declaration inside a
    // preprocessor conditional must resolve it the way the startup scan
    // does (scan_quick alone leaves the name empty) instead of dropping
    // the provider and leaving its importers unresolved.
    TempDir tmp;
    tmp.touch("m.cpp", "#if 1\nexport module m;\n#endif\n");
    write_cdb(tmp,
              project.cdb,
              build_cdb_json({
                  {tmp.root, tmp.path("m.cpp"), {"-std=c++20"}}
    }));
    scan_all(project.cdb, project.toolchain, project.dep_graph);
    project.dep_graph.build_reverse_map();
    auto iface = fixture.intern(tmp.path("m.cpp"));
    ZASSERT(project.dep_graph.module_of(iface) == "m");
    tmp.touch("m.cpp", "#if 1\nexport module m;\n#endif\nexport int v;\n");

    IndexInternals::plant_shard(project.index, iface, shard_of(*read_file(tmp.path("m.cpp"))));

    served.disk_changed({iface});

    ZEXPECT(project.dep_graph.module_of(iface) == "m");
    ZEXPECT(ranges::contains(project.dep_graph.lookup_module("m"), iface));
}

ZEST_CASE(CrashMarksLostDirty) {
    auto first = fixture.open("/proj/a.cpp", "");
    auto second = fixture.open("/proj/b.cpp", "");
    auto now = Session::Clock::now();
    first->crashed(feature::RequestKind::Compile, "death", "crashed", now);
    second->crashed(feature::RequestKind::Compile, "death", "crashed", now);

    served.asts_lost({first->path_id, second->path_id});

    ZASSERT(invalidated(first->path_id));
    ZASSERT(invalidated(second->path_id));
    // A crash loses build products, not compile inputs: what crashed on
    // them gets no retry.
    auto later = now + std::chrono::minutes(1);
    ZASSERT(first->crash_stops(feature::RequestKind::Compile, later));
    ZASSERT(second->crash_stops(feature::RequestKind::Compile, later));
}

ZEST_CASE(EvictionMarksLost) {
    auto session = fixture.open("/proj/a.cpp", "");
    auto now = Session::Clock::now();
    session->crashed(feature::RequestKind::Compile, "death", "crashed", now);

    served.asts_lost({session->path_id});

    // Same loss as a crash, scoped to one document.
    ZASSERT(invalidated(session->path_id));
    ZASSERT(session->crash_stops(feature::RequestKind::Compile, now + std::chrono::minutes(1)));
}

ZEST_CASE(BatchChangesDeduplicate) {
    // The file exists: opening a document looks at it, and one seen
    // missing is a removal.
    TempDir tmp;
    tmp.touch("a.h");
    auto saved = fixture.open(tmp.path("a.h"), "")->path_id;

    served.disk_changed({saved, saved});

    ZASSERT(queued(ReindexReason::ContentChanged) == llvm::SmallVector<Fid>{saved});
}

ZEST_CASE(DiskChangeClosedCascades) {
    auto header = fixture.intern("/proj/h.h");
    auto open_tu = fixture.open("/proj/a.cpp", "")->path_id;
    auto closed_tu = fixture.intern("/proj/b.cpp");
    project.dep_graph.set_includes(open_tu, 0, {{header}});
    project.dep_graph.set_includes(closed_tu, 0, {{header}});
    project.dep_graph.build_reverse_map();

    served.disk_changed({header});

    // A closed file's disk change cascades exactly like a save, plus the
    // file's own stale shard is refreshed. The changed file's own index is
    // untrustworthy; its dependent only rebuilds semantics.
    ZASSERT(invalidated(open_tu));
    ZASSERT(queued(ReindexReason::ContentChanged) == llvm::SmallVector<Fid>{header});
    ZASSERT(queued(ReindexReason::DepsOnly) == llvm::SmallVector<Fid>{closed_tu});
}

ZEST_CASE(DiskChangeOpenCascades) {
    // Open or not, a disk change reaches every file reading the disk: the
    // open header's includers cascade now, not at its close, and only the
    // header's own document — whose compile reads its buffer — is spared.
    // The header exists: opening a document looks at it, and one seen
    // missing is a removal.
    TempDir tmp;
    tmp.touch("h.h");
    auto header = fixture.open(tmp.path("h.h"), "")->path_id;
    auto open_tu = fixture.open(tmp.path("a.cpp"), "")->path_id;
    auto closed_tu = fixture.intern(tmp.path("b.cpp"));
    project.dep_graph.set_includes(open_tu, 0, {{header}});
    project.dep_graph.set_includes(closed_tu, 0, {{header}});
    project.dep_graph.build_reverse_map();

    served.disk_changed({header});

    ZASSERT(invalidated(open_tu));
    ZASSERT(!invalidated(header));
    ZASSERT(queued(ReindexReason::ContentChanged) == llvm::SmallVector<Fid>{header});
    ZASSERT(queued(ReindexReason::DepsOnly) == llvm::SmallVector<Fid>{closed_tu});
}

ZEST_CASE(CompiledIncluderCascades) {
    // An includer the lexical scan never saw (a macro include) but whose
    // indexed compile read the file is a dependent all the same.
    auto header = fixture.intern("/proj/m.h");
    auto scanned = fixture.intern("/proj/a.cpp");
    auto compiled = fixture.intern("/proj/b.cpp");
    project.dep_graph.set_includes(scanned, 0, {{header}});
    project.dep_graph.set_includes(compiled, 0, {});
    project.dep_graph.build_reverse_map();
    index::TUManifest manifest;
    manifest.built_at = 1;
    manifest.tu_version = project.file_table.intern_version(compiled, 1);
    manifest.contributions = {
        {project.file_table.intern_version(header, 1), 1}
    };
    project.index.apply_manifest(compiled, std::move(manifest));

    served.disk_changed({header});

    llvm::SmallVector<Fid> reindexed{scanned, compiled};
    ranges::sort(reindexed);
    ZASSERT(queued(ReindexReason::DepsOnly) == reindexed);
    // Until they are indexed again, what their runs walked is behind the
    // disk — and the file's own, should it be a unit of its own.
    ZEXPECT(IndexInternals::behind(project.index, compiled));
    ZEXPECT(IndexInternals::behind(project.index, scanned));
    ZEXPECT(IndexInternals::behind(project.index, header));
}

ZEST_CASE(ModuleReadHeaderCascades) {
    // A header only a module unit's PCM read (its global module fragment):
    // no include edge names the importers, the unit's recorded inputs do.
    auto header = fixture.intern("/proj/gmf.h");
    auto mod = fixture.intern("/proj/m.cppm");
    auto user = fixture.intern("/proj/user.cpp");
    served.sched.graph.declare(
        NodeId{
            Family::PCM,
            user.raw
    },
        {NodeId{Family::PCM, mod.raw}});
    project.pcm_builds[mod].deps.push_back({.path_id = header});

    served.disk_changed({header});

    ZASSERT(served.sched.pump.pending_reason(user) == ReindexReason::DepsOnly);
}

ZEST_CASE(FailedModuleHeaderCascades) {
    // The module's build failed on the header: it is no less an input, and
    // its importers hear of its change through its failure record.
    auto header = fixture.intern("/proj/gmf.h");
    auto mod = fixture.intern("/proj/m.cppm");
    auto user = fixture.intern("/proj/user.cpp");
    served.sched.graph.declare(
        NodeId{
            Family::PCM,
            user.raw
    },
        {NodeId{Family::PCM, mod.raw}});
    FailureRecord error;
    error.cause = "use of undeclared identifier 'x'";
    project.failures.pcm_builds.commit(mod, {.record = error}, {{.path_id = header}});

    ZASSERT(ranges::contains(served.readers(header), mod));
    served.disk_changed({header});
    ZASSERT(served.sched.pump.pending_reason(user) == ReindexReason::DepsOnly);

    served.sched.pump.clear_pending(user);
    project.file_table.disk.saw_missing(header);
    served.disk_changed({header});
    ZASSERT(served.sched.pump.pending_reason(user) == ReindexReason::DepsOnly);
}

ZEST_CASE(AppearedHeaderCascades) {
    // A header appearing where compiles looked for it: the closed TU whose
    // indexed compile looked reindexes, the open document whose AST looked
    // recompiles.
    auto header = fixture.intern("/proj/gen.h");
    auto closed = fixture.intern("/proj/b.cpp");
    auto open = fixture.open("/proj/a.cpp", "")->path_id;
    index::TUManifest manifest;
    manifest.built_at = 1;
    manifest.tu_version = project.file_table.intern_version(closed, 1);
    manifest.absent = {project.file_table.intern_version(header, 0)};
    project.index.apply_manifest(closed, std::move(manifest));
    fixture.entry(open).deps = DepsSnapshot{
        DepState{.path_id = header, .missing = true}
    };

    served.disk_changed({header});

    ZASSERT(queued(ReindexReason::DepsOnly) == llvm::SmallVector<Fid>{closed});
    ZASSERT(invalidated(open));
}

ZEST_CASE(DiskRemovedScrubsSourceRole) {
    auto header = fixture.intern("/proj/h.h");
    auto removed_tu = fixture.intern("/proj/gone.cpp");
    auto other_tu = fixture.intern("/proj/kept.cpp");
    project.dep_graph.set_includes(removed_tu, 0, {{header}});
    project.dep_graph.set_includes(other_tu, 0, {{header}});
    project.dep_graph.build_reverse_map();
    // A change observed just before the deletion.
    served.sched.pump.enqueue(removed_tu, ReindexReason::ContentChanged);
    project.file_table.disk.saw_missing(removed_tu);
    auto epoch = project.context_epoch;

    served.disk_changed({removed_tu});

    // The removed file stops being an includer (and thus a host-source
    // candidate); surviving includers are untouched, shards are kept.
    ZASSERT(project.dep_graph.get_includers(header) == llvm::ArrayRef<Fid>{other_tu});
    ZASSERT(project.dep_graph.get_all_includes(removed_tu).empty());
    ZASSERT(project.context_epoch == epoch + 1);
    // The removal clears any pending-reindex state recorded earlier: the
    // shard keeps serving and nothing is left to reindex.
    ZASSERT(served.sched.pump.save_debt().empty());
}

ZEST_CASE(RemoveRecreateOrder) {
    TempDir tmp;
    tmp.touch("a.cpp");
    auto file = fixture.intern(tmp.path("a.cpp"));
    project.dep_graph.set_includes(file, 0, {});
    project.dep_graph.build_reverse_map();

    // Change then delete: the removal is the later fact, the clear wins.
    served.disk_changed({file});
    fs::remove_all(tmp.path("a.cpp"));
    project.file_table.disk.look(file);
    served.disk_changed({file});
    ZASSERT(!served.sched.pump.pending_reason(file).has_value());

    // Delete then recreate (an editor's atomic save): the later change must
    // survive — the recreated file needs its reindex.
    project.dep_graph.set_includes(file, 0, {});
    project.dep_graph.build_reverse_map();
    tmp.touch("a.cpp", "int a;");
    project.file_table.disk.look(file);
    served.disk_changed({file});
    ZASSERT(served.sched.pump.pending_reason(file) == ReindexReason::ContentChanged);
}

ZEST_CASE(CDBAddedScansAndEnqueues) {
    TempDir tmp;
    tmp.touch("inc/header.h", R"(int x = 1;)");
    tmp.touch("src/main.cpp", R"(#include "header.h")");
    auto json = build_cdb_json({
        {tmp.root, tmp.path("src/main.cpp"), {"-I", tmp.path("inc")}}
    });
    write_cdb(tmp, project.cdb, json);
    auto main_id = fixture.intern(tmp.path("src/main.cpp"));
    auto header_id = fixture.intern(tmp.path("inc/header.h"));
    index_unit(main_id);

    fixture.build_changed({.added = {main_id}});

    // The rescan resolved the new entry's includes; the new file reindexes.
    // A command change rewrites the index as thoroughly as an edit.
    ZASSERT(project.dep_graph.get_includers(header_id) == llvm::ArrayRef<Fid>{main_id});
    ZASSERT(queued(ReindexReason::ContentChanged) == llvm::SmallVector<Fid>{main_id});
    ZASSERT(!project.index.indexed(main_id));
    ZASSERT(queued(ReindexReason::DepsOnly).empty());
}

ZEST_CASE(CDBChangedSplitsOpenClosed) {
    TempDir tmp;
    tmp.touch("a.cpp", R"(int a;)");
    tmp.touch("b.cpp", R"(int b;)");
    auto json = build_cdb_json({
        {tmp.root, tmp.path("a.cpp"), {}},
        {tmp.root, tmp.path("b.cpp"), {}}
    });
    write_cdb(tmp, project.cdb, json);
    auto open_id = fixture.open(tmp.path("a.cpp"), "")->path_id;
    auto closed_id = fixture.intern(tmp.path("b.cpp"));
    index_unit(open_id);
    index_unit(closed_id);
    IndexInternals::plant_shard(project.index, open_id, shard_of(""));
    IndexInternals::plant_shard(project.index, closed_id, shard_of(""));

    fixture.build_changed({
        .changed = {open_id, closed_id}
    });

    // Flag changes recompile open files and reindex closed ones; the
    // pull-side cache keys (canonical flags) miss on their own.
    ZASSERT(invalidated(open_id));
    llvm::SmallVector<Fid> reindexed{open_id, closed_id};
    ranges::sort(reindexed);
    ZASSERT(queued(ReindexReason::ContentChanged) == reindexed);
    ZASSERT(queued(ReindexReason::DepsOnly).empty());

    // Both indexes were built under the old command and look fresh to
    // content-only validation: dropped so the queued reindexes are not
    // filtered out, here or after a restart. The shards themselves stay
    // with the indexer, which masks and retires them off the manifests.
    ZASSERT(!project.index.indexed(open_id));
    ZASSERT(!project.index.indexed(closed_id));
    ZASSERT(project.index.shard(closed_id) != nullptr);
    ZASSERT(project.index.shard(open_id) != nullptr);
}

ZEST_CASE(CDBAddedOpenMarksDirty) {
    auto file = fixture.open("/proj/a.cpp", "")->path_id;
    index_unit(file);

    fixture.build_changed({.added = {file}});

    // The open file gained its first real entry: drop the guessed command
    // it was compiled with, and queue the reindex that builds its shard
    // under the real command once open-file indexing is on.
    ZASSERT(invalidated(file));
    ZASSERT(queued(ReindexReason::ContentChanged) == llvm::SmallVector<Fid>{file});
    ZASSERT(!project.index.indexed(file));
    ZASSERT(queued(ReindexReason::DepsOnly).empty());
}

ZEST_CASE(CDBChangedDropsHostedContext) {
    auto host = fixture.intern("/proj/host.cpp");
    auto open_header = fixture.open("/proj/open.h", "")->path_id;
    auto other_header = fixture.open("/proj/other.h", "")->path_id;
    fixture.entry(open_header).latest =
        std::make_shared<DocumentCompile>(DocumentCompile{.host = host});
    fixture.entry(other_header).latest = std::make_shared<DocumentCompile>();
    index_unit(host);

    fixture.build_changed({.changed = {host}});

    // The open header borrowing the changed entry compiled under a command
    // that no longer exists: it recompiles. Only the host's index is
    // dropped — no header has one of its own. Unrelated headers are
    // untouched.
    ZASSERT(invalidated(open_header));
    ZASSERT(!invalidated(other_header));
    ZASSERT(!project.index.indexed(host));
}

ZEST_CASE(CDBChangedCascadesModule) {
    auto mod = fixture.intern("/proj/m.cppm");
    auto open_user = fixture.open("/proj/open_user.cppm", "")->path_id;
    auto closed_user = fixture.intern("/proj/closed_user.cppm");
    // The consumer edges PCMBuilder::declare_deps declares in production —
    // no rounds.
    auto node = [](Fid pid) {
        return NodeId{Family::PCM, pid.raw};
    };
    served.sched.graph.declare(node(open_user), {node(mod)});
    served.sched.graph.declare(node(closed_user), {node(mod)});
    index_unit(mod);

    fixture.build_changed({.changed = {mod}});

    // A module unit's flag change cascades through the compile graph
    // exactly like a content change: importers' PCMs went stale. The
    // unit itself is reached twice (its own entry changed AND the cascade
    // dirtied its PCM); the pump's absorbing upgrade leaves it
    // ContentChanged.
    ZEXPECT(invalidated(open_user));
    ZEXPECT(queued(ReindexReason::ContentChanged) == llvm::SmallVector<Fid>{mod});
    ZEXPECT(!project.index.indexed(mod));
    ZEXPECT(queued(ReindexReason::DepsOnly) == llvm::SmallVector<Fid>{closed_user});
}

ZEST_CASE(DiskRemovedReindexesIncluders) {
    auto header = fixture.intern("/proj/h.h");
    auto open_tu = fixture.open("/proj/a.cpp", "")->path_id;
    auto closed_tu = fixture.intern("/proj/b.cpp");
    project.dep_graph.set_includes(open_tu, 0, {{header}});
    project.dep_graph.set_includes(closed_tu, 0, {{header}});
    project.dep_graph.build_reverse_map();
    project.file_table.disk.saw_missing(header);

    served.disk_changed({header});

    // Dependents now compile against a missing include: open ones
    // recompile, closed ones reindex.
    ZASSERT(invalidated(open_tu));
    ZASSERT(queued(ReindexReason::DepsOnly) == llvm::SmallVector<Fid>{closed_tu});
    ZASSERT(queued(ReindexReason::ContentChanged).empty());
}

ZEST_CASE(CDBRemovedDropsSourceRole) {
    TempDir tmp;
    tmp.touch("inc/h.h", R"(int x;)");
    tmp.touch("kept.cpp", R"(#include "inc/h.h")");
    // The pre-reload graph still shows gone.cpp as an includer; the
    // database has already been reloaded without it.
    auto gone_id = fixture.intern(tmp.path("gone.cpp"));
    auto header_id = fixture.intern(tmp.path("inc/h.h"));
    project.dep_graph.set_includes(gone_id, 0, {{header_id}});
    project.dep_graph.build_reverse_map();
    auto json = build_cdb_json({
        {tmp.root, tmp.path("kept.cpp"), {}}
    });
    write_cdb(tmp, project.cdb, json);
    auto kept_id = fixture.intern(tmp.path("kept.cpp"));
    index_unit(gone_id);

    fixture.build_changed({.removed = {gone_id}});

    // The rebuild resolves includes from the surviving entries only. The
    // removed entry's index leaves: its database still loads and simply
    // stopped compiling the file.
    ZASSERT(project.dep_graph.get_all_includes(gone_id).empty());
    ZASSERT(project.dep_graph.get_includers(header_id) == llvm::ArrayRef<Fid>{kept_id});
    ZASSERT(!project.index.indexed(gone_id));
    ZASSERT(queued(ReindexReason::ContentChanged).empty());
}

ZEST_CASE(CDBRemovedStillCovered) {
    /// The entry left the database but a rule's default command still
    /// covers the file: a command change, not a retirement — the index is
    /// rebuilt under the default command instead of leaving.
    TempDir tmp;
    tmp.touch("gone.cpp", R"(int x;)");
    tmp.touch("kept.cpp", R"(int y;)");
    project.settings.rules.push_back(Rule{.default_command = std::string("clang++")});
    project.root = CanonicalPath(Spelling::absolute(tmp.root));
    project.settings.finalize(project.root);
    project.build.reset_active("");
    auto gone_id = fixture.intern(tmp.path("gone.cpp"));
    auto json = build_cdb_json({
        {tmp.root, tmp.path("kept.cpp"), {}}
    });
    write_cdb(tmp, project.cdb, json);
    index_unit(gone_id);

    fixture.build_changed({.removed = {gone_id}});

    ZASSERT(!project.index.indexed(gone_id));
    ZASSERT(queued(ReindexReason::ContentChanged) == llvm::SmallVector<Fid>{gone_id});
}

ZEST_CASE(CDBEmptyDeltaNoEffects) {
    auto epoch = project.context_epoch;

    fixture.build_changed({});

    ZASSERT(served.sched.pump.save_debt().empty());
    ZASSERT(project.context_epoch == epoch);
}

ZEST_CASE(BatchDiskEventsDeduplicate) {
    auto first = fixture.intern("/proj/a.h");
    auto second = fixture.intern("/proj/b.h");

    served.disk_changed({first, first, second});

    llvm::SmallVector<Fid> expected{first, second};
    ranges::sort(expected);
    ZASSERT(queued(ReindexReason::ContentChanged) == expected);
    ZASSERT(queued(ReindexReason::DepsOnly).empty());
}

};  // ZEST_SUITE(Changes)

ZEST_SUITE(ChoiceRecheck, Harness) {

ZEST_CASE(SurvivingEdgeKeepsChoice) {
    TempDir tmp;
    tmp.touch("host.cpp", R"(#include "h.h")");
    tmp.touch("h.h");
    write_cdb(tmp,
              project.cdb,
              build_cdb_json({
                  {tmp.root, tmp.path("host.cpp"), {}}
    }));
    auto host = fixture.intern(tmp.path("host.cpp"));
    auto header = fixture.intern(tmp.path("h.h"));
    project.dep_graph.set_includes(host, 0, {{header}});
    project.dep_graph.build_reverse_map();

    auto session = fixture.open(tmp.path("h.h"), "");
    fixture.server.choices[header] = {
        .choice = ContextChoice{host, std::nullopt, ""},
        .project = &served,
    };
    auto generation = session->generation;

    fixture.served->recheck_choices();
    auto* choice = fixture.server.choice(header);
    ZASSERT(choice != nullptr);
    ZASSERT(choice->active);
    ZASSERT(session->generation == generation);
}

ZEST_CASE(RemovedEdgeDeactivatesChoice) {
    // The host still compiles but no longer includes the header.
    TempDir tmp;
    tmp.touch("host.cpp", "int x;\n");
    tmp.touch("h.h");
    write_cdb(tmp,
              project.cdb,
              build_cdb_json({
                  {tmp.root, tmp.path("host.cpp"), {}}
    }));
    auto host = fixture.intern(tmp.path("host.cpp"));
    auto header = fixture.intern(tmp.path("h.h"));
    project.dep_graph.set_includes(host, 0, {});
    project.dep_graph.build_reverse_map();

    auto session = fixture.open(tmp.path("h.h"), "");
    fixture.server.choices[header] = {
        .choice = ContextChoice{host, std::nullopt, ""},
        .project = &served,
    };
    auto generation = session->generation;
    take_tree(project, tree_request(project, host, project.build.commands(host).front()));

    fixture.entry(header).current = true;
    fixture.served->recheck_choices();
    auto* choice = fixture.server.choice(header);
    ZASSERT(choice != nullptr);
    ZASSERT(!choice->active);
    ZASSERT(!fixture.entry(header).current);
    ZASSERT(session->generation == generation + 1);
}

ZEST_CASE(VanishedOccurrenceDeactivatesChoice) {
    TempDir tmp;
    // The host still compiles and includes the header, but only once — the
    // chosen occurrence #1 no longer exists.
    tmp.touch("host.cpp", R"(#include "h.h")");
    tmp.touch("h.h");
    write_cdb(tmp,
              project.cdb,
              build_cdb_json({
                  {tmp.root, tmp.path("host.cpp"), {}}
    }));
    auto host = fixture.intern(tmp.path("host.cpp"));
    auto header = fixture.intern(tmp.path("h.h"));
    project.dep_graph.set_includes(host, 0, {{header}});
    project.dep_graph.build_reverse_map();

    fixture.open(tmp.path("h.h"), "");
    fixture.server.choices[header] = {
        .choice = ContextChoice{host, 1, ""},
        .project = &served,
    };
    take_tree(project, tree_request(project, host, project.build.commands(host).front()));

    fixture.served->recheck_choices();
    auto* choice = fixture.server.choice(header);
    ZASSERT(choice != nullptr);
    ZASSERT(!choice->active);
}

};  // ZEST_SUITE(ChoiceRecheck)

}  // namespace
}  // namespace clice::testing
