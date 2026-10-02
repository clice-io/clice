#include <string>
#include <vector>

#include "test/test.h"
#include "test/tester.h"
#include "feature/feature.h"
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
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/xxhash.h"

namespace clice::testing {
namespace {

ZEST_SUITE(IndexQuery, Tester) {

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
index::IndexQuery query{project.project_index, project.file_table, &gate, &live};

Fid main_id;
Fid header_id;

std::vector<index::IndexQuery::Located> search(llvm::StringRef text, std::size_t limit = 10) {
    return query.search(*index::SymbolQuery::parse(text), limit);
}

std::vector<index::IndexQuery::Located> locate(llvm::StringRef text) {
    return query.locate(*index::SymbolQuery::parse(text));
}

/// Mirror of the indexer's merge over in-memory sources: project symbols,
/// per-section shard blobs, and the TU manifest with its contributions —
/// so live-variant masks and staleness gates behave as in production.
void merge_into_workspace() {
    auto wire = index::build_tu_index(*unit);
    auto view = index::TUIndex::from_bytes(wire);
    ASSERT(view.loaded());

    auto& project_index = project.project_index;
    llvm::SmallVector<Fid> file_ids_map;
    for(std::uint32_t i = 0; i < view.path_count(); i += 1) {
        file_ids_map.push_back(project.file_table.intern(Spelling::absolute(view.path(i))));
    }
    llvm::SmallVector<index::SymbolHash> added;
    ASSERT(project_index.merge(view, file_ids_map, &added));
    project.project_index.search_pending.insert(added.begin(), added.end());
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
        project.project_index.shards[global_id] = index::Shard::from_buffer(
            llvm::MemoryBuffer::getMemBufferCopy(view.section_blob(section)));
        consumed[local_id] = project.project_index.shards[global_id].content_hash();
        if(llvm::sys::path::filename(view.path(local_id)) == "header.h") {
            header_id = global_id;
        }
    }

    llvm::SmallVector<VersionID> fv_of;
    for(std::uint32_t i = 0; i < view.path_count(); i += 1) {
        auto hash = consumed[i] != 0 ? consumed[i] : view.path_hash(i);
        fv_of.push_back(project.file_table.intern_version(file_ids_map[i], hash));
    }

    index::TUManifest manifest;
    manifest.tu_fv = fv_of[view.path_count() - 1];
    for(std::uint32_t i = 0; i < view.node_count(); i += 1) {
        auto node = view.node(i);
        manifest.nodes.push_back({.file = fv_of[node.file].raw,
                                  .parent = node.parent,
                                  .line = node.line,
                                  .skipped = node.skipped});
    }
    llvm::SmallVector<std::uint32_t> contribution_paths;
    for(std::uint32_t section = 0; section < view.section_count(); section += 1) {
        manifest.contributions.emplace_back(fv_of[view.section_path(section)],
                                            view.section_hash(section));
        contribution_paths.push_back(view.section_path(section));
    }
    auto local_fanout = view.local_fanout(contribution_paths);
    ASSERT(local_fanout);
    manifest.local_fanout = std::move(*local_fanout);

    for(auto path_id:
        project_index.apply_manifest(project.file_table, main_id, std::move(manifest))) {
        auto it = project.project_index.shards.find(path_id);
        if(it != project.project_index.shards.end()) {
            it->second.set_live(project_index.live_variants(path_id));
        }
    }
}

ZEST_CASE(DefinitionAcrossFiles) {
    add_file("header.h", R"(
        struct §(def)⟦§(def)Widget⟧ { int value; };
    )");
    add_main("main.cpp", R"(
        #include "header.h"
        §(use)⟦§(use)Widget⟧ instance;
    )");
    ASSERT(compile());
    merge_into_workspace();

    auto hit_offset = point("use");
    index::SymbolHash symbol = 0;
    project.project_index.shards[main_id].lookup(hit_offset, [&](const index::Occurrence& o) {
        symbol = o.target;
        return false;
    });
    ASSERT(symbol != 0);

    auto site = query.first_site(symbol, Fid{}, RelationKind::Definition);
    ASSERT(site);
    ASSERT(site->path.ends_with("header.h"));
}

ZEST_CASE(ReferencesAcrossFiles) {
    add_file("header.h", R"(
        int shared_fn();
    )");
    add_main("main.cpp", R"(
        #include "header.h"
        int call() { return §(use)⟦§(use)shared_fn⟧(); }
    )");
    ASSERT(compile());
    merge_into_workspace();

    index::SymbolHash symbol = 0;
    project.project_index.shards[main_id].lookup(point("use"), [&](const index::Occurrence& o) {
        symbol = o.target;
        return false;
    });
    ASSERT(symbol != 0);

    auto references = query.sites(symbol, Fid{}, RelationKind::Reference);
    ASSERT(!references.empty());
}

ZEST_CASE(LocalReferencesClosed) {
    add_main("main.cpp", R"(
        int compute(int §(param)p) {
            int §(local)q = p;
            return §(use)q + p;
        }
    )");
    ASSERT(compile());
    merge_into_workspace();

    for(auto [marker, expected]: {
            std::pair{"use",   2U},
            std::pair{"param", 3U}
    }) {
        auto cursor = query.symbol_at(main_id, point(marker));
        ASSERT(cursor);
        EXPECT(query.references(*cursor, true).size() == expected);
    }
}

ZEST_CASE(InternalAcrossFiles) {
    add_file("header.h", R"(
        static int §(def)helper() { return 1; }
        inline int via_header() { return §(header_use)helper(); }
    )");
    add_main("main.cpp", R"(
        #include "header.h"
        int call() { return §(main_use)helper(); }
    )");
    ASSERT(compile());
    merge_into_workspace();

    auto from_main = query.symbol_at(main_id, point("main_use"));
    auto from_header = query.symbol_at(header_id, point("def", "header.h"));
    ASSERT(from_main);
    ASSERT(from_header);
    ASSERT(from_main->symbols == from_header->symbols);
    EXPECT(query.references(*from_main, true).size() == 3U);
    EXPECT(query.references(*from_header, true).size() == 3U);
    auto located = query.resolve_at(*from_main);
    ASSERT(located.size() == 1U);
    EXPECT(located.front().site.path.ends_with("header.h"));
}

ZEST_CASE(InternalTypeTarget) {
    add_file("header.h", R"(
        namespace {
        struct Point { int x; };
        struct Pair { Point first; int second; };
        Pair make() { return {}; }
        }
    )");
    add_main("main.cpp", R"(
        #include "header.h"
        int read() { auto [§(var)point, count] = make(); return point.x + count; }
    )");
    ASSERT(compile());
    merge_into_workspace();

    auto cursor = query.symbol_at(main_id, point("var"));
    ASSERT(cursor);
    auto sites = query.target_sites(cursor->symbols.front(), main_id, RelationKind::TypeDefinition);
    ASSERT(sites.size() == 1U);
    EXPECT(sites.front().path.ends_with("header.h"));
}

ZEST_CASE(InternalAcrossUnits) {
    llvm::StringRef header = "static int helper() { return 1; }\n";
    add_file("header.h", header);
    add_main("a.cpp", R"(
        #include "header.h"
        int a() { return §(use)helper(); }
    )");
    ASSERT(compile());
    merge_into_workspace();
    auto a_id = main_id;
    auto a_use = point("use");

    clear();
    add_file("header.h", header);
    add_main("b.cpp", R"(
        #include "header.h"
        int b() { return helper(); }
    )");
    ASSERT(compile());
    merge_into_workspace();

    auto cursor = query.symbol_at(a_id, a_use);
    ASSERT(cursor);
    EXPECT(query.references(*cursor, true).size() == 3U);
}

ZEST_CASE(OverloadSetCursor) {
    add_main("main.cpp", R"(
        void §(int)take(int);
        void §(double)take(double);
        template <class T> void call(T t) { §(use)take(t); }
    )");
    ASSERT(compile());
    merge_into_workspace();

    auto cursor = query.symbol_at(main_id, point("use"));
    ASSERT(cursor);
    ASSERT(cursor->symbols.size() == 2U);
    auto sites = query.definition(*cursor);
    ASSERT(sites.size() == 2U);
    EXPECT(sites[0].range.begin == point("int"));
    EXPECT(sites[1].range.begin == point("double"));
    EXPECT(query.resolve_at(*cursor).size() == 2U);
}

ZEST_CASE(SearchSymbols) {
    add_main("main.cpp", R"(
        struct Searchable { int field; };
        Searchable instance;
    )");
    ASSERT(compile());
    merge_into_workspace();

    auto results = search("Searchable");
    ASSERT(!results.empty());
    ASSERT(results.front().symbol.name == "Searchable");
}

ZEST_CASE(QualifiedNames) {
    add_main("main.cpp", R"(
        namespace outer { inline namespace v2 { namespace inner {
            template <typename T> struct Widget { void §(method)⟦§(method)paint⟧(); };
            template <> struct Widget<int> { void paint() {} };
        }
        void versioned() {}
        namespace { void §(hidden)⟦§(hidden)hidden⟧() {} }
        } }
        template <typename T> void outer::inner::Widget<T>::paint() {}
    )");
    ASSERT(compile());
    merge_into_workspace();

    index::SymbolHash method = 0;
    project.project_index.shards[main_id].lookup(point("method"), [&](const index::Occurrence& o) {
        method = o.target;
        return false;
    });
    ASSERT(method != 0);
    ASSERT(query.qualified_name(method) == "outer::inner::Widget::paint");

    auto info = query.symbol_info(method);
    ASSERT(info);
    ASSERT(info->name == "paint");
    ASSERT(query.qualified_name(info->parent) == "outer::inner::Widget");

    // Locating by a qualified name matches the parent chain, a bare one
    // the symbol's own name; both find the specialization by its
    // arguments.
    auto by_qualified = locate("inner::Widget<int>");
    ASSERT(by_qualified.size() == std::size_t(1));
    ASSERT(by_qualified.front().symbol.args == "<int>");
    ASSERT(query.qualified_name(by_qualified.front().symbol.hash) == "outer::inner::Widget<int>");
    auto by_bare = locate("Widget<int>");
    ASSERT(by_bare.size() == std::size_t(1));
    ASSERT(by_bare.front().symbol.hash == by_qualified.front().symbol.hash);
    ASSERT(locate("v2::Widget").empty());

    // Search matches the displayed name too, and a member of an inline
    // namespace reports the enclosing named one as its container.
    auto searched = search("Widget<int>");
    ASSERT(searched.size() == std::size_t(1));
    ASSERT(searched.front().symbol.hash == by_qualified.front().symbol.hash);
    ASSERT(query.container_name(searched.front().symbol.hash) == "outer::inner");
    auto versioned = search("versioned");
    ASSERT(versioned.size() == std::size_t(1));
    ASSERT(query.container_name(versioned.front().symbol.hash) == "outer");
    ASSERT(query.qualified_name(versioned.front().symbol.hash) == "outer::versioned");
    index::SymbolHash hidden = 0;
    project.project_index.shards[main_id].lookup(point("hidden"), [&](const index::Occurrence& o) {
        hidden = o.target;
        return false;
    });
    ASSERT(hidden != 0);
    ASSERT(query.qualified_name(hidden) == "outer::hidden");

    // A scoped query keeps the results whose container lists the scope's
    // components in order; a leading `::` pins the container exactly.
    ASSERT(search("inner::paint").size() == std::size_t(2));
    ASSERT(search("outer::inner::paint").size() == std::size_t(2));
    ASSERT(search("outer::paint").size() == std::size_t(2));
    ASSERT(search("inner::outer::paint").empty());
    ASSERT(search("::inner::paint").empty());
    ASSERT(search("::outer::versioned").size() == std::size_t(1));
    ASSERT(search("inner::*").size() == std::size_t(2));
    ASSERT(search("inner::").size() == std::size_t(2));
    ASSERT(search("inner::**").size() == std::size_t(4));
    ASSERT(search("paint kind:struct").empty());
    ASSERT(search("pai").size() == std::size_t(2));
    ASSERT(search(R"("paint")").size() == std::size_t(2));
    ASSERT(search("Wid*").size() == std::size_t(2));
}

ZEST_CASE(LocalsAndCursors) {
    add_main("main.cpp", R"(
        struct S { int operator()() { int hidden = 0; return hidden; } };
        static void helper() {}
        void use() { helper(); }
    )");
    ASSERT(compile());
    merge_into_workspace();

    // A callable's locals are no search target, whatever its kind.
    ASSERT(search("hidden").empty());
    ASSERT(!search("use").empty());

    // A cursor on the file's own static function resolves through the
    // serving source, which the global table knows nothing about.
    index::SymbolQuery at;
    at.position = {.path = project.file_table.resolve(main_id).str(), .line = 3, .column = 21};
    auto located = query.locate(at);
    ASSERT(located.size() == std::size_t(1));
    ASSERT(located.front().symbol.name == "helper");
    ASSERT(located.front().site.path.ends_with("main.cpp"));
    // The line alone lists it too.
    at.position->column.reset();
    auto on_line = query.locate(at);
    ASSERT(on_line.size() == std::size_t(1));
    ASSERT(on_line.front().symbol.name == "helper");
}

ZEST_CASE(LocalSymbolName) {
    add_main("main.cpp", R"(
        static int §(local)⟦§(local)hidden⟧() { return 1; }
        int use() { return hidden(); }
    )");
    ASSERT(compile());
    merge_into_workspace();

    index::SymbolHash symbol = 0;
    project.project_index.shards[main_id].lookup(point("local"), [&](const index::Occurrence& o) {
        symbol = o.target;
        return false;
    });
    ASSERT(symbol != 0);

    // TU-local names are not in the project table; the query falls back to
    // the shard's own local-name table.
    auto info = query.symbol_info(symbol);
    ASSERT(info);
    ASSERT(info->name == "hidden");
}

ZEST_CASE(OpenSessionServedByShard) {
    add_file("header.h", R"(
        struct §(def)⟦§(def)Widget⟧ { int value; };
    )");
    add_main("main.cpp", R"(
        #include "header.h"
        §(use)⟦§(use)Widget⟧ instance;
    )");
    ASSERT(compile());
    merge_into_workspace();

    // Open the document with exactly the indexed content and never
    // compile it: freshness clause 4 serves it from its shard.
    auto session = store.open(main_id);
    store.apply_open(*session, unit->main_content().str(), 1);
    ASSERT(!projections.index_current(session->path_id));

    auto cursor = query.symbol_at(main_id, point("use"));
    ASSERT(cursor);
    auto sites = query.definition(*cursor);
    ASSERT(!sites.empty());
    ASSERT(sites.front().path.ends_with("header.h"));
}

ZEST_CASE(DivergedBufferWithdrawsShard) {
    add_main("main.cpp", R"(
        int stale_fn() { return 1; }
        int use() { return §(use)⟦§(use)stale_fn⟧(); }
    )");
    ASSERT(compile());
    merge_into_workspace();

    auto session = store.open(main_id);
    auto edited = unit->main_content().str() + "// edited\n";
    store.apply_open(*session, edited, 1);

    // The buffer no longer matches the rows' content: the shard withdraws
    // and the un-compiled session resolves nothing.
    ASSERT(!query.serving(main_id).has_value());
    ASSERT(!query.symbol_at(main_id, point("use")).has_value());
}

ZEST_CASE(HeaderEdgesFromHostManifest) {
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
    ASSERT(compile());
    merge_into_workspace();

    // The header has no manifest of its own; its directive is a node of
    // the host TU's manifest hanging off the header's node.
    auto session = store.open(header_id);
    store.apply_open(*session, sources.all_files.lookup("header.h").content, 1);
    auto edges = query.include_edges(session->path_id);
    ASSERT(edges.size() == std::size_t(1));
    ASSERT(llvm::StringRef(edges[0].target).ends_with("inner.h"));

    // The TU's own manifest still answers for the TU itself.
    auto main_session = store.open(main_id);
    store.apply_open(*main_session, unit->main_content().str(), 1);
    auto main_edges = query.include_edges(main_session->path_id);
    ASSERT(main_edges.size() == std::size_t(1));
    ASSERT(llvm::StringRef(main_edges[0].target).ends_with("header.h"));
}

ZEST_CASE(StaleContributionSuppressed) {
    add_main("main.cpp", R"(
        int stale_fn() { return 1; }
        int use() { return §(use)⟦§(use)stale_fn⟧(); }
    )");
    ASSERT(compile());
    merge_into_workspace();

    index::SymbolHash symbol = 0;
    project.project_index.shards[main_id].lookup(point("use"), [&](const index::Occurrence& o) {
        symbol = o.target;
        return false;
    });
    ASSERT(!query.sites(symbol, Fid{}, RelationKind::Reference).empty());

    // Rows of text the disk no longer holds point nowhere: the file's
    // contribution disappears from cross-file results until its rows
    // describe the disk again.
    project.file_table.observe(main_id, DiskObservation{.hash = 1});
    ASSERT(query.sites(symbol, Fid{}, RelationKind::Reference).empty());
}

ZEST_CASE(ClassNameOverConstructor) {
    add_main("main.cpp", R"(
        namespace outer {
        struct Widget {
            Widget();
            Widget(int);
        };
        }
    )");
    ASSERT(compile());
    merge_into_workspace();

    for(auto name: {"Widget", "outer::Widget"}) {
        auto results = locate(name);
        ASSERT(results.size() == 1U);
        ASSERT(results.front().symbol.kind == SymbolKind::Struct);
    }
    ASSERT(locate("outer::Widget::Widget").size() == 2U);
}

ZEST_CASE(UndefinedBesideStaleUse) {
    add_file("header.h", R"(
        int external();
    )");
    add_main("main.cpp", R"(
        #include "header.h"
        int use() { return external(); }
    )");
    ASSERT(compile());
    merge_into_workspace();

    // A file using the symbol moved on; nothing defines the symbol, so the
    // declaration still places it.
    project.file_table.observe(main_id, DiskObservation{.hash = 1});
    auto results = search("external");
    ASSERT(results.size() == 1U);
    ASSERT(results.front().site.path.ends_with("header.h"));
}

ZEST_CASE(DeletedDefinitionFallsBack) {
    llvm::StringRef header = R"(
        int removed();
    )";
    add_file("header.h", header);
    add_main("main.cpp", R"(
        #include "header.h"
        int removed() { return 0; }
    )");
    ASSERT(compile());
    merge_into_workspace();

    clear();
    add_file("header.h", header);
    add_main("use.cpp", R"(
        #include "header.h"
        int use() { return removed(); }
    )");
    ASSERT(compile());
    merge_into_workspace();
    auto use_id = main_id;

    // The table keeps the definition the first unit reported; the rows
    // no longer hold it, so the declaration places the symbol — a file
    // that only used it moving on changes nothing.
    clear();
    add_file("header.h", header);
    add_main("main.cpp", R"(
        #include "header.h"
        int kept() { return removed(); }
    )");
    ASSERT(compile());
    merge_into_workspace();
    project.file_table.observe(use_id, DiskObservation{.hash = 1});

    auto results = search("removed");
    ASSERT(results.size() == 1U);
    ASSERT(results.front().site.path.ends_with("header.h"));
}

};  // ZEST_SUITE(IndexQuery)

}  // namespace
}  // namespace clice::testing
