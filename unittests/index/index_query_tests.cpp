module;

#include "modules/prelude.h"

module clice;

import :feature.feature;
import :index.query;
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
import :unittests.utils.merge_unit;
import :unittests.utils;
import :unittests.utils.tester;
import :worker.pool;

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

    std::optional<index::IndexSource> buffer_index(Fid) const override {
        return std::nullopt;
    }

    void each_session(llvm::function_ref<bool(const index::IndexSource&)>) const override {}

    void each_session_index(llvm::function_ref<bool(const index::TUIndex&)> visit) const override {
        for(auto& table: tables) {
            visits += 1;
            if(!visit(table)) {
                return;
            }
        }
    }

    void each_preamble(llvm::function_ref<bool(const index::IndexSource&)>) const override {}

    void each_overlay(llvm::function_ref<bool(const index::TUIndex&)>) const override {}

    std::shared_ptr<index::TUIndex> preamble_blob(Fid) const override {
        return nullptr;
    }
};

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
PCHFamily pch{graph, project, pool};
TURunFamily turun{graph, project, resolver, pcm, pch, index_store, pool};
IndexPump indexer{loop, project, turun, index_store, pool};
ServerLiveSources live{project, pch, store, projections};
index::FreshnessGate gate{project.file_table};
index::IndexQuery query{project.project_index, project.file_table, &gate, &live};

Fid focus_id;
Fid header_id;

std::vector<index::IndexQuery::Located> search(llvm::StringRef text, std::size_t limit = 10) {
    return query.search(*index::SymbolQuery::parse(text), limit);
}

std::vector<index::IndexQuery::Located> locate(llvm::StringRef text) {
    return query.locate(*index::SymbolQuery::parse(text));
}

/// Compile the added sources as the background index does and merge the
/// unit's index.
void index_workspace() {
    kind = CompilationKind::Whole;
    ZASSERT(compile());
    merge_unit(project, *unit, focus_id);
    if(auto header = project.file_table.find(Spelling::absolute(TestVFS::path("header.h")))) {
        header_id = *header;
    }
}

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
    project.project_index.shards[focus_id].lookup(hit_offset, [&](const index::Occurrence& o) {
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
    project.project_index.shards[focus_id].lookup(point("use"), [&](const index::Occurrence& o) {
        symbol = o.target;
        return false;
    });
    ZASSERT(symbol != 0);

    auto references = query.sites(symbol, Fid{}, RelationKind::Reference);
    ZASSERT(!references.empty());
}

ZEST_CASE(LocalReferencesClosed) {
    add_focus("main.cpp", R"(
        int compute(int §(param)p) {
            int §(local)q = p;
            return §(use)q + p;
        }
    )");
    index_workspace();

    for(auto [marker, expected]: {
            std::pair{"use",   2U},
            std::pair{"param", 3U}
    }) {
        auto cursor = query.symbol_at(focus_id, point(marker));
        ZASSERT(cursor);
        ZEXPECT(query.references(*cursor, true).size() == expected);
    }
}

ZEST_CASE(HighlightsClosedFile) {
    add_focus("main.cpp", R"(
        int tally() {
            int §(decl)⟦count⟧ = 0;
            §(write)⟦count⟧ += 1;
            return §(read)⟦§(read)count⟧;
        }
    )");
    index_workspace();

    auto cursor = query.symbol_at(focus_id, point("read"));
    ZASSERT(cursor);
    auto highlights = query.document_highlights(*cursor);
    ZASSERT(highlights.size() == 3U);
    ZEXPECT(highlights[0].site.range == range("decl"));
    ZEXPECT(highlights[0].kind == index::HighlightKind::Text);
    ZEXPECT(highlights[1].site.range == range("write"));
    ZEXPECT(highlights[1].kind == index::HighlightKind::Write);
    ZEXPECT(highlights[2].site.range == range("read"));
    ZEXPECT(highlights[2].kind == index::HighlightKind::Read);
}

ZEST_CASE(InternalAcrossFiles) {
    add_file("header.h", R"(
        static int §(def)helper() { return 1; }
        inline int via_header() { return §(header_use)helper(); }
    )");
    add_focus("main.cpp", R"(
        #include "header.h"
        int call() { return §(main_use)helper(); }
    )");
    index_workspace();

    auto from_main = query.symbol_at(focus_id, point("main_use"));
    auto from_header = query.symbol_at(header_id, point("def", "header.h"));
    ZASSERT(from_main);
    ZASSERT(from_header);
    ZASSERT(from_main->symbols == from_header->symbols);
    ZEXPECT(query.references(*from_main, true).size() == 3U);
    ZEXPECT(query.references(*from_header, true).size() == 3U);
    auto located = query.resolve_at(*from_main);
    ZASSERT(located.size() == 1U);
    ZEXPECT(located.front().site.path.ends_with("header.h"));
}

ZEST_CASE(InternalTypeTarget) {
    add_file("header.h", R"(
        namespace {
        struct Point { int x; };
        struct Pair { Point first; int second; };
        Pair make() { return {}; }
        }
    )");
    add_focus("main.cpp", R"(
        #include "header.h"
        int read() { auto [§(var)point, count] = make(); return point.x + count; }
    )");
    index_workspace();

    auto cursor = query.symbol_at(focus_id, point("var"));
    ZASSERT(cursor);
    auto sites = query.type_definition(*cursor, cursor->symbols.front());
    ZASSERT(sites.size() == 1U);
    ZEXPECT(sites.front().path.ends_with("header.h"));
}

ZEST_CASE(InternalAcrossUnits) {
    llvm::StringRef header = "static int helper() { return 1; }\n";
    add_file("header.h", header);
    add_focus("a.cpp", R"(
        #include "header.h"
        int a() { return §(use)helper(); }
    )");
    index_workspace();
    auto a_id = focus_id;
    auto a_use = point("use");

    clear();
    add_file("header.h", header);
    add_focus("b.cpp", R"(
        #include "header.h"
        int b() { return helper(); }
    )");
    index_workspace();

    auto cursor = query.symbol_at(a_id, a_use);
    ZASSERT(cursor);
    ZEXPECT(query.references(*cursor, true).size() == 3U);
}

ZEST_CASE(OverloadSetCursor) {
    add_focus("main.cpp", R"(
        void §(int)take(int);
        void §(double)take(double);
        template <class T> void call(T t) { §(use)take(t); }
    )");
    index_workspace();

    auto cursor = query.symbol_at(focus_id, point("use"));
    ZASSERT(cursor);
    ZASSERT(cursor->symbols.size() == 2U);
    auto sites = query.definition(*cursor);
    ZASSERT(sites.size() == 2U);
    ZEXPECT(sites[0].range.begin == point("int"));
    ZEXPECT(sites[1].range.begin == point("double"));
    ZEXPECT(query.resolve_at(*cursor).size() == 2U);
}

ZEST_CASE(SearchSymbols) {
    add_focus("main.cpp", R"(
        struct Searchable { int field; };
        Searchable instance;
    )");
    index_workspace();

    auto results = search("Searchable");
    ZASSERT(!results.empty());
    ZASSERT(results.front().symbol.name == "Searchable");
}

ZEST_CASE(InternalNamesSearched) {
    // A file merged since the search index was built is read for its
    // names of internal linkage, in no table; a function's locals stay
    // out.
    add_focus("main.cpp", R"(
        static int cached_total(int cached_count) {
            int cached_sum = cached_count;
            return cached_sum;
        }
        namespace { struct CachedEntry {}; }
    )");
    index_workspace();

    std::vector<std::string> names;
    for(auto& result: search("cached")) {
        names.push_back(result.symbol.name);
    }
    llvm::sort(names);
    ZEXPECT(names == (std::vector<std::string>{"CachedEntry", "cached_total"}));
}

ZEST_CASE(UnnamedScopes) {
    // An unnamed enum's enumerators and an anonymous union's members are
    // named through the enclosing scope, as lookup names them.
    add_focus("main.cpp", R"(
        namespace outer {
            enum { §(size)⟦§(size)kSize⟧ = 4 };
            struct Holder { union { int §(member)⟦§(member)member⟧; }; };
            typedef struct { int §(field)⟦§(field)field⟧; } Point;
        }
    )");
    index_workspace();

    auto at = [&](llvm::StringRef name) {
        index::SymbolHash found = 0;
        project.project_index.shards[focus_id].lookup(point(name), [&](const index::Occurrence& o) {
            found = o.target;
            return false;
        });
        return found;
    };
    ZASSERT(query.qualified_name(at("size")) == "outer::kSize");
    ZASSERT(query.qualified_name(at("member")) == "outer::Holder::member");
    // An unnamed class with a declarator or a typedef name is no anonymous
    // scope: lookup never names its members through `outer`.
    ZASSERT(query.qualified_name(at("field")) == "outer::(anonymous struct)::field");
    ZASSERT(search("outer::kSize").size() == std::size_t(1));
    ZASSERT(search("Holder::member").size() == std::size_t(1));
}

ZEST_CASE(QualifiedNames) {
    add_focus("main.cpp", R"(
        namespace outer { inline namespace v2 { namespace inner {
            template <typename T> struct Widget { void §(method)⟦§(method)paint⟧(); };
            template <> struct Widget<int> { void paint() {} };
        }
        void versioned() {}
        namespace { void §(hidden)⟦§(hidden)hidden⟧() {} }
        } }
        template <typename T> void outer::inner::Widget<T>::paint() {}
    )");
    index_workspace();

    index::SymbolHash method = 0;
    project.project_index.shards[focus_id].lookup(point("method"), [&](const index::Occurrence& o) {
        method = o.target;
        return false;
    });
    ZASSERT(method != 0);
    ZASSERT(query.qualified_name(method) == "outer::inner::Widget::paint");

    auto info = query.symbol_info(method);
    ZASSERT(info);
    ZASSERT(info->name == "paint");
    ZASSERT(query.qualified_name(info->parent) == "outer::inner::Widget");

    // Locating by a qualified name matches the parent chain, a bare one
    // the symbol's own name; both find the specialization by its
    // arguments.
    auto by_qualified = locate("inner::Widget<int>");
    ZASSERT(by_qualified.size() == std::size_t(1));
    ZASSERT(by_qualified.front().symbol.args == "<int>");
    ZASSERT(query.qualified_name(by_qualified.front().symbol.hash) == "outer::inner::Widget<int>");
    auto by_bare = locate("Widget<int>");
    ZASSERT(by_bare.size() == std::size_t(1));
    ZASSERT(by_bare.front().symbol.hash == by_qualified.front().symbol.hash);
    ZASSERT(locate("v2::Widget").empty());

    // Search matches the displayed name too, and a member of an inline
    // namespace reports the enclosing named one as its container.
    auto searched = search("Widget<int>");
    ZASSERT(searched.size() == std::size_t(1));
    ZASSERT(searched.front().symbol.hash == by_qualified.front().symbol.hash);
    ZASSERT(query.container_name(searched.front().symbol.hash) == "outer::inner");
    auto versioned = search("versioned");
    ZASSERT(versioned.size() == std::size_t(1));
    ZASSERT(query.container_name(versioned.front().symbol.hash) == "outer");
    ZASSERT(query.qualified_name(versioned.front().symbol.hash) == "outer::versioned");
    index::SymbolHash hidden = 0;
    project.project_index.shards[focus_id].lookup(point("hidden"), [&](const index::Occurrence& o) {
        hidden = o.target;
        return false;
    });
    ZASSERT(hidden != 0);
    ZASSERT(query.qualified_name(hidden) == "outer::hidden");

    // A scoped query keeps the results whose container lists the scope's
    // components in order; a leading `::` pins the container exactly.
    ZASSERT(search("inner::paint").size() == std::size_t(2));
    ZASSERT(search("outer::inner::paint").size() == std::size_t(2));
    ZASSERT(search("outer::paint").size() == std::size_t(2));
    ZASSERT(search("inner::outer::paint").empty());
    ZASSERT(search("::inner::paint").empty());
    ZASSERT(search("::outer::versioned").size() == std::size_t(1));
    ZASSERT(search("inner::*").size() == std::size_t(2));
    ZASSERT(search("inner::").size() == std::size_t(2));
    ZASSERT(search("inner::**").size() == std::size_t(4));
    ZASSERT(search("paint kind:struct").empty());
    ZASSERT(search("pai").size() == std::size_t(2));
    ZASSERT(search(R"("paint")").size() == std::size_t(2));
    ZASSERT(search("Wid*").size() == std::size_t(2));
}

ZEST_CASE(LocalsAndCursors) {
    add_focus("main.cpp", R"(
        struct S { int operator()() { int hidden = 0; return hidden; } };
        static void helper() {}
        void use() { helper(); }
    )");
    index_workspace();

    // A callable's locals are no search target, whatever its kind.
    ZASSERT(search("hidden").empty());
    ZASSERT(!search("use").empty());

    // A cursor on the file's own static function resolves through the
    // serving source, which the global table knows nothing about.
    index::SymbolQuery at;
    at.position = {.path = project.file_table.resolve(focus_id).str(), .line = 3, .column = 21};
    auto located = query.locate(at);
    ZASSERT(located.size() == std::size_t(1));
    ZASSERT(located.front().symbol.name == "helper");
    ZASSERT(located.front().site.path.ends_with("main.cpp"));
    // The line alone lists it too.
    at.position->column.reset();
    auto on_line = query.locate(at);
    ZASSERT(on_line.size() == std::size_t(1));
    ZASSERT(on_line.front().symbol.name == "helper");
}

ZEST_CASE(LocalSymbolName) {
    add_focus("main.cpp", R"(
        static int §(local)⟦§(local)hidden⟧() { return 1; }
        int use() { return hidden(); }
    )");
    index_workspace();

    index::SymbolHash symbol = 0;
    project.project_index.shards[focus_id].lookup(point("local"), [&](const index::Occurrence& o) {
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

ZEST_CASE(LocalsByPath) {
    add_focus("main.cpp", R"(
        struct S { int operator()() { int hidden = 0; return hidden; } };
        static void helper(int hidden) {}
        void use() { helper(1); }
    )");
    index_workspace();

    ZASSERT(search("hidden").empty());
    ZASSERT(search("hidden path:main.cpp").size() == 2U);
    ZASSERT(search("helper::hidden path:main.cpp").size() == 1U);

    auto helper = locate(R"("helper" path:main.cpp)");
    ZASSERT(helper.size() == 1U);
    ZEXPECT(helper.front().symbol.scope == index::SymbolScope::TULocal);
    ZEXPECT(helper.front().site.path.ends_with("main.cpp"));
}

ZEST_CASE(LocateEveryExactMatch) {
    std::string source;
    for(int i = 0; i < 70; i += 1) {
        source += std::format("namespace n{} {{ int value; }}\n", i);
    }
    add_focus("main.cpp", source);
    index_workspace();

    ZEXPECT(locate("value").size() == 70U);
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
    auto sites = query.definition(*cursor);
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
    auto edges = query.include_edges(session->path_id);
    ZASSERT(edges.size() == std::size_t(1));
    ZASSERT(llvm::StringRef(edges[0].target).ends_with("inner.h"));

    // The TU's own manifest still answers for the TU itself.
    auto focus_session = store.open(focus_id);
    store.apply_open(*focus_session, unit->focus_file().content().str(), 1);
    auto focus_edges = query.include_edges(focus_session->path_id);
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
    project.project_index.shards[focus_id].lookup(point("use"), [&](const index::Occurrence& o) {
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

ZEST_CASE(ClassNameOverConstructor) {
    add_focus("main.cpp", R"(
        namespace outer {
        struct Widget {
            Widget();
            Widget(int);
        };
        }
    )");
    index_workspace();

    for(auto name: {"Widget", "outer::Widget"}) {
        auto results = locate(name);
        ZASSERT(results.size() == 1U);
        ZASSERT(results.front().symbol.kind == SymbolKind::Struct);
    }
    ZASSERT(locate("outer::Widget::Widget").size() == 2U);
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
        add_focus(std::format("main{}.cpp", buffer), text);
        ZASSERT(compile());
        sessions.tables.push_back(index::TUIndex::from_buffer(
            llvm::MemoryBuffer::getMemBufferCopy(index::build_tu_index(*unit))));
    }
}

ZEST_CASE(ScopedSearchScalesLinearly) {
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
    ZASSERT(visits_with(8) <= few * 4);
}

};  // ZEST_SUITE(IndexQuery)

}  // namespace
}  // namespace clice::testing
