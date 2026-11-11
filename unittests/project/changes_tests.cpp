module clice;

import :index.shard;
import :index.symbol;
import :index.project_index.manifest;
import :project.background;
import :project.graph.pcm;
import :project.graph.task_graph;
import :project.project;
import :settings.rules;
import :settings.settings;
import :support.failure;
import :support.text;
import :unittests.utils.cdb_helper;
import :unittests.utils.index_internals;
import :unittests.utils.indexer_fixture;
import :unittests.utils.project_fixture;
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

/// The project the changes are handed to. Its owner's open documents'
/// compiles parse the bodies of their headers and serve their index, so
/// that a dependency change splits open files from closed ones
/// (SkippedBodiesReindexOpen covers the default).
struct Harness : ProjectFixture {
    Harness() {
        project.settings.project.skip_header_bodies = false;
    }

    /// The files the background index owes a reindex for `reason`, sorted.
    llvm::SmallVector<Fid> queued(ReindexReason reason) {
        auto& queue = project.background_index.queue();
        return ranges::to<llvm::SmallVector<Fid>>(
            views::filter(IndexerFixture::pending_files(queue),
                          [&](Fid file) { return queue.pending_reason(file) == reason; }));
    }

    /// Give the unit an index of its own.
    void index_unit(Fid tu) {
        index::TUManifest manifest;
        manifest.built_at = 1;
        manifest.tu_version = project.file_table.intern_version(tu, 1);
        project.index.apply_manifest(tu, std::move(manifest));
    }
};

ZEST_SUITE(Changes, Harness) {

ZEST_CASE(EmptyBatchNoEffects) {
    project.files_changed({}, open_files);

    ZASSERT(IndexerFixture::pending_files(project.background_index.queue()).empty());
}

ZEST_CASE(NewProviderDirtiesImporters) {
    // Consumers that scanned the name unresolved hold durable edges to
    // its unresolved-import node; the first provider cascades through them. A
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
    auto iface = intern(tmp.path("m.cppm"));
    auto closed = list_unit(project.cdb, "/proj/closed.cpp");
    auto open = open_document("/proj/open.cpp");
    index_unit(closed);
    project.graph.set_edges({NodeKind::Unit, closed.raw}, {unresolved_import("m")});
    project.graph.set_edges({NodeKind::AST, open.raw}, {unresolved_import("m")});

    auto affected = project.files_changed({iface}, open_files);

    ZEXPECT(project.background_index.queue().pending_reason(closed) ==
            ReindexReason::ContentChanged);
    ZEXPECT(ranges::contains(affected.stale, open));
    ZEXPECT(project.index.indexed(closed));

    // The same save again: the name already has its provider.
    project.background_index.forget(closed);
    affected = project.files_changed({iface}, open_files);
    ZEXPECT(!project.background_index.queue().pending_reason(closed).has_value());
    ZEXPECT(!ranges::contains(affected.stale, open));
}

ZEST_CASE(ReloadProviderCascades) {
    // The database-reload variant of provider appearance: the provider-set
    // diff drives the same unresolved-import cascade, and a consumer removed from the build by the
    // very same reload leaves the index without a reindex being owed.
    TempDir tmp;
    tmp.touch("m.cppm", "export module m;\nexport int mv();\n");
    // The database already reloaded: only the provider remains.
    write_cdb(tmp,
              project.cdb,
              build_cdb_json({
                  {tmp.root, tmp.path("m.cppm"), {}}
    }));
    auto iface = intern(tmp.path("m.cppm"));
    auto removed = intern(tmp.path("old.cpp"));
    index_unit(removed);
    project.graph.set_edges({NodeKind::Unit, removed.raw}, {unresolved_import("m")});

    project.update_members({.added = {iface}, .removed = {removed}}, open_files);

    ZEXPECT(!project.background_index.queue().pending_reason(removed).has_value());
    ZEXPECT(!project.index.indexed(removed));
}

ZEST_CASE(DiskRemovedDropsProvider) {
    // Deleting a provider must leave the module map too: a later
    // replacement provider would otherwise sit behind the deleted one in
    // the candidate list and never be selected.
    auto iface = intern("/proj/m.cppm");
    project.dep_graph.add_module("m", iface);
    project.file_table.disk.saw_missing(iface);

    project.files_changed({iface}, open_files);

    ZEXPECT(project.dep_graph.lookup_module("m").empty());
}

ZEST_CASE(DiskChangeSparesSession) {
    TempDir tmp;
    tmp.touch("a.cpp", "int x;");
    auto saved = list_unit(project.cdb, tmp.path("a.cpp"));
    open_document(tmp.path("a.cpp"));
    project.file_table.disk.read(saved);

    auto affected = project.files_changed({saved}, open_files);

    // The open file's own compile reads its buffer, never its disk: it is
    // not stale, while its disk index is.
    ZASSERT(!ranges::contains(affected.stale, saved));
    ZASSERT(queued(ReindexReason::ContentChanged) == llvm::SmallVector<Fid>{saved});
}

ZEST_CASE(CascadeSplitsOpenClosed) {
    auto mod = list_unit(project.cdb, "/proj/m.cppm");
    auto open_user = open_document("/proj/open_user.cppm");
    auto closed_user = list_unit(project.cdb, "/proj/closed_user.cppm");
    // The edges their attempts declare in production — no attempt runs.
    project.graph.set_edges(module_node(open_user), {module_node(mod)});
    project.graph.set_edges(module_node(closed_user), {module_node(mod)});

    auto affected = project.files_changed({mod}, open_files);

    // Cascade-dirtied module units split by document state: open buffers
    // recompile, closed files go back to the background indexer. The
    // changed unit, dirtied too, is owed the reindex of its own content.
    ZEXPECT(ranges::contains(affected.stale, open_user));
    ZEXPECT(queued(ReindexReason::DepsOnly) == llvm::SmallVector<Fid>{closed_user});
    ZEXPECT(queued(ReindexReason::ContentChanged) == llvm::SmallVector<Fid>{mod});
}

ZEST_CASE(ChainHitAndMiss) {
    auto saved = intern("/proj/inner.h");
    auto other = intern("/proj/other.h");
    // The chain files a header's context embeds are inputs of its compile.
    auto hit = open_document("/proj/hit.h", {saved});
    auto miss = open_document("/proj/miss.h", {other});

    auto affected = project.files_changed({saved}, open_files);

    // The header whose context embeds the saved file recompiles; no header
    // has an index of its own to rebuild.
    ZASSERT(ranges::contains(affected.stale, hit));
    ZASSERT(!ranges::contains(affected.stale, miss));
    ZASSERT(IndexerFixture::pending_files(project.background_index.queue()).empty());
}

ZEST_CASE(SaveMarksDependents) {
    auto header = intern("/proj/h.h");
    auto open_tu = open_document("/proj/a.cpp");
    auto closed_tu = list_unit(project.cdb, "/proj/b.cpp");
    project.dep_graph.set_includes(open_tu, 0, {{header}});
    project.dep_graph.set_includes(closed_tu, 0, {{header}});
    project.dep_graph.build_reverse_map();

    auto affected = project.files_changed({header}, open_files);

    // Open dependents recompile, closed ones reindex; the old/new dependent
    // snapshots overlap fully here, so this also proves the dedup. A
    // dependent's own content did not change: deps-only. The header is no
    // unit: it owes no reindex of its own.
    ZASSERT(ranges::contains(affected.stale, open_tu));
    ZASSERT(queued(ReindexReason::DepsOnly) == llvm::SmallVector<Fid>{closed_tu});
    ZASSERT(queued(ReindexReason::ContentChanged).empty());
}

ZEST_CASE(SkippedBodiesReindexOpen) {
    project.settings.project.skip_header_bodies = true;
    auto header = intern("/proj/h.h");
    auto open_tu = list_unit(project.cdb, "/proj/a.cpp");
    open_document("/proj/a.cpp");
    auto closed_tu = list_unit(project.cdb, "/proj/b.cpp");
    project.dep_graph.set_includes(open_tu, 0, {{header}});
    project.dep_graph.set_includes(closed_tu, 0, {{header}});
    project.dep_graph.build_reverse_map();

    auto affected = project.files_changed({header}, open_files);

    // An open file's compile skipping the bodies of its headers serves no
    // index of what they hold: the open dependent reindexes too.
    ZASSERT(ranges::contains(affected.stale, open_tu));
    llvm::SmallVector<Fid> reindexed{open_tu, closed_tu};
    ranges::sort(reindexed);
    ZASSERT(queued(ReindexReason::DepsOnly) == reindexed);
    ZASSERT(!project.background_index.left_to_document(open_tu));
}

ZEST_CASE(TransitiveDependentsEnqueue) {
    auto header = intern("/proj/h.h");
    auto middle = intern("/proj/g.h");
    auto root = list_unit(project.cdb, "/proj/c.cpp");
    project.dep_graph.set_includes(middle, 0, {{header}});
    project.dep_graph.set_includes(root, 0, {{middle}});
    project.dep_graph.build_reverse_map();

    project.files_changed({header}, open_files);

    // Only root TUs own index shards; neither header is one.
    ZASSERT(queued(ReindexReason::DepsOnly) == llvm::SmallVector<Fid>{root});
    ZASSERT(queued(ReindexReason::ContentChanged).empty());
}

ZEST_CASE(ForcedHeaderReachesUnits) {
    // A header reached only through a command's forced include: its
    // change reaches the units forcing that header in, open or closed.
    auto header = intern("/proj/h.h");
    auto forced = intern("/proj/force.h");
    auto open = open_document("/proj/a.cpp");
    auto closed = list_unit(project.cdb, "/proj/b.cpp");
    project.dep_graph.set_includes(forced, 0, {{header}});
    project.dep_graph.add_forced_include(open, forced);
    project.dep_graph.add_forced_include(closed, forced);
    project.dep_graph.build_reverse_map();

    auto affected = project.files_changed({header}, open_files);

    ZASSERT(ranges::contains(affected.stale, open));
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
    auto header = intern(tmp.path("h.h"));
    auto known = intern(tmp.path("a.cpp"));
    // Open, so that its own change spares its compile: only the header's
    // cascade recompiles it.
    auto added = open_document(tmp.path("b.cpp"));
    tmp.touch("b.cpp", R"(#include "h.h")");

    auto affected = project.files_changed({added, header}, open_files);

    ZASSERT(ranges::contains(queued(ReindexReason::DepsOnly), known));
    ZASSERT(ranges::contains(affected.stale, added));
    ZASSERT(ranges::contains(project.dep_graph.get_includers(header), added));
}

ZEST_CASE(RemovalThenChangeKeepsClear) {
    // A unit removed earlier in the batch is no longer an includer when a
    // header it included changes: its clear survives, nothing requeues it.
    TempDir tmp;
    tmp.touch("h.h", "int changed;");
    auto header = intern(tmp.path("h.h"));
    auto removed = list_unit(project.cdb, tmp.path("gone.cpp"));
    auto kept = list_unit(project.cdb, tmp.path("kept.cpp"));
    project.dep_graph.set_includes(removed, 0, {{header}});
    project.dep_graph.set_includes(kept, 0, {{header}});
    project.dep_graph.build_reverse_map();
    // A change observed just before the deletion.
    project.background_index.enqueue(removed, ReindexReason::ContentChanged);
    project.file_table.disk.saw_missing(removed);

    project.files_changed({removed, header}, open_files);

    ZASSERT(!project.background_index.queue().pending_reason(removed).has_value());
    ZASSERT(project.background_index.queue().pending_reason(kept) == ReindexReason::DepsOnly);
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
    auto iface = intern(tmp.path("m.cpp"));
    ZASSERT(project.dep_graph.module_of(iface) == "m");
    tmp.touch("m.cpp", "#if 1\nexport module m;\n#endif\nexport int v;\n");

    IndexInternals::plant_shard(project.index, iface, shard_of(*read_file(tmp.path("m.cpp"))));

    project.files_changed({iface}, open_files);

    ZEXPECT(project.dep_graph.module_of(iface) == "m");
    ZEXPECT(ranges::contains(project.dep_graph.lookup_module("m"), iface));
}

ZEST_CASE(BatchChangesDeduplicate) {
    // The file exists: opening a document looks at it, and one seen
    // missing is a removal.
    TempDir tmp;
    tmp.touch("a.cpp");
    auto saved = list_unit(project.cdb, tmp.path("a.cpp"));
    open_document(tmp.path("a.cpp"));

    project.files_changed({saved, saved}, open_files);

    ZASSERT(queued(ReindexReason::ContentChanged) == llvm::SmallVector<Fid>{saved});
}

ZEST_CASE(DiskChangeClosedCascades) {
    auto header = intern("/proj/h.h");
    auto open_tu = open_document("/proj/a.cpp");
    auto closed_tu = list_unit(project.cdb, "/proj/b.cpp");
    project.dep_graph.set_includes(open_tu, 0, {{header}});
    project.dep_graph.set_includes(closed_tu, 0, {{header}});
    project.dep_graph.build_reverse_map();

    auto affected = project.files_changed({header}, open_files);

    // A closed file's disk change cascades exactly like a save: its
    // dependent only rebuilds semantics, and the header, no unit, owes no
    // reindex of its own.
    ZASSERT(ranges::contains(affected.stale, open_tu));
    ZASSERT(queued(ReindexReason::ContentChanged).empty());
    ZASSERT(queued(ReindexReason::DepsOnly) == llvm::SmallVector<Fid>{closed_tu});
}

ZEST_CASE(DiskChangeOpenCascades) {
    // Open or not, a disk change reaches every file reading the disk: the
    // open header's includers cascade now, not at its close, and only the
    // header's own document — whose compile reads its buffer — is spared.
    // The header, no unit, owes no reindex of its own. It exists: opening a
    // document looks at it, and one seen missing is a removal.
    TempDir tmp;
    tmp.touch("h.h");
    auto header = open_document(tmp.path("h.h"));
    auto open_tu = open_document(tmp.path("a.cpp"));
    auto closed_tu = list_unit(project.cdb, tmp.path("b.cpp"));
    project.dep_graph.set_includes(open_tu, 0, {{header}});
    project.dep_graph.set_includes(closed_tu, 0, {{header}});
    project.dep_graph.build_reverse_map();

    auto affected = project.files_changed({header}, open_files);

    ZASSERT(ranges::contains(affected.stale, open_tu));
    ZASSERT(!ranges::contains(affected.stale, header));
    ZASSERT(queued(ReindexReason::ContentChanged).empty());
    ZASSERT(queued(ReindexReason::DepsOnly) == llvm::SmallVector<Fid>{closed_tu});
}

ZEST_CASE(CompiledIncluderCascades) {
    // An includer the lexical scan never saw (a macro include) but whose
    // indexed compile read the file is a dependent all the same.
    auto header = intern("/proj/m.h");
    auto scanned = list_unit(project.cdb, "/proj/a.cpp");
    auto compiled = list_unit(project.cdb, "/proj/b.cpp");
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

    project.files_changed({header}, open_files);

    llvm::SmallVector<Fid> reindexed{scanned, compiled};
    ranges::sort(reindexed);
    ZASSERT(queued(ReindexReason::DepsOnly) == reindexed);
    // Until they are indexed again, what their runs walked is outdated —
    // and the file's own, should it be a unit of its own.
    ZEXPECT(IndexInternals::outdated(project.index, compiled));
    ZEXPECT(IndexInternals::outdated(project.index, scanned));
    ZEXPECT(IndexInternals::outdated(project.index, header));
}

ZEST_CASE(ModuleReadHeaderCascades) {
    // A header only a module unit's PCM read (its global module fragment):
    // no include edge names the importers, the unit's recorded inputs do.
    auto header = intern("/proj/gmf.h");
    auto mod = intern("/proj/m.cppm");
    auto user = list_unit(project.cdb, "/proj/user.cpp");
    project.graph.set_edges(NodeId{NodeKind::Unit, user.raw}, {module_node(mod)});
    project.pcms[module_node(mod)].inputs.push_back({.file = header});

    project.files_changed({header}, open_files);

    ZASSERT(project.background_index.queue().pending_reason(user) == ReindexReason::DepsOnly);
}

ZEST_CASE(FailedModuleHeaderCascades) {
    // The module's build failed on the header: it is no less an input, and
    // its importers hear of its change through its failure record.
    auto header = intern("/proj/gmf.h");
    auto mod = intern("/proj/m.cppm");
    auto user = list_unit(project.cdb, "/proj/user.cpp");
    project.graph.set_edges(NodeId{NodeKind::Unit, user.raw}, {module_node(mod)});
    FailureRecord error;
    error.cause = "use of undeclared identifier 'x'";
    project.pcms[module_node(mod)].failure = {{.record = error}, {{.file = header}}};

    ZASSERT(ranges::contains(project.readers(header), mod));
    project.files_changed({header}, open_files);
    ZASSERT(project.background_index.queue().pending_reason(user) == ReindexReason::DepsOnly);

    project.background_index.forget(user);
    project.file_table.disk.saw_missing(header);
    project.files_changed({header}, open_files);
    ZASSERT(project.background_index.queue().pending_reason(user) == ReindexReason::DepsOnly);
}

ZEST_CASE(AppearedHeaderCascades) {
    // A header appearing where compiles looked for it: the closed TU whose
    // indexed compile looked reindexes, the open document whose AST looked
    // recompiles.
    auto header = intern("/proj/gen.h");
    auto closed = list_unit(project.cdb, "/proj/b.cpp");
    auto open = open_document("/proj/a.cpp", {header});
    index::TUManifest manifest;
    manifest.built_at = 1;
    manifest.tu_version = project.file_table.intern_version(closed, 1);
    manifest.absent = {project.file_table.intern_version(header, 0)};
    project.index.apply_manifest(closed, std::move(manifest));

    auto affected = project.files_changed({header}, open_files);

    ZASSERT(queued(ReindexReason::DepsOnly) == llvm::SmallVector<Fid>{closed});
    ZASSERT(ranges::contains(affected.stale, open));
}

ZEST_CASE(DiskRemovedScrubsSourceRole) {
    auto header = intern("/proj/h.h");
    auto removed_tu = list_unit(project.cdb, "/proj/gone.cpp");
    auto other_tu = list_unit(project.cdb, "/proj/kept.cpp");
    project.dep_graph.set_includes(removed_tu, 0, {{header}});
    project.dep_graph.set_includes(other_tu, 0, {{header}});
    project.dep_graph.build_reverse_map();
    // A change observed just before the deletion.
    project.background_index.enqueue(removed_tu, ReindexReason::ContentChanged);
    project.file_table.disk.saw_missing(removed_tu);
    auto epoch = project.context_epoch();

    project.files_changed({removed_tu}, open_files);

    // The removed file stops being an includer (and thus a host-source
    // candidate); surviving includers are untouched, shards are kept.
    ZASSERT(project.dep_graph.get_includers(header) == llvm::ArrayRef<Fid>{other_tu});
    ZASSERT(project.dep_graph.get_all_includes(removed_tu).empty());
    ZASSERT(project.context_epoch() == epoch + 1);
    // The removal clears any pending-reindex state recorded earlier: the
    // shard keeps serving and nothing is left to reindex.
    ZASSERT(IndexerFixture::pending_files(project.background_index.queue()).empty());
}

ZEST_CASE(RemoveRecreateOrder) {
    TempDir tmp;
    tmp.touch("a.cpp");
    auto file = list_unit(project.cdb, tmp.path("a.cpp"));
    project.dep_graph.set_includes(file, 0, {});
    project.dep_graph.build_reverse_map();

    // Change then delete: the removal is the later fact, the clear wins.
    project.files_changed({file}, open_files);
    fs::remove_all(tmp.path("a.cpp"));
    project.file_table.disk.look(file);
    project.files_changed({file}, open_files);
    ZASSERT(!project.background_index.queue().pending_reason(file).has_value());

    // Delete then recreate (an editor's atomic save): the later change must
    // survive — the recreated file needs its reindex.
    project.dep_graph.set_includes(file, 0, {});
    project.dep_graph.build_reverse_map();
    tmp.touch("a.cpp", "int a;");
    project.file_table.disk.look(file);
    project.files_changed({file}, open_files);
    ZASSERT(project.background_index.queue().pending_reason(file) == ReindexReason::ContentChanged);
}

ZEST_CASE(CDBAddedScansAndEnqueues) {
    TempDir tmp;
    tmp.touch("inc/header.h", R"(int x = 1;)");
    tmp.touch("src/main.cpp", R"(#include "header.h")");
    auto json = build_cdb_json({
        {tmp.root, tmp.path("src/main.cpp"), {"-I", tmp.path("inc")}}
    });
    write_cdb(tmp, project.cdb, json);
    auto main_id = intern(tmp.path("src/main.cpp"));
    auto header_id = intern(tmp.path("inc/header.h"));
    index_unit(main_id);

    project.update_members({.added = {main_id}}, open_files);

    // The rescan resolved the new entry's includes; the new file reindexes.
    // A command change rewrites the index as thoroughly as an edit, and the
    // old index serves until the reindex merges.
    ZASSERT(project.dep_graph.get_includers(header_id) == llvm::ArrayRef<Fid>{main_id});
    ZASSERT(queued(ReindexReason::ContentChanged) == llvm::SmallVector<Fid>{main_id});
    ZASSERT(project.index.indexed(main_id));
    ZASSERT(queued(ReindexReason::DepsOnly).empty());
}

ZEST_CASE(CDBChangedCascadesModule) {
    TempDir tmp;
    tmp.touch("m.cppm", "export module m;\n");
    write_cdb(tmp,
              project.cdb,
              build_cdb_json({
                  {tmp.root, tmp.path("m.cppm"),           {}},
                  {tmp.root, tmp.path("closed_user.cppm"), {}},
    }));
    auto mod = intern(tmp.path("m.cppm"));
    auto open_user = open_document(tmp.path("open_user.cppm"));
    auto closed_user = intern(tmp.path("closed_user.cppm"));
    // The edges their attempts declare in production — no attempt runs.
    project.graph.set_edges(module_node(open_user), {module_node(mod)});
    project.graph.set_edges(module_node(closed_user), {module_node(mod)});
    index_unit(mod);

    auto affected = project.update_members({.changed = {mod}}, open_files);

    // A module unit's flag change cascades through the compile graph
    // exactly like a content change: importers' PCMs went stale. The
    // unit itself is reached twice (its own entry changed AND the cascade
    // dirtied its PCM); the queue's absorbing upgrade leaves it
    // ContentChanged.
    ZEXPECT(ranges::contains(affected.stale, open_user));
    ZEXPECT(queued(ReindexReason::ContentChanged) == llvm::SmallVector<Fid>{mod});
    ZEXPECT(project.index.indexed(mod));
    ZEXPECT(queued(ReindexReason::DepsOnly) == llvm::SmallVector<Fid>{closed_user});
}

ZEST_CASE(DiskRemovedReindexesIncluders) {
    auto header = intern("/proj/h.h");
    auto open_tu = open_document("/proj/a.cpp");
    auto closed_tu = list_unit(project.cdb, "/proj/b.cpp");
    project.dep_graph.set_includes(open_tu, 0, {{header}});
    project.dep_graph.set_includes(closed_tu, 0, {{header}});
    project.dep_graph.build_reverse_map();
    project.file_table.disk.saw_missing(header);

    auto affected = project.files_changed({header}, open_files);

    // Dependents now compile against a missing include: open ones
    // recompile, closed ones reindex.
    ZASSERT(ranges::contains(affected.stale, open_tu));
    ZASSERT(queued(ReindexReason::DepsOnly) == llvm::SmallVector<Fid>{closed_tu});
    ZASSERT(queued(ReindexReason::ContentChanged).empty());
}

ZEST_CASE(CDBRemovedDropsSourceRole) {
    TempDir tmp;
    tmp.touch("inc/h.h", R"(int x;)");
    tmp.touch("kept.cpp", R"(#include "inc/h.h")");
    // The pre-reload graph still shows gone.cpp as an includer; the
    // database has already been reloaded without it.
    auto gone_id = intern(tmp.path("gone.cpp"));
    auto header_id = intern(tmp.path("inc/h.h"));
    project.dep_graph.set_includes(gone_id, 0, {{header_id}});
    project.dep_graph.build_reverse_map();
    auto json = build_cdb_json({
        {tmp.root, tmp.path("kept.cpp"), {}}
    });
    write_cdb(tmp, project.cdb, json);
    auto kept_id = intern(tmp.path("kept.cpp"));
    index_unit(gone_id);

    project.update_members({.removed = {gone_id}}, open_files);

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
    /// covers the file: a command change, not a removal from the build — the index is
    /// rebuilt under the default command instead of leaving, and serves until then.
    TempDir tmp;
    tmp.touch("gone.cpp", R"(int x;)");
    tmp.touch("kept.cpp", R"(int y;)");
    project.settings.rules.push_back(Rule{.default_command = std::string("clang++")});
    project.root = CanonicalPath(Spelling::absolute(tmp.root));
    project.settings.finalize(project.root);
    project.rules = Rules(project.settings, project.root, "");
    auto gone_id = intern(tmp.path("gone.cpp"));
    auto json = build_cdb_json({
        {tmp.root, tmp.path("kept.cpp"), {}}
    });
    write_cdb(tmp, project.cdb, json);
    index_unit(gone_id);

    project.update_members({.removed = {gone_id}}, open_files);

    ZASSERT(project.index.indexed(gone_id));
    ZASSERT(queued(ReindexReason::ContentChanged) == llvm::SmallVector<Fid>{gone_id});
}

ZEST_CASE(CDBEmptyDeltaNoEffects) {
    auto epoch = project.context_epoch();

    project.update_members({}, open_files);

    ZASSERT(IndexerFixture::pending_files(project.background_index.queue()).empty());
    ZASSERT(project.context_epoch() == epoch);
}

ZEST_CASE(BatchDiskEventsDeduplicate) {
    auto first = list_unit(project.cdb, "/proj/a.cpp");
    auto second = list_unit(project.cdb, "/proj/b.cpp");

    project.files_changed({first, first, second}, open_files);

    llvm::SmallVector<Fid> expected{first, second};
    ranges::sort(expected);
    ZASSERT(queued(ReindexReason::ContentChanged) == expected);
    ZASSERT(queued(ReindexReason::DepsOnly).empty());
}

};  // ZEST_SUITE(Changes)

}  // namespace
}  // namespace clice::testing
