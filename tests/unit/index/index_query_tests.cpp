#include <format>
#include <string>
#include <vector>

#include "test/merge_unit.h"
#include "test/test.h"
#include "test/tester.h"
#include "feature/feature.h"
#include "index/query.h"
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

namespace clice::testing {
namespace {

/// Open buffers reduced to their session tables, counting the tables the
/// queries visit.
struct CountingSessions : index::LiveSources {
    std::vector<index::TUIndex> tables;
    mutable std::size_t visits = 0;

    bool is_open(Fid) const override {
        return false;
    }

    std::optional<index::RowSource> claim(Fid) const override {
        return std::nullopt;
    }

    void each_session(llvm::function_ref<bool(const index::RowSource&)>) const override {}

    void each_session_index(llvm::function_ref<bool(const index::TUIndex&)> visit) const override {
        for(auto& table: tables) {
            visits += 1;
            if(!visit(table)) {
                return;
            }
        }
    }

    void each_preamble(llvm::function_ref<bool(const index::RowSource&)>) const override {}

    void each_overlay(llvm::function_ref<bool(const index::TUIndex&)>) const override {}

    std::shared_ptr<index::TUIndex> preamble_blob(Fid) const override {
        return nullptr;
    }
};

TEST_SUITE(IndexQuery, Tester) {

kota::event_loop loop;
FileTable files;
Project project{files};
SessionStore store;
WorkerPool pool{loop};
CommandResolver resolver{project};
TaskGraph graph{loop};
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

void merge_into_workspace() {
    merge_unit(project, *unit, main_id);
    if(auto header = project.file_table.find(Spelling::absolute(TestVFS::path("header.h")))) {
        header_id = *header;
    }
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
    project.project_index.shards[main_id].lookup(hit_offset, [&](const index::Occurrence& o) {
        symbol = o.target;
        return false;
    });
    ASSERT_TRUE(symbol != 0);

    auto site = query.first_site(symbol, Fid{}, RelationKind::Definition);
    ASSERT_TRUE(site.has_value());
    ASSERT_TRUE(site->path.ends_with("header.h"));
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
    project.project_index.shards[main_id].lookup(point("use"), [&](const index::Occurrence& o) {
        symbol = o.target;
        return false;
    });
    ASSERT_TRUE(symbol != 0);

    auto references = query.sites(symbol, Fid{}, RelationKind::Reference);
    ASSERT_FALSE(references.empty());
}

TEST_CASE(LocalReferencesClosed) {
    add_main("main.cpp", R"(
        int compute(int §(param)p) {
            int §(local)q = p;
            return §(use)q + p;
        }
    )");
    ASSERT_TRUE(compile());
    merge_into_workspace();

    for(auto [marker, expected]: {
            std::pair{"use",   2U},
            std::pair{"param", 3U}
    }) {
        auto cursor = query.symbol_at(main_id, point(marker));
        ASSERT_TRUE(cursor.has_value());
        EXPECT_EQ(query.references(*cursor, true).size(), expected);
    }
}

TEST_CASE(InternalAcrossFiles) {
    add_file("header.h", R"(
        static int §(def)helper() { return 1; }
        inline int via_header() { return §(header_use)helper(); }
    )");
    add_main("main.cpp", R"(
        #include "header.h"
        int call() { return §(main_use)helper(); }
    )");
    ASSERT_TRUE(compile());
    merge_into_workspace();

    auto from_main = query.symbol_at(main_id, point("main_use"));
    auto from_header = query.symbol_at(header_id, point("def", "header.h"));
    ASSERT_TRUE(from_main.has_value());
    ASSERT_TRUE(from_header.has_value());
    ASSERT_EQ(from_main->symbols, from_header->symbols);
    EXPECT_EQ(query.references(*from_main, true).size(), 3U);
    EXPECT_EQ(query.references(*from_header, true).size(), 3U);
    auto located = query.resolve_at(*from_main);
    ASSERT_EQ(located.size(), 1U);
    EXPECT_TRUE(located.front().site.path.ends_with("header.h"));
}

TEST_CASE(InternalTypeTarget) {
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
    ASSERT_TRUE(compile());
    merge_into_workspace();

    auto cursor = query.symbol_at(main_id, point("var"));
    ASSERT_TRUE(cursor.has_value());
    auto sites = query.target_sites(cursor->symbols.front(), main_id, RelationKind::TypeDefinition);
    ASSERT_EQ(sites.size(), 1U);
    EXPECT_TRUE(sites.front().path.ends_with("header.h"));
}

TEST_CASE(InternalAcrossUnits) {
    llvm::StringRef header = "static int helper() { return 1; }\n";
    add_file("header.h", header);
    add_main("a.cpp", R"(
        #include "header.h"
        int a() { return §(use)helper(); }
    )");
    ASSERT_TRUE(compile());
    merge_into_workspace();
    auto a_id = main_id;
    auto a_use = point("use");

    clear();
    add_file("header.h", header);
    add_main("b.cpp", R"(
        #include "header.h"
        int b() { return helper(); }
    )");
    ASSERT_TRUE(compile());
    merge_into_workspace();

    auto cursor = query.symbol_at(a_id, a_use);
    ASSERT_TRUE(cursor.has_value());
    EXPECT_EQ(query.references(*cursor, true).size(), 3U);
}

TEST_CASE(OverloadSetCursor) {
    add_main("main.cpp", R"(
        void §(int)take(int);
        void §(double)take(double);
        template <class T> void call(T t) { §(use)take(t); }
    )");
    ASSERT_TRUE(compile());
    merge_into_workspace();

    auto cursor = query.symbol_at(main_id, point("use"));
    ASSERT_TRUE(cursor.has_value());
    ASSERT_EQ(cursor->symbols.size(), 2U);
    auto sites = query.definition(*cursor);
    ASSERT_EQ(sites.size(), 2U);
    EXPECT_EQ(sites[0].range.begin, point("int"));
    EXPECT_EQ(sites[1].range.begin, point("double"));
    EXPECT_EQ(query.resolve_at(*cursor).size(), 2U);
}

TEST_CASE(SearchSymbols) {
    add_main("main.cpp", R"(
        struct Searchable { int field; };
        Searchable instance;
    )");
    ASSERT_TRUE(compile());
    merge_into_workspace();

    auto results = search("Searchable");
    ASSERT_FALSE(results.empty());
    ASSERT_EQ(results.front().symbol.name, "Searchable");
}

TEST_CASE(UnnamedScopes) {
    // An unnamed enum's enumerators and an anonymous union's members are
    // named through the enclosing scope, as lookup names them.
    add_main("main.cpp", R"(
        namespace outer {
            enum { §(size)⟦§(size)kSize⟧ = 4 };
            struct Holder { union { int §(member)⟦§(member)member⟧; }; };
            typedef struct { int §(field)⟦§(field)field⟧; } Point;
        }
    )");
    ASSERT_TRUE(compile());
    merge_into_workspace();

    auto at = [&](llvm::StringRef name) {
        index::SymbolHash found = 0;
        project.project_index.shards[main_id].lookup(point(name), [&](const index::Occurrence& o) {
            found = o.target;
            return false;
        });
        return found;
    };
    ASSERT_EQ(query.qualified_name(at("size")), "outer::kSize");
    ASSERT_EQ(query.qualified_name(at("member")), "outer::Holder::member");
    // An unnamed class with a declarator or a typedef name is no anonymous
    // scope: lookup never names its members through `outer`.
    ASSERT_EQ(query.qualified_name(at("field")), "outer::(anonymous struct)::field");
    ASSERT_EQ(search("outer::kSize").size(), std::size_t(1));
    ASSERT_EQ(search("Holder::member").size(), std::size_t(1));
}

TEST_CASE(QualifiedNames) {
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
    ASSERT_TRUE(compile());
    merge_into_workspace();

    index::SymbolHash method = 0;
    project.project_index.shards[main_id].lookup(point("method"), [&](const index::Occurrence& o) {
        method = o.target;
        return false;
    });
    ASSERT_TRUE(method != 0);
    ASSERT_EQ(query.qualified_name(method), "outer::inner::Widget::paint");

    auto info = query.symbol_info(method);
    ASSERT_TRUE(info.has_value());
    ASSERT_EQ(info->name, "paint");
    ASSERT_EQ(query.qualified_name(info->parent), "outer::inner::Widget");

    // Locating by a qualified name matches the parent chain, a bare one
    // the symbol's own name; both find the specialization by its
    // arguments.
    auto by_qualified = locate("inner::Widget<int>");
    ASSERT_EQ(by_qualified.size(), std::size_t(1));
    ASSERT_EQ(by_qualified.front().symbol.args, "<int>");
    ASSERT_EQ(query.qualified_name(by_qualified.front().symbol.hash), "outer::inner::Widget<int>");
    auto by_bare = locate("Widget<int>");
    ASSERT_EQ(by_bare.size(), std::size_t(1));
    ASSERT_EQ(by_bare.front().symbol.hash, by_qualified.front().symbol.hash);
    ASSERT_TRUE(locate("v2::Widget").empty());

    // Search matches the displayed name too, and a member of an inline
    // namespace reports the enclosing named one as its container.
    auto searched = search("Widget<int>");
    ASSERT_EQ(searched.size(), std::size_t(1));
    ASSERT_EQ(searched.front().symbol.hash, by_qualified.front().symbol.hash);
    ASSERT_EQ(query.container_name(searched.front().symbol.hash), "outer::inner");
    auto versioned = search("versioned");
    ASSERT_EQ(versioned.size(), std::size_t(1));
    ASSERT_EQ(query.container_name(versioned.front().symbol.hash), "outer");
    ASSERT_EQ(query.qualified_name(versioned.front().symbol.hash), "outer::versioned");
    index::SymbolHash hidden = 0;
    project.project_index.shards[main_id].lookup(point("hidden"), [&](const index::Occurrence& o) {
        hidden = o.target;
        return false;
    });
    ASSERT_TRUE(hidden != 0);
    ASSERT_EQ(query.qualified_name(hidden), "outer::hidden");

    // A scoped query keeps the results whose container lists the scope's
    // components in order; a leading `::` pins the container exactly.
    ASSERT_EQ(search("inner::paint").size(), std::size_t(2));
    ASSERT_EQ(search("outer::inner::paint").size(), std::size_t(2));
    ASSERT_EQ(search("outer::paint").size(), std::size_t(2));
    ASSERT_TRUE(search("inner::outer::paint").empty());
    ASSERT_TRUE(search("::inner::paint").empty());
    ASSERT_EQ(search("::outer::versioned").size(), std::size_t(1));
    ASSERT_EQ(search("inner::*").size(), std::size_t(2));
    ASSERT_EQ(search("inner::").size(), std::size_t(2));
    ASSERT_EQ(search("inner::**").size(), std::size_t(4));
    ASSERT_TRUE(search("paint kind:struct").empty());
    ASSERT_EQ(search("pai").size(), std::size_t(2));
    ASSERT_EQ(search(R"("paint")").size(), std::size_t(2));
    ASSERT_EQ(search("Wid*").size(), std::size_t(2));
}

TEST_CASE(LocalsAndCursors) {
    add_main("main.cpp", R"(
        struct S { int operator()() { int hidden = 0; return hidden; } };
        static void helper() {}
        void use() { helper(); }
    )");
    ASSERT_TRUE(compile());
    merge_into_workspace();

    // A callable's locals are no search target, whatever its kind.
    ASSERT_TRUE(search("hidden").empty());
    ASSERT_FALSE(search("use").empty());

    // A cursor on the file's own static function resolves through the
    // serving source, which the global table knows nothing about.
    index::SymbolQuery at;
    at.position = {.path = project.file_table.resolve(main_id).str(), .line = 3, .column = 21};
    auto located = query.locate(at);
    ASSERT_EQ(located.size(), std::size_t(1));
    ASSERT_EQ(located.front().symbol.name, "helper");
    ASSERT_TRUE(located.front().site.path.ends_with("main.cpp"));
    // The line alone lists it too.
    at.position->column.reset();
    auto on_line = query.locate(at);
    ASSERT_EQ(on_line.size(), std::size_t(1));
    ASSERT_EQ(on_line.front().symbol.name, "helper");
}

TEST_CASE(LocalSymbolName) {
    add_main("main.cpp", R"(
        static int §(local)⟦§(local)hidden⟧() { return 1; }
        int use() { return hidden(); }
    )");
    ASSERT_TRUE(compile());
    merge_into_workspace();

    index::SymbolHash symbol = 0;
    project.project_index.shards[main_id].lookup(point("local"), [&](const index::Occurrence& o) {
        symbol = o.target;
        return false;
    });
    ASSERT_TRUE(symbol != 0);

    // TU-local names are not in the project table; the query falls back to
    // the shard's own local-name table.
    auto info = query.symbol_info(symbol);
    ASSERT_TRUE(info.has_value());
    ASSERT_EQ(info->name, "hidden");
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
    store.apply_open(*session, unit->main_content().str(), 1);
    ASSERT_FALSE(projections.index_current(session->path_id));

    auto cursor = query.symbol_at(main_id, point("use"));
    ASSERT_TRUE(cursor.has_value());
    auto sites = query.definition(*cursor);
    ASSERT_FALSE(sites.empty());
    ASSERT_TRUE(sites.front().path.ends_with("header.h"));
}

TEST_CASE(DivergedBufferWithdrawsShard) {
    add_main("main.cpp", R"(
        int stale_fn() { return 1; }
        int use() { return §(use)⟦§(use)stale_fn⟧(); }
    )");
    ASSERT_TRUE(compile());
    merge_into_workspace();

    auto session = store.open(main_id);
    auto edited = unit->main_content().str() + "// edited\n";
    store.apply_open(*session, edited, 1);

    // The buffer no longer matches the rows' content: the shard withdraws
    // and the un-compiled session resolves nothing.
    ASSERT_FALSE(query.serving(main_id).has_value());
    ASSERT_FALSE(query.symbol_at(main_id, point("use")).has_value());
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
    auto edges = query.include_edges(session->path_id);
    ASSERT_EQ(edges.size(), std::size_t(1));
    ASSERT_TRUE(llvm::StringRef(edges[0].target).ends_with("inner.h"));

    // The TU's own manifest still answers for the TU itself.
    auto main_session = store.open(main_id);
    store.apply_open(*main_session, unit->main_content().str(), 1);
    auto main_edges = query.include_edges(main_session->path_id);
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
    project.project_index.shards[main_id].lookup(point("use"), [&](const index::Occurrence& o) {
        symbol = o.target;
        return false;
    });
    ASSERT_FALSE(query.sites(symbol, Fid{}, RelationKind::Reference).empty());

    // Rows of text the disk no longer holds point nowhere: the file's
    // contribution disappears from cross-file results until its rows
    // describe the disk again.
    project.file_table.observe(main_id, DiskObservation{.hash = 1});
    ASSERT_TRUE(query.sites(symbol, Fid{}, RelationKind::Reference).empty());
}

TEST_CASE(ClassNameOverConstructor) {
    add_main("main.cpp", R"(
        namespace outer {
        struct Widget {
            Widget();
            Widget(int);
        };
        }
    )");
    ASSERT_TRUE(compile());
    merge_into_workspace();

    for(auto name: {"Widget", "outer::Widget"}) {
        auto results = locate(name);
        ASSERT_EQ(results.size(), 1U);
        ASSERT_EQ(results.front().symbol.kind, SymbolKind::Struct);
    }
    ASSERT_EQ(locate("outer::Widget::Widget").size(), 2U);
}

TEST_CASE(UndefinedBesideStaleUse) {
    add_file("header.h", R"(
        int external();
    )");
    add_main("main.cpp", R"(
        #include "header.h"
        int use() { return external(); }
    )");
    ASSERT_TRUE(compile());
    merge_into_workspace();

    // A file using the symbol moved on; nothing defines the symbol, so the
    // declaration still places it.
    project.file_table.observe(main_id, DiskObservation{.hash = 1});
    auto results = search("external");
    ASSERT_EQ(results.size(), 1U);
    ASSERT_TRUE(results.front().site.path.ends_with("header.h"));
}

TEST_CASE(DeletedDefinitionFallsBack) {
    llvm::StringRef header = R"(
        int removed();
    )";
    add_file("header.h", header);
    add_main("main.cpp", R"(
        #include "header.h"
        int removed() { return 0; }
    )");
    ASSERT_TRUE(compile());
    merge_into_workspace();

    clear();
    add_file("header.h", header);
    add_main("use.cpp", R"(
        #include "header.h"
        int use() { return removed(); }
    )");
    ASSERT_TRUE(compile());
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
    ASSERT_TRUE(compile());
    merge_into_workspace();
    project.file_table.observe(use_id, DiskObservation{.hash = 1});

    auto results = search("removed");
    ASSERT_EQ(results.size(), 1U);
    ASSERT_TRUE(results.front().site.path.ends_with("header.h"));
}

/// Open `buffers` documents, each declaring its own functions in one
/// shared namespace.
void open_buffers(CountingSessions& sessions, int buffers) {
    for(int buffer = 0; buffer < buffers; buffer += 1) {
        clear();
        std::string text = "namespace app { namespace shared {\n";
        for(int i = 0; i < 40; i += 1) {
            text += std::format("int fn{}_{}();\n", buffer, i);
        }
        text += "} }\n";
        add_main(std::format("main{}.cpp", buffer), text);
        ASSERT_TRUE(compile());
        sessions.tables.push_back(index::TUIndex::from_buffer(
            llvm::MemoryBuffer::getMemBufferCopy(index::build_tu_index(*unit, true))));
    }
}

TEST_CASE(ScopedSearchScalesLinearly) {
    auto visits_with = [&](int buffers) {
        CountingSessions sessions;
        open_buffers(sessions, buffers);
        index::IndexQuery session_query{project.project_index,
                                        project.file_table,
                                        nullptr,
                                        &sessions};
        session_query.search(*index::SymbolQuery::parse("app::fn0_1"), 10);
        return sessions.visits;
    };
    // Four times the open buffers: a scan of each buffer's table grows
    // four times, a lookup through every table per candidate sixteen.
    auto few = visits_with(2);
    ASSERT_LE(visits_with(8), few * 4);
}

};  // TEST_SUITE(IndexQuery)

}  // namespace
}  // namespace clice::testing
