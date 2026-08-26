#include <string>
#include <vector>

#include "test/test.h"
#include "test/tester.h"
#include "feature/feature.h"
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
TaskGraph graph{loop};
PCMFamily pcm{graph, workspace, resolver, pool};
ASTProjectionTable projections;
IndexStore index_store{loop, workspace};
TURunFamily turun{graph, workspace, resolver, pcm, index_store, pool};
IndexPump indexer{loop, workspace, turun, index_store, pool};
clice::IndexQuery query{workspace, store, indexer, projections};

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

    // The consumed-content hash per TU-local path: the section's own
    // record where rows exist, the wire's hash otherwise — mirroring the
    // indexer, so FileVersions match the shard generations they pin.
    llvm::SmallVector<std::uint64_t> consumed(view.path_count(), 0);
    for(std::uint32_t section = 0; section < view.section_count(); section += 1) {
        auto local_id = view.section_path(section);
        auto global_id = file_ids_map[local_id];
        // A section blob is already the final shard encoding: install the
        // bytes verbatim, as the indexer's first-variant path does.
        workspace.shards[global_id] = index::Shard::from_buffer(
            llvm::MemoryBuffer::getMemBufferCopy(view.section_blob(section)));
        consumed[local_id] = workspace.shards[global_id].content_hash();
        if(llvm::sys::path::filename(view.path(local_id)) == "header.h") {
            header_id = global_id;
        }
    }

    llvm::SmallVector<std::uint32_t> fv_of;
    for(std::uint32_t i = 0; i < view.path_count(); i += 1) {
        auto hash = consumed[i] != 0 ? consumed[i] : view.path_hash(i);
        fv_of.push_back(project.intern_file_version(file_ids_map[i], hash));
    }

    index::TUManifest manifest;
    manifest.tu_fv = fv_of[view.path_count() - 1];
    for(std::uint32_t i = 0; i < view.location_count(); i += 1) {
        auto location = view.location(i);
        manifest.nodes.push_back({fv_of[location.path_id], location.include, location.line});
    }
    for(std::uint32_t section = 0; section < view.section_count(); section += 1) {
        manifest.contributions.emplace_back(fv_of[view.section_path(section)],
                                            view.section_hash(section));
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

TEST_CASE(OpenSessionServedByShard) {
    add_file("header.h", R"(
        struct §(def)⟦§(def)Widget⟧ { int value; };
    )");
    add_main("main.cpp", R"(
        #include "header.h"
        §(use)⟦§(use)Widget⟧ instance;
    )");
    ASSERT_TRUE(compile());
    merge_into_workspace();

    // Open the document with exactly the indexed content and never
    // compile it: freshness clause 4 serves it from its shard.
    auto session = store.open(main_id);
    store.apply_open(*session, unit->interested_content().str(), 1);
    ASSERT_FALSE(projections.index_current(session->path_id));

    auto position = feature::to_position(session->line_map(), point("use"));
    ASSERT_TRUE(position.has_value());
    auto locations = query.query_definition(main_path(), *position, session.get());
    ASSERT_FALSE(locations.empty());
    ASSERT_TRUE(llvm::StringRef(locations.front().uri).ends_with("header.h"));
}

TEST_CASE(DivergedBufferWithdrawsShard) {
    add_main("main.cpp", R"(
        int stale_fn() { return 1; }
        int use() { return §(use)⟦§(use)stale_fn⟧(); }
    )");
    ASSERT_TRUE(compile());
    merge_into_workspace();

    auto session = store.open(main_id);
    auto edited = unit->interested_content().str() + "// edited\n";
    store.apply_open(*session, edited, 1);

    // The buffer no longer matches the rows' content: the shard withdraws
    // and the un-compiled session resolves nothing.
    auto position = feature::to_position(session->line_map(), point("use"));
    ASSERT_TRUE(position.has_value());
    ASSERT_TRUE(query.query_definition(main_path(), *position, session.get()).empty());
}

TEST_CASE(HeaderEdgesFromHostManifest) {
    add_file("inner.h", R"(
        int inner_value();
    )");
    add_file("header.h", R"(
        #include "inner.h"
        struct Widget { int value; };
    )");
    add_main("main.cpp", R"(
        #include "header.h"
        Widget instance;
    )");
    ASSERT_TRUE(compile());
    merge_into_workspace();

    // The header has no manifest of its own; its directive is a node of
    // the host TU's manifest hanging off the header's node.
    auto session = store.open(header_id);
    store.apply_open(*session, sources.all_files.lookup("header.h").content, 1);
    auto edges = query.include_edges(*session);
    ASSERT_EQ(edges.size(), std::size_t(1));
    ASSERT_TRUE(llvm::StringRef(edges[0].target).ends_with("inner.h"));

    // The TU's own manifest still answers for the TU itself.
    auto main_session = store.open(main_id);
    store.apply_open(*main_session, unit->interested_content().str(), 1);
    auto main_edges = query.include_edges(*main_session);
    ASSERT_EQ(main_edges.size(), std::size_t(1));
    ASSERT_TRUE(llvm::StringRef(main_edges[0].target).ends_with("header.h"));
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
