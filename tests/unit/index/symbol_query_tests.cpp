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

ZEST_SUITE(SymbolQuery) {

ZEST_CASE(Modes) {
    auto fuzzy = parsed("foo");
    EXPECT(fuzzy.mode == Mode::Fuzzy);
    EXPECT(fuzzy.pattern == "foo");
    EXPECT(fuzzy.scope.empty());
    EXPECT(fuzzy.by_pattern());

    auto exact = parsed(R"("foo")");
    EXPECT(exact.mode == Mode::Exact);
    EXPECT(exact.pattern == "foo");

    auto glob = parsed("get*Name");
    EXPECT(glob.mode == Mode::Glob);
    EXPECT(glob.pattern == "get*Name");

    auto everything = parsed("");
    EXPECT(everything.mode == Mode::Fuzzy);
    EXPECT(everything.pattern.empty());

    EXPECT(parsed("   ").mode == Mode::Fuzzy);
    EXPECT(parsed("*").mode == Mode::Members);
    EXPECT(parsed("**").mode == Mode::Subtree);
}

ZEST_CASE(Scopes) {
    auto qualified = parsed("ns::Foo::bar");
    EXPECT(scope_names(qualified) == (std::vector<std::string>{"ns", "Foo"}));
    EXPECT(qualified.pattern == "bar");
    EXPECT(!qualified.absolute);

    auto absolute = parsed("::ns::bar");
    EXPECT(absolute.absolute);
    EXPECT(scope_names(absolute) == (std::vector<std::string>{"ns"}));
    EXPECT(absolute.pattern == "bar");

    auto top = parsed("::bar");
    EXPECT(top.absolute);
    EXPECT(top.scope.empty());

    auto members = parsed("ns::*");
    EXPECT(members.mode == Mode::Members);
    EXPECT(scope_names(members) == (std::vector<std::string>{"ns"}));
    EXPECT(parsed("ns::").mode == Mode::Members);
    EXPECT(parsed("ns::").pattern.empty());

    auto subtree = parsed("ns::**");
    EXPECT(subtree.mode == Mode::Subtree);
    EXPECT(subtree.direct() == false);
    EXPECT(parsed("::ns::foo").direct());

    auto quoted = parsed(R"(ns::"foo")");
    EXPECT(quoted.mode == Mode::Exact);
    EXPECT(scope_names(quoted) == (std::vector<std::string>{"ns"}));
    auto wholly_quoted = parsed(R"("ns::foo")");
    EXPECT(wholly_quoted.mode == Mode::Exact);
    EXPECT(scope_names(wholly_quoted) == (std::vector<std::string>{"ns"}));
    EXPECT(wholly_quoted.pattern == "foo");
    auto quoted_args = parsed(R"("ns::Box<std::string>")");
    EXPECT(quoted_args.mode == Mode::Exact);
    EXPECT(scope_names(quoted_args) == (std::vector<std::string>{"ns"}));
    EXPECT(quoted_args.pattern == "Box");
    EXPECT(quoted_args.args == "<std::string>");
}

ZEST_CASE(Arguments) {
    auto special = parsed("Widget<int>");
    EXPECT(special.pattern == "Widget");
    EXPECT(special.args == "<int>");
    auto nested = parsed("ns::Box<std::pair<int, int>>::get");
    EXPECT(scope_names(nested) == (std::vector<std::string>{"ns", "Box<std::pair<int, int>>"}));
    EXPECT(nested.pattern == "get");
    EXPECT(parsed("operator<<").pattern == "operator<<");
    EXPECT(parsed("operator<=>").pattern == "operator<=>");
    EXPECT(parsed("operator>").pattern == "operator>");
    EXPECT(parsed("Foo::operator<").pattern == "operator<");
    EXPECT(parsed("operator->").pattern == "operator->");
    EXPECT(parsed("operator<<").args.empty());
    auto named = parsed("binary_operator<int>");
    EXPECT(named.pattern == "binary_operator");
    EXPECT(named.args == "<int>");
    auto member = parsed("binary_operator<int>::apply");
    EXPECT(scope_names(member) == (std::vector<std::string>{"binary_operator<int>"}));
    EXPECT(member.pattern == "apply");
    EXPECT(index::args_match("", "<int>"));
    EXPECT(index::args_match("<int,4>", "<int, 4>"));
    EXPECT(!index::args_match("<int>", "<long>"));
    EXPECT(!index::args_match("<int>", ""));
}

ZEST_CASE(HandlesAndPositions) {
    auto handle = parsed("#1a2b");
    EXPECT(handle.handle);
    EXPECT(*handle.handle == index::SymbolHash(0x1a2b));
    EXPECT(!handle.by_pattern());
    EXPECT(error_of("#xyz") == "invalid symbol id '#xyz'");
    EXPECT(error_of("#") == "invalid symbol id '#'");

    auto line = parsed("src/a.cpp:120");
    EXPECT(line.position);
    EXPECT(line.position->path == "src/a.cpp");
    EXPECT(line.position->line == 120);
    EXPECT(!line.position->column.has_value());

    auto cursor = parsed("a.h:12:8");
    EXPECT(cursor.position->path == "a.h");
    EXPECT(cursor.position->line == 12);
    EXPECT(cursor.position->column.value_or(0) == 8);

    auto windows = parsed(R"(C:\src\a.cpp:3)");
    EXPECT(windows.position->path == R"(C:\src\a.cpp)");
    EXPECT(windows.position->line == 3);

    // A colon inside a name that is no file is just a name.
    EXPECT(!parsed("Foo:3").position.has_value());
    EXPECT(parsed("Foo:3").pattern == "Foo:3");
    EXPECT(error_of("a.cpp:0") == "lines and columns count from 1");
}

ZEST_CASE(Filters) {
    auto filtered = parsed("foo kind:function,Method path:src/index/");
    EXPECT(filtered.pattern == "foo");
    EXPECT(filtered.kinds.size() == std::size_t(2));
    EXPECT(filtered.kinds[0] == SymbolKind::Function);
    EXPECT(filtered.kinds[1] == SymbolKind::Method);
    EXPECT(filtered.paths == (std::vector<std::string>{"src/index/"}));
    auto spaced_path = parsed(R"(foo path:"src/my file.cpp")");
    EXPECT(spaced_path.paths == (std::vector<std::string>{"src/my file.cpp"}));
    auto spaced_place = parsed(R"("src/my file.cpp:12")");
    EXPECT(spaced_place.position);
    EXPECT(spaced_place.position->path == "src/my file.cpp");
    auto repeated = parsed("kind:struct kind:class Foo");
    EXPECT(repeated.kinds.size() == std::size_t(2));
    EXPECT(repeated.pattern == "Foo");
    EXPECT(error_of("foo kind:banana") == "unknown symbol kind 'banana'");
    EXPECT(error_of("foo bar") == "one name per query; 'bar' is a second");
    EXPECT(error_of(R"("foo)") == "unterminated quote");
    EXPECT(error_of("Foo<int") == "unbalanced '<'");
    EXPECT(error_of("*::foo") == "a scope names a container: '*'");
    auto spaced = parsed(R"(Box<int, 4> kind:struct)");
    EXPECT(spaced.pattern == "Box");
    EXPECT(spaced.args == "<int, 4>");
    EXPECT(spaced.kinds.size() == std::size_t(1));
}

ZEST_CASE(Globs) {
    EXPECT(index::glob_matches("foo*", "foobar"));
    EXPECT(index::glob_matches("*_test", "unit_test"));
    EXPECT(!index::glob_matches("*_test", "unit_tests"));
    EXPECT(index::glob_matches("get?Name", "getXName"));
    EXPECT(!index::glob_matches("get?Name", "getName"));
    EXPECT(index::glob_matches("*foo*", "xfoox"));
    EXPECT(index::glob_matches("foo", "FOO"));
    EXPECT(!index::glob_matches("Foo", "foo"));
    EXPECT(index::glob_matches("*", ""));
    EXPECT(index::glob_matches("a*b*c", "aXbYc"));
    EXPECT(!index::glob_matches("a*b*c", "aXcYb"));
    auto literals = index::glob_literals("get*Na?e*");
    EXPECT(literals.size() == std::size_t(3));
    EXPECT(literals[0] == "get");
    EXPECT(literals[1] == "Na");
    EXPECT(literals[2] == "e");
}

ZEST_CASE(Paths) {
    EXPECT(index::path_matches("a.cpp", "/w/src/a.cpp"));
    EXPECT(!index::path_matches("a.cpp", "/w/src/ba.cpp"));
    EXPECT(index::path_matches("src/a.cpp", "/w/src/a.cpp"));
    EXPECT(!index::path_matches("src/a.cpp", "/w/xsrc/a.cpp"));
    EXPECT(index::path_matches("src/index/", "/w/src/index/a.cpp"));
    EXPECT(!index::path_matches("src/index/", "/w/src/indexer/a.cpp"));
    EXPECT(index::path_matches("/w/src/", "/w/src/a.cpp"));
    EXPECT(index::path_matches("/w/src/a.cpp", "/w/src/a.cpp"));
    EXPECT(!index::path_matches("/w/src/a.cpp", "/w/src/a.cpp2"));
    EXPECT(index::path_matches("/w/src", "/w/src/a.cpp"));
    EXPECT(!index::path_matches("/w/src/", "/x/w/src/a.cpp"));
    EXPECT(index::path_matches(R"(src\a.cpp)", "C:/w/src/a.cpp"));
    EXPECT(index::path_matches("C:/w/", R"(C:\w\src\a.cpp)"));
}

ZEST_CASE(Scope) {
    auto sub = parsed("inner::paint");
    EXPECT(index::in_scope(sub, chain({"outer", "inner", "Widget"})));
    EXPECT(index::in_scope(sub, chain({"inner"})));
    EXPECT(!index::in_scope(sub, chain({"outer"})));
    EXPECT(!index::in_scope(parsed("inner::outer::paint"), chain({"outer", "inner"})));
    EXPECT(index::in_scope(parsed("outer::paint"), chain({"outer", "inner", "Widget"})));
    EXPECT(index::in_scope(parsed("paint"), chain({"outer"})));
    EXPECT(index::in_scope(parsed("paint"), chain({})));

    auto absolute = parsed("::outer::inner::paint");
    EXPECT(index::in_scope(absolute, chain({"outer", "inner"})));
    EXPECT(!index::in_scope(absolute, chain({"outer", "inner", "Widget"})));
    EXPECT(!index::in_scope(parsed("::inner::paint"), chain({"outer", "inner"})));
    EXPECT(index::in_scope(parsed("::paint"), chain({})));
    EXPECT(!index::in_scope(parsed("::paint"), chain({"outer"})));

    auto members = parsed("inner::*");
    EXPECT(index::in_scope(members, chain({"outer", "inner"})));
    EXPECT(!index::in_scope(members, chain({"outer", "inner", "Widget"})));
    EXPECT(index::in_scope(parsed("outer::inner::*"), chain({"outer", "v2", "inner"})));
    EXPECT(index::in_scope(parsed("*"), chain({"outer"})));
    EXPECT(index::in_scope(parsed("::*"), chain({})));
    EXPECT(!index::in_scope(parsed("::*"), chain({"outer"})));

    auto subtree = parsed("inner::**");
    EXPECT(index::in_scope(subtree, chain({"outer", "inner", "Widget"})));
    EXPECT(index::in_scope(parsed("::outer::**"), chain({"outer", "inner"})));
    EXPECT(!index::in_scope(parsed("::inner::**"), chain({"outer", "inner"})));

    EXPECT(index::in_scope(parsed("Widget<int>::paint"), chain({"inner", "Widget<int>"})));
    EXPECT(!index::in_scope(parsed("Widget<int>::paint"), chain({"inner", "Widget<long>"})));
    EXPECT(index::in_scope(parsed("Widget::paint"), chain({"inner", "Widget<int>"})));
    EXPECT(index::in_scope(parsed("INNER::paint"), chain({"inner"})));
}

};  // ZEST_SUITE(SymbolQuery)

}  // namespace
}  // namespace clice::testing
