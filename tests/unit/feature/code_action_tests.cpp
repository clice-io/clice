/// What each code action renders is pinned by the snapshot corpus
/// (tests/snap/code_action/); these cases apply one action and compile the
/// result, which the corpus cannot: the edited file must be the expected
/// text, compile cleanly and, for a definition, define the function the
/// action was offered on.

#include <format>
#include <string>
#include <utility>
#include <vector>

#include "test/test.h"
#include "test/tester.h"
#include "feature/feature.h"

#include "llvm/ADT/STLExtras.h"
#include "clang/AST/RecursiveASTVisitor.h"

namespace clice::testing {

namespace {

/// The function declared at a main-file offset.
struct FunctionAt : clang::RecursiveASTVisitor<FunctionAt> {
    CompilationUnitRef unit;
    std::uint32_t offset;
    const clang::FunctionDecl* found = nullptr;

    FunctionAt(CompilationUnitRef unit, std::uint32_t offset) : unit(unit), offset(offset) {}

    bool shouldVisitTemplateInstantiations() const {
        return false;
    }

    bool VisitFunctionDecl(clang::FunctionDecl* decl) {
        auto location = decl->getLocation();
        if(location.isFileID() && unit.file_id(location) == unit.main_file() &&
           unit.file_offset(location) == offset) {
            found = decl;
        }
        return true;
    }
};

TEST_SUITE(code_action, Tester) {

std::vector<feature::CodeAction> actions;
std::string original;
std::string applied;

void run(llvm::StringRef code, llvm::StringRef main = "main.cpp") {
    clear();
    add_main(main, code);
    ASSERT_TRUE(compile("-std=c++23"));
}

/// The titles of the definitions offered at the marker.
std::vector<std::string> definitions(llvm::StringRef marker) {
    auto offset = point(marker);
    actions = feature::code_actions(*unit, {offset, offset});
    std::vector<std::string> titles;
    for(const auto& action: actions) {
        if(llvm::StringRef(action.title).starts_with("Define")) {
            titles.push_back(action.title);
        }
    }
    return titles;
}

/// Apply the action titled `title` offered at the marker, its definitions
/// resolved and formatted as `clice inspect` does without an index.
void apply(llvm::StringRef marker, llvm::StringRef title) {
    auto offset = point(marker);
    actions = feature::code_actions(*unit, {offset, offset});
    auto action = llvm::find_if(actions, [&](const feature::CodeAction& action) {
        return action.title == title;
    });
    ASSERT_TRUE(action != actions.end());
    original = unit->main_content().str();
    auto edits = action->edits;
    if(action->index) {
        auto* request = std::get_if<feature::DefineRequest>(&*action->index);
        ASSERT_TRUE(request != nullptr);
        auto text =
            feature::assemble_definitions(request->pieces, [](std::uint64_t) { return false; });
        ASSERT_TRUE(text.has_value());
        edits =
            feature::format_edits(unit->file_path(unit->main_file()),
                                  original,
                                  {
                                      {request->range, request->before + *text + request->after}
        });
    }
    llvm::sort(edits,
               [](const auto& lhs, const auto& rhs) { return lhs.range.begin > rhs.range.begin; });
    applied = original;
    for(const auto& edit: edits) {
        applied.replace(edit.range.begin, edit.range.length(), edit.text);
    }
}

/// The edited main file is `expected` and compiles without errors; with
/// a marker, the function declared there is defined.
void EXPECT_COMPILES(llvm::StringRef expected, llvm::StringRef marker = "") {
    ASSERT_EQ(applied, expected);
    auto offset = marker.empty() ? 0 : point(marker);
    auto main = src_path;
    std::vector<std::pair<std::string, std::string>> others;
    for(auto& [file, source]: sources.all_files) {
        if(file != main) {
            others.emplace_back(file.str(), source.content);
        }
    }
    clear();
    for(auto& [file, content]: others) {
        add_file(file, content);
    }
    add_main(main, applied);
    ASSERT_TRUE(compile("-std=c++23"));
    std::vector<std::string> errors;
    for(auto& diagnostic: unit->diagnostics()) {
        if(diagnostic.id.level >= DiagnosticLevel::Error) {
            errors.push_back(diagnostic.message);
        }
    }
    EXPECT_EQ(errors, std::vector<std::string>{});
    if(!marker.empty()) {
        FunctionAt visitor(*unit, offset);
        visitor.TraverseDecl(unit->tu());
        ASSERT_TRUE(visitor.found != nullptr);
        EXPECT_TRUE(visitor.found->isDefined());
    }
}

/// The definition went to the end of the file, after a blank line.
void EXPECT_APPENDED(llvm::StringRef definition, llvm::StringRef marker = "") {
    EXPECT_COMPILES(original + "\n" + definition.str(), marker);
}

TEST_CASE(UnnamedTemplateParameter) {
    run(R"(
template <typename T, typename = void>
struct Unnamed {
    void §(f)f();
};
)");
    apply("f", "Define 'Unnamed<T, T1>::f' out of line");
    EXPECT_APPENDED("template <typename T, typename T1>\nvoid Unnamed<T, T1>::f() {\n}\n", "f");

    run(R"(
template <class>
struct O {
    template <class>
    struct I {
        void §(f)f();
    };
};
)");
    apply("f", "Define 'O<T0>::I<T0_>::f' out of line");
    EXPECT_APPENDED("template <class T0>\ntemplate <class T0_>\nvoid O<T0>::I<T0_>::f() {\n}\n",
                    "f");

    run(R"(
struct T1 {};
template <class T, class = void>
struct U {
    T1 §(f)f();
};
)");
    apply("f", "Define 'U<T, T1_>::f' out of line");
    EXPECT_APPENDED("template <class T, class T1_>\nT1 U<T, T1_>::f() {\n}\n", "f");

    run(R"(
struct T0 {};
struct Outer {
    template <class>
    struct Inner;
};
template <class>
struct Outer::Inner {
    void §(f)f(T0);
};
)");
    apply("f", "Define 'Outer::Inner<T0_>::f' out of line");
    EXPECT_APPENDED("template <class T0_>\nvoid Outer::Inner<T0_>::f(T0) {\n}\n", "f");
}

TEST_CASE(ConstrainedTemplateParameter) {
    run(R"(
template <class T>
concept Small = sizeof(T) <= 4;

template <Small T, Small... Ts>
struct Box {
    void §(f)f();
};
)");
    apply("f", "Define 'Box<T, Ts...>::f' out of line");
    EXPECT_APPENDED("template <Small T, Small... Ts>\nvoid Box<T, Ts...>::f() {\n}\n", "f");

    run(R"(
namespace ns {
template <class T>
concept Small = sizeof(T) <= 4;
}
template <ns::Small auto V, ns::Small auto... Vs>
struct Value {
    void §(f)f();
};
)");
    apply("f", "Define 'Value<V, Vs...>::f' out of line");
    EXPECT_APPENDED(
        "template <ns::Small auto V, ns::Small auto... Vs>\nvoid Value<V, Vs...>::f() {\n}\n",
        "f");

    run(R"(
struct Cfg;
namespace ns {
template <class T>
concept Tiny = true;
template <class T, class U>
concept Same = true;
template <Tiny T, Same<int> U>
struct B {
    void §(f)f(Cfg c);
};
}
struct Cfg {};
)");
    apply("f", "Define 'ns::B<T, U>::f' out of line");
    EXPECT_APPENDED("template <ns::Tiny T, ns::Same<int> U>\nvoid ns::B<T, U>::f(Cfg c) {\n}\n",
                    "f");
}

TEST_CASE(ShadowedByParameter) {
    llvm::StringRef code = R"(
struct G {};
namespace app {
struct T {};
template <class T>
struct S {
    ::app::T §(f)f();
    template <class G>
    ::G §(g)g();
};
}
)";
    run(code);
    apply("f", "Define 'S<T>::f' out of line");
    EXPECT_COMPILES(R"(
struct G {};
namespace app {
struct T {};
template <class T>
struct S {
    ::app::T f();
    template <class G>
    ::G g();
};

template <class T>
app::T S<T>::f() {
}

}
)",
                    "f");

    run(code);
    apply("g", "Define 'S<T>::g' out of line");
    EXPECT_COMPILES(R"(
struct G {};
namespace app {
struct T {};
template <class T>
struct S {
    ::app::T f();
    template <class G>
    ::G g();
};

template <class T>
template <class G>
::G S<T>::g() {
}

}
)",
                    "g");

    llvm::StringRef global = R"(
struct T {};
template <char C>
struct Tag {};
template <class T>
struct S {
    ::T §(f)f();
    Tag<'T'> §(tag)tag();
};
)";
    run(global);
    apply("f", "Define 'S<T>::f' out of line");
    EXPECT_APPENDED("template <class T>\n::T S<T>::f() {\n}\n", "f");

    run(global);
    apply("tag", "Define 'S<T>::tag' out of line");
    EXPECT_APPENDED("template <class T>\nTag<'T'> S<T>::tag() {\n}\n", "tag");

    run(R"(
template <class>
concept C = true;
template <class C, ::C U>
struct Box {
    void §(f)f();
};
)");
    apply("f", "Define 'Box<C, U>::f' out of line");
    EXPECT_APPENDED("template <class C, ::C U>\nvoid Box<C, U>::f() {\n}\n", "f");
}

TEST_CASE(TemplateMemberReturnType) {
    llvm::StringRef code = R"(
template <class T>
struct Cont {
    using value_type = T;
    using size_type = unsigned long;
    struct Node {};
    template <class U>
    using Alias = U;
    value_type §(front)front() const;
    size_type §(size)size() const;
    const Node* §(head)head();
    Cont §(clone)clone();
    Alias<T> §(alias)alias();
};
)";
    std::pair<llvm::StringRef, llvm::StringRef> cases[] = {
        {"front", "typename Cont<T>::value_type Cont<T>::front() const" },
        {"size",  "typename Cont<T>::size_type Cont<T>::size() const"   },
        {"head",  "const typename Cont<T>::Node* Cont<T>::head()"       },
        {"clone", "Cont<T> Cont<T>::clone()"                            },
        {"alias", "typename Cont<T>::template Alias<T> Cont<T>::alias()"},
    };
    for(auto [marker, head]: cases) {
        run(code);
        apply(marker, std::format("Define 'Cont<T>::{}' out of line", marker));
        EXPECT_APPENDED(std::format("template <class T>\n{} {{\n}}\n", head), marker);
    }
}

TEST_CASE(SpecifiersAmongReturnType) {
    llvm::StringRef code = R"(
struct S {
    unsigned static long §(f)f();
    const static int §(k)k();
};
)";
    run(code);
    apply("f", "Define 'S::f' out of line");
    EXPECT_APPENDED("unsigned long S::f() {\n}\n", "f");

    run(code);
    apply("k", "Define 'S::k' out of line");
    EXPECT_APPENDED("const int S::k() {\n}\n", "k");
}

TEST_CASE(ConditionalExplicit) {
    run(R"(
template <class T>
struct Cond {
    explicit(sizeof(T) > 1) §(c)Cond(int);
};
)");
    apply("c", "Define 'Cond<T>::Cond' out of line");
    EXPECT_APPENDED("template <class T>\nCond<T>::Cond(int) {\n}\n", "c");
}

TEST_CASE(ReturnTypeAroundName) {
    run(R"(
struct S {
    using R = int;
    R (*§(fp)fp())(int);
};
)");
    apply("fp", "Define 'S::fp' out of line");
    EXPECT_APPENDED("S::R (*S::fp())(int) {\n}\n", "fp");
}

TEST_CASE(ReturnTypeTokenEdges) {
    llvm::StringRef code = R"(
template <class T>
struct V {};
struct O {
    struct I {
        int x;
    };
    V<V<int>> §(rows)rows();
    const V<V<int>>& §(ref)ref();
    int I::* §(field)field();
};
)";
    std::pair<llvm::StringRef, llvm::StringRef> cases[] = {
        {"rows",  "V<V<int>> O::rows()"      },
        {"ref",   "const V<V<int>>& O::ref()"},
        {"field", "int O::I::* O::field()"   },
    };
    for(auto [marker, head]: cases) {
        run(code);
        apply(marker, std::format("Define 'O::{}' out of line", marker));
        EXPECT_APPENDED(std::format("{} {{\n}}\n", head), marker);
    }
}

TEST_CASE(OverrideReturnAroundName) {
    run(R"(
struct Base {
    using R = int;
    virtual R (*handler(int x) const)(int) = 0;
};
struct §(d)Derived : Base {
};
)");
    apply("d", "Implement pure virtual methods of 'Derived'");
    EXPECT_COMPILES(R"(
struct Base {
    using R = int;
    virtual R (*handler(int x) const)(int) = 0;
};
struct Derived : Base {
    Base::R (*handler(int x) const)(int) override;
};
)");
}

TEST_CASE(FunctionTypedefMember) {
    run(R"(
using Handler = void(int);
struct §(s)S {
    Handler §(h)on_event;
    void g();
};
)");
    EXPECT_EQ(definitions("h"), std::vector<std::string>{});
    apply("s", "Define missing members of 'S'");
    EXPECT_APPENDED("void S::g() {\n}\n");
}

TEST_CASE(SameLineNamespace) {
    run(R"(
namespace detail { void §(h)helper(); }
)");
    apply("h", "Define 'helper' out of line");
    EXPECT_COMPILES(R"(
namespace detail { void helper();
void helper() {
}
 }
)",
                    "h");

    run(R"(
namespace ns { struct S { void §(f)f(); }; }
)");
    apply("f", "Define 'S::f' out of line");
    EXPECT_COMPILES(R"(
namespace ns { struct S { void f(); };
void S::f() {
}
 }
)",
                    "f");
}

TEST_CASE(DeclaratorAfterClass) {
    llvm::StringRef code = R"(
struct H { void §(f)f(); int* p() { return new int; } } const hs[] = {
    {},
};
typedef struct T { void §(g)g(); } Alias;
int after;
)";
    run(code);
    apply("f", "Define 'H::f' out of line");
    EXPECT_COMPILES(R"(
struct H { void f(); int* p() { return new int; } } const hs[] = {
    {},
};

void H::f() {
}

typedef struct T { void g(); } Alias;
int after;
)",
                    "f");

    run(code);
    apply("g", "Define 'T::g' out of line");
    EXPECT_COMPILES(R"(
struct H { void f(); int* p() { return new int; } } const hs[] = {
    {},
};
typedef struct T { void g(); } Alias;

void T::g() {
}

int after;
)",
                    "g");
}

TEST_CASE(AttributeAfterClass) {
    run(R"(
#define PACKED __attribute__((packed))
struct Packed { void §(f)f(); char c; } __attribute__((packed));
struct Macro { void §(g)g(); char c; } PACKED;
)");
    apply("f", "Define 'Packed::f' out of line");
    EXPECT_COMPILES(R"(
#define PACKED __attribute__((packed))
struct Packed { void f(); char c; } __attribute__((packed));

void Packed::f() {
}

struct Macro { void g(); char c; } PACKED;
)",
                    "f");

    run(R"(
#define PACKED __attribute__((packed))
struct Macro { void §(g)g(); char c; } PACKED;
)");
    apply("g", "Define 'Macro::g' out of line");
    EXPECT_APPENDED("void Macro::g() {\n}\n", "g");
}

TEST_CASE(NestedClassMember) {
    run(R"(
struct Outer { struct Inner; };
struct Outer::Inner { void §(g)g(); };
)");
    apply("g", "Define 'Outer::Inner::g' out of line");
    EXPECT_APPENDED("void Outer::Inner::g() {\n}\n", "g");
}

TEST_CASE(BlockScopeDeclaration) {
    run(R"(
void outer() {
    void §(i)inner();
    inner();
}
)");
    apply("i", "Define 'inner' out of line");
    EXPECT_APPENDED("void inner() {\n}\n", "i");

    run(R"(
struct A {
    void m() {
        void §(i)inner();
        inner();
    }
};
)");
    apply("i", "Define 'inner' out of line");
    EXPECT_APPENDED("void inner() {\n}\n", "i");

    run(R"(
auto l = [] {
    void §(i)inner();
    inner();
};
)");
    EXPECT_EQ(definitions("i"), std::vector<std::string>{});
}

TEST_CASE(UnclosedClass) {
    run(R"(
struct P {
    P §(c)clone();
)");
    EXPECT_EQ(definitions("c"), std::vector<std::string>{"Define 'clone' inline"});
}

TEST_CASE(TypeCompletedLater) {
    llvm::StringRef code = R"(
struct Config;
struct Missing;
Config §(load)load();
struct §(p)P {
    void §(take)take(Config c);
    void §(make)make(Missing m);
    void plain();
};
struct Config {
    int x;
};
)";
    run(code);
    apply("load", "Define 'load' out of line");
    EXPECT_APPENDED("Config load() {\n}\n", "load");

    run(code);
    EXPECT_EQ(definitions("take"), std::vector<std::string>{"Define 'P::take' out of line"});
    apply("take", "Define 'P::take' out of line");
    EXPECT_APPENDED("void P::take(Config c) {\n}\n", "take");

    run(code);
    EXPECT_EQ(definitions("make"), std::vector<std::string>{});
    apply("p", "Define missing members of 'P'");
    EXPECT_APPENDED("void P::take(Config c) {\n}\n\nvoid P::plain() {\n}\n");

    run(R"(
namespace a { struct C; }
namespace b { void §(f)f(a::C c); }
namespace a { struct C {}; }
)");
    EXPECT_EQ(definitions("f"), std::vector<std::string>{});

    run(R"(
struct C;
namespace { C §(f)f(); }
struct C {};
)");
    EXPECT_EQ(definitions("f"), std::vector<std::string>{});

    run(R"(
struct C;
C §(h)h();
extern "C" {
struct C {};
}
)");
    EXPECT_EQ(definitions("h"), std::vector<std::string>{});

    run(R"(
struct C;
namespace a { C §(g)g(); }
struct C {};
)");
    apply("g", "Define 'a::g' out of line");
    EXPECT_APPENDED("C a::g() {\n}\n", "g");
}

TEST_CASE(LayoutKeptWithoutStyle) {
    llvm::StringRef code = R"(
namespace app {
struct  S {   int   §(f)f( ) ;   };
}
)";
    run(code);
    apply("f", "Define 'f' inline");
    EXPECT_COMPILES(R"(
namespace app {
struct  S {   int   f( )  {}   };
}
)",
                    "f");

    run(code);
    apply("f", "Define 'S::f' out of line");
    EXPECT_COMPILES(R"(
namespace app {
struct  S {   int   f( ) ;   };

int   S::f( ) {
}

}
)",
                    "f");
}

TEST_CASE(HeaderDefinitionInline) {
    llvm::StringRef code = R"(
struct W {
    §(ctor)W();
    void §(w)w();
    [[nodiscard]] static int §(n)n();
};
)";
    run(code, "widget.h");
    EXPECT_EQ(definitions("w"),
              std::vector<std::string>{
                  "Define 'w' inline",
                  "Define 'W::w' out of line",
                  "Define 'W::w'",
              });
    apply("n", "Define 'W::n' out of line");
    EXPECT_APPENDED("[[nodiscard]] inline int W::n() {\n}\n", "n");

    run(code, "widget.h");
    apply("ctor", "Define 'W::W' out of line");
    EXPECT_APPENDED("inline W::W() {\n}\n", "ctor");

    run("void §(f)f();\n", "widget.h");
    apply("f", "Define 'f' out of line");
    EXPECT_APPENDED("inline void f() {\n}\n", "f");
}

TEST_CASE(HeaderInternalLinkage) {
    run(R"(
static int §(s)s();
namespace {
int §(a)a();
struct §(hidden)Hidden {
    void g();
};
}
)",
        "widget.h");
    EXPECT_EQ(definitions("s"), std::vector<std::string>{"Define 's' out of line"});
    EXPECT_EQ(definitions("a"), std::vector<std::string>{"Define 'a' out of line"});
    EXPECT_EQ(definitions("hidden"),
              std::vector<std::string>{"Define missing members of 'Hidden'"});
    apply("hidden", "Define missing members of 'Hidden'");
    EXPECT_COMPILES(R"(
static int s();
namespace {
int a();
struct Hidden {
    void g();
};

void Hidden::g() {
}

}
)");
}

TEST_CASE(MissingFromPreamble) {
    llvm::StringRef code = R"(
#[widget.h]
#pragma once
struct Tag {};
struct Widget {
    explicit Widget(int id);
    static Tag tag();
    static const Tag& last();
    virtual void draw(int scale = 1);
    void done();
};
#[main.cpp]
#include "widget.h"

void Widget::§(d)done() {
}
)";
    llvm::StringRef expected = R"(Widget::Widget(int id) {
}

Tag Widget::tag() {
}

const Tag& Widget::last() {
}

void Widget::draw(int scale) {
}
)";
    for(bool pch: {true, false}) {
        clear();
        add_files("main.cpp", code);
        ASSERT_TRUE(pch ? compile_with_pch("-std=c++23") : compile("-std=c++23"));
        apply("d", "Define missing members of 'Widget'");
        EXPECT_APPENDED(expected);
    }
}

};  // TEST_SUITE(code_action)

}  // namespace

}  // namespace clice::testing
