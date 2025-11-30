#include <iostream>

#include "Test/Test.h"
#include "Support/GlobPattern.h"

namespace clice::testing {

namespace {

#define PATDEF(NAME, PAT)                                                                          \
    const char* PatString_##NAME = PAT;                                                            \
    auto Res##NAME = clice::GlobPattern::create(PatString_##NAME, 100);                            \
    if(!Res##NAME.has_value()) {                                                                   \
        std::cout << Res##NAME.error() << '\n';                                                    \
    }                                                                                              \
    assert(Res##NAME.has_value());                                                                 \
    auto NAME = Res##NAME.value();

TEST_SUITE(GlobPattern) {

TEST_CASE(PatternSema) {
    auto Pat1 = clice::GlobPattern::create("**/****.{c,cc}", 100);
    ASSERT_FALSE(Pat1.has_value());

    auto Pat2 = clice::GlobPattern::create("/foo/bar/baz////aaa.{c,cc}", 100);
    ASSERT_FALSE(Pat2.has_value());

    auto Pat3 = clice::GlobPattern::create("/foo/bar/baz/**////*.{c,cc}", 100);
    ASSERT_FALSE(Pat3.has_value());
}

TEST_CASE(MaxSubGlob) {
    auto Pat1 = clice::GlobPattern::create("{AAA,BBB,AB*}");
    ASSERT_TRUE(Pat1.has_value());
    ASSERT_TRUE(Pat1->match("AAA"));
    ASSERT_TRUE(Pat1->match("BBB"));
    ASSERT_TRUE(Pat1->match("AB"));
    ASSERT_TRUE(Pat1->match("ABCD"));
    ASSERT_FALSE(Pat1->match("CCC"));
    ASSERT_TRUE(Pat1->match("ABCDE"));
}

TEST_CASE(Simple) {
    PATDEF(Pat1, "node_modules")
    ASSERT_TRUE(Pat1.match("node_modules"));
    ASSERT_FALSE(Pat1.match("node_module"));
    ASSERT_FALSE(Pat1.match("/node_modules"));
    ASSERT_FALSE(Pat1.match("test/node_modules"));

    PATDEF(Pat2, "test.txt")
    ASSERT_TRUE(Pat2.match("test.txt"));
    ASSERT_FALSE(Pat2.match("test?txt"));
    ASSERT_FALSE(Pat2.match("/text.txt"));
    ASSERT_FALSE(Pat2.match("test/test.txt"));

    PATDEF(Pat3, "test(.txt")
    ASSERT_TRUE(Pat3.match("test(.txt"));
    ASSERT_FALSE(Pat3.match("test?txt"));

    PATDEF(Pat4, "qunit")
    ASSERT_TRUE(Pat4.match("qunit"));
    ASSERT_FALSE(Pat4.match("qunit.css"));
    ASSERT_FALSE(Pat4.match("test/qunit"));

    PATDEF(Pat5, "/DNXConsoleApp/**/*.cs")
    ASSERT_TRUE(Pat5.match("/DNXConsoleApp/Program.cs"));
    ASSERT_TRUE(Pat5.match("/DNXConsoleApp/foo/Program.cs"));
}

TEST_CASE(DotHidden) {
    PATDEF(Pat1, ".*")
    ASSERT_TRUE(Pat1.match(".git"));
    ASSERT_TRUE(Pat1.match(".hidden.txt"));
    ASSERT_FALSE(Pat1.match("git"));
    ASSERT_FALSE(Pat1.match("hidden.txt"));
    ASSERT_FALSE(Pat1.match("path/.git"));
    ASSERT_FALSE(Pat1.match("path/.hidden.txt"));

    PATDEF(Pat2, "**/.*")
    ASSERT_TRUE(Pat2.match(".git"));
    ASSERT_TRUE(Pat2.match("/.git"));
    ASSERT_TRUE(Pat2.match(".hidden.txt"));
    ASSERT_FALSE(Pat2.match("git"));
    ASSERT_FALSE(Pat2.match("hidden.txt"));
    ASSERT_TRUE(Pat2.match("path/.git"));
    ASSERT_TRUE(Pat2.match("path/.hidden.txt"));
    ASSERT_TRUE(Pat2.match("/path/.git"));
    ASSERT_TRUE(Pat2.match("/path/.hidden.txt"));
    ASSERT_FALSE(Pat2.match("path/git"));
    ASSERT_FALSE(Pat2.match("pat.h/hidden.txt"));

    PATDEF(Pat3, "._*")
    ASSERT_TRUE(Pat3.match("._git"));
    ASSERT_TRUE(Pat3.match("._hidden.txt"));
    ASSERT_FALSE(Pat3.match("git"));
    ASSERT_FALSE(Pat3.match("hidden.txt"));
    ASSERT_FALSE(Pat3.match("path/._git"));
    ASSERT_FALSE(Pat3.match("path/._hidden.txt"));

    PATDEF(Pat4, "**/._*")
    ASSERT_TRUE(Pat4.match("._git"));
    ASSERT_TRUE(Pat4.match("._hidden.txt"));
    ASSERT_FALSE(Pat4.match("git"));
    ASSERT_FALSE(Pat4.match("hidden._txt"));
    ASSERT_TRUE(Pat4.match("path/._git"));
    ASSERT_TRUE(Pat4.match("path/._hidden.txt"));
    ASSERT_TRUE(Pat4.match("/path/._git"));
    ASSERT_TRUE(Pat4.match("/path/._hidden.txt"));
    ASSERT_FALSE(Pat4.match("path/git"));
    ASSERT_FALSE(Pat4.match("pat.h/hidden._txt"));
}

TEST_CASE(EscapeCharacter) {
    PATDEF(Pat1, R"(\*star)")
    ASSERT_TRUE(Pat1.match("*star"));

    PATDEF(Pat2, R"(\{\*\})")
    ASSERT_TRUE(Pat2.match("{*}"));
}

TEST_CASE(BracketExpr) {
    PATDEF(Pat1, R"([a-zA-Z\]])")
    ASSERT_TRUE(Pat1.match(R"(])"));
    ASSERT_FALSE(Pat1.match(R"([)"));
    ASSERT_TRUE(Pat1.match(R"(s)"));
    ASSERT_TRUE(Pat1.match(R"(S)"));
    ASSERT_FALSE(Pat1.match(R"(0)"));

    PATDEF(Pat2, R"([\\^a-zA-Z""\\])")
    ASSERT_TRUE(Pat2.match(R"(")"));
    ASSERT_TRUE(Pat2.match(R"(^)"));
    ASSERT_TRUE(Pat2.match(R"(\)"));
    ASSERT_TRUE(Pat2.match(R"(")"));
    ASSERT_TRUE(Pat2.match(R"(x)"));
    ASSERT_TRUE(Pat2.match(R"(X)"));
    ASSERT_FALSE(Pat2.match(R"(0)"));

    PATDEF(Pat3, R"([!0-9a-fA-F\-+\*])")
    ASSERT_FALSE(Pat3.match("1"));
    ASSERT_FALSE(Pat3.match("*"));
    ASSERT_TRUE(Pat3.match("s"));
    ASSERT_TRUE(Pat3.match("S"));
    ASSERT_TRUE(Pat3.match("H"));
    ASSERT_TRUE(Pat3.match("]"));

    PATDEF(Pat4, R"([^\^0-9a-fA-F\-+\*])")
    ASSERT_FALSE(Pat4.match("1"));
    ASSERT_FALSE(Pat4.match("*"));
    ASSERT_FALSE(Pat4.match("^"));
    ASSERT_TRUE(Pat4.match("s"));
    ASSERT_TRUE(Pat4.match("S"));
    ASSERT_TRUE(Pat4.match("H"));
    ASSERT_TRUE(Pat4.match("]"));

    PATDEF(Pat5, R"([\*-\^])")
    ASSERT_TRUE(Pat5.match("*"));
    ASSERT_FALSE(Pat5.match("a"));
    ASSERT_FALSE(Pat5.match("z"));
    ASSERT_TRUE(Pat5.match("A"));
    ASSERT_TRUE(Pat5.match("Z"));
    ASSERT_TRUE(Pat5.match("\\"));
    ASSERT_TRUE(Pat5.match("^"));
    ASSERT_TRUE(Pat5.match("-"));

    PATDEF(Pat6, "foo.[^0-9]")
    ASSERT_FALSE(Pat6.match("foo.5"));
    ASSERT_FALSE(Pat6.match("foo.8"));
    ASSERT_FALSE(Pat6.match("bar.5"));
    ASSERT_TRUE(Pat6.match("foo.f"));

    PATDEF(Pat7, "foo.[!0-9]")
    ASSERT_FALSE(Pat7.match("foo.5"));
    ASSERT_FALSE(Pat7.match("foo.8"));
    ASSERT_FALSE(Pat7.match("bar.5"));
    ASSERT_TRUE(Pat7.match("foo.f"));

    PATDEF(Pat8, "foo.[0!^*?]")
    ASSERT_FALSE(Pat8.match("foo.5"));
    ASSERT_FALSE(Pat8.match("foo.8"));
    ASSERT_TRUE(Pat8.match("foo.0"));
    ASSERT_TRUE(Pat8.match("foo.!"));
    ASSERT_TRUE(Pat8.match("foo.^"));
    ASSERT_TRUE(Pat8.match("foo.*"));
    ASSERT_TRUE(Pat8.match("foo.?"));

    PATDEF(Pat9, "foo[/]bar")
    ASSERT_FALSE(Pat9.match("foo/bar"));

    PATDEF(Pat10, "foo.[[]")
    ASSERT_TRUE(Pat10.match("foo.["));

    PATDEF(Pat11, "foo.[]]")
    ASSERT_TRUE(Pat11.match("foo.]"));

    PATDEF(Pat12, "foo.[][!]")
    ASSERT_TRUE(Pat12.match("foo.]"));
    ASSERT_TRUE(Pat12.match("foo.["));
    ASSERT_TRUE(Pat12.match("foo.!"));

    PATDEF(Pat13, "foo.[]-]")
    ASSERT_TRUE(Pat13.match("foo.]"));
    ASSERT_TRUE(Pat13.match("foo.-"));

    PATDEF(Pat14, "foo.[0-9]")
    ASSERT_TRUE(Pat14.match("foo.5"));
    ASSERT_TRUE(Pat14.match("foo.8"));
    ASSERT_FALSE(Pat14.match("bar.5"));
    ASSERT_FALSE(Pat14.match("foo.f"));
}

TEST_CASE(BraceExpr) {
    PATDEF(Pat1, "*foo[0-9a-z].{c,cpp,cppm,?pp}")
    ASSERT_FALSE(Pat1.match("foo1.cc"));
    ASSERT_TRUE(Pat1.match("foo2.cpp"));
    ASSERT_TRUE(Pat1.match("foo3.cppm"));
    ASSERT_TRUE(Pat1.match("foot.cppm"));
    ASSERT_TRUE(Pat1.match("foot.hpp"));
    ASSERT_TRUE(Pat1.match("foot.app"));
    ASSERT_FALSE(Pat1.match("fooD.cppm"));
    ASSERT_FALSE(Pat1.match("BarfooD.cppm"));
    ASSERT_FALSE(Pat1.match("foofooD.cppm"));

    PATDEF(Pat2, "proj/{build*,include,src}/*.{cc,cpp,h,hpp}")
    ASSERT_TRUE(Pat2.match("proj/include/foo.cc"));
    ASSERT_TRUE(Pat2.match("proj/include/bar.cpp"));
    ASSERT_FALSE(Pat2.match("proj/include/xxx/yyy/zzz/foo.cc"));
    ASSERT_TRUE(Pat2.match("proj/build-yyy/foo.h"));
    ASSERT_TRUE(Pat2.match("proj/build-xxx/foo.cpp"));
    ASSERT_TRUE(Pat2.match("proj/build/foo.cpp"));
    ASSERT_FALSE(Pat2.match("proj/build-xxx/xxx/yyy/zzz/foo.cpp"));

    PATDEF(Pat3, "*.{html,js}")
    ASSERT_TRUE(Pat3.match("foo.js"));
    ASSERT_TRUE(Pat3.match("foo.html"));
    ASSERT_FALSE(Pat3.match("folder/foo.js"));
    ASSERT_FALSE(Pat3.match("/node_modules/foo.js"));
    ASSERT_FALSE(Pat3.match("foo.jss"));
    ASSERT_FALSE(Pat3.match("some.js/test"));

    PATDEF(Pat4, "*.{html}")
    ASSERT_TRUE(Pat4.match("foo.html"));
    ASSERT_FALSE(Pat4.match("foo.js"));
    ASSERT_FALSE(Pat4.match("folder/foo.js"));
    ASSERT_FALSE(Pat4.match("/node_modules/foo.js"));
    ASSERT_FALSE(Pat4.match("foo.jss"));
    ASSERT_FALSE(Pat4.match("some.js/test"));

    PATDEF(Pat5, "{node_modules,testing}")
    ASSERT_TRUE(Pat5.match("node_modules"));
    ASSERT_TRUE(Pat5.match("testing"));
    ASSERT_FALSE(Pat5.match("node_module"));
    ASSERT_FALSE(Pat5.match("dtesting"));

    PATDEF(Pat6, "**/{foo,bar}")
    ASSERT_TRUE(Pat6.match("foo"));
    ASSERT_TRUE(Pat6.match("bar"));
    ASSERT_TRUE(Pat6.match("test/foo"));
    ASSERT_TRUE(Pat6.match("test/bar"));
    ASSERT_TRUE(Pat6.match("other/more/foo"));
    ASSERT_TRUE(Pat6.match("other/more/bar"));
    ASSERT_TRUE(Pat6.match("/foo"));
    ASSERT_TRUE(Pat6.match("/bar"));
    ASSERT_TRUE(Pat6.match("/test/foo"));
    ASSERT_TRUE(Pat6.match("/test/bar"));
    ASSERT_TRUE(Pat6.match("/other/more/foo"));
    ASSERT_TRUE(Pat6.match("/other/more/bar"));

    PATDEF(Pat7, "{foo,bar}/**")
    ASSERT_TRUE(Pat7.match("foo"));
    ASSERT_TRUE(Pat7.match("bar"));
    ASSERT_TRUE(Pat7.match("bar/"));
    ASSERT_TRUE(Pat7.match("foo/test"));
    ASSERT_TRUE(Pat7.match("bar/test"));
    ASSERT_TRUE(Pat7.match("bar/test/"));
    ASSERT_TRUE(Pat7.match("foo/other/more"));
    ASSERT_TRUE(Pat7.match("bar/other/more"));
    ASSERT_TRUE(Pat7.match("bar/other/more/"));

    PATDEF(Pat8, "{**/*.d.ts,**/*.js}")
    ASSERT_TRUE(Pat8.match("foo.js"));
    ASSERT_TRUE(Pat8.match("testing/foo.js"));
    ASSERT_TRUE(Pat8.match("/testing/foo.js"));
    ASSERT_TRUE(Pat8.match("foo.d.ts"));
    ASSERT_TRUE(Pat8.match("testing/foo.d.ts"));
    ASSERT_TRUE(Pat8.match("/testing/foo.d.ts"));
    ASSERT_FALSE(Pat8.match("foo.d"));
    ASSERT_FALSE(Pat8.match("testing/foo.d"));
    ASSERT_FALSE(Pat8.match("/testing/foo.d"));

    PATDEF(Pat9, "{**/*.d.ts,**/*.js,path/simple.jgs}")
    ASSERT_TRUE(Pat9.match("foo.js"));
    ASSERT_TRUE(Pat9.match("testing/foo.js"));
    ASSERT_TRUE(Pat9.match("/testing/foo.js"));
    ASSERT_TRUE(Pat9.match("path/simple.jgs"));
    ASSERT_FALSE(Pat9.match("/path/simple.jgs"));

    PATDEF(Pat10, "{**/*.d.ts,**/*.js,foo.[0-9]}")
    ASSERT_TRUE(Pat10.match("foo.5"));
    ASSERT_TRUE(Pat10.match("foo.8"));
    ASSERT_FALSE(Pat10.match("bar.5"));
    ASSERT_FALSE(Pat10.match("foo.f"));
    ASSERT_TRUE(Pat10.match("foo.js"));

    PATDEF(Pat11, "prefix/{**/*.d.ts,**/*.js,foo.[0-9]}")
    ASSERT_TRUE(Pat11.match("prefix/foo.5"));
    ASSERT_TRUE(Pat11.match("prefix/foo.8"));
    ASSERT_FALSE(Pat11.match("prefix/bar.5"));
    ASSERT_FALSE(Pat11.match("prefix/foo.f"));
    ASSERT_TRUE(Pat11.match("prefix/foo.js"));
}

TEST_CASE(WildGlob) {
    PATDEF(Pat1, "**/*")
    ASSERT_TRUE(Pat1.match("foo"));
    ASSERT_TRUE(Pat1.match("foo/bar/baz"));

    PATDEF(Pat2, "**/[0-9]*")
    ASSERT_TRUE(Pat2.match("114514foo"));
    ASSERT_FALSE(Pat2.match("foo/bar/baz/xxx/yyy/zzz"));
    ASSERT_FALSE(Pat2.match("foo/bar/baz/xxx/yyy/zzz114514"));
    ASSERT_TRUE(Pat2.match("foo/bar/baz/xxx/yyy/114514"));
    ASSERT_TRUE(Pat2.match("foo/bar/baz/xxx/yyy/114514zzz"));

    PATDEF(Pat3, "**/*[0-9]")
    ASSERT_TRUE(Pat3.match("foo5"));
    ASSERT_FALSE(Pat3.match("foo/bar/baz/xxx/yyy/zzz"));
    ASSERT_TRUE(Pat3.match("foo/bar/baz/xxx/yyy/zzz114514"));

    PATDEF(Pat4, "**/include/test/*.{cc,hh,c,h,cpp,hpp}")
    ASSERT_TRUE(Pat4.match("include/test/aaa.cc"));
    ASSERT_TRUE(Pat4.match("/include/test/aaa.cc"));
    ASSERT_TRUE(Pat4.match("xxx/yyy/include/test/aaa.cc"));
    ASSERT_TRUE(Pat4.match("include/foo/bar/baz/include/test/bbb.hh"));
    ASSERT_TRUE(Pat4.match("include/include/include/include/include/test/bbb.hpp"));

    PATDEF(Pat5, "**include/test/*.{cc,hh,c,h,cpp,hpp}")
    ASSERT_TRUE(Pat5.match("include/test/fff.hpp"));
    ASSERT_TRUE(Pat5.match("xxx-yyy-include/test/fff.hpp"));
    ASSERT_TRUE(Pat5.match("xxx-yyy-include/test/.hpp"));
    ASSERT_TRUE(Pat5.match("/include/test/aaa.cc"));
    ASSERT_TRUE(Pat5.match("include/foo/bar/baz/include/test/bbb.hh"));

    PATDEF(Pat6, "**/*foo.{c,cpp}")
    ASSERT_TRUE(Pat6.match("bar/foo.cpp"));
    ASSERT_TRUE(Pat6.match("bar/barfoo.cpp"));
    ASSERT_TRUE(Pat6.match("/foofoo.cpp"));
    ASSERT_TRUE(Pat6.match("foo/foo/foo/foo/foofoo.cpp"));
    ASSERT_TRUE(Pat6.match("foofoo.cpp"));
    ASSERT_TRUE(Pat6.match("barfoo.cpp"));
    ASSERT_TRUE(Pat6.match("foo.cpp"));

    // Boundary test of `**`
    PATDEF(Pat7, "**")
    ASSERT_TRUE(Pat7.match("foo"));
    ASSERT_TRUE(Pat7.match("foo/bar/baz"));

    PATDEF(Pat8, "x/**")
    ASSERT_TRUE(Pat8.match("x/"));
    ASSERT_TRUE(Pat8.match("x/foo/bar/baz"));
    ASSERT_TRUE(Pat8.match("x"));

    PATDEF(Pat9, "**/x")
    ASSERT_TRUE(Pat9.match("x"));
    ASSERT_TRUE(Pat9.match("/x"));
    ASSERT_TRUE(Pat9.match("/x/x/x/x/x"));

    PATDEF(Pat10, "**/*")
    ASSERT_TRUE(Pat10.match("foo"));
    ASSERT_TRUE(Pat10.match("foo/bar"));
    ASSERT_TRUE(Pat10.match("foo/bar/baz"));

    PATDEF(Pat11, "**/*.{cc,cpp}")
    ASSERT_TRUE(Pat11.match("foo/bar/baz.cc"));
    ASSERT_TRUE(Pat11.match("foo/foo/foo.cpp"));
    ASSERT_TRUE(Pat11.match("foo/bar/.cc"));

    PATDEF(Pat12, "**/*?.{cc,cpp}")
    ASSERT_TRUE(Pat12.match("foo/bar/baz/xxx/yyy/zzz/aaa.cc"));
    ASSERT_TRUE(Pat12.match("foo/bar/baz/xxx/yyy/zzz/a.cc"));
    ASSERT_FALSE(Pat12.match("foo/bar/baz/xxx/yyy/zzz/.cc"));

    PATDEF(Pat13, "**/?*.{cc,cpp}")
    ASSERT_TRUE(Pat13.match("foo/bar/baz/xxx/yyy/zzz/aaa.cc"));
    ASSERT_TRUE(Pat13.match("foo/bar/baz/xxx/yyy/zzz/a.cc"));
    ASSERT_FALSE(Pat13.match("foo/bar/baz/xxx/yyy/zzz/.cc"));

    PATDEF(Pat14, "**/*[0-9]")
    ASSERT_TRUE(Pat14.match("foo5"));
    ASSERT_FALSE(Pat14.match("foo/bar/baz/xxx/yyy/zzz"));
    ASSERT_TRUE(Pat14.match("foo/bar/baz/xxx/yyy/zzz114514"));

    PATDEF(Pat15, "**/*")
    ASSERT_TRUE(Pat15.match("foo"));
    ASSERT_TRUE(Pat15.match("foo/bar/baz"));

    PATDEF(Pat16, "**/*.js")
    ASSERT_TRUE(Pat16.match("foo.js"));
    ASSERT_TRUE(Pat16.match("/foo.js"));
    ASSERT_TRUE(Pat16.match("folder/foo.js"));
    ASSERT_TRUE(Pat16.match("/node_modules/foo.js"));
    ASSERT_FALSE(Pat16.match("foo.jss"));
    ASSERT_FALSE(Pat16.match("some.js/test"));
    ASSERT_FALSE(Pat16.match("/some.js/test"));

    PATDEF(Pat17, "**/project.json")
    ASSERT_TRUE(Pat17.match("project.json"));
    ASSERT_TRUE(Pat17.match("/project.json"));
    ASSERT_TRUE(Pat17.match("some/folder/project.json"));
    ASSERT_TRUE(Pat17.match("/some/folder/project.json"));
    ASSERT_FALSE(Pat17.match("some/folder/file_project.json"));
    ASSERT_FALSE(Pat17.match("some/folder/fileproject.json"));
    ASSERT_FALSE(Pat17.match("some/rrproject.json"));

    PATDEF(Pat18, "test/**")
    ASSERT_TRUE(Pat18.match("test"));
    ASSERT_TRUE(Pat18.match("test/foo"));
    ASSERT_TRUE(Pat18.match("test/foo/"));
    ASSERT_TRUE(Pat18.match("test/foo.js"));
    ASSERT_TRUE(Pat18.match("test/other/foo.js"));
    ASSERT_FALSE(Pat18.match("est/other/foo.js"));

    PATDEF(Pat19, "**")
    ASSERT_TRUE(Pat19.match("/"));
    ASSERT_TRUE(Pat19.match("foo.js"));
    ASSERT_TRUE(Pat19.match("folder/foo.js"));
    ASSERT_TRUE(Pat19.match("folder/foo/"));
    ASSERT_TRUE(Pat19.match("/node_modules/foo.js"));
    ASSERT_TRUE(Pat19.match("foo.jss"));
    ASSERT_TRUE(Pat19.match("some.js/test"));

    PATDEF(Pat20, "test/**/*.js")
    ASSERT_TRUE(Pat20.match("test/foo.js"));
    ASSERT_TRUE(Pat20.match("test/other/foo.js"));
    ASSERT_TRUE(Pat20.match("test/other/more/foo.js"));
    ASSERT_FALSE(Pat20.match("test/foo.ts"));
    ASSERT_FALSE(Pat20.match("test/other/foo.ts"));
    ASSERT_FALSE(Pat20.match("test/other/more/foo.ts"));

    PATDEF(Pat21, "**/**/*.js")
    ASSERT_TRUE(Pat21.match("foo.js"));
    ASSERT_TRUE(Pat21.match("/foo.js"));
    ASSERT_TRUE(Pat21.match("folder/foo.js"));
    ASSERT_TRUE(Pat21.match("/node_modules/foo.js"));
    ASSERT_FALSE(Pat21.match("foo.jss"));
    ASSERT_FALSE(Pat21.match("some.js/test"));

    PATDEF(Pat22, "**/node_modules/**/*.js")
    ASSERT_FALSE(Pat22.match("foo.js"));
    ASSERT_FALSE(Pat22.match("folder/foo.js"));
    ASSERT_TRUE(Pat22.match("node_modules/foo.js"));
    ASSERT_TRUE(Pat22.match("/node_modules/foo.js"));
    ASSERT_TRUE(Pat22.match("node_modules/some/folder/foo.js"));
    ASSERT_TRUE(Pat22.match("/node_modules/some/folder/foo.js"));
    ASSERT_FALSE(Pat22.match("node_modules/some/folder/foo.ts"));
    ASSERT_FALSE(Pat22.match("foo.jss"));
    ASSERT_FALSE(Pat22.match("some.js/test"));

    PATDEF(Pat23, "{**/node_modules/**,**/.git/**,**/bower_components/**}")
    ASSERT_TRUE(Pat23.match("node_modules"));
    ASSERT_TRUE(Pat23.match("/node_modules"));
    ASSERT_TRUE(Pat23.match("/node_modules/more"));
    ASSERT_TRUE(Pat23.match("some/test/node_modules"));
    ASSERT_TRUE(Pat23.match("/some/test/node_modules"));
    ASSERT_TRUE(Pat23.match("bower_components"));
    ASSERT_TRUE(Pat23.match("bower_components/more"));
    ASSERT_TRUE(Pat23.match("/bower_components"));
    ASSERT_TRUE(Pat23.match("some/test/bower_components"));
    ASSERT_TRUE(Pat23.match("/some/test/bower_components"));
    ASSERT_TRUE(Pat23.match(".git"));
    ASSERT_TRUE(Pat23.match("/.git"));
    ASSERT_TRUE(Pat23.match("some/test/.git"));
    ASSERT_TRUE(Pat23.match("/some/test/.git"));
    ASSERT_FALSE(Pat23.match("tempting"));
    ASSERT_FALSE(Pat23.match("/tempting"));
    ASSERT_FALSE(Pat23.match("some/test/tempting"));
    ASSERT_FALSE(Pat23.match("/some/test/tempting"));

    PATDEF(Pat24, "{**/package.json,**/project.json}")
    ASSERT_TRUE(Pat24.match("package.json"));
    ASSERT_TRUE(Pat24.match("/package.json"));
    ASSERT_FALSE(Pat24.match("xpackage.json"));
    ASSERT_FALSE(Pat24.match("/xpackage.json"));

    PATDEF(Pat25, "some/**/*.js")
    ASSERT_TRUE(Pat25.match("some/foo.js"));
    ASSERT_TRUE(Pat25.match("some/folder/foo.js"));
    ASSERT_FALSE(Pat25.match("something/foo.js"));
    ASSERT_FALSE(Pat25.match("something/folder/foo.js"));

    PATDEF(Pat26, "some/**/*")
    ASSERT_TRUE(Pat26.match("some/foo.js"));
    ASSERT_TRUE(Pat26.match("some/folder/foo.js"));
    ASSERT_FALSE(Pat26.match("something/foo.js"));
    ASSERT_FALSE(Pat26.match("something/folder/foo.js"));
}

};  // TEST_SUITE(GlobPattern)

}  // namespace

}  // namespace clice::testing
