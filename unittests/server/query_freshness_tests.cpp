module;

#include "modules/prelude.h"

module clice;

import :index.indexer;
import :index.query;
import :index.shard;
import :index.tu_index;
import :project.command_resolver;
import :project.index_store;
import :sched.families.pch;
import :sched.families.pcm;
import :sched.families.turun;
import :sched.graph;
import :sched.index.pump;
import :server.ast_projection;
import :server.live_sources;
import :server.session_store;
import :unittests.utils.tester;
import :unittests.utils;
import :worker.pool;

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
PCHFamily pch{graph, project, pool};
TURunFamily turun{graph, project, resolver, pcm, pch, index_store, pool};
IndexPump indexer{loop, project, turun, index_store, pool};
ServerLiveSources live{project, pch, store, projections};
index::FreshnessGate gate{project.file_table};
index::IndexQuery index_query{project.project_index, project.file_table, &gate, &live};
index::IndexQuery disk_query{project.project_index, project.file_table, &gate, nullptr};

Fid focus_id;
Fid header_id;

/// Build a TU index from the added sources and merge it into the
/// workspace, installing each section's blob verbatim as the file's shard.
void merge_into_workspace() {
    auto bytes = index::build_tu_index(*unit);
    auto view = index::TUIndex::from_bytes(bytes);
    ZASSERT(view.loaded());

    llvm::SmallVector<Fid> file_ids_map;
    for(std::uint32_t i = 0; i < view.path_count(); i += 1) {
        file_ids_map.push_back(project.file_table.intern(Spelling::absolute(view.path(i))));
    }
    project.project_index.merge(view, file_ids_map);
    focus_id = file_ids_map[view.path_count() - 1];

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

/// Files contributing reference relations for a symbol, by basename.
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
    ZASSERT(std::ranges::contains(reference_files(hash), "main.cpp"));
    ZASSERT(index_query.first_site(hash, Fid{}, RelationKind::Definition));

    // Awaiting a reindex for a dependency change only: the disk still
    // holds the text its shard indexed, so the shard keeps serving.
    indexer.enqueue(focus_id, ReindexReason::DepsOnly);
    ZASSERT(std::ranges::contains(reference_files(hash), "main.cpp"));

    // Line-based resolution in the file works while its index is current.
    index::SymbolQuery by_line;
    by_line.position = {.path = project.file_table.resolve(focus_id).str(), .line = 3};
    ZASSERT(!disk_query.locate(by_line).empty());

    // The disk was seen holding other text: the file's contribution is
    // skipped until its shard describes the disk again; other files' shards
    // are unaffected.
    project.file_table.disk.observe(focus_id, DiskObservation{.hash = 1});
    ZASSERT(!std::ranges::contains(reference_files(hash), "main.cpp"));
    ZASSERT(index_query.first_site(hash, Fid{}, RelationKind::Definition));

    // Cursor-style resolution against the stale index is unresolvable: the
    // line numbers describe text that no longer exists.
    ZASSERT(disk_query.locate(by_line).empty());

    // A changed definition file drops out of definition lookups.
    project.file_table.disk.observe(header_id, DiskObservation{.hash = 1});
    ZASSERT(!index_query.first_site(hash, Fid{}, RelationKind::Definition).has_value());

    // With background indexing disabled nothing would ever catch up:
    // the last-known index keeps serving instead of leaving a permanent hole.
    gate.options.withhold = false;
    ZASSERT(std::ranges::contains(reference_files(hash), "main.cpp"));
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
    ZASSERT(std::ranges::contains(reference_files(hash), "main.cpp"));

    // The index of a file seen gone points at text that is gone with it.
    project.file_table.disk.saw_missing(focus_id);
    ZASSERT(!std::ranges::contains(reference_files(hash), "main.cpp"));
}

};  // ZEST_SUITE(QueryFreshness)

}  // namespace
}  // namespace clice::testing
