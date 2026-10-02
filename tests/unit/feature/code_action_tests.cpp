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

/// The titles of the actions offered at the marker, those starting with
/// `prefix`.
std::vector<std::string> titles(llvm::StringRef marker, llvm::StringRef prefix) {
    auto offset = point(marker);
    actions = feature::code_actions(*unit, {offset, offset});
    std::vector<std::string> titles;
    for(const auto& action: actions) {
        if(llvm::StringRef(action.title).starts_with(prefix)) {
            titles.push_back(action.title);
        }
    }
    return titles;
}

std::vector<std::string> definitions(llvm::StringRef marker) {
    return titles(marker, "Define");
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

    run(R"(
#define T0 int
template <class>
struct M {
    void §(f)f();
};
#undef T0
)");
    apply("f", "Define 'M<T0_>::f' out of line");
    EXPECT_COMPILES(R"(
#define T0 int
template <class>
struct M {
    void f();
};

template <class T0_>
void M<T0_>::f() {
}

#undef T0
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
struct Ω {};
template <char C>
struct Tag {};
template <class T, class Ω>
struct S {
    ::T §(f)f();
    Tag<'T'> §(tag)tag();
    ::Ω §(omega)omega();
};
)";
    run(global);
    apply("f", "Define 'S<T, Ω>::f' out of line");
    EXPECT_APPENDED("template <class T, class Ω>\n::T S<T, Ω>::f() {\n}\n", "f");

    run(global);
    apply("tag", "Define 'S<T, Ω>::tag' out of line");
    EXPECT_APPENDED("template <class T, class Ω>\nTag<'T'> S<T, Ω>::tag() {\n}\n", "tag");

    run(global);
    apply("omega", "Define 'S<T, Ω>::omega' out of line");
    EXPECT_APPENDED("template <class T, class Ω>\n::Ω S<T, Ω>::omega() {\n}\n", "omega");

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

    run(R"(
struct G {};
template <class T, class U>
concept C = true;
template <class G, C<::G> T>
struct Box {
    void §(f)f();
};
)");
    apply("f", "Define 'Box<G, T>::f' out of line");
    EXPECT_APPENDED("template <class G, C<::G> T>\nvoid Box<G, T>::f() {\n}\n", "f");

    run(R"(
template <class>
concept C = true;
namespace app {
struct C;
template <::C T>
struct S {
    void §(f)f();
};
}
)");
    apply("f", "Define 'S<T>::f' out of line");
    EXPECT_COMPILES(R"(
template <class>
concept C = true;
namespace app {
struct C;
template <::C T>
struct S {
    void f();
};

template <::C T>
void S<T>::f() {
}

}
)",
                    "f");
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
    llvm::StringRef code = R"(
struct S {
    using R = int;
    R (*§(fp)fp())(int);
    const R (S::*§(mp)mp(int x) const noexcept)();
    struct C {};
    R (C::*§(cp)cp())();
};
)";
    run(code);
    apply("fp", "Define 'S::fp' out of line");
    EXPECT_APPENDED("S::R (*S::fp())(int) {\n}\n", "fp");

    run(code);
    apply("mp", "Define 'S::mp' out of line");
    EXPECT_APPENDED("const S::R (S::*S::mp(int x) const noexcept)() {\n}\n", "mp");

    run(code);
    apply("cp", "Define 'S::cp' out of line");
    EXPECT_APPENDED("S::R (S::C::*S::cp())() {\n}\n", "cp");
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

    run(R"(
struct C;
namespace a {
inline namespace v1 {
struct S {
    void §(f)f(C c);
};
}
}
struct C {};
)");
    EXPECT_EQ(definitions("f"), std::vector<std::string>{});

    run(R"(
template <class T>
struct Later;
struct P {
    void §(take)take(Later<int> l);
};
)");
    EXPECT_EQ(definitions("take"), std::vector<std::string>{});

    run(R"(
template <class T>
struct Later;
struct P {
    void §(take)take(Later<int> l);
};
template <class T>
struct Later {};
)");
    apply("take", "Define 'P::take' out of line");
    EXPECT_APPENDED("void P::take(Later<int> l) {\n}\n", "take");

    run(R"(
template <class F>
struct Function;
template <class R, class... Args>
struct Function<R(Args...)> {};
struct W {
    void §(set)set(Function<void()> callback);
};
)");
    apply("set", "Define 'W::set' out of line");
    EXPECT_APPENDED("void W::set(Function<void()> callback) {\n}\n", "set");
}

TEST_CASE(DeducedTypeNames) {
    run(R"(
namespace n {
struct X {};
struct X make();
int X;
}
§(shadowed)auto shadowed = n::make();

template <int* P>
struct Box {};
class C {
    static int x;

public:
    static auto get() {
        return Box<&x>{};
    }
};
§(private)auto hidden = C::get();
)");
    EXPECT_EQ(titles("shadowed", "Replace"), std::vector<std::string>{});
    EXPECT_EQ(titles("private", "Replace"), std::vector<std::string>{});

    run(R"(
int* _Nonnull get();
const §(p)auto p = get();
)");
    apply("p", "Replace 'const auto' with 'int* _Nonnull const'");
    EXPECT_COMPILES(R"(
int* _Nonnull get();
int* _Nonnull const p = get();
)");

    run(R"(
int* get();
auto a = get();
const §(b)auto b = a;
)");
    apply("b", "Replace 'const auto' with 'int* const'");
    EXPECT_COMPILES(R"(
int* get();
auto a = get();
int* const b = a;
)");

    run(R"(
int* const p = nullptr;
const §(q)decltype(p) q = p;
)");
    apply("q", "Replace 'const decltype(p)' with 'int* const'");
    EXPECT_COMPILES(R"(
int* const p = nullptr;
int* const q = p;
)");
}

TEST_CASE(ConstructorParameters) {
    llvm::StringRef move = R"(
namespace std {
template <class T>
T&& move(T& value) {
    return static_cast<T&&>(value);
}
}
)";
    run(move.str() + R"(
struct Handle {
    Handle() = default;
    Handle(Handle&&) = default;
};
template <class T>
struct §(holder)Holder {
    T value;
    int count;
};
Holder<Handle> held(Handle(), 1);
)");
    apply("holder", "Generate a memberwise constructor for 'Holder'");
    EXPECT_COMPILES(move.str() + R"(
struct Handle {
    Handle() = default;
    Handle(Handle&&) = default;
};
template <class T>
struct Holder {
    T value;
    int count;
    Holder(T value, int count) : value(std::move(value)), count(count) {}
};
Holder<Handle> held(Handle(), 1);
)");

    run(R"(
class Key {
    friend struct Door;
    Key(const Key&) = default;

public:
    Key() = default;
};
struct §(door)Door {
    Key key;
    int n;
};
Door door(Key(), 1);
)");
    apply("door", "Generate a memberwise constructor for 'Door'");
    EXPECT_COMPILES(R"(
class Key {
    friend struct Door;
    Key(const Key&) = default;

public:
    Key() = default;
};
struct Door {
    Key key;
    int n;
    Door(const Key& key, int n) : key(key), n(n) {}
};
Door door(Key(), 1);
)");

    run(move.str() + R"(
class Token {
    template <class>
    friend struct Keeper;
    Token(const Token&) = default;

public:
    Token() = default;
};
template <class T>
struct §(keeper)Keeper {
    Token token;
    T value;
};
Keeper<int> kept(Token(), 1);
)");
    apply("keeper", "Generate a memberwise constructor for 'Keeper'");
    EXPECT_COMPILES(move.str() + R"(
class Token {
    template <class>
    friend struct Keeper;
    Token(const Token&) = default;

public:
    Token() = default;
};
template <class T>
struct Keeper {
    Token token;
    T value;
    Keeper(const Token& token, T value) : token(token), value(std::move(value)) {}
};
Keeper<int> kept(Token(), 1);
)");

    run(R"(
struct Pinned {
    Pinned(const Pinned&) = delete;
    template <class U = int>
        requires(sizeof(U) > 64)
    Pinned(Pinned&&);
};
struct Stuck {
    Stuck(const Stuck&) = delete;
    template <class U>
    Stuck(Stuck&&);
};
struct §(pinned)OnPinned {
    Pinned pinned;
    int n;
};
struct §(stuck)OnStuck {
    Stuck stuck;
    int n;
};
)");
    EXPECT_EQ(titles("pinned", "Generate"), std::vector<std::string>{});
    EXPECT_EQ(titles("stuck", "Generate"), std::vector<std::string>{});
}

TEST_CASE(MacroAcrossLines) {
    run(R"(
#define FLAG 1
#if 1 /*
*/ && §(flag)FLAG
#endif
#define NEG -1
int a = 1 -\
§(neg)NEG;
)");
    EXPECT_EQ(titles("flag", "Expand"), std::vector<std::string>{});
    apply("neg", "Expand macro 'NEG'");
    EXPECT_COMPILES(R"(
#define FLAG 1
#if 1 /*
*/ && FLAG
#endif
#define NEG -1
int a = 1 - -1;
)");
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
