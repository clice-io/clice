#include <string>
#include <vector>

#include "test/test.h"
#include "index/symbol_query.h"

#include "llvm/ADT/SmallVector.h"

namespace clice::testing {
namespace {

using index::SymbolQuery;
using Mode = SymbolQuery::Mode;

SymbolQuery parsed(llvm::StringRef text) {
    auto query = SymbolQuery::parse(text);
    return query ? *query : SymbolQuery{};
}

std::string error_of(llvm::StringRef text) {
    auto query = SymbolQuery::parse(text);
    return query ? "" : query.error();
}

std::vector<std::string> scope_names(const SymbolQuery& query) {
    std::vector<std::string> names;
    for(auto& segment: query.scope) {
        names.push_back(segment.name + segment.args);
    }
    return names;
}

/// A container chain from `a::b::c` spelling.
llvm::SmallVector<index::ScopeEntry> chain(std::initializer_list<llvm::StringRef> names) {
    llvm::SmallVector<index::ScopeEntry> entries;
    for(auto name: names) {
        auto open = name.find('<');
        if(open == llvm::StringRef::npos) {
            entries.push_back({.name = name, .args = {}});
        } else {
            entries.push_back({.name = name.take_front(open), .args = name.drop_front(open)});
        }
    }
    return entries;
}

TEST_SUITE(SymbolQuery) {

TEST_CASE(Modes) {
    auto fuzzy = parsed("foo");
    EXPECT_EQ(fuzzy.mode, Mode::Fuzzy);
    EXPECT_EQ(fuzzy.pattern, "foo");
    EXPECT_TRUE(fuzzy.scope.empty());
    EXPECT_TRUE(fuzzy.by_pattern());

    auto exact = parsed(R"("foo")");
    EXPECT_EQ(exact.mode, Mode::Exact);
    EXPECT_EQ(exact.pattern, "foo");

    auto glob = parsed("get*Name");
    EXPECT_EQ(glob.mode, Mode::Glob);
    EXPECT_EQ(glob.pattern, "get*Name");

    auto everything = parsed("");
    EXPECT_EQ(everything.mode, Mode::Fuzzy);
    EXPECT_TRUE(everything.pattern.empty());

    EXPECT_EQ(parsed("   ").mode, Mode::Fuzzy);
    EXPECT_EQ(parsed("*").mode, Mode::Members);
    EXPECT_EQ(parsed("**").mode, Mode::Subtree);
}

TEST_CASE(Scopes) {
    auto qualified = parsed("ns::Foo::bar");
    EXPECT_EQ(scope_names(qualified), (std::vector<std::string>{"ns", "Foo"}));
    EXPECT_EQ(qualified.pattern, "bar");
    EXPECT_FALSE(qualified.absolute);

    auto absolute = parsed("::ns::bar");
    EXPECT_TRUE(absolute.absolute);
    EXPECT_EQ(scope_names(absolute), (std::vector<std::string>{"ns"}));
    EXPECT_EQ(absolute.pattern, "bar");

    auto top = parsed("::bar");
    EXPECT_TRUE(top.absolute);
    EXPECT_TRUE(top.scope.empty());

    auto members = parsed("ns::*");
    EXPECT_EQ(members.mode, Mode::Members);
    EXPECT_EQ(scope_names(members), (std::vector<std::string>{"ns"}));
    EXPECT_EQ(parsed("ns::").mode, Mode::Members);
    EXPECT_TRUE(parsed("ns::").pattern.empty());

    auto subtree = parsed("ns::**");
    EXPECT_EQ(subtree.mode, Mode::Subtree);
    EXPECT_TRUE(subtree.direct() == false);
    EXPECT_TRUE(parsed("::ns::foo").direct());

    auto quoted = parsed(R"(ns::"foo")");
    EXPECT_EQ(quoted.mode, Mode::Exact);
    EXPECT_EQ(scope_names(quoted), (std::vector<std::string>{"ns"}));
    auto wholly_quoted = parsed(R"("ns::foo")");
    EXPECT_EQ(wholly_quoted.mode, Mode::Exact);
    EXPECT_EQ(scope_names(wholly_quoted), (std::vector<std::string>{"ns"}));
    EXPECT_EQ(wholly_quoted.pattern, "foo");
}

TEST_CASE(Arguments) {
    auto special = parsed("Widget<int>");
    EXPECT_EQ(special.pattern, "Widget");
    EXPECT_EQ(special.args, "<int>");
    auto nested = parsed("ns::Box<std::pair<int, int>>::get");
    EXPECT_EQ(scope_names(nested), (std::vector<std::string>{"ns", "Box<std::pair<int, int>>"}));
    EXPECT_EQ(nested.pattern, "get");
    EXPECT_EQ(parsed("operator<<").pattern, "operator<<");
    EXPECT_EQ(parsed("operator<=>").pattern, "operator<=>");
    EXPECT_EQ(parsed("operator>").pattern, "operator>");
    EXPECT_EQ(parsed("Foo::operator<").pattern, "operator<");
    EXPECT_EQ(parsed("operator->").pattern, "operator->");
    EXPECT_TRUE(parsed("operator<<").args.empty());
    EXPECT_TRUE(index::args_match("", "<int>"));
    EXPECT_TRUE(index::args_match("<int,4>", "<int, 4>"));
    EXPECT_FALSE(index::args_match("<int>", "<long>"));
    EXPECT_FALSE(index::args_match("<int>", ""));
}

TEST_CASE(HandlesAndPositions) {
    auto handle = parsed("#1a2b");
    EXPECT_TRUE(handle.handle.has_value());
    EXPECT_EQ(*handle.handle, index::SymbolHash(0x1a2b));
    EXPECT_FALSE(handle.by_pattern());

    auto line = parsed("src/a.cpp:120");
    EXPECT_TRUE(line.position.has_value());
    EXPECT_EQ(line.position->path, "src/a.cpp");
    EXPECT_EQ(line.position->line, 120);
    EXPECT_FALSE(line.position->column.has_value());

    auto cursor = parsed("a.h:12:8");
    EXPECT_EQ(cursor.position->path, "a.h");
    EXPECT_EQ(cursor.position->line, 12);
    EXPECT_EQ(cursor.position->column.value_or(0), 8);

    auto windows = parsed(R"(C:\src\a.cpp:3)");
    EXPECT_EQ(windows.position->path, R"(C:\src\a.cpp)");
    EXPECT_EQ(windows.position->line, 3);

    // A colon inside a name that is no file is just a name.
    EXPECT_FALSE(parsed("Foo:3").position.has_value());
    EXPECT_EQ(parsed("Foo:3").pattern, "Foo:3");
    EXPECT_EQ(error_of("a.cpp:0"), "lines and columns count from 1");
}

TEST_CASE(Filters) {
    auto filtered = parsed("foo kind:function,Method path:src/index/");
    EXPECT_EQ(filtered.pattern, "foo");
    EXPECT_EQ(filtered.kinds.size(), std::size_t(2));
    EXPECT_TRUE(filtered.kinds[0] == SymbolKind::Function);
    EXPECT_TRUE(filtered.kinds[1] == SymbolKind::Method);
    EXPECT_EQ(filtered.paths, (std::vector<std::string>{"src/index/"}));
    auto repeated = parsed("kind:struct kind:class Foo");
    EXPECT_EQ(repeated.kinds.size(), std::size_t(2));
    EXPECT_EQ(repeated.pattern, "Foo");
    EXPECT_EQ(error_of("foo kind:banana"), "unknown symbol kind 'banana'");
    EXPECT_EQ(error_of("foo bar"), "one name per query; 'bar' is a second");
    EXPECT_EQ(error_of(R"("foo)"), "unterminated quote");
    EXPECT_EQ(error_of("Foo<int"), "unbalanced '<'");
    EXPECT_EQ(error_of("*::foo"), "a scope names a container: '*'");
    auto spaced = parsed(R"(Box<int, 4> kind:struct)");
    EXPECT_EQ(spaced.pattern, "Box");
    EXPECT_EQ(spaced.args, "<int, 4>");
    EXPECT_EQ(spaced.kinds.size(), std::size_t(1));
}

TEST_CASE(Globs) {
    EXPECT_TRUE(index::glob_matches("foo*", "foobar"));
    EXPECT_TRUE(index::glob_matches("*_test", "unit_test"));
    EXPECT_FALSE(index::glob_matches("*_test", "unit_tests"));
    EXPECT_TRUE(index::glob_matches("get?Name", "getXName"));
    EXPECT_FALSE(index::glob_matches("get?Name", "getName"));
    EXPECT_TRUE(index::glob_matches("*foo*", "xfoox"));
    EXPECT_TRUE(index::glob_matches("foo", "FOO"));
    EXPECT_FALSE(index::glob_matches("Foo", "foo"));
    EXPECT_TRUE(index::glob_matches("*", ""));
    EXPECT_TRUE(index::glob_matches("a*b*c", "aXbYc"));
    EXPECT_FALSE(index::glob_matches("a*b*c", "aXcYb"));
    auto literals = index::glob_literals("get*Na?e*");
    EXPECT_EQ(literals.size(), std::size_t(3));
    EXPECT_EQ(literals[0], "get");
    EXPECT_EQ(literals[1], "Na");
    EXPECT_EQ(literals[2], "e");
}

TEST_CASE(Paths) {
    EXPECT_TRUE(index::path_matches("a.cpp", "/w/src/a.cpp"));
    EXPECT_FALSE(index::path_matches("a.cpp", "/w/src/ba.cpp"));
    EXPECT_TRUE(index::path_matches("src/a.cpp", "/w/src/a.cpp"));
    EXPECT_FALSE(index::path_matches("src/a.cpp", "/w/xsrc/a.cpp"));
    EXPECT_TRUE(index::path_matches("src/index/", "/w/src/index/a.cpp"));
    EXPECT_FALSE(index::path_matches("src/index/", "/w/src/indexer/a.cpp"));
    EXPECT_TRUE(index::path_matches("/w/src/", "/w/src/a.cpp"));
    EXPECT_FALSE(index::path_matches("/w/src/", "/x/w/src/a.cpp"));
    EXPECT_TRUE(index::path_matches(R"(src\a.cpp)", "C:/w/src/a.cpp"));
    EXPECT_TRUE(index::path_matches("C:/w/", R"(C:\w\src\a.cpp)"));
}

TEST_CASE(Scope) {
    auto sub = parsed("inner::paint");
    EXPECT_TRUE(index::in_scope(sub, chain({"outer", "inner", "Widget"})));
    EXPECT_TRUE(index::in_scope(sub, chain({"inner"})));
    EXPECT_FALSE(index::in_scope(sub, chain({"outer"})));
    EXPECT_FALSE(index::in_scope(parsed("inner::outer::paint"), chain({"outer", "inner"})));
    EXPECT_TRUE(index::in_scope(parsed("outer::paint"), chain({"outer", "inner", "Widget"})));
    EXPECT_TRUE(index::in_scope(parsed("paint"), chain({"outer"})));
    EXPECT_TRUE(index::in_scope(parsed("paint"), chain({})));

    auto absolute = parsed("::outer::inner::paint");
    EXPECT_TRUE(index::in_scope(absolute, chain({"outer", "inner"})));
    EXPECT_FALSE(index::in_scope(absolute, chain({"outer", "inner", "Widget"})));
    EXPECT_FALSE(index::in_scope(parsed("::inner::paint"), chain({"outer", "inner"})));
    EXPECT_TRUE(index::in_scope(parsed("::paint"), chain({})));
    EXPECT_FALSE(index::in_scope(parsed("::paint"), chain({"outer"})));

    auto members = parsed("inner::*");
    EXPECT_TRUE(index::in_scope(members, chain({"outer", "inner"})));
    EXPECT_FALSE(index::in_scope(members, chain({"outer", "inner", "Widget"})));
    EXPECT_TRUE(index::in_scope(parsed("outer::inner::*"), chain({"outer", "v2", "inner"})));
    EXPECT_TRUE(index::in_scope(parsed("*"), chain({"outer"})));
    EXPECT_TRUE(index::in_scope(parsed("::*"), chain({})));
    EXPECT_FALSE(index::in_scope(parsed("::*"), chain({"outer"})));

    auto subtree = parsed("inner::**");
    EXPECT_TRUE(index::in_scope(subtree, chain({"outer", "inner", "Widget"})));
    EXPECT_TRUE(index::in_scope(parsed("::outer::**"), chain({"outer", "inner"})));
    EXPECT_FALSE(index::in_scope(parsed("::inner::**"), chain({"outer", "inner"})));

    EXPECT_TRUE(index::in_scope(parsed("Widget<int>::paint"), chain({"inner", "Widget<int>"})));
    EXPECT_FALSE(index::in_scope(parsed("Widget<int>::paint"), chain({"inner", "Widget<long>"})));
    EXPECT_TRUE(index::in_scope(parsed("Widget::paint"), chain({"inner", "Widget<int>"})));
    EXPECT_TRUE(index::in_scope(parsed("INNER::paint"), chain({"inner"})));
}

};  // TEST_SUITE(SymbolQuery)

}  // namespace
}  // namespace clice::testing
