module;

#include "modules/prelude.h"

module clice;

import :index.indexer;
import :index.query;
import :index.search;
import :index.symbol;
import :index.symbol_query;
import :index.tu_index;
import :semantic.symbol;
import :support.bytes;
import :unittests.utils.index_query_tester;
import :vfs.ids;

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

    void for_each_session(llvm::function_ref<bool(const index::IndexSource&)>) const override {}

    void for_each_session_index(
        llvm::function_ref<bool(const index::TUIndex&)> visit) const override {
        for(auto& table: tables) {
            visits += 1;
            if(!visit(table)) {
                return;
            }
        }
    }

    void for_each_preamble(llvm::function_ref<bool(const index::IndexSource&)>) const override {}

    void for_each_overlay(llvm::function_ref<bool(const index::TUIndex&)>) const override {}

    std::shared_ptr<index::TUIndex> preamble_index(Fid) const override {
        return nullptr;
    }
};

ZEST_SUITE(IndexSearch, IndexQueryTester) {

std::vector<index::IndexQuery::Located> locate(llvm::StringRef text) {
    return index::locate(query, *index::SymbolQuery::parse(text));
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
    std::ranges::sort(names);
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
        project.index.shard(focus_id)->occurrences_at(point(name), [&](const index::Occurrence& o) {
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
    project.index.shard(focus_id)->occurrences_at(point("method"), [&](const index::Occurrence& o) {
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
    project.index.shard(focus_id)->occurrences_at(point("hidden"), [&](const index::Occurrence& o) {
        hidden = o.target;
        return false;
    });
    ZASSERT(hidden != 0);
    ZASSERT(query.qualified_name(hidden, focus_id) == "outer::hidden");

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
    // serving source, which no symbol record names.
    index::SymbolQuery at;
    at.position = {.path = project.file_table.resolve(focus_id).str(), .line = 3, .column = 21};
    auto located = index::locate(query, at);
    ZASSERT(located.size() == std::size_t(1));
    ZASSERT(located.front().symbol.name == "helper");
    ZASSERT(located.front().site.path.ends_with("main.cpp"));
    // The line alone lists it too.
    at.position->column.reset();
    auto on_line = index::locate(query, at);
    ZASSERT(on_line.size() == std::size_t(1));
    ZASSERT(on_line.front().symbol.name == "helper");
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
    ZEXPECT(helper.front().symbol.scope == index::SymbolScope::Internal);
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
            llvm::MemoryBuffer::getMemBufferCopy(as_chars(index::build_tu_index(*unit)))));
    }
}

ZEST_CASE(ScopedSearchScalesLinearly) {
    auto visits_with = [&](int buffers) {
        CountingSessions sessions;
        open_buffers(sessions, buffers);
        index::IndexQuery session_query{project.index, project.file_table, nullptr, &sessions};
        index::search(session_query, *index::SymbolQuery::parse("app::fn0_1"), 10);
        return sessions.visits;
    };
    // Four times the open buffers: a scan of each buffer's table grows
    // four times, a lookup through every table per candidate sixteen.
    auto few = visits_with(2);
    ZASSERT(visits_with(8) <= few * 4);
}

};  // ZEST_SUITE(IndexSearch)

}  // namespace
}  // namespace clice::testing
