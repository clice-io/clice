#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "test/test.h"
#include "test/tester.h"
#include "index/shard.h"
#include "index/tu_index.h"
#include "server/compiler/context_resolver.h"
#include "server/compiler/indexer.h"
#include "server/service/query.h"
#include "server/state/session_store.h"
#include "server/worker/worker_pool.h"

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
Indexer indexer{loop, workspace, pool, resolver, store};
IndexQuery index_query{workspace, store, indexer};
IndexQuery agent_query{workspace, store, indexer, {.disk_only = true}};

std::uint32_t main_id = 0;
std::uint32_t header_id = 0;

/// Build a TUIndex from the added sources and merge it into the workspace
/// with real contents, so shards can map their rows to positions.
void merge_into_workspace() {
    auto tu_index = index::TUIndex::build(*unit);
    std::string wire;
    llvm::raw_string_ostream wos(wire);
    tu_index.serialize(wos);
    auto view = index::TUIndexView::from(wire);
    ASSERT_TRUE(view.has_value());

    llvm::SmallVector<std::uint32_t> file_ids_map;
    for(std::uint32_t i = 0; i < view->path_count(); i += 1) {
        file_ids_map.push_back(workspace.path_pool.intern(view->path(i)));
    }
    ASSERT_TRUE(workspace.project_index.merge(*view, file_ids_map));
    main_id = file_ids_map[view->path_count() - 1];

    auto content_of = [&](llvm::StringRef path) -> llvm::StringRef {
        auto it = sources.all_files.find(llvm::sys::path::filename(path));
        return it != sources.all_files.end() ? llvm::StringRef(it->second.content)
                                             : llvm::StringRef();
    };
    auto lookup_symbol = [&](index::SymbolHash hash) {
        return view->find_symbol(hash);
    };

    for(std::uint32_t section = 0; section < view->section_count(); section += 1) {
        auto local_id = view->section_path(section);
        auto rows = view->decode_section_rows(section);
        ASSERT_TRUE(rows.has_value());
        auto content = content_of(view->path(local_id));
        index::VariantInput fresh{view->section_rows_hash(section), &*rows, lookup_symbol};
        std::string bytes;
        llvm::raw_string_ostream os(bytes);
        index::write_shard(index::Shard(), {}, fresh, content, llvm::xxh3_64bits(content), os);
        workspace.shards[file_ids_map[local_id]] =
            index::Shard::from_buffer(llvm::MemoryBuffer::getMemBufferCopy(bytes));
        if(llvm::sys::path::filename(view->path(local_id)) == "header.h") {
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
