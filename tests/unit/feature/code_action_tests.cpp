/// What each code action renders is pinned by the snapshot corpus
/// (tests/snap/code_action/); these cases apply one action and compile the
/// result, which the corpus cannot: the edited file must be the expected
/// text, compile cleanly and, for a definition, define the function the
/// action was offered on.

#include <string>
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
std::string applied;

void run(llvm::StringRef code, llvm::StringRef main = "main.cpp") {
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
/// resolved as an empty index resolves them.
void apply(llvm::StringRef marker, llvm::StringRef title) {
    auto offset = point(marker);
    actions = feature::code_actions(*unit, {offset, offset});
    auto action = llvm::find_if(actions, [&](const feature::CodeAction& action) {
        return action.title == title;
    });
    ASSERT_TRUE(action != actions.end());
    auto edits = action->edits;
    if(action->index) {
        auto* request = std::get_if<feature::DefineRequest>(&*action->index);
        ASSERT_TRUE(request != nullptr);
        auto text =
            feature::assemble_definitions(request->pieces, [](std::uint64_t) { return false; });
        ASSERT_TRUE(text.has_value());
        edits.push_back({request->range, request->before + *text + request->after});
    }
    llvm::sort(edits,
               [](const auto& lhs, const auto& rhs) { return lhs.range.begin > rhs.range.begin; });
    applied = unit->main_content().str();
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

TEST_CASE(UnnamedTemplateParameter) {
    run(R"(
template <typename T, typename = void>
struct Unnamed {
    void §(f)f();
};
)");
    apply("f", "Define 'Unnamed<T, T1>::f' out of line");
    EXPECT_COMPILES(R"(
template <typename T, typename = void>
struct Unnamed {
    void f();
};

template <typename T, typename T1>
void Unnamed<T, T1>::f() {
}
)",
                    "f");
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
    EXPECT_COMPILES(R"(
template <class T>
concept Small = sizeof(T) <= 4;

template <Small T, Small... Ts>
struct Box {
    void f();
};

template <Small T, Small... Ts>
void Box<T, Ts...>::f() {
}
)",
                    "f");
}

TEST_CASE(TemplateMemberReturnType) {
    llvm::StringRef code = R"(
template <class T>
struct Cont {
    using size_type = unsigned long;
    struct Node {};
    size_type §(size)size() const;
    const Node* §(head)head();
    Cont §(clone)clone();
};
)";
    run(code);
    apply("size", "Define 'Cont<T>::size' out of line");
    EXPECT_COMPILES(R"(
template <class T>
struct Cont {
    using size_type = unsigned long;
    struct Node {};
    size_type size() const;
    const Node* head();
    Cont clone();
};

template <class T>
typename Cont<T>::size_type Cont<T>::size() const {
}
)",
                    "size");

    clear();
    run(code);
    apply("head", "Define 'Cont<T>::head' out of line");
    EXPECT_COMPILES(R"(
template <class T>
struct Cont {
    using size_type = unsigned long;
    struct Node {};
    size_type size() const;
    const Node* head();
    Cont clone();
};

template <class T>
const typename Cont<T>::Node* Cont<T>::head() {
}
)",
                    "head");

    clear();
    run(code);
    apply("clone", "Define 'Cont<T>::clone' out of line");
    EXPECT_COMPILES(R"(
template <class T>
struct Cont {
    using size_type = unsigned long;
    struct Node {};
    size_type size() const;
    const Node* head();
    Cont clone();
};

template <class T>
Cont<T> Cont<T>::clone() {
}
)",
                    "clone");
}

TEST_CASE(SpecifiersAmongReturnType) {
    run(R"(
struct S {
    unsigned static long §(f)f();
    const static int §(k)k();
};
)");
    apply("f", "Define 'S::f' out of line");
    EXPECT_COMPILES(R"(
struct S {
    unsigned static long f();
    const static int k();
};

unsigned long S::f() {
}
)",
                    "f");

    clear();
    run(R"(
struct S {
    unsigned static long §(f)f();
    const static int §(k)k();
};
)");
    apply("k", "Define 'S::k' out of line");
    EXPECT_COMPILES(R"(
struct S {
    unsigned static long f();
    const static int k();
};

const int S::k() {
}
)",
                    "k");
}

TEST_CASE(ConditionalExplicit) {
    run(R"(
template <bool B>
struct Cond {
    explicit(B) §(c)Cond(int);
};
)");
    apply("c", "Define 'Cond<B>::Cond' out of line");
    EXPECT_COMPILES(R"(
template <bool B>
struct Cond {
    explicit(B) Cond(int);
};

template <bool B>
Cond<B>::Cond(int) {
}
)",
                    "c");
}

TEST_CASE(ReturnTypeAroundName) {
    run(R"(
struct S {
    using R = int;
    R (*§(fp)fp())(int);
};
)");
    apply("fp", "Define 'S::fp' out of line");
    EXPECT_COMPILES(R"(
struct S {
    using R = int;
    R (*fp())(int);
};

S::R (*S::fp())(int) {
}
)",
                    "fp");
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
struct S {
    Handler §(h)on_event;
};
)");
    EXPECT_EQ(definitions("h"), std::vector<std::string>{});
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

    clear();
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
    run(R"(
struct H { void §(f)f(); int* p() { return new int; } } const hs[] = {
    {},
};
typedef struct T { void §(g)g(); } Alias;
int after;
)");
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

    clear();
    run(R"(
struct H { void §(f)f(); int* p() { return new int; } } const hs[] = {
    {},
};
typedef struct T { void §(g)g(); } Alias;
int after;
)");
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

TEST_CASE(NestedClassOutOfLine) {
    run(R"(
struct Outer { struct Inner; };
struct Outer::Inner { void §(g)g(); };
)");
    apply("g", "Define 'Outer::Inner::g' out of line");
    EXPECT_COMPILES(R"(
struct Outer { struct Inner; };
struct Outer::Inner { void g(); };

void Outer::Inner::g() {
}
)",
                    "g");
}

TEST_CASE(BlockScopeDeclaration) {
    run(R"(
void outer() {
    void §(i)inner();
    inner();
}
)");
    apply("i", "Define 'inner' out of line");
    EXPECT_COMPILES(R"(
void outer() {
    void inner();
    inner();
}

void inner() {
}
)",
                    "i");
}

TEST_CASE(TypeCompletedLater) {
    llvm::StringRef code = R"(
struct Config;
Config §(load)load();
struct P {
    void §(take)take(Config c);
};
struct Config {
    int x;
};
struct Missing;
Missing §(make)make();
)";
    run(code);
    apply("load", "Define 'load' out of line");
    EXPECT_COMPILES(R"(
struct Config;
Config load();
struct P {
    void take(Config c);
};
struct Config {
    int x;
};

Config load() {
}

struct Missing;
Missing make();
)",
                    "load");

    clear();
    run(code);
    EXPECT_EQ(definitions("take"), std::vector<std::string>{"Define 'P::take' out of line"});
    apply("take", "Define 'P::take' out of line");
    EXPECT_COMPILES(R"(
struct Config;
Config load();
struct P {
    void take(Config c);
};
struct Config {
    int x;
};

void P::take(Config c) {
}

struct Missing;
Missing make();
)",
                    "take");

    clear();
    run(code);
    EXPECT_EQ(definitions("make"), std::vector<std::string>{});
}

TEST_CASE(LayoutKeptWithoutStyle) {
    run(R"(
struct  S {   int   §(f)f( ) ;   };
)");
    apply("f", "Define 'f' inline");
    EXPECT_COMPILES(R"(
struct  S {   int   f( )  {}   };
)",
                    "f");
}

TEST_CASE(HeaderDefinitionInline) {
    run(R"(
struct W {
    void §(w)w();
    [[nodiscard]] static int §(n)n();
};
void §(f)f();
static int §(s)s();
namespace {
int §(a)a();
}
)",
        "widget.h");
    EXPECT_EQ(definitions("w"),
              std::vector<std::string>{
                  "Define 'w' inline",
                  "Define 'W::w' out of line",
                  "Define 'W::w'",
              });
    EXPECT_EQ(definitions("s"), std::vector<std::string>{"Define 's' out of line"});
    EXPECT_EQ(definitions("a"), std::vector<std::string>{"Define 'a' out of line"});

    apply("n", "Define 'W::n' out of line");
    EXPECT_COMPILES(R"(
struct W {
    void w();
    [[nodiscard]] static int n();
};

[[nodiscard]] inline int W::n() {
}

void f();
static int s();
namespace {
int a();
}
)",
                    "n");
}

TEST_CASE(MissingFromPreamble) {
    llvm::StringRef code = R"(
#[widget.h]
#pragma once
struct Tag {};
struct Widget {
    explicit Widget(int id);
    static const Tag& tag();
    virtual void draw(int scale = 1);
    void done();
};
#[main.cpp]
#include "widget.h"

void Widget::§(d)done() {
}
)";
    llvm::StringRef expected = R"(#include "widget.h"

void Widget::done() {
}

Widget::Widget(int id) {
}

const Tag& Widget::tag() {
}

void Widget::draw(int scale) {
}
)";
    add_files("main.cpp", code);
    ASSERT_TRUE(compile_with_pch("-std=c++23"));
    apply("d", "Define missing members of 'Widget'");
    EXPECT_COMPILES(expected);

    clear();
    add_files("main.cpp", code);
    ASSERT_TRUE(compile("-std=c++23"));
    apply("d", "Define missing members of 'Widget'");
    EXPECT_COMPILES(expected);
}

};  // TEST_SUITE(code_action)

}  // namespace

}  // namespace clice::testing
