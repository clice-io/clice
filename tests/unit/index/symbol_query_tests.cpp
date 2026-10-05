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
    ZEXPECT(fuzzy.mode == Mode::Fuzzy);
    ZEXPECT(fuzzy.pattern == "foo");
    ZEXPECT(fuzzy.scope.empty());
    ZEXPECT(fuzzy.by_pattern());

    auto exact = parsed(R"("foo")");
    ZEXPECT(exact.mode == Mode::Exact);
    ZEXPECT(exact.pattern == "foo");

    auto glob = parsed("get*Name");
    ZEXPECT(glob.mode == Mode::Glob);
    ZEXPECT(glob.pattern == "get*Name");

    auto everything = parsed("");
    ZEXPECT(everything.mode == Mode::Fuzzy);
    ZEXPECT(everything.pattern.empty());

    ZEXPECT(parsed("   ").mode == Mode::Fuzzy);
    ZEXPECT(parsed("*").mode == Mode::Members);
    ZEXPECT(parsed("**").mode == Mode::Subtree);
}

ZEST_CASE(Scopes) {
    auto qualified = parsed("ns::Foo::bar");
    ZEXPECT(scope_names(qualified) == (std::vector<std::string>{"ns", "Foo"}));
    ZEXPECT(qualified.pattern == "bar");
    ZEXPECT(!qualified.absolute);

    auto absolute = parsed("::ns::bar");
    ZEXPECT(absolute.absolute);
    ZEXPECT(scope_names(absolute) == (std::vector<std::string>{"ns"}));
    ZEXPECT(absolute.pattern == "bar");

    auto top = parsed("::bar");
    ZEXPECT(top.absolute);
    ZEXPECT(top.scope.empty());

    auto members = parsed("ns::*");
    ZEXPECT(members.mode == Mode::Members);
    ZEXPECT(scope_names(members) == (std::vector<std::string>{"ns"}));
    ZEXPECT(parsed("ns::").mode == Mode::Members);
    ZEXPECT(parsed("ns::").pattern.empty());

    auto subtree = parsed("ns::**");
    ZEXPECT(subtree.mode == Mode::Subtree);
    ZEXPECT(subtree.direct() == false);
    ZEXPECT(parsed("::ns::foo").direct());

    auto quoted = parsed(R"(ns::"foo")");
    ZEXPECT(quoted.mode == Mode::Exact);
    ZEXPECT(scope_names(quoted) == (std::vector<std::string>{"ns"}));
    auto wholly_quoted = parsed(R"("ns::foo")");
    ZEXPECT(wholly_quoted.mode == Mode::Exact);
    ZEXPECT(scope_names(wholly_quoted) == (std::vector<std::string>{"ns"}));
    ZEXPECT(wholly_quoted.pattern == "foo");
    auto quoted_args = parsed(R"("ns::Box<std::string>")");
    ZEXPECT(quoted_args.mode == Mode::Exact);
    ZEXPECT(scope_names(quoted_args) == (std::vector<std::string>{"ns"}));
    ZEXPECT(quoted_args.pattern == "Box");
    ZEXPECT(quoted_args.args == "<std::string>");
}

ZEST_CASE(Arguments) {
    auto special = parsed("Widget<int>");
    ZEXPECT(special.pattern == "Widget");
    ZEXPECT(special.args == "<int>");
    auto nested = parsed("ns::Box<std::pair<int, int>>::get");
    ZEXPECT(scope_names(nested) == (std::vector<std::string>{"ns", "Box<std::pair<int, int>>"}));
    ZEXPECT(nested.pattern == "get");
    ZEXPECT(parsed("operator<<").pattern == "operator<<");
    ZEXPECT(parsed("operator<=>").pattern == "operator<=>");
    ZEXPECT(parsed("operator>").pattern == "operator>");
    ZEXPECT(parsed("Foo::operator<").pattern == "operator<");
    ZEXPECT(parsed("operator->").pattern == "operator->");
    ZEXPECT(parsed("operator<<").args.empty());
    auto named = parsed("binary_operator<int>");
    ZEXPECT(named.pattern == "binary_operator");
    ZEXPECT(named.args == "<int>");
    auto member = parsed("binary_operator<int>::apply");
    ZEXPECT(scope_names(member) == (std::vector<std::string>{"binary_operator<int>"}));
    ZEXPECT(member.pattern == "apply");
    ZEXPECT(index::args_match("", "<int>"));
    ZEXPECT(index::args_match("<int,4>", "<int, 4>"));
    ZEXPECT(!index::args_match("<int>", "<long>"));
    ZEXPECT(!index::args_match("<int>", ""));
}

ZEST_CASE(HandlesAndPositions) {
    auto handle = parsed("#1a2b");
    ZEXPECT(handle.handle);
    ZEXPECT(*handle.handle == index::SymbolHash(0x1a2b));
    ZEXPECT(!handle.by_pattern());
    ZEXPECT(error_of("#xyz") == "invalid symbol id '#xyz'");
    ZEXPECT(error_of("#") == "invalid symbol id '#'");

    auto line = parsed("src/a.cpp:120");
    ZEXPECT(line.position);
    ZEXPECT(line.position->path == "src/a.cpp");
    ZEXPECT(line.position->line == 120);
    ZEXPECT(!line.position->column.has_value());

    auto cursor = parsed("a.h:12:8");
    ZEXPECT(cursor.position->path == "a.h");
    ZEXPECT(cursor.position->line == 12);
    ZEXPECT(cursor.position->column.value_or(0) == 8);

    auto windows = parsed(R"(C:\src\a.cpp:3)");
    ZEXPECT(windows.position->path == R"(C:\src\a.cpp)");
    ZEXPECT(windows.position->line == 3);

    // A colon inside a name that is no file is just a name.
    ZEXPECT(!parsed("Foo:3").position.has_value());
    ZEXPECT(parsed("Foo:3").pattern == "Foo:3");
    ZEXPECT(error_of("a.cpp:0") == "lines and columns count from 1");
}

ZEST_CASE(Filters) {
    auto filtered = parsed("foo kind:function,Method path:src/index/");
    ZEXPECT(filtered.pattern == "foo");
    ZEXPECT(filtered.kinds.size() == std::size_t(2));
    ZEXPECT(filtered.kinds[0] == SymbolKind::Function);
    ZEXPECT(filtered.kinds[1] == SymbolKind::Method);
    ZEXPECT(filtered.paths == (std::vector<std::string>{"src/index/"}));
    auto spaced_path = parsed(R"(foo path:"src/my file.cpp")");
    ZEXPECT(spaced_path.paths == (std::vector<std::string>{"src/my file.cpp"}));
    auto spaced_place = parsed(R"("src/my file.cpp:12")");
    ZEXPECT(spaced_place.position);
    ZEXPECT(spaced_place.position->path == "src/my file.cpp");
    auto repeated = parsed("kind:struct kind:class Foo");
    ZEXPECT(repeated.kinds.size() == std::size_t(2));
    ZEXPECT(repeated.pattern == "Foo");
    ZEXPECT(error_of("foo kind:banana") == "unknown symbol kind 'banana'");
    ZEXPECT(error_of("foo bar") == "one name per query; 'bar' is a second");
    ZEXPECT(error_of(R"("foo)") == "unterminated quote");
    ZEXPECT(error_of("Foo<int") == "unbalanced '<'");
    ZEXPECT(error_of("*::foo") == "a scope names a container: '*'");
    auto spaced = parsed(R"(Box<int, 4> kind:struct)");
    ZEXPECT(spaced.pattern == "Box");
    ZEXPECT(spaced.args == "<int, 4>");
    ZEXPECT(spaced.kinds.size() == std::size_t(1));
}

ZEST_CASE(Globs) {
    ZEXPECT(index::glob_matches("foo*", "foobar"));
    ZEXPECT(index::glob_matches("*_test", "unit_test"));
    ZEXPECT(!index::glob_matches("*_test", "unit_tests"));
    ZEXPECT(index::glob_matches("get?Name", "getXName"));
    ZEXPECT(!index::glob_matches("get?Name", "getName"));
    ZEXPECT(index::glob_matches("*foo*", "xfoox"));
    ZEXPECT(index::glob_matches("foo", "FOO"));
    ZEXPECT(!index::glob_matches("Foo", "foo"));
    ZEXPECT(index::glob_matches("*", ""));
    ZEXPECT(index::glob_matches("a*b*c", "aXbYc"));
    ZEXPECT(!index::glob_matches("a*b*c", "aXcYb"));
    auto literals = index::glob_literals("get*Na?e*");
    ZEXPECT(literals.size() == std::size_t(3));
    ZEXPECT(literals[0] == "get");
    ZEXPECT(literals[1] == "Na");
    ZEXPECT(literals[2] == "e");
}

ZEST_CASE(Paths) {
    ZEXPECT(index::path_matches("a.cpp", "/w/src/a.cpp"));
    ZEXPECT(!index::path_matches("a.cpp", "/w/src/ba.cpp"));
    ZEXPECT(index::path_matches("src/a.cpp", "/w/src/a.cpp"));
    ZEXPECT(!index::path_matches("src/a.cpp", "/w/xsrc/a.cpp"));
    ZEXPECT(index::path_matches("src/index/", "/w/src/index/a.cpp"));
    ZEXPECT(!index::path_matches("src/index/", "/w/src/indexer/a.cpp"));
    ZEXPECT(index::path_matches("/w/src/", "/w/src/a.cpp"));
    ZEXPECT(index::path_matches("/w/src/a.cpp", "/w/src/a.cpp"));
    ZEXPECT(!index::path_matches("/w/src/a.cpp", "/w/src/a.cpp2"));
    ZEXPECT(index::path_matches("/w/src", "/w/src/a.cpp"));
    ZEXPECT(!index::path_matches("/w/src/", "/x/w/src/a.cpp"));
    ZEXPECT(index::path_matches(R"(src\a.cpp)", "C:/w/src/a.cpp"));
    ZEXPECT(index::path_matches("C:/w/", R"(C:\w\src\a.cpp)"));
}

ZEST_CASE(Scope) {
    auto sub = parsed("inner::paint");
    ZEXPECT(index::in_scope(sub, chain({"outer", "inner", "Widget"})));
    ZEXPECT(index::in_scope(sub, chain({"inner"})));
    ZEXPECT(!index::in_scope(sub, chain({"outer"})));
    ZEXPECT(!index::in_scope(parsed("inner::outer::paint"), chain({"outer", "inner"})));
    ZEXPECT(index::in_scope(parsed("outer::paint"), chain({"outer", "inner", "Widget"})));
    ZEXPECT(index::in_scope(parsed("paint"), chain({"outer"})));
    ZEXPECT(index::in_scope(parsed("paint"), chain({})));

    auto absolute = parsed("::outer::inner::paint");
    ZEXPECT(index::in_scope(absolute, chain({"outer", "inner"})));
    ZEXPECT(!index::in_scope(absolute, chain({"outer", "inner", "Widget"})));
    ZEXPECT(!index::in_scope(parsed("::inner::paint"), chain({"outer", "inner"})));
    ZEXPECT(index::in_scope(parsed("::paint"), chain({})));
    ZEXPECT(!index::in_scope(parsed("::paint"), chain({"outer"})));

    auto members = parsed("inner::*");
    ZEXPECT(index::in_scope(members, chain({"outer", "inner"})));
    ZEXPECT(!index::in_scope(members, chain({"outer", "inner", "Widget"})));
    ZEXPECT(index::in_scope(parsed("outer::inner::*"), chain({"outer", "v2", "inner"})));
    ZEXPECT(index::in_scope(parsed("*"), chain({"outer"})));
    ZEXPECT(index::in_scope(parsed("::*"), chain({})));
    ZEXPECT(!index::in_scope(parsed("::*"), chain({"outer"})));

    auto subtree = parsed("inner::**");
    ZEXPECT(index::in_scope(subtree, chain({"outer", "inner", "Widget"})));
    ZEXPECT(index::in_scope(parsed("::outer::**"), chain({"outer", "inner"})));
    ZEXPECT(!index::in_scope(parsed("::inner::**"), chain({"outer", "inner"})));

    ZEXPECT(index::in_scope(parsed("Widget<int>::paint"), chain({"inner", "Widget<int>"})));
    ZEXPECT(!index::in_scope(parsed("Widget<int>::paint"), chain({"inner", "Widget<long>"})));
    ZEXPECT(index::in_scope(parsed("Widget::paint"), chain({"inner", "Widget<int>"})));
    ZEXPECT(index::in_scope(parsed("INNER::paint"), chain({"inner"})));
}

};  // ZEST_SUITE(SymbolQuery)

}  // namespace
}  // namespace clice::testing
