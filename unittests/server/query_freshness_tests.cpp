module clice;

import :index.query;
import :index.search;
import :index.shard;
import :index.tu_index;
import :project.project;
import :sched.index.pump;
import :server.served_project;
import :unittests.server.served_project_fixture;
import :unittests.utils.merge_unit;
import :unittests.utils.tester;
import :unittests.utils;
import :vfs.path;

namespace clice::testing {
namespace {

ZEST_SUITE(QueryFreshness, Tester) {

ServedProjectFixture fx;
Project& project = fx.served->project;
IndexPump& indexer = fx.served->sched.pump;
index::IndexQuery& index_query = project.index_query;
index::IndexQuery disk_query{project.index, project.file_table, &project.freshness, nullptr};

Fid focus_id;
Fid header_id;

/// Build a TU index from the added sources and merge it into the
/// workspace.
void merge_into_workspace() {
    merge_unit(project, *unit, focus_id);
    header_id = project.file_table.intern(Spelling::absolute(TestVFS::path("header.h")));
}

/// The symbol hash at an offset in a file's merged shard.
index::SymbolHash symbol_at(Fid path_id, std::uint32_t offset) {
    index::SymbolHash result = 0;
    project.index.shard(path_id)->occurrences_at(offset, [&](const index::Occurrence& o) {
        result = o.target;
        return false;
    });
    return result;
}

/// Files contributing reference relations for a symbol, by basename.
std::vector<std::string> reference_files(index::SymbolHash hash) {
    std::vector<std::string> files;
    for(auto& site: disk_query.sites(hash, Fid{}, RelationKind::Reference)) {
        files.push_back(std::string(path::filename(site.path)));
    }
    return files;
}

ZEST_CASE(PendingReasonUpgrade) {
    auto file = project.file_table.intern(Spelling::absolute("/proj/upgrade.cpp"));
    ZASSERT(!indexer.pending_reason(file).has_value());

    indexer.enqueue(file, ReindexReason::DepsOnly);
    ZASSERT(indexer.pending_reason(file) == ReindexReason::DepsOnly);
    ZASSERT(indexer.pending_files() == 1u);

    // ContentChanged absorbs a queued DepsOnly without a second queue entry.
    indexer.enqueue(file, ReindexReason::ContentChanged);
    ZASSERT(indexer.pending_reason(file) == ReindexReason::ContentChanged);
    ZASSERT(indexer.pending_files() == 1u);

    // A later deps-only cascade never downgrades it.
    indexer.enqueue(file, ReindexReason::DepsOnly);
    ZASSERT(indexer.pending_reason(file) == ReindexReason::ContentChanged);
}

ZEST_CASE(GateWithholdsStaleShard) {
    add_file("header.h", R"(
        int helper() { return 1; }
    )");
    add_focus("main.cpp", R"(
        #include "header.h"
        int main() {
            return §(use)helper();
        }
    )");
    kind = CompilationKind::Whole;
    ZASSERT(compile());
    merge_into_workspace();

    auto hash = symbol_at(focus_id, point("use"));
    ZASSERT(hash != 0UL);

    // Baseline: the main TU contributes its reference relation, and the
    // definition resolves into the header shard.
    ZASSERT(ranges::contains(reference_files(hash), "main.cpp"));
    ZASSERT(index_query.first_site(hash, Fid{}, RelationKind::Definition));

    // Awaiting a reindex for a dependency change only: the disk still
    // holds the text its shard indexed, so the shard keeps serving.
    indexer.enqueue(focus_id, ReindexReason::DepsOnly);
    ZASSERT(ranges::contains(reference_files(hash), "main.cpp"));

    // Line-based resolution in the file works while its index is current.
    index::SymbolQuery by_line;
    by_line.position = {.path = project.file_table.resolve(focus_id).str(), .line = 3};
    ZASSERT(!index::locate(disk_query, by_line).empty());

    // The disk was seen holding other text: the file's contribution is
    // skipped until its shard describes the disk again; other files' shards
    // are unaffected.
    project.file_table.disk.observe(focus_id, DiskObservation{.hash = 1});
    ZASSERT(!ranges::contains(reference_files(hash), "main.cpp"));
    ZASSERT(index_query.first_site(hash, Fid{}, RelationKind::Definition));

    // Cursor-style resolution against the stale index is unresolvable: the
    // line numbers describe text that no longer exists.
    ZASSERT(index::locate(disk_query, by_line).empty());

    // A changed definition file drops out of definition lookups.
    project.file_table.disk.observe(header_id, DiskObservation{.hash = 1});
    ZASSERT(!index_query.first_site(hash, Fid{}, RelationKind::Definition).has_value());

    // With background indexing disabled nothing would ever catch up:
    // the last-known index keeps serving instead of leaving a permanent hole.
    project.freshness.options.withhold = false;
    ZASSERT(ranges::contains(reference_files(hash), "main.cpp"));
}

ZEST_CASE(DeletedFileWithdrawsIndex) {
    add_focus("main.cpp", R"(
        int helper() { return 1; }
        int use() { return §(use)helper(); }
    )");
    kind = CompilationKind::Whole;
    ZASSERT(compile());
    merge_into_workspace();
    auto hash = symbol_at(focus_id, point("use"));
    ZASSERT(hash != 0UL);
    ZASSERT(ranges::contains(reference_files(hash), "main.cpp"));

    // The index of a file seen gone points at text that is gone with it.
    project.file_table.disk.saw_missing(focus_id);
    ZASSERT(!ranges::contains(reference_files(hash), "main.cpp"));
}

};  // ZEST_SUITE(QueryFreshness)

}  // namespace
}  // namespace clice::testing
