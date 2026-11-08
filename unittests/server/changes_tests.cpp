module clice;

import :feature.diagnostics;
import :index.shard;
import :index.symbol;
import :index.tu_manifest;
import :project.background;
import :project.command;
import :project.project;
import :server.served_project;
import :server.session;
import :support.text;
import :unittests.server.served_project_fixture;
import :unittests.utils.cdb_helper;
import :unittests.utils.index_internals;
import :unittests.utils.indexer_fixture;
import :unittests.utils.temp_dir;
import :unittests.utils;
import :vfs.ids;

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
    /// invalidation moves its node's generation on.
    bool invalidated(Fid path_id) {
        return fixture.generation(path_id) != 0;
    }

    /// The files the background index owes a reindex for `reason`, sorted.
    llvm::SmallVector<Fid> queued(ReindexReason reason) {
        auto& queue = served.project.background_index.queue();
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

ZEST_SUITE(DocumentChanges, Harness) {

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

    fixture.commands_changed({
        .changed = {open_id, closed_id}
    });

    // Flag changes recompile open files and reindex closed ones; the
    // pull-side cache keys (canonical flags) miss on their own.
    ZASSERT(invalidated(open_id));
    llvm::SmallVector<Fid> reindexed{open_id, closed_id};
    ranges::sort(reindexed);
    ZASSERT(queued(ReindexReason::ContentChanged) == reindexed);
    ZASSERT(queued(ReindexReason::DepsOnly).empty());

    // Both indexes were built under the old command: they serve until the
    // reindexes merge, which no freshness check filters out — their
    // manifests name the old command.
    ZASSERT(project.index.indexed(open_id));
    ZASSERT(project.index.indexed(closed_id));
    ZASSERT(project.index.shard(closed_id) != nullptr);
    ZASSERT(project.index.shard(open_id) != nullptr);
    // What they walked under the old command no other unit may skip.
    ZEXPECT(IndexInternals::outdated(project.index, open_id));
    ZEXPECT(IndexInternals::outdated(project.index, closed_id));
}

ZEST_CASE(CDBAddedOpenMarksDirty) {
    TempDir tmp;
    tmp.touch("a.cpp");
    write_cdb(tmp,
              project.cdb,
              build_cdb_json({
                  {tmp.root, tmp.path("a.cpp"), {}}
    }));
    auto file = fixture.open(tmp.path("a.cpp"), "")->path_id;
    index_unit(file);

    fixture.commands_changed({.added = {file}});

    // The open file gained its first real entry: drop the guessed command
    // it was compiled with, and queue the reindex that builds its shard
    // under the real command once open-file indexing is on.
    ZASSERT(invalidated(file));
    ZASSERT(queued(ReindexReason::ContentChanged) == llvm::SmallVector<Fid>{file});
    ZASSERT(project.index.indexed(file));
    ZASSERT(queued(ReindexReason::DepsOnly).empty());
}

ZEST_CASE(CDBChangedDropsHostedContext) {
    // The other header borrows the command of its sibling unit, which stays.
    TempDir tmp;
    tmp.touch("host.cpp", R"(#include "open.h")");
    tmp.touch("open.h");
    tmp.touch("other/other.cpp");
    tmp.touch("other/other.h");
    write_cdb(tmp,
              project.cdb,
              build_cdb_json({
                  {tmp.root, tmp.path("host.cpp"),        {"-DBEFORE"}},
                  {tmp.root, tmp.path("other/other.cpp"), {}          },
    }));
    scan_all(project.cdb, project.toolchain, project.dep_graph);
    project.dep_graph.build_reverse_map();
    auto host = fixture.intern(tmp.path("host.cpp"));
    auto open_header = fixture.open(tmp.path("open.h"), "")->path_id;
    auto other_header = fixture.open(tmp.path("other/other.h"), "")->path_id;
    for(auto header: {open_header, other_header}) {
        auto selection = project.select_command(header, {.open = true});
        fixture.entry(header).latest = std::make_shared<DocumentCompile>(
            DocumentCompile{.host = selection.host, .command = selection.ref.command});
    }
    ZASSERT(fixture.entry(open_header).latest->host == host);
    index_unit(host);

    write_cdb(tmp,
              project.cdb,
              build_cdb_json({
                  {tmp.root, tmp.path("host.cpp"),        {"-DAFTER"}},
                  {tmp.root, tmp.path("other/other.cpp"), {}         },
    }));
    fixture.commands_changed({.changed = {host}});

    // The open header compiling in the changed entry compiled under a command
    // that no longer exists: it recompiles. Only the host reindexes — no
    // header has an index of its own. Unrelated headers are untouched.
    ZASSERT(invalidated(open_header));
    ZASSERT(!invalidated(other_header));
    ZASSERT(queued(ReindexReason::ContentChanged) == llvm::SmallVector<Fid>{host});
}

};  // ZEST_SUITE(DocumentChanges)

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
    project.take_tree_in_process(project.tree_request(host, project.candidates(host).front()));

    fixture.compiled_now(header);
    fixture.served->recheck_choices();
    auto* choice = fixture.server.choice(header);
    ZASSERT(choice != nullptr);
    ZASSERT(!choice->active);
    ZASSERT(!fixture.served->current(header));
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
    project.take_tree_in_process(project.tree_request(host, project.candidates(host).front()));

    fixture.served->recheck_choices();
    auto* choice = fixture.server.choice(header);
    ZASSERT(choice != nullptr);
    ZASSERT(!choice->active);
}

};  // ZEST_SUITE(ChoiceRecheck)

}  // namespace
}  // namespace clice::testing
