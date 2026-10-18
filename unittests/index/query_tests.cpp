module;

#include "modules/prelude.h"

module clice;

import :index.navigation;
import :index.symbol;
import :semantic.symbol;
import :unittests.utils.index_query_tester;
import :vfs.file_system;
import :vfs.ids;

namespace clice::testing {
namespace {

ZEST_SUITE(IndexQuery, IndexQueryTester) {

ZEST_CASE(DefinitionAcrossFiles) {
    add_file("header.h", R"(
        struct §(def)⟦§(def)Widget⟧ { int value; };
    )");
    add_focus("main.cpp", R"(
        #include "header.h"
        §(use)⟦§(use)Widget⟧ instance;
    )");
    index_workspace();

    auto hit_offset = point("use");
    index::SymbolHash symbol = 0;
    project.index.shard(focus_id)->occurrences_at(hit_offset, [&](const index::Occurrence& o) {
        symbol = o.target;
        return false;
    });
    ZASSERT(symbol != 0);

    auto site = query.first_site(symbol, Fid{}, RelationKind::Definition);
    ZASSERT(site);
    ZASSERT(site->path.ends_with("header.h"));
}

ZEST_CASE(ReferencesAcrossFiles) {
    add_file("header.h", R"(
        int shared_fn();
    )");
    add_focus("main.cpp", R"(
        #include "header.h"
        int call() { return §(use)⟦§(use)shared_fn⟧(); }
    )");
    index_workspace();

    index::SymbolHash symbol = 0;
    project.index.shard(focus_id)->occurrences_at(point("use"), [&](const index::Occurrence& o) {
        symbol = o.target;
        return false;
    });
    ZASSERT(symbol != 0);

    auto references = query.sites(symbol, Fid{}, RelationKind::Reference);
    ZASSERT(!references.empty());
}

ZEST_CASE(LocalSymbolName) {
    add_focus("main.cpp", R"(
        static int §(local)⟦§(local)hidden⟧() { return 1; }
        int use() { return hidden(); }
    )");
    index_workspace();

    index::SymbolHash symbol = 0;
    project.index.shard(focus_id)->occurrences_at(point("local"), [&](const index::Occurrence& o) {
        symbol = o.target;
        return false;
    });
    ZASSERT(symbol != 0);

    // TU-local names are not in the project table; the query falls back to
    // the shard's own local-name table.
    auto info = query.symbol_info(symbol);
    ZASSERT(info);
    ZASSERT(info->name == "hidden");
}

ZEST_CASE(OpenSessionServedByShard) {
    add_file("header.h", R"(
        struct §(def)⟦§(def)Widget⟧ { int value; };
    )");
    add_focus("main.cpp", R"(
        #include "header.h"
        §(use)⟦§(use)Widget⟧ instance;
    )");
    index_workspace();

    // Open the document with exactly the indexed content and never
    // compile it: freshness clause 4 serves it from its shard.
    auto session = store.open(focus_id);
    store.apply_open(*session, unit->focus_file().content().str(), 1);
    ZASSERT(!projections.index_current(session->path_id));

    auto cursor = query.symbol_at(focus_id, point("use"));
    ZASSERT(cursor);
    auto sites = index::definition(query, *cursor);
    ZASSERT(!sites.empty());
    ZASSERT(sites.front().path.ends_with("header.h"));
}

ZEST_CASE(DivergedBufferWithdrawsShard) {
    add_focus("main.cpp", R"(
        int stale_fn() { return 1; }
        int use() { return §(use)⟦§(use)stale_fn⟧(); }
    )");
    index_workspace();

    auto session = store.open(focus_id);
    auto edited = unit->focus_file().content().str() + "// edited\n";
    store.apply_open(*session, edited, 1);

    // The buffer no longer matches the shard's content: the shard withdraws
    // and the un-compiled session resolves nothing.
    ZASSERT(!query.serving(focus_id).has_value());
    ZASSERT(!query.symbol_at(focus_id, point("use")).has_value());
}

ZEST_CASE(HeaderEdgesFromHostManifest) {
    add_file("inner.h", R"(
        int inner_value();
    )");
    add_file("header.h", R"(
        #include "inner.h"
        struct Widget { int value; };
    )");
    add_focus("main.cpp", R"(
        #include "header.h"
        Widget instance;
    )");
    index_workspace();

    // The header has no manifest of its own; its directive is a node of
    // the host TU's manifest hanging off the header's node.
    auto session = store.open(header_id);
    store.apply_open(*session, sources.all_files.lookup("header.h").content, 1);
    auto edges = query.project().include_edges(session->path_id);
    ZASSERT(edges.size() == std::size_t(1));
    ZASSERT(llvm::StringRef(edges[0].target).ends_with("inner.h"));

    // The TU's own manifest still answers for the TU itself.
    auto focus_session = store.open(focus_id);
    store.apply_open(*focus_session, unit->focus_file().content().str(), 1);
    auto focus_edges = query.project().include_edges(focus_session->path_id);
    ZASSERT(focus_edges.size() == std::size_t(1));
    ZASSERT(llvm::StringRef(focus_edges[0].target).ends_with("header.h"));
}

ZEST_CASE(StaleContributionSuppressed) {
    add_focus("main.cpp", R"(
        int stale_fn() { return 1; }
        int use() { return §(use)⟦§(use)stale_fn⟧(); }
    )");
    index_workspace();

    index::SymbolHash symbol = 0;
    project.index.shard(focus_id)->occurrences_at(point("use"), [&](const index::Occurrence& o) {
        symbol = o.target;
        return false;
    });
    ZASSERT(!query.sites(symbol, Fid{}, RelationKind::Reference).empty());

    // Entries of text the disk no longer holds point nowhere: the file's
    // contribution disappears from cross-file results until its index
    // describes the disk again.
    project.file_table.disk.observe(focus_id, DiskObservation{.hash = 1});
    ZASSERT(query.sites(symbol, Fid{}, RelationKind::Reference).empty());
}

ZEST_CASE(UndefinedBesideStaleUse) {
    add_file("header.h", R"(
        int external();
    )");
    add_focus("main.cpp", R"(
        #include "header.h"
        int use() { return external(); }
    )");
    index_workspace();

    // A file using the symbol moved on; nothing defines the symbol, so the
    // declaration still places it.
    project.file_table.disk.observe(focus_id, DiskObservation{.hash = 1});
    auto results = search("external");
    ZASSERT(results.size() == 1U);
    ZASSERT(results.front().site.path.ends_with("header.h"));
}

ZEST_CASE(DeletedDefinitionFallsBack) {
    llvm::StringRef header = R"(
        int removed();
    )";
    add_file("header.h", header);
    add_focus("main.cpp", R"(
        #include "header.h"
        int removed() { return 0; }
    )");
    index_workspace();

    clear();
    add_file("header.h", header);
    add_focus("use.cpp", R"(
        #include "header.h"
        int use() { return removed(); }
    )");
    index_workspace();
    auto use_id = focus_id;

    // The table keeps the definition the first unit reported; the shards
    // no longer hold it, so the declaration places the symbol — a file
    // that only used it moving on changes nothing.
    clear();
    add_file("header.h", header);
    add_focus("main.cpp", R"(
        #include "header.h"
        int kept() { return removed(); }
    )");
    index_workspace();
    project.file_table.disk.observe(use_id, DiskObservation{.hash = 1});

    auto results = search("removed");
    ZASSERT(results.size() == 1U);
    ZASSERT(results.front().site.path.ends_with("header.h"));
}

};  // ZEST_SUITE(IndexQuery)

}  // namespace
}  // namespace clice::testing
