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

suite<"GlobPattern"> glob_pattern_tests = [] {
    test("PatternSema") = [&] {
        auto Pat1 = clice::GlobPattern::create("**/****.{c,cc}", 100);
        expect(that % Pat1.has_value() == false);

        auto Pat2 = clice::GlobPattern::create("/foo/bar/baz////aaa.{c,cc}", 100);
        expect(that % Pat2.has_value() == false);

        auto Pat3 = clice::GlobPattern::create("/foo/bar/baz/**////*.{c,cc}", 100);
        expect(that % Pat3.has_value() == false);
    };

    test("MaxSubGlob") = [&] {
        auto Pat1 = clice::GlobPattern::create("{AAA,BBB,AB*}");
        expect(that % Pat1.has_value() == true);
        expect(that % Pat1->match("AAA") == true);
        expect(that % Pat1->match("BBB") == true);
        expect(that % Pat1->match("AB") == true);
        expect(that % Pat1->match("ABCD") == true);
        expect(that % Pat1->match("CCC") == false);
        expect(that % Pat1->match("ABCDE") == true);
    };

    test("Simple") = [&] {
        PATDEF(Pat1, "node_modules")
        expect(that % Pat1.match("node_modules") == true);
        expect(that % Pat1.match("node_module") == false);
        expect(that % Pat1.match("/node_modules") == false);
        expect(that % Pat1.match("test/node_modules") == false);

        PATDEF(Pat2, "test.txt")
        expect(that % Pat2.match("test.txt") == true);
        expect(that % Pat2.match("test?txt") == false);
        expect(that % Pat2.match("/text.txt") == false);
        expect(that % Pat2.match("test/test.txt") == false);

        PATDEF(Pat3, "test(.txt")
        expect(that % Pat3.match("test(.txt") == true);
        expect(that % Pat3.match("test?txt") == false);

        PATDEF(Pat4, "qunit")
        expect(that % Pat4.match("qunit") == true);
        expect(that % Pat4.match("qunit.css") == false);
        expect(that % Pat4.match("test/qunit") == false);

        PATDEF(Pat5, "/DNXConsoleApp/**/*.cs")
        expect(that % Pat5.match("/DNXConsoleApp/Program.cs") == true);
        expect(that % Pat5.match("/DNXConsoleApp/foo/Program.cs") == true);
    };

    test("DotHidden") = [&] {
        PATDEF(Pat1, ".*")
        expect(that % Pat1.match(".git") == true);
        expect(that % Pat1.match(".hidden.txt") == true);
        expect(that % Pat1.match("git") == false);
        expect(that % Pat1.match("hidden.txt") == false);
        expect(that % Pat1.match("path/.git") == false);
        expect(that % Pat1.match("path/.hidden.txt") == false);

        PATDEF(Pat2, "**/.*")
        expect(that % Pat2.match(".git") == true);
        expect(that % Pat2.match("/.git") == true);
        expect(that % Pat2.match(".hidden.txt") == true);
        expect(that % Pat2.match("git") == false);
        expect(that % Pat2.match("hidden.txt") == false);
        expect(that % Pat2.match("path/.git") == true);
        expect(that % Pat2.match("path/.hidden.txt") == true);
        expect(that % Pat2.match("/path/.git") == true);
        expect(that % Pat2.match("/path/.hidden.txt") == true);
        expect(that % Pat2.match("path/git") == false);
        expect(that % Pat2.match("pat.h/hidden.txt") == false);

        PATDEF(Pat3, "._*")
        expect(that % Pat3.match("._git") == true);
        expect(that % Pat3.match("._hidden.txt") == true);
        expect(that % Pat3.match("git") == false);
        expect(that % Pat3.match("hidden.txt") == false);
        expect(that % Pat3.match("path/._git") == false);
        expect(that % Pat3.match("path/._hidden.txt") == false);

        PATDEF(Pat4, "**/._*")
        expect(that % Pat4.match("._git") == true);
        expect(that % Pat4.match("._hidden.txt") == true);
        expect(that % Pat4.match("git") == false);
        expect(that % Pat4.match("hidden._txt") == false);
        expect(that % Pat4.match("path/._git") == true);
        expect(that % Pat4.match("path/._hidden.txt") == true);
        expect(that % Pat4.match("/path/._git") == true);
        expect(that % Pat4.match("/path/._hidden.txt") == true);
        expect(that % Pat4.match("path/git") == false);
        expect(that % Pat4.match("pat.h/hidden._txt") == false);
    };

    test("EscapeCharacter") = [&] {
        PATDEF(Pat1, R"(\*star)")
        expect(that % Pat1.match("*star") == true);

        PATDEF(Pat2, R"(\{\*\})")
        expect(that % Pat2.match("{*}") == true);
    };

    test("BracketExpr") = [&] {
        PATDEF(Pat1, R"([a-zA-Z\]])")
        expect(that % Pat1.match(R"(])") == true);
        expect(that % Pat1.match(R"([)") == false);
        expect(that % Pat1.match(R"(s)") == true);
        expect(that % Pat1.match(R"(S)") == true);
        expect(that % Pat1.match(R"(0)") == false);

        PATDEF(Pat2, R"([\^a-zA-Z""\\])")
        expect(that % Pat2.match(R"(")") == true);
        expect(that % Pat2.match(R"(^)") == true);
        expect(that % Pat2.match(R"(\)") == true);
        expect(that % Pat2.match(R"(")") == true);
        expect(that % Pat2.match(R"(x)") == true);
        expect(that % Pat2.match(R"(X)") == true);
        expect(that % Pat2.match(R"(0)") == false);

        PATDEF(Pat3, R"([!0-9a-fA-F\-+\*])")
        expect(that % Pat3.match("1") == false);
        expect(that % Pat3.match("*") == false);
        expect(that % Pat3.match("s") == true);
        expect(that % Pat3.match("S") == true);
        expect(that % Pat3.match("H") == true);
        expect(that % Pat3.match("]") == true);

        PATDEF(Pat4, R"([^\^0-9a-fA-F\-+\*])")
        expect(that % Pat4.match("1") == false);
        expect(that % Pat4.match("*") == false);
        expect(that % Pat4.match("^") == false);
        expect(that % Pat4.match("s") == true);
        expect(that % Pat4.match("S") == true);
        expect(that % Pat4.match("H") == true);
        expect(that % Pat4.match("]") == true);

        PATDEF(Pat5, R"([\*-\^])")
        expect(that % Pat5.match("*") == true);
        expect(that % Pat5.match("a") == false);
        expect(that % Pat5.match("z") == false);
        expect(that % Pat5.match("A") == true);
        expect(that % Pat5.match("Z") == true);
        expect(that % Pat5.match("\\") == true);
        expect(that % Pat5.match("^") == true);
        expect(that % Pat5.match("-") == true);

        PATDEF(Pat6, "foo.[^0-9]")
        expect(that % Pat6.match("foo.5") == false);
        expect(that % Pat6.match("foo.8") == false);
        expect(that % Pat6.match("bar.5") == false);
        expect(that % Pat6.match("foo.f") == true);

        PATDEF(Pat7, "foo.[!0-9]")
        expect(that % Pat7.match("foo.5") == false);
        expect(that % Pat7.match("foo.8") == false);
        expect(that % Pat7.match("bar.5") == false);
        expect(that % Pat7.match("foo.f") == true);

        PATDEF(Pat8, "foo.[0!^*?]")
        expect(that % Pat8.match("foo.5") == false);
        expect(that % Pat8.match("foo.8") == false);
        expect(that % Pat8.match("foo.0") == true);
        expect(that % Pat8.match("foo.!") == true);
        expect(that % Pat8.match("foo.^") == true);
        expect(that % Pat8.match("foo.*") == true);
        expect(that % Pat8.match("foo.?") == true);

        PATDEF(Pat9, "foo[/]bar")
        expect(that % Pat9.match("foo/bar") == false);

        PATDEF(Pat10, "foo.[[]")
        expect(that % Pat10.match("foo.[") == true);

        PATDEF(Pat11, "foo.[]]")
        expect(that % Pat11.match("foo.]") == true);

        PATDEF(Pat12, "foo.[][!]")
        expect(that % Pat12.match("foo.]") == true);
        expect(that % Pat12.match("foo.[") == true);
        expect(that % Pat12.match("foo.!") == true);

        PATDEF(Pat13, "foo.[]-]")
        expect(that % Pat13.match("foo.]") == true);
        expect(that % Pat13.match("foo.-") == true);

        PATDEF(Pat14, "foo.[0-9]")
        expect(that % Pat14.match("foo.5") == true);
        expect(that % Pat14.match("foo.8") == true);
        expect(that % Pat14.match("bar.5") == false);
        expect(that % Pat14.match("foo.f") == false);
    };

    test("BraceExpr") = [&] {
        PATDEF(Pat1, "*foo[0-9a-z].{c,cpp,cppm,?pp}")
        expect(that % Pat1.match("foo1.cc") == false);
        expect(that % Pat1.match("foo2.cpp") == true);
        expect(that % Pat1.match("foo3.cppm") == true);
        expect(that % Pat1.match("foot.cppm") == true);
        expect(that % Pat1.match("foot.hpp") == true);
        expect(that % Pat1.match("foot.app") == true);
        expect(that % Pat1.match("fooD.cppm") == false);
        expect(that % Pat1.match("BarfooD.cppm") == false);
        expect(that % Pat1.match("foofooD.cppm") == false);

        PATDEF(Pat2, "proj/{build*,include,src}/*.{cc,cpp,h,hpp}")
        expect(that % Pat2.match("proj/include/foo.cc") == true);
        expect(that % Pat2.match("proj/include/bar.cpp") == true);
        expect(that % Pat2.match("proj/include/xxx/yyy/zzz/foo.cc") == false);
        expect(that % Pat2.match("proj/build-yyy/foo.h") == true);
        expect(that % Pat2.match("proj/build-xxx/foo.cpp") == true);
        expect(that % Pat2.match("proj/build/foo.cpp") == true);
        expect(that % Pat2.match("proj/build-xxx/xxx/yyy/zzz/foo.cpp") == false);

        PATDEF(Pat3, "*.{html,js}")
        expect(that % Pat3.match("foo.js") == true);
        expect(that % Pat3.match("foo.html") == true);
        expect(that % Pat3.match("folder/foo.js") == false);
        expect(that % Pat3.match("/node_modules/foo.js") == false);
        expect(that % Pat3.match("foo.jss") == false);
        expect(that % Pat3.match("some.js/test") == false);

        PATDEF(Pat4, "*.{html}")
        expect(that % Pat4.match("foo.html") == true);
        expect(that % Pat4.match("foo.js") == false);
        expect(that % Pat4.match("folder/foo.js") == false);
        expect(that % Pat4.match("/node_modules/foo.js") == false);
        expect(that % Pat4.match("foo.jss") == false);
        expect(that % Pat4.match("some.js/test") == false);

        PATDEF(Pat5, "{node_modules,testing}")
        expect(that % Pat5.match("node_modules") == true);
        expect(that % Pat5.match("testing") == true);
        expect(that % Pat5.match("node_module") == false);
        expect(that % Pat5.match("dtesting") == false);

        PATDEF(Pat6, "**/{foo,bar}")
        expect(that % Pat6.match("foo") == true);
        expect(that % Pat6.match("bar") == true);
        expect(that % Pat6.match("test/foo") == true);
        expect(that % Pat6.match("test/bar") == true);
        expect(that % Pat6.match("other/more/foo") == true);
        expect(that % Pat6.match("other/more/bar") == true);
        expect(that % Pat6.match("/foo") == true);
        expect(that % Pat6.match("/bar") == true);
        expect(that % Pat6.match("/test/foo") == true);
        expect(that % Pat6.match("/test/bar") == true);
        expect(that % Pat6.match("/other/more/foo") == true);
        expect(that % Pat6.match("/other/more/bar") == true);

        PATDEF(Pat7, "{foo,bar}/**")
        expect(that % Pat7.match("foo") == true);
        expect(that % Pat7.match("bar") == true);
        expect(that % Pat7.match("bar/") == true);
        expect(that % Pat7.match("foo/test") == true);
        expect(that % Pat7.match("bar/test") == true);
        expect(that % Pat7.match("bar/test/") == true);
        expect(that % Pat7.match("foo/other/more") == true);
        expect(that % Pat7.match("bar/other/more") == true);
        expect(that % Pat7.match("bar/other/more/") == true);

        PATDEF(Pat8, "{**/*.d.ts,**/*.js}")
        expect(that % Pat8.match("foo.js") == true);
        expect(that % Pat8.match("testing/foo.js") == true);
        expect(that % Pat8.match("/testing/foo.js") == true);
        expect(that % Pat8.match("foo.d.ts") == true);
        expect(that % Pat8.match("testing/foo.d.ts") == true);
        expect(that % Pat8.match("/testing/foo.d.ts") == true);
        expect(that % Pat8.match("foo.d") == false);
        expect(that % Pat8.match("testing/foo.d") == false);
        expect(that % Pat8.match("/testing/foo.d") == false);

        PATDEF(Pat9, "{**/*.d.ts,**/*.js,path/simple.jgs}")
        expect(that % Pat9.match("foo.js") == true);
        expect(that % Pat9.match("testing/foo.js") == true);
        expect(that % Pat9.match("/testing/foo.js") == true);
        expect(that % Pat9.match("path/simple.jgs") == true);
        expect(that % Pat9.match("/path/simple.jgs") == false);

        PATDEF(Pat10, "{**/*.d.ts,**/*.js,foo.[0-9]}")
        expect(that % Pat10.match("foo.5") == true);
        expect(that % Pat10.match("foo.8") == true);
        expect(that % Pat10.match("bar.5") == false);
        expect(that % Pat10.match("foo.f") == false);
        expect(that % Pat10.match("foo.js") == true);

        PATDEF(Pat11, "prefix/{**/*.d.ts,**/*.js,foo.[0-9]}")
        expect(that % Pat11.match("prefix/foo.5") == true);
        expect(that % Pat11.match("prefix/foo.8") == true);
        expect(that % Pat11.match("prefix/bar.5") == false);
        expect(that % Pat11.match("prefix/foo.f") == false);
        expect(that % Pat11.match("prefix/foo.js") == true);
    };

    test("WildGlob") = [&] {
        PATDEF(Pat1, "**/*")
        expect(that % Pat1.match("foo") == true);
        expect(that % Pat1.match("foo/bar/baz") == true);

        PATDEF(Pat2, "**/[0-9]*")
        expect(that % Pat2.match("114514foo") == true);
        expect(that % Pat2.match("foo/bar/baz/xxx/yyy/zzz") == false);
        expect(that % Pat2.match("foo/bar/baz/xxx/yyy/zzz114514") == false);
        expect(that % Pat2.match("foo/bar/baz/xxx/yyy/114514") == true);
        expect(that % Pat2.match("foo/bar/baz/xxx/yyy/114514zzz") == true);

        PATDEF(Pat3, "**/*[0-9]")
        expect(that % Pat3.match("foo5") == true);
        expect(that % Pat3.match("foo/bar/baz/xxx/yyy/zzz") == false);
        expect(that % Pat3.match("foo/bar/baz/xxx/yyy/zzz114514") == true);

        PATDEF(Pat4, "**/include/test/*.{cc,hh,c,h,cpp,hpp}")
        expect(that % Pat4.match("include/test/aaa.cc") == true);
        expect(that % Pat4.match("/include/test/aaa.cc") == true);
        expect(that % Pat4.match("xxx/yyy/include/test/aaa.cc") == true);
        expect(that % Pat4.match("include/foo/bar/baz/include/test/bbb.hh") == true);
        expect(that % Pat4.match("include/include/include/include/include/test/bbb.hpp") == true);

        PATDEF(Pat5, "**include/test/*.{cc,hh,c,h,cpp,hpp}")
        expect(that % Pat5.match("include/test/fff.hpp") == true);
        expect(that % Pat5.match("xxx-yyy-include/test/fff.hpp") == true);
        expect(that % Pat5.match("xxx-yyy-include/test/.hpp") == true);
        expect(that % Pat5.match("/include/test/aaa.cc") == true);
        expect(that % Pat5.match("include/foo/bar/baz/include/test/bbb.hh") == true);

        PATDEF(Pat6, "**/*foo.{c,cpp}")
        expect(that % Pat6.match("bar/foo.cpp") == true);
        expect(that % Pat6.match("bar/barfoo.cpp") == true);
        expect(that % Pat6.match("/foofoo.cpp") == true);
        expect(that % Pat6.match("foo/foo/foo/foo/foofoo.cpp") == true);
        expect(that % Pat6.match("foofoo.cpp") == true);
        expect(that % Pat6.match("barfoo.cpp") == true);
        expect(that % Pat6.match("foo.cpp") == true);

        // Boundary test of `**`
        PATDEF(Pat7, "**")
        expect(that % Pat7.match("foo") == true);
        expect(that % Pat7.match("foo/bar/baz") == true);

        PATDEF(Pat8, "x/**")
        expect(that % Pat8.match("x/") == true);
        expect(that % Pat8.match("x/foo/bar/baz") == true);
        expect(that % Pat8.match("x") == true);

        PATDEF(Pat9, "**/x")
        expect(that % Pat9.match("x") == true);
        expect(that % Pat9.match("/x") == true);
        expect(that % Pat9.match("/x/x/x/x/x") == true);

        PATDEF(Pat10, "**/*")
        expect(that % Pat10.match("foo") == true);
        expect(that % Pat10.match("foo/bar") == true);
        expect(that % Pat10.match("foo/bar/baz") == true);

        PATDEF(Pat11, "**/*.{cc,cpp}")
        expect(that % Pat11.match("foo/bar/baz.cc") == true);
        expect(that % Pat11.match("foo/foo/foo.cpp") == true);
        expect(that % Pat11.match("foo/bar/.cc") == true);

        PATDEF(Pat12, "**/*?.{cc,cpp}")
        expect(that % Pat12.match("foo/bar/baz/xxx/yyy/zzz/aaa.cc") == true);
        expect(that % Pat12.match("foo/bar/baz/xxx/yyy/zzz/a.cc") == true);
        expect(that % Pat12.match("foo/bar/baz/xxx/yyy/zzz/.cc") == false);

        PATDEF(Pat13, "**/?*.{cc,cpp}")
        expect(that % Pat13.match("foo/bar/baz/xxx/yyy/zzz/aaa.cc") == true);
        expect(that % Pat13.match("foo/bar/baz/xxx/yyy/zzz/a.cc") == true);
        expect(that % Pat13.match("foo/bar/baz/xxx/yyy/zzz/.cc") == false);

        PATDEF(Pat14, "**/*[0-9]")
        expect(that % Pat14.match("foo5") == true);
        expect(that % Pat14.match("foo/bar/baz/xxx/yyy/zzz") == false);
        expect(that % Pat14.match("foo/bar/baz/xxx/yyy/zzz114514") == true);

        PATDEF(Pat15, "**/*")
        expect(that % Pat15.match("foo") == true);
        expect(that % Pat15.match("foo/bar/baz") == true);

        PATDEF(Pat16, "**/*.js")
        expect(that % Pat16.match("foo.js") == true);
        expect(that % Pat16.match("/foo.js") == true);
        expect(that % Pat16.match("folder/foo.js") == true);
        expect(that % Pat16.match("/node_modules/foo.js") == true);
        expect(that % Pat16.match("foo.jss") == false);
        expect(that % Pat16.match("some.js/test") == false);
        expect(that % Pat16.match("/some.js/test") == false);

        PATDEF(Pat17, "**/project.json")
        expect(that % Pat17.match("project.json") == true);
        expect(that % Pat17.match("/project.json") == true);
        expect(that % Pat17.match("some/folder/project.json") == true);
        expect(that % Pat17.match("/some/folder/project.json") == true);
        expect(that % Pat17.match("some/folder/file_project.json") == false);
        expect(that % Pat17.match("some/folder/fileproject.json") == false);
        expect(that % Pat17.match("some/rrproject.json") == false);

        PATDEF(Pat18, "test/**")
        expect(that % Pat18.match("test") == true);
        expect(that % Pat18.match("test/foo") == true);
        expect(that % Pat18.match("test/foo/") == true);
        expect(that % Pat18.match("test/foo.js") == true);
        expect(that % Pat18.match("test/other/foo.js") == true);
        expect(that % Pat18.match("est/other/foo.js") == false);

        PATDEF(Pat19, "**")
        expect(that % Pat19.match("/") == true);
        expect(that % Pat19.match("foo.js") == true);
        expect(that % Pat19.match("folder/foo.js") == true);
        expect(that % Pat19.match("folder/foo/") == true);
        expect(that % Pat19.match("/node_modules/foo.js") == true);
        expect(that % Pat19.match("foo.jss") == true);
        expect(that % Pat19.match("some.js/test") == true);

        PATDEF(Pat20, "test/**/*.js")
        expect(that % Pat20.match("test/foo.js") == true);
        expect(that % Pat20.match("test/other/foo.js") == true);
        expect(that % Pat20.match("test/other/more/foo.js") == true);
        expect(that % Pat20.match("test/foo.ts") == false);
        expect(that % Pat20.match("test/other/foo.ts") == false);
        expect(that % Pat20.match("test/other/more/foo.ts") == false);

        PATDEF(Pat21, "**/**/*.js")
        expect(that % Pat21.match("foo.js") == true);
        expect(that % Pat21.match("/foo.js") == true);
        expect(that % Pat21.match("folder/foo.js") == true);
        expect(that % Pat21.match("/node_modules/foo.js") == true);
        expect(that % Pat21.match("foo.jss") == false);
        expect(that % Pat21.match("some.js/test") == false);

        PATDEF(Pat22, "**/node_modules/**/*.js")
        expect(that % Pat22.match("foo.js") == false);
        expect(that % Pat22.match("folder/foo.js") == false);
        expect(that % Pat22.match("node_modules/foo.js") == true);
        expect(that % Pat22.match("/node_modules/foo.js") == true);
        expect(that % Pat22.match("node_modules/some/folder/foo.js") == true);
        expect(that % Pat22.match("/node_modules/some/folder/foo.js") == true);
        expect(that % Pat22.match("node_modules/some/folder/foo.ts") == false);
        expect(that % Pat22.match("foo.jss") == false);
        expect(that % Pat22.match("some.js/test") == false);

        PATDEF(Pat23, "{**/node_modules/**,**/.git/**,**/bower_components/**}")
        expect(that % Pat23.match("node_modules") == true);
        expect(that % Pat23.match("/node_modules") == true);
        expect(that % Pat23.match("/node_modules/more") == true);
        expect(that % Pat23.match("some/test/node_modules") == true);
        expect(that % Pat23.match("/some/test/node_modules") == true);
        expect(that % Pat23.match("bower_components") == true);
        expect(that % Pat23.match("bower_components/more") == true);
        expect(that % Pat23.match("/bower_components") == true);
        expect(that % Pat23.match("some/test/bower_components") == true);
        expect(that % Pat23.match("/some/test/bower_components") == true);
        expect(that % Pat23.match(".git") == true);
        expect(that % Pat23.match("/.git") == true);
        expect(that % Pat23.match("some/test/.git") == true);
        expect(that % Pat23.match("/some/test/.git") == true);
        expect(that % Pat23.match("tempting") == false);
        expect(that % Pat23.match("/tempting") == false);
        expect(that % Pat23.match("some/test/tempting") == false);
        expect(that % Pat23.match("/some/test/tempting") == false);

        PATDEF(Pat24, "{**/package.json,**/project.json}")
        expect(that % Pat24.match("package.json") == true);
        expect(that % Pat24.match("/package.json") == true);
        expect(that % Pat24.match("xpackage.json") == false);
        expect(that % Pat24.match("/xpackage.json") == false);

        PATDEF(Pat25, "some/**/*.js")
        expect(that % Pat25.match("some/foo.js") == true);
        expect(that % Pat25.match("some/folder/foo.js") == true);
        expect(that % Pat25.match("something/foo.js") == false);
        expect(that % Pat25.match("something/folder/foo.js") == false);

        PATDEF(Pat26, "some/**/*")
        expect(that % Pat26.match("some/foo.js") == true);
        expect(that % Pat26.match("some/folder/foo.js") == true);
        expect(that % Pat26.match("something/foo.js") == false);
        expect(that % Pat26.match("something/folder/foo.js") == false);
    };
};

}  // namespace

}  // namespace clice::testing
