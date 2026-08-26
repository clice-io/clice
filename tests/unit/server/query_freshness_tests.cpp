#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "test/test.h"
#include "test/tester.h"
#include "index/shard.h"
#include "index/tu_index.h"
#include "sched/context.h"
#include "sched/families/pcm.h"
#include "sched/families/turun.h"
#include "sched/graph.h"
#include "sched/index/pump.h"
#include "sched/index/store.h"
#include "server/service/query.h"
#include "server/state/ast_projection.h"
#include "server/state/session_store.h"
#include "worker/pool.h"

#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/xxhash.h"

namespace clice::testing {
namespace {

TEST_SUITE(QueryFreshness, Tester) {

kota::event_loop loop;
Workspace workspace;
SessionStore store;
WorkerPool pool{loop};
ContextResolver resolver{workspace};
TaskGraph graph{loop};
PCMFamily pcm{graph, workspace, resolver, pool};
ASTProjectionTable projections;
IndexStore index_store{loop, workspace};
TURunFamily turun{graph, workspace, resolver, pcm, index_store, pool};
IndexPump indexer{loop, workspace, turun, index_store, pool};
IndexQuery index_query{workspace, store, indexer, projections};
IndexQuery agent_query{workspace, store, indexer, projections, {.disk_only = true}};

std::uint32_t main_id = 0;
std::uint32_t header_id = 0;

/// Build an envelope from the added sources and merge it into the
/// workspace, installing each section's blob verbatim as the file's shard.
void merge_into_workspace() {
    auto wire = index::build_tu_index(*unit);
    auto view = index::TUIndex::from_bytes(wire);
    ASSERT_TRUE(view.loaded());

    llvm::SmallVector<std::uint32_t> file_ids_map;
    for(std::uint32_t i = 0; i < view.path_count(); i += 1) {
        file_ids_map.push_back(workspace.path_pool.intern(view.path(i)));
    }
    ASSERT_TRUE(workspace.project_index.merge(view, file_ids_map));
    main_id = file_ids_map[view.path_count() - 1];

    for(std::uint32_t section = 0; section < view.section_count(); section += 1) {
        auto local_id = view.section_path(section);
        workspace.shards[file_ids_map[local_id]] = index::Shard::from_buffer(
            llvm::MemoryBuffer::getMemBufferCopy(view.section_blob(section)));
        if(llvm::sys::path::filename(view.path(local_id)) == "header.h") {
            header_id = file_ids_map[local_id];
        }
    }
}

/// The symbol hash at an offset in a file's merged shard.
index::SymbolHash symbol_at(std::uint32_t path_id, std::uint32_t offset) {
    index::SymbolHash result = 0;
    workspace.shards[path_id].lookup(offset, [&](const index::Occurrence& o) {
        result = o.target;
        return false;
    });
    return result;
}

/// Files contributing reference rows for a symbol, by basename.
std::vector<std::string> reference_files(index::SymbolHash hash) {
    std::vector<std::string> files;
    for(auto& ref: agent_query.collect_references(hash, RelationKind::Reference)) {
        files.push_back(llvm::sys::path::filename(ref.file).str());
    }
    return files;
}

TEST_CASE(PendingReasonUpgrade) {
    auto file = workspace.path_pool.intern("/proj/upgrade.cpp");
    ASSERT_FALSE(indexer.pending_reason(file).has_value());

    indexer.enqueue(file, ReindexReason::DepsOnly);
    ASSERT_TRUE(indexer.pending_reason(file) == ReindexReason::DepsOnly);
    ASSERT_EQ(indexer.pending_files(), 1u);

    // ContentChanged absorbs a queued DepsOnly without a second queue entry.
    indexer.enqueue(file, ReindexReason::ContentChanged);
    ASSERT_TRUE(indexer.pending_reason(file) == ReindexReason::ContentChanged);
    ASSERT_EQ(indexer.pending_files(), 1u);

    // A later deps-only cascade never downgrades it.
    indexer.enqueue(file, ReindexReason::DepsOnly);
    ASSERT_TRUE(indexer.pending_reason(file) == ReindexReason::ContentChanged);
}

TEST_CASE(PendingGateSplitsRows) {
    workspace.config.project.enable_indexing = true;

    add_file("header.h", R"(
        int helper() { return 1; }
    )");
    add_main("main.cpp", R"(
        #include "header.h"
        int main() {
            return §(use)helper();
        }
    )");
    ASSERT_TRUE(compile());
    merge_into_workspace();

    auto hash = symbol_at(main_id, point("use"));
    ASSERT_NE(hash, 0UL);

    // Baseline: the main TU contributes its reference row, and the
    // definition resolves into the header shard.
    ASSERT_TRUE(std::ranges::contains(reference_files(hash), "main.cpp"));
    ASSERT_TRUE(index_query.find_definition_location(hash).has_value());

    // Pending for a dependency change only: the previous rows keep serving.
    indexer.enqueue(main_id, ReindexReason::DepsOnly);
    ASSERT_TRUE(std::ranges::contains(reference_files(hash), "main.cpp"));

    // Line-based resolution in the file works while its rows are current.
    agentic::ReadSymbolParams by_line;
    by_line.path = std::string(workspace.path_pool.resolve(main_id));
    by_line.line = 3;
    ASSERT_FALSE(agent_query.locate_symbols(by_line).empty());

    // The file's own content changed: its contribution is skipped until the
    // reindex lands; other files' rows are unaffected.
    indexer.enqueue(main_id, ReindexReason::ContentChanged);
    ASSERT_FALSE(std::ranges::contains(reference_files(hash), "main.cpp"));
    ASSERT_TRUE(index_query.find_definition_location(hash).has_value());

    // Cursor-style resolution against the stale rows is unresolvable: the
    // line numbers describe text that no longer exists.
    ASSERT_TRUE(agent_query.locate_symbols(by_line).empty());

    // A content-changed definition file drops out of definition lookups.
    indexer.enqueue(header_id, ReindexReason::ContentChanged);
    ASSERT_FALSE(index_query.find_definition_location(hash).has_value());

    // With background indexing disabled nothing would ever catch up:
    // last-known rows keep serving instead of leaving a permanent hole.
    workspace.config.project.enable_indexing = false;
    ASSERT_TRUE(std::ranges::contains(reference_files(hash), "main.cpp"));
}

};  // TEST_SUITE(QueryFreshness)

}  // namespace
}  // namespace clice::testing
