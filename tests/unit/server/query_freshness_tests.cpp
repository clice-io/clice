#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "test/test.h"
#include "test/tester.h"
#include "index/query.h"
#include "index/shard.h"
#include "index/tu_index.h"
#include "project/command_resolver.h"
#include "project/index_store.h"
#include "sched/families/pch.h"
#include "sched/families/pcm.h"
#include "sched/families/turun.h"
#include "sched/graph.h"
#include "sched/index/pump.h"
#include "server/ast_projection.h"
#include "server/live_sources.h"
#include "server/session_store.h"
#include "worker/pool.h"

#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/xxhash.h"

namespace clice::testing {
namespace {

ZEST_SUITE(QueryFreshness, Tester) {

kota::event_loop loop;
FileTable files;
Project project{files};
SessionStore store;
WorkerPool pool{loop};
CommandResolver resolver{project};
TaskGraph graph;
PCMFamily pcm{graph, project, resolver, pool};
ASTProjectionTable projections;
IndexStore index_store{loop, project, resolver};
TURunFamily turun{graph, project, resolver, pcm, index_store, pool};
IndexPump indexer{loop, project, turun, index_store, pool};
PCHFamily pch{graph, project, pool};
ServerLiveSources live{project, pch, store, projections};
index::FreshnessGate gate{project.file_table};
index::IndexQuery index_query{project.project_index, project.file_table, &gate, &live};
index::IndexQuery disk_query{project.project_index, project.file_table, &gate, nullptr};

Fid main_id;
Fid header_id;

/// Build an envelope from the added sources and merge it into the
/// workspace, installing each section's blob verbatim as the file's shard.
void merge_into_workspace() {
    auto wire = index::build_tu_index(*unit);
    auto view = index::TUIndex::from_bytes(wire);
    ZASSERT(view.loaded());

    llvm::SmallVector<Fid> file_ids_map;
    for(std::uint32_t i = 0; i < view.path_count(); i += 1) {
        file_ids_map.push_back(project.file_table.intern(Spelling::absolute(view.path(i))));
    }
    ZASSERT(project.project_index.merge(view, file_ids_map));
    main_id = file_ids_map[view.path_count() - 1];

    for(std::uint32_t section = 0; section < view.section_count(); section += 1) {
        auto local_id = view.section_path(section);
        project.project_index.shards[file_ids_map[local_id]] = index::Shard::from_buffer(
            llvm::MemoryBuffer::getMemBufferCopy(view.section_blob(section)));
        if(llvm::sys::path::filename(view.path(local_id)) == "header.h") {
            header_id = file_ids_map[local_id];
        }
    }
}

/// The symbol hash at an offset in a file's merged shard.
index::SymbolHash symbol_at(Fid path_id, std::uint32_t offset) {
    index::SymbolHash result = 0;
    project.project_index.shards[path_id].lookup(offset, [&](const index::Occurrence& o) {
        result = o.target;
        return false;
    });
    return result;
}

/// Files contributing reference rows for a symbol, by basename.
std::vector<std::string> reference_files(index::SymbolHash hash) {
    std::vector<std::string> files;
    for(auto& site: disk_query.sites(hash, Fid{}, RelationKind::Reference)) {
        files.push_back(llvm::sys::path::filename(site.path).str());
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

ZEST_CASE(GateSplitsRows) {
    add_file("header.h", R"(
        int helper() { return 1; }
    )");
    add_main("main.cpp", R"(
        #include "header.h"
        int main() {
            return §(use)helper();
        }
    )");
    ZASSERT(compile());
    merge_into_workspace();

    auto hash = symbol_at(main_id, point("use"));
    ZASSERT(hash != 0UL);

    // Baseline: the main TU contributes its reference row, and the
    // definition resolves into the header shard.
    ZASSERT(std::ranges::contains(reference_files(hash), "main.cpp"));
    ZASSERT(index_query.first_site(hash, Fid{}, RelationKind::Definition));

    // Awaiting a reindex for a dependency change only: the disk still
    // holds the text the rows indexed, so they keep serving.
    indexer.enqueue(main_id, ReindexReason::DepsOnly);
    ZASSERT(std::ranges::contains(reference_files(hash), "main.cpp"));

    // Line-based resolution in the file works while its rows are current.
    index::SymbolQuery by_line;
    by_line.position = {.path = project.file_table.resolve(main_id).str(), .line = 3};
    ZASSERT(!disk_query.locate(by_line).empty());

    // The disk was seen holding other text: the file's contribution is
    // skipped until its rows describe the disk again; other files' rows
    // are unaffected.
    project.file_table.observe(main_id, DiskObservation{.hash = 1});
    ZASSERT(!std::ranges::contains(reference_files(hash), "main.cpp"));
    ZASSERT(index_query.first_site(hash, Fid{}, RelationKind::Definition));

    // Cursor-style resolution against the stale rows is unresolvable: the
    // line numbers describe text that no longer exists.
    ZASSERT(disk_query.locate(by_line).empty());

    // A changed definition file drops out of definition lookups.
    project.file_table.observe(header_id, DiskObservation{.hash = 1});
    ZASSERT(!index_query.first_site(hash, Fid{}, RelationKind::Definition).has_value());

    // With background indexing disabled nothing would ever catch up:
    // last-known rows keep serving instead of leaving a permanent hole.
    gate.options.withhold = false;
    ZASSERT(std::ranges::contains(reference_files(hash), "main.cpp"));
}

ZEST_CASE(DeletedFileWithdrawsRows) {
    add_main("main.cpp", R"(
        int helper() { return 1; }
        int use() { return §(use)helper(); }
    )");
    ZASSERT(compile());
    merge_into_workspace();
    auto hash = symbol_at(main_id, point("use"));
    ZASSERT(hash != 0UL);
    ZASSERT(std::ranges::contains(reference_files(hash), "main.cpp"));

    // The rows of a file seen gone point at text that is gone with it.
    project.file_table.saw_missing(main_id);
    ZASSERT(!std::ranges::contains(reference_files(hash), "main.cpp"));
}

};  // ZEST_SUITE(QueryFreshness)

}  // namespace
}  // namespace clice::testing
