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
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/xxhash.h"

namespace clice::testing {
namespace {

TEST_SUITE(IndexQuery, Tester) {

kota::event_loop loop;
Workspace workspace;
SessionStore store;
WorkerPool pool{loop};
ContextResolver resolver{workspace};
Indexer indexer{loop, workspace, pool, resolver, store};
clice::IndexQuery query{workspace, store, indexer};

std::uint32_t main_id = 0;
std::uint32_t header_id = 0;

/// Mirror of the indexer's merge over in-memory sources: project symbols,
/// per-section shard blobs, and the TU manifest with its contributions —
/// so live-variant masks and staleness gates behave as in production.
void merge_into_workspace() {
    auto wire = index::build_tu_index(*unit);
    auto view = index::TUIndex::from_bytes(wire);
    ASSERT_TRUE(view.loaded());

    auto& project = workspace.project_index;
    llvm::SmallVector<std::uint32_t> file_ids_map;
    for(std::uint32_t i = 0; i < view.path_count(); i += 1) {
        file_ids_map.push_back(workspace.path_pool.intern(view.path(i)));
    }
    ASSERT_TRUE(project.merge(view, file_ids_map));
    main_id = file_ids_map[view.path_count() - 1];

    index::TUManifest manifest;
    manifest.tu_fv = project.intern_file_version(main_id, view.path_hash(view.path_count() - 1));

    for(std::uint32_t section = 0; section < view.section_count(); section += 1) {
        auto local_id = view.section_path(section);
        auto global_id = file_ids_map[local_id];
        // A section blob is already the final shard encoding: install the
        // bytes verbatim, as the indexer's first-variant path does.
        workspace.shards[global_id] = index::Shard::from_buffer(
            llvm::MemoryBuffer::getMemBufferCopy(view.section_blob(section)));

        auto fv = project.intern_file_version(global_id, view.path_hash(local_id));
        manifest.contributions.emplace_back(fv, view.section_hash(section));
        if(llvm::sys::path::filename(view.path(local_id)) == "header.h") {
            header_id = global_id;
        }
    }

    for(auto path_id: project.apply_manifest(main_id, std::move(manifest))) {
        auto it = workspace.shards.find(path_id);
        if(it != workspace.shards.end()) {
            it->second.set_live(project.live_variants(path_id));
        }
    }
}

std::string main_path() {
    return std::string(workspace.path_pool.resolve(main_id));
}

TEST_CASE(DefinitionAcrossFiles) {
    add_file("header.h", R"(
        struct §(def)⟦§(def)Widget⟧ { int value; };
    )");
    add_main("main.cpp", R"(
        #include "header.h"
        §(use)⟦§(use)Widget⟧ instance;
    )");
    ASSERT_TRUE(compile());
    merge_into_workspace();

    auto hit_offset = point("use");
    index::SymbolHash symbol = 0;
    workspace.shards[main_id].lookup(hit_offset, [&](const index::Occurrence& o) {
        symbol = o.target;
        return false;
    });
    ASSERT_TRUE(symbol != 0);

    auto location = query.find_definition_location(symbol);
    ASSERT_TRUE(location.has_value());
    ASSERT_TRUE(llvm::StringRef(location->uri).ends_with("header.h"));
}

TEST_CASE(ReferencesAcrossFiles) {
    add_file("header.h", R"(
        int shared_fn();
    )");
    add_main("main.cpp", R"(
        #include "header.h"
        int call() { return §(use)⟦§(use)shared_fn⟧(); }
    )");
    ASSERT_TRUE(compile());
    merge_into_workspace();

    index::SymbolHash symbol = 0;
    workspace.shards[main_id].lookup(point("use"), [&](const index::Occurrence& o) {
        symbol = o.target;
        return false;
    });
    ASSERT_TRUE(symbol != 0);

    auto references = query.collect_references(symbol, RelationKind::Reference);
    ASSERT_FALSE(references.empty());
}

TEST_CASE(SearchSymbols) {
    add_main("main.cpp", R"(
        struct Searchable { int field; };
        Searchable instance;
    )");
    ASSERT_TRUE(compile());
    merge_into_workspace();

    auto results = query.search_symbols("Searchable", 10);
    ASSERT_FALSE(results.empty());
    ASSERT_EQ(results.front().name, "Searchable");
}

TEST_CASE(LocalSymbolName) {
    add_main("main.cpp", R"(
        static int §(local)⟦§(local)hidden⟧() { return 1; }
        int use() { return hidden(); }
    )");
    ASSERT_TRUE(compile());
    merge_into_workspace();

    index::SymbolHash symbol = 0;
    workspace.shards[main_id].lookup(point("local"), [&](const index::Occurrence& o) {
        symbol = o.target;
        return false;
    });
    ASSERT_TRUE(symbol != 0);

    // TU-local names are not in the project table; the query falls back to
    // the shard's own local-name table.
    std::string name;
    SymbolKind kind;
    ASSERT_TRUE(query.find_symbol_info(symbol, name, kind));
    ASSERT_EQ(name, "hidden");
}

TEST_CASE(StaleContributionSuppressed) {
    add_main("main.cpp", R"(
        int stale_fn() { return 1; }
        int use() { return §(use)⟦§(use)stale_fn⟧(); }
    )");
    ASSERT_TRUE(compile());
    merge_into_workspace();

    index::SymbolHash symbol = 0;
    workspace.shards[main_id].lookup(point("use"), [&](const index::Occurrence& o) {
        symbol = o.target;
        return false;
    });
    ASSERT_FALSE(query.collect_references(symbol, RelationKind::Reference).empty());

    // A content-changed pending file's rows describe text that no longer
    // exists: its contribution disappears from cross-file results until
    // the reindex lands.
    indexer.enqueue(main_id, ReindexReason::ContentChanged);
    ASSERT_TRUE(query.collect_references(symbol, RelationKind::Reference).empty());
}

};  // TEST_SUITE(IndexQuery)

}  // namespace
}  // namespace clice::testing
