#include <map>
#include <set>
#include <string>
#include <utility>

#include "test/test.h"
#include "test/tester.h"
#include "semantic/decls.h"
#include "support/logging.h"

#include "llvm/Support/Path.h"
#include "clang/AST/DeclTemplate.h"
#include "clang/AST/RecursiveASTVisitor.h"

namespace clice::testing {

namespace {

/// Every named declaration of a compile, keyed by file and the offset of
/// its name; a template and the declaration it describes share an offset
/// and the template wins, matching what an occurrence resolves to.
/// Whether the file is the fixture, by the path both sides spell natively.
bool is_fixture(Tester& tester, clang::FileID fid, llvm::StringRef file) {
    llvm::SmallString<128> expected(TestVFS::path(file));
    llvm::sys::path::native(expected);
    llvm::sys::path::remove_dots(expected, true);
    llvm::SmallString<128> actual(tester.unit->file_path(fid));
    llvm::sys::path::native(actual);
    llvm::sys::path::remove_dots(actual, true);
    return actual == expected;
}

struct Located : clang::RecursiveASTVisitor<Located> {
    clang::SourceManager& SM;
    std::map<std::pair<clang::FileID, std::uint32_t>, const clang::NamedDecl*> decls;

    explicit Located(clang::SourceManager& SM) : SM(SM) {}

    bool shouldVisitTemplateInstantiations() const {
        return false;
    }

    bool VisitNamedDecl(clang::NamedDecl* decl) {
        auto location = decl->getLocation();
        if(location.isInvalid()) {
            return true;
        }
        auto [fid, offset] = SM.getDecomposedLoc(SM.getSpellingLoc(location));
        decls.try_emplace({fid, offset}, decl);
        return true;
    }
};

const clang::NamedDecl* find_decl(Tester& tester, llvm::StringRef file, llvm::StringRef marker) {
    Located located(tester.unit->context().getSourceManager());
    located.TraverseDecl(tester.unit->tu());
    auto offset = tester[file, marker];
    for(auto& [key, decl]: located.decls) {
        if(key.second == offset && is_fixture(tester, key.first, file)) {
            return decl;
        }
    }
    LOG_FATAL("no declaration at marker {} in {}", marker, file);
}

/// The symbol identity an occurrence of the marked declaration gets.
std::uint64_t entity_at(Tester& tester, llvm::StringRef file, llvm::StringRef marker) {
    return tester.unit->entity(decls::normalize(find_decl(tester, file, marker)));
}

std::uint64_t macro_entity(Tester& tester, llvm::StringRef file, llvm::StringRef name) {
    for(auto& [fid, directive]: tester.unit->directives()) {
        if(!is_fixture(tester, fid, file)) {
            continue;
        }
        for(auto& macro: directive.macros) {
            if(macro.kind == MacroRef::Def && tester.unit->token_spelling(macro.loc) == name) {
                return tester.unit->entity(macro.macro);
            }
        }
    }
    LOG_FATAL("no macro {} defined in {}", name, file);
}

TEST_SUITE(identity, Tester) {

std::uint64_t entity(llvm::StringRef marker) {
    return entity_at(*this, "main.cpp", marker);
}

TEST_CASE(RequiresClauseEquivalence) {
    add_main("main.cpp", R"cpp(
template <typename T> struct A;

template <typename T> requires (__is_same(T, float))
struct §(1)A<T>;

using FLOAT = float;
template <typename T> requires (__is_same(T, FLOAT))
struct §(2)A<T>;

template <typename T> requires (__is_same(T, A<int>))
struct §(11)A<T>;
template <typename T> requires (__is_same(T, A<float>))
struct §(12)A<T>;
template <typename T> requires (__is_same(T, A<FLOAT>))
struct §(13)A<T>;
)cpp");
    ASSERT_TRUE(compile());

    EXPECT_EQ(entity("1"), entity("2"));
    EXPECT_NE(entity("11"), entity("12"));
    EXPECT_EQ(entity("12"), entity("13"));
}

TEST_CASE(ConceptConstraintPosition) {
    add_main("main.cpp", R"cpp(
template<typename T>
concept C = requires(T t) { true; };
template<typename T, typename U>
struct A;

template<typename T, C U>
struct §(1)A<T, U>;

template<C T, typename U>
struct §(2)A<T, U>;
)cpp");
    ASSERT_TRUE(compile());

    EXPECT_NE(entity("1"), entity("2"));
}

TEST_CASE(ConstrainedPackDiffers) {
    add_main("main.cpp", R"cpp(
template<typename... Ts>
struct §(1)A;
)cpp");
    ASSERT_TRUE(compile());

    Tester other;
    other.add_main("main.cpp", R"cpp(
template<typename T>
concept C = requires(T t) { true; };
template<C... Ts>
struct §(2)A;
)cpp");
    ASSERT_TRUE(other.compile());

    EXPECT_NE(entity("1"), entity_at(other, "main.cpp", "2"));
}

TEST_CASE(TemplateArgumentExpression) {
    add_main("main.cpp", R"cpp(
template <typename T, int N> struct C;

template <int N> struct §(1)C<float, N>;
template <int M> struct §(2)C<float, M>;
template <char c> struct §(3)C<float, c>;
)cpp");
    ASSERT_TRUE(compile());

    EXPECT_EQ(entity("1"), entity("2"));
    EXPECT_NE(entity("1"), entity("3"));
}

TEST_CASE(FunctionRequiresClause) {
    add_main("main.cpp", R"cpp(
template<typename T>
void §(1)func(T t) requires (sizeof(T) == 4) {};

template<typename T>
void §(2)func(T t) requires (sizeof(T) == 8) {};
)cpp");
    ASSERT_TRUE(compile());

    EXPECT_NE(entity("1"), entity("2"));
}

TEST_CASE(VariableTemplateConstraint) {
    add_main("main.cpp", R"cpp(
template <typename T>
constexpr T §(1)pi = 3.14;
)cpp");
    ASSERT_TRUE(compile());

    Tester other;
    other.add_main("main.cpp", R"cpp(
template <typename T>
concept integral = requires (T t) { t + 1; };
template <integral T>
constexpr T §(2)pi = 3;
)cpp");
    ASSERT_TRUE(other.compile());

    EXPECT_NE(entity("1"), entity_at(other, "main.cpp", "2"));
}

TEST_CASE(DeducedParameterType) {
    add_main("main.cpp", R"cpp(
template<typename T>
struct array {};
template<typename U, array arr>
struct §(1)L;
)cpp");
    ASSERT_TRUE(compile());

    Tester other;
    other.add_main("main.cpp", R"cpp(
template<typename U, int arr>
struct §(2)L;
)cpp");
    ASSERT_TRUE(other.compile());

    EXPECT_NE(entity("1"), entity_at(other, "main.cpp", "2"));
}

TEST_CASE(TemplateParamObject) {
    add_main("main.cpp", R"cpp(
template<typename T> struct array {
  constexpr array(T x_) : x(x_) {}
  int x;
};
template<array U> struct L;
template<> struct §(1)L<{1}>;
template<> struct §(2)L<{2}>;
)cpp");
    ASSERT_TRUE(compile());

    EXPECT_NE(entity("1"), entity("2"));
}

TEST_CASE(PlaceholderConstraint) {
    add_main("main.cpp", R"cpp(
template<auto N> struct §(1)M;
)cpp");
    ASSERT_TRUE(compile());

    Tester other;
    other.add_main("main.cpp", R"cpp(
template<typename T> concept C = requires {requires true;};
template<C auto N> struct §(2)M;
)cpp");
    ASSERT_TRUE(other.compile());

    EXPECT_NE(entity("1"), entity_at(other, "main.cpp", "2"));
}

TEST_CASE(DependentTemplateName) {
    llvm::StringRef content = R"cpp(
template <typename MetaFun>
struct X {
    template <template <typename, typename> class Tmpl>
    class Y;

    template <>
    class §(1)Y<MetaFun::template apply>;

    template <>
    class §(2)Y<MetaFun::template apply2>;
};
)cpp";
    add_main("main.cpp", content);
    ASSERT_TRUE(compile());

    EXPECT_NE(entity("1"), entity("2"));

    Tester again;
    again.add_main("main.cpp", content);
    ASSERT_TRUE(again.compile());

    EXPECT_EQ(entity("1"), entity_at(again, "main.cpp", "1"));
    EXPECT_EQ(entity("2"), entity_at(again, "main.cpp", "2"));
}

TEST_CASE(DeducingThis) {
    add_main("main.cpp", R"cpp(
class A {
public:
    template <typename Self>
    void §(1)foo(this Self&& s);

    template <typename Self>
    void §(2)foo(Self&& s);
};
)cpp");
    ASSERT_TRUE(compile("-std=c++23"));

    EXPECT_NE(entity("1"), entity("2"));
}

TEST_CASE(MacroPathDistinct) {
    add_file("a/x.h", R"cpp(
#define FOO 1
)cpp");
    add_file("b/x.h", R"cpp(
#define FOO 2
)cpp");
    add_main("main.cpp", R"cpp(
#include "a/x.h"
#undef FOO
#include "b/x.h"
)cpp");
    ASSERT_TRUE(compile());

    EXPECT_NE(macro_entity(*this, "a/x.h", "FOO"), macro_entity(*this, "b/x.h", "FOO"));
}

TEST_CASE(NamespaceLambdasDistinct) {
    add_main("main.cpp", R"cpp(
auto §(l1)l1 = [](int) {};
auto §(l2)l2 = [](int) {};
void §(a1)accept(decltype(l1));
void §(a2)accept(decltype(l2));
)cpp");
    ASSERT_TRUE(compile());

    EXPECT_NE(entity("a1"), entity("a2"));
}

TEST_CASE(MemberRequiresDistinct) {
    add_main("main.cpp", R"cpp(
template<typename T> concept C = requires(T t) { t + 1; };
template<typename T> concept D = requires(T t) { t - 1; };
template<typename T> struct A {
    void §(1)f() requires C<T>;
    void §(2)f() requires D<T>;
};
)cpp");
    ASSERT_TRUE(compile());

    EXPECT_NE(entity("1"), entity("2"));
}

TEST_CASE(SpecializationReturnType) {
    add_main("main.cpp", R"cpp(
struct X { using A = int; using B = long; };
template<typename T> typename T::A g();
template<typename T> typename T::B g();
template<> X::A §(1)g<X>();
template<> X::B §(2)g<X>();
)cpp");
    ASSERT_TRUE(compile());

    EXPECT_NE(entity("1"), entity("2"));
}

TEST_CASE(TemplateFriendDefinition) {
    add_main("main.cpp", R"cpp(
template<typename T> struct X {
    friend void §(1)f(int);
};
void §(2)f(int) {}
)cpp");
    ASSERT_TRUE(compile());

    EXPECT_EQ(entity("1"), entity("2"));
}

TEST_CASE(MemberVariableTemplate) {
    add_main("main.cpp", R"cpp(
struct S {
    template<typename T> static constexpr bool §(1)value = true;
};
)cpp");
    ASSERT_TRUE(compile());

    auto* decl = find_decl(*this, "main.cpp", "1");
    auto* wrapper = llvm::dyn_cast<clang::VarTemplateDecl>(decl);
    ASSERT_TRUE(wrapper != nullptr);
    EXPECT_EQ(unit->entity(wrapper), unit->entity(wrapper->getTemplatedDecl()));
}

TEST_CASE(MemberPointerOverloads) {
    add_main("main.cpp", R"cpp(
struct S { int m; int mf(); };
void §(1)p(int S::*);
void §(2)p(int (S::*)());
)cpp");
    ASSERT_TRUE(compile());

    EXPECT_NE(entity("1"), entity("2"));
}

TEST_CASE(ConversionRenamedParameter) {
    add_main("main.cpp", R"cpp(
template<typename T> struct A {
    §(1)operator T*();
};
template<typename U> A<U>::§(2)operator U*() { return nullptr; }
)cpp");
    ASSERT_TRUE(compile());

    EXPECT_EQ(entity("1"), entity("2"));
}

TEST_CASE(NoexceptPointerOverloads) {
    add_main("main.cpp", R"cpp(
void §(1)q(void (*)());
void §(2)q(void (*)() noexcept);
)cpp");
    ASSERT_TRUE(compile());

    EXPECT_NE(entity("1"), entity("2"));
}

TEST_CASE(AnonymousNamespaceFiles) {
    llvm::StringRef content = R"cpp(
namespace { struct §(s)S {}; }
namespace { typedef enum { A } §(e)E; }
)cpp";
    add_main("a/x.cpp", content);
    ASSERT_TRUE(compile());

    Tester other;
    other.add_main("b/x.cpp", content);
    ASSERT_TRUE(other.compile());

    EXPECT_NE(entity_at(*this, "a/x.cpp", "s"), entity_at(other, "b/x.cpp", "s"));
    EXPECT_NE(entity_at(*this, "a/x.cpp", "e"), entity_at(other, "b/x.cpp", "e"));
}

TEST_CASE(TopLevelConstParameter) {
    add_main("main.cpp", R"cpp(
void §(1)c(const int);
void §(2)c(int) {}
)cpp");
    ASSERT_TRUE(compile());

    EXPECT_EQ(entity("1"), entity("2"));
}

TEST_CASE(DeclarationArgumentType) {
    add_main("main.cpp", R"cpp(
inline int a[1];
template<auto> struct X;
template<> struct §(1)X<a> {};
template<> struct §(2)X<&a> {};
)cpp");
    ASSERT_TRUE(compile());

    EXPECT_NE(entity("1"), entity("2"));
}

TEST_CASE(StructuralValuePath) {
    add_main("main.cpp", R"cpp(
union U { int a; int b; };
inline U u;
template<int*> struct X;
template<> struct §(1)X<&u.a> {};
template<> struct §(2)X<&u.b> {};
)cpp");
    ASSERT_TRUE(compile());

    EXPECT_NE(entity("1"), entity("2"));
}

TEST_CASE(ConstrainedFriendLexical) {
    add_main("main.cpp", R"cpp(
template<typename T> struct A {
    friend void §(1)h(A) requires (sizeof(T) > 1) {}
};
template<typename T> struct B {
    friend void §(2)h(A<T>) requires (sizeof(T) > 1) {}
};
)cpp");
    ASSERT_TRUE(compile());

    EXPECT_NE(entity("1"), entity("2"));
}

TEST_CASE(ExternCAcrossNamespaces) {
    add_main("main.cpp", R"cpp(
namespace A { extern "C" void §(1)f(); }
namespace B { extern "C" void §(2)f(); }
)cpp");
    ASSERT_TRUE(compile());

    EXPECT_EQ(entity("1"), entity("2"));
}

TEST_CASE(IncludedLocals) {
    add_file("a.inc", "int §(x)x;");
    add_file("b.inc", "int §(x)x;");
    add_main("main.cpp", R"cpp(
void f() {
    {
#include "a.inc"
    }
    {
#include "b.inc"
    }
}
)cpp");
    ASSERT_TRUE(compile());

    EXPECT_NE(entity_at(*this, "a.inc", "x"), entity_at(*this, "b.inc", "x"));
}

TEST_CASE(SpecializationMemberConstraint) {
    add_main("main.cpp", R"cpp(
template<typename T> struct S { static constexpr int n = 0; };
template<typename U> void §(1)f(U) requires (sizeof(U) == S<int>::n);
template<typename U> void §(2)f(U) requires (sizeof(U) == S<double>::n);
)cpp");
    ASSERT_TRUE(compile());

    EXPECT_NE(entity("1"), entity("2"));
}

TEST_CASE(OffsetofMembers) {
    add_main("main.cpp", R"cpp(
template<typename T> void §(1)f(T) requires (__builtin_offsetof(T, x) == 0);
template<typename T> void §(2)f(T) requires (__builtin_offsetof(T, y) == 0);
)cpp");
    ASSERT_TRUE(compile());

    EXPECT_NE(entity("1"), entity("2"));
}

TEST_CASE(ParenthesesDistinct) {
    add_main("main.cpp", R"cpp(
template<int N> void §(1)f() requires (N > 0);
template<int N> void §(2)f() requires ((N) > 0);
)cpp");
    ASSERT_TRUE(compile());

    EXPECT_NE(entity("1"), entity("2"));
}

TEST_CASE(DecltypeSizeofDistinct) {
    add_main("main.cpp", R"cpp(
template<typename T> auto §(1)f() -> decltype(sizeof(T));
template<typename T> auto §(2)f() -> __SIZE_TYPE__;
)cpp");
    ASSERT_TRUE(compile());

    EXPECT_NE(entity("1"), entity("2"));
}

TEST_CASE(ConceptResolvedThroughUsing) {
    add_file("common.h", R"cpp(
namespace A { template<typename T> concept C = sizeof(T) == 1; }
namespace B { template<typename T> concept C = sizeof(T) == 2; }
)cpp");
    add_file("h.h", R"cpp(
template<typename T> requires C<T> void §(f)f();
)cpp");
    add_main("main.cpp", R"cpp(
#include "common.h"
using A::C;
#include "h.h"
)cpp");
    ASSERT_TRUE(compile());

    Tester other;
    other.add_file("common.h", R"cpp(
namespace A { template<typename T> concept C = sizeof(T) == 1; }
namespace B { template<typename T> concept C = sizeof(T) == 2; }
)cpp");
    other.add_file("h.h", R"cpp(
template<typename T> requires C<T> void §(f)f();
)cpp");
    other.add_main("main.cpp", R"cpp(
#include "common.h"
using B::C;
#include "h.h"
)cpp");
    ASSERT_TRUE(other.compile());

    EXPECT_NE(entity_at(*this, "h.h", "f"), entity_at(other, "h.h", "f"));
}

TEST_CASE(EarlierDecltypeNoEffect) {
    llvm::StringRef header = R"cpp(
template<typename T> requires C<T> void §(f)f();
)cpp";
    add_main("main.cpp", R"cpp(
template<typename T> concept C = true;
#include "h.h"
)cpp");
    add_file("h.h", header);
    ASSERT_TRUE(compile());

    Tester other;
    other.add_file("h.h", header);
    other.add_main("main.cpp", R"cpp(
template<typename T> concept C = true;
template<typename T> using Noise = decltype(::C<T>);
#include "h.h"
)cpp");
    ASSERT_TRUE(other.compile());

    EXPECT_EQ(entity_at(*this, "h.h", "f"), entity_at(other, "h.h", "f"));
}

TEST_CASE(MacroRedefinition) {
    add_main("main.cpp", R"cpp(
#define FOO 1
#undef FOO
#define FOO 2
)cpp");
    ASSERT_TRUE(compile());

    std::vector<std::uint64_t> entities;
    for(auto& [fid, directive]: unit->directives()) {
        for(auto& macro: directive.macros) {
            if(macro.kind == MacroRef::Def) {
                entities.push_back(unit->entity(macro.macro));
            }
        }
    }
    ASSERT_EQ(entities.size(), 2U);
    EXPECT_NE(entities[0], entities[1]);
}

TEST_CASE(CopyDeductionCandidate) {
    llvm::StringRef header = R"cpp(
#pragma once
template<typename §(t)T, typename §(d)D = int>
struct Box {
    Box(T*);
    template<typename U, typename E> Box(Box<U, E>&&);
};
Box<char> make();
)cpp";

    add_file("box.h", header);
    add_main("a.cpp", R"cpp(
#include "box.h"
Box deduced = make();
)cpp");
    ASSERT_TRUE(compile());

    Tester other;
    other.add_file("box.h", header);
    other.add_main("b.cpp", R"cpp(
#include "box.h"
Box<char> spelled = make();
)cpp");
    ASSERT_TRUE(other.compile());

    EXPECT_EQ(entity_at(*this, "box.h", "t"), entity_at(other, "box.h", "t"));
    EXPECT_EQ(entity_at(*this, "box.h", "d"), entity_at(other, "box.h", "d"));
}

TEST_CASE(CopyDeductionCandidateRedeclared) {
    llvm::StringRef header = R"cpp(
#pragma once
template<typename T> struct Box;
template<typename §(t)T> struct Box { Box(T*); };
Box<char> make();
)cpp";

    add_file("box.h", header);
    add_main("a.cpp", R"cpp(
#include "box.h"
Box deduced = make();
)cpp");
    ASSERT_TRUE(compile());

    Tester other;
    other.add_file("box.h", header);
    other.add_main("b.cpp", R"cpp(
#include "box.h"
)cpp");
    ASSERT_TRUE(other.compile());

    EXPECT_EQ(entity_at(*this, "box.h", "t"), entity_at(other, "box.h", "t"));
}

TEST_CASE(ComplexSubobjectArgument) {
    add_main("main.cpp", R"cpp(
inline _Complex int z;
template<int*> struct X;
template<> struct §(real)X<&__real__ z> {};
template<> struct §(imag)X<&__imag__ z> {};
)cpp");
    ASSERT_TRUE(compile());

    EXPECT_NE(entity_at(*this, "main.cpp", "real"), entity_at(*this, "main.cpp", "imag"));
}

TEST_CASE(ImplicitConstructorUse) {
    llvm::StringRef header = R"cpp(
#pragma once
struct §(s)S { int x; };
)cpp";
    auto copy_constructor = [](Tester& tester) {
        auto* record = llvm::cast<clang::CXXRecordDecl>(find_decl(tester, "s.h", "s"));
        for(auto* ctor: record->ctors()) {
            if(ctor->isCopyConstructor()) {
                return tester.unit->entity(ctor);
            }
        }
        LOG_FATAL("no copy constructor declared");
    };

    add_file("s.h", header);
    add_main("a.cpp", R"cpp(
#include "s.h"
inline S a;
inline S b(a);
)cpp");
    ASSERT_TRUE(compile());

    Tester other;
    other.add_file("s.h", header);
    other.add_main("b.cpp", R"cpp(
#include "s.h"
struct T : S { using S::S; };
)cpp");
    ASSERT_TRUE(other.compile());

    EXPECT_EQ(copy_constructor(*this), copy_constructor(other));
}

TEST_CASE(NestedMacroLambdas) {
    add_main("main.cpp", R"cpp(
#define L []{}
#define BOTH auto a = L; auto b = L;
BOTH
void §(f1)f(decltype(a));
void §(f2)f(decltype(b));
)cpp");
    ASSERT_TRUE(compile());

    EXPECT_NE(entity_at(*this, "main.cpp", "f1"), entity_at(*this, "main.cpp", "f2"));
}

TEST_CASE(TemplateNameSpelling) {
    llvm::StringRef header = R"cpp(
#pragma once
namespace N { template<class> struct A; }
using N::A;
template<class, template<class> class> inline int v;
#ifdef WARM
template<class T> using Seed = decltype(v<T, N::A>);
#endif
template<class T> auto §(f)f() -> decltype(v<T, A>);
)cpp";

    add_file("h.h", header);
    add_main("a.cpp", R"cpp(
#define WARM
#include "h.h"
)cpp");
    ASSERT_TRUE(compile());

    Tester other;
    other.add_file("h.h", header);
    other.add_main("b.cpp", R"cpp(
#include "h.h"
)cpp");
    ASSERT_TRUE(other.compile());

    EXPECT_EQ(entity_at(*this, "h.h", "f"), entity_at(other, "h.h", "f"));
}

TEST_CASE(ArrayFillerArgument) {
    llvm::StringRef header = R"cpp(
#pragma once
struct S { int a[2]; };
template<S> struct X;
#ifdef WARM
using Seed = X<S{{0, 0}}>;
#endif
template<> struct §(x)X<S{{0}}> {};
void §(f)f(X<S{{0}}>);
)cpp";

    add_file("h.h", header);
    add_main("a.cpp", R"cpp(
#define WARM
#include "h.h"
)cpp");
    ASSERT_TRUE(compile());

    Tester other;
    other.add_file("h.h", header);
    other.add_main("b.cpp", R"cpp(
#include "h.h"
)cpp");
    ASSERT_TRUE(other.compile());

    EXPECT_EQ(entity_at(*this, "h.h", "x"), entity_at(other, "h.h", "x"));
    EXPECT_EQ(entity_at(*this, "h.h", "f"), entity_at(other, "h.h", "f"));
}

TEST_CASE(InstantiatedLambdas) {
    add_main("main.cpp", R"cpp(
template<class T> inline auto v = []{};
void §(f1)f(decltype(v<int>));
void §(f2)f(decltype(v<double>));
)cpp");
    ASSERT_TRUE(compile());

    EXPECT_NE(entity_at(*this, "main.cpp", "f1"), entity_at(*this, "main.cpp", "f2"));
}

TEST_CASE(LocalClassConstructors) {
    add_main("main.cpp", R"cpp(
void f() {
    struct §(s)S {};
    S a;
    S b(a);
    S c(static_cast<S&&>(a));
}
)cpp");
    ASSERT_TRUE(compile());

    auto* record = llvm::cast<clang::CXXRecordDecl>(find_decl(*this, "main.cpp", "s"));
    std::set<std::uint64_t> entities;
    for(auto* ctor: record->ctors()) {
        entities.insert(unit->entity(ctor));
    }
    EXPECT_EQ(entities.size(), 3U);
}

TEST_CASE(MacroArgumentTwice) {
    add_main("main.cpp", R"cpp(
#define BOTH(X) auto a = X; auto b = X;
BOTH([]{})
void §(f1)f(decltype(a));
void §(f2)f(decltype(b));
)cpp");
    ASSERT_TRUE(compile());

    EXPECT_NE(entity_at(*this, "main.cpp", "f1"), entity_at(*this, "main.cpp", "f2"));
}

TEST_CASE(MacroArgumentPreExpanded) {
    add_main("main.cpp", R"cpp(
#define F void f(decltype([]{})) {}
#define TWO F F
#define ID(...) __VA_ARGS__
ID(TWO)
)cpp");
    ASSERT_TRUE(compile());

    std::set<std::uint64_t> entities;
    for(auto* decl: unit->tu()->decls()) {
        if(auto* function = llvm::dyn_cast<clang::FunctionDecl>(decl);
           function && function->getName() == "f") {
            entities.insert(unit->entity(decls::normalize(function)));
        }
    }
    EXPECT_EQ(entities.size(), 2U);
}

TEST_CASE(CpuSpecificVersions) {
    triple = "x86_64-unknown-linux-gnu";
    add_main("main.cpp", R"cpp(
__attribute__((cpu_specific(generic))) void §(generic)f() {}
__attribute__((cpu_specific(pentium_4))) void §(pentium)f() {}
)cpp");
    ASSERT_TRUE(compile());

    EXPECT_NE(entity_at(*this, "main.cpp", "generic"), entity_at(*this, "main.cpp", "pentium"));
}

TEST_CASE(NoreturnRedeclarationOrder) {
    llvm::StringRef plain = R"cpp(
#pragma once
void §(f)f();
)cpp";
    llvm::StringRef attributed = R"cpp(
#pragma once
__attribute__((noreturn)) void §(f)f();
)cpp";

    add_file("plain.h", plain);
    add_file("attributed.h", attributed);
    add_main("a.cpp", R"cpp(
#include "plain.h"
#include "attributed.h"
)cpp");
    ASSERT_TRUE(compile());

    Tester other;
    other.add_file("plain.h", plain);
    other.add_file("attributed.h", attributed);
    other.add_main("b.cpp", R"cpp(
#include "attributed.h"
#include "plain.h"
)cpp");
    ASSERT_TRUE(other.compile());

    EXPECT_EQ(entity_at(*this, "plain.h", "f"), entity_at(other, "plain.h", "f"));
    EXPECT_EQ(entity_at(*this, "plain.h", "f"), entity_at(*this, "attributed.h", "f"));
}

TEST_CASE(PastedTemplateParameters) {
    add_main("main.cpp", R"cpp(
#define PARAMS class T##1, class T##2
template<PARAMS> struct §(x)X { T1 a; T2 b; };
)cpp");
    ASSERT_TRUE(compile());

    auto* parameters = llvm::cast<clang::ClassTemplateDecl>(find_decl(*this, "main.cpp", "x"))
                           ->getTemplateParameters();
    ASSERT_EQ(parameters->size(), 2U);
    EXPECT_NE(unit->entity(parameters->getParam(0)), unit->entity(parameters->getParam(1)));
}

TEST_CASE(FileScopeTypedefs) {
    add_main("a.cpp", R"cpp(
typedef struct { int §(field)x; } §(state)State;
using §(alias)Result = int;
)cpp");
    ASSERT_TRUE(compile());

    Tester other;
    other.add_main("b.cpp", R"cpp(
typedef struct { int §(field)x; } §(state)State;
using §(alias)Result = int;
)cpp");
    ASSERT_TRUE(other.compile());

    for(auto marker: {"state", "alias", "field"}) {
        EXPECT_NE(entity_at(*this, "a.cpp", marker), entity_at(other, "b.cpp", marker));
    }
}

TEST_CASE(HeaderAcrossUnits) {
    llvm::StringRef first = R"cpp(
#pragma once
namespace ns {
inline namespace v1 { struct §(s)S { void §(m)method(int) const; }; }
template<typename T> requires (sizeof(T) > 1) struct §(t)Tpl { T value; };
template<typename T> struct §(p)Tpl<T*> {};
static void §(st)helper();
namespace { int §(an)counter; }
auto §(lam)lam = [](auto x) { return x; };
template<typename T> auto §(sf)sfinae(T t) -> decltype(t.foo());
enum class §(en)Color { Red };
using §(al)Alias = S;
#define §(mac)MAC 1
inline void §(ov)overloaded(int);
inline void §(ov2)overloaded(double);
}
)cpp";
    llvm::StringRef second = R"cpp(
#pragma once
struct §(other)Other {};
)cpp";
    const char* markers[] = {"s", "m", "t", "p", "st", "an", "lam", "sf", "en", "al", "ov", "ov2"};

    add_file("first.h", first);
    add_file("second.h", second);
    add_main("a.cpp", R"cpp(
#include "first.h"
#include "second.h"
)cpp");
    ASSERT_TRUE(compile());

    Tester other;
    other.add_file("first.h", first);
    other.add_file("second.h", second);
    other.add_main("b.cpp", R"cpp(
#include "second.h"
#include "first.h"
)cpp");
    ASSERT_TRUE(other.compile());

    for(auto marker: markers) {
        EXPECT_EQ(entity_at(*this, "first.h", marker), entity_at(other, "first.h", marker));
    }
    EXPECT_EQ(macro_entity(*this, "first.h", "MAC"), macro_entity(other, "first.h", "MAC"));
    EXPECT_NE(entity_at(*this, "first.h", "ov"), entity_at(*this, "first.h", "ov2"));
}

TEST_CASE(ParentChain) {
    add_main("main.cpp", R"cpp(
namespace §(ns)ns {
extern "C" { void §(c)c_linkage(); }
template <typename T> struct §(box)Box { void §(method)method(); };
template <> struct §(spec)Box<int> { void §(spec_method)method(); };
void §(fn)fn() { struct §(local)Local {}; }
}
void §(global)global();
)cpp");
    ASSERT_TRUE(compile());

    auto parent = [&](llvm::StringRef marker) {
        return unit->parent(find_decl(*this, "main.cpp", marker));
    };
    EXPECT_EQ(parent("global"), 0u);
    EXPECT_EQ(parent("ns"), 0u);
    EXPECT_EQ(parent("c"), entity("ns"));
    EXPECT_EQ(parent("box"), entity("ns"));
    EXPECT_EQ(parent("method"), entity("box"));
    EXPECT_EQ(parent("spec_method"), entity("spec"));
    EXPECT_NE(entity("spec"), entity("box"));
    EXPECT_EQ(parent("local"), entity("fn"));
}

TEST_CASE(ModuleEntity) {
    add_main("main.cpp", "int x;");
    ASSERT_TRUE(compile());
    Tester other;
    other.add_main("main.cpp", "int y;");
    ASSERT_TRUE(other.compile());

    EXPECT_EQ(unit->module_entity("foo"), other.unit->module_entity("foo"));
    EXPECT_NE(unit->module_entity("foo"), unit->module_entity("foo:part"));
}

};  // TEST_SUITE(identity)

}  // namespace

}  // namespace clice::testing
