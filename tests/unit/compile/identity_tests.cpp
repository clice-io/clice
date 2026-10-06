module;

#include "modules/prelude.h"

#include "support/logging.macros.h"

module clice;

import :semantic.decls;
import :support.logging;
import :tests.unit.test.test;
import :tests.unit.test.tester;

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

ZEST_SUITE(identity, Tester) {

std::uint64_t entity(llvm::StringRef marker) {
    return entity_at(*this, "main.cpp", marker);
}

ZEST_CASE(RequiresClauseEquivalence) {
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
    ZASSERT(compile());

    ZEXPECT(entity("1") == entity("2"));
    ZEXPECT(entity("11") != entity("12"));
    ZEXPECT(entity("12") == entity("13"));
}

ZEST_CASE(ConceptConstraintPosition) {
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
    ZASSERT(compile());

    ZEXPECT(entity("1") != entity("2"));
}

ZEST_CASE(ConstrainedPackDiffers) {
    add_main("main.cpp", R"cpp(
template<typename... Ts>
struct §(1)A;
)cpp");
    ZASSERT(compile());

    Tester other;
    other.add_main("main.cpp", R"cpp(
template<typename T>
concept C = requires(T t) { true; };
template<C... Ts>
struct §(2)A;
)cpp");
    ZASSERT(other.compile());

    ZEXPECT(entity("1") != entity_at(other, "main.cpp", "2"));
}

ZEST_CASE(TemplateArgumentExpression) {
    add_main("main.cpp", R"cpp(
template <typename T, int N> struct C;

template <int N> struct §(1)C<float, N>;
template <int M> struct §(2)C<float, M>;
template <char c> struct §(3)C<float, c>;
)cpp");
    ZASSERT(compile());

    ZEXPECT(entity("1") == entity("2"));
    ZEXPECT(entity("1") != entity("3"));
}

ZEST_CASE(FunctionRequiresClause) {
    add_main("main.cpp", R"cpp(
template<typename T>
void §(1)func(T t) requires (sizeof(T) == 4) {};

template<typename T>
void §(2)func(T t) requires (sizeof(T) == 8) {};
)cpp");
    ZASSERT(compile());

    ZEXPECT(entity("1") != entity("2"));
}

ZEST_CASE(VariableTemplateConstraint) {
    add_main("main.cpp", R"cpp(
template <typename T>
constexpr T §(1)pi = 3.14;
)cpp");
    ZASSERT(compile());

    Tester other;
    other.add_main("main.cpp", R"cpp(
template <typename T>
concept integral = requires (T t) { t + 1; };
template <integral T>
constexpr T §(2)pi = 3;
)cpp");
    ZASSERT(other.compile());

    ZEXPECT(entity("1") != entity_at(other, "main.cpp", "2"));
}

ZEST_CASE(DeducedParameterType) {
    add_main("main.cpp", R"cpp(
template<typename T>
struct array {};
template<typename U, array arr>
struct §(1)L;
)cpp");
    ZASSERT(compile());

    Tester other;
    other.add_main("main.cpp", R"cpp(
template<typename U, int arr>
struct §(2)L;
)cpp");
    ZASSERT(other.compile());

    ZEXPECT(entity("1") != entity_at(other, "main.cpp", "2"));
}

ZEST_CASE(TemplateParamObject) {
    add_main("main.cpp", R"cpp(
template<typename T> struct array {
  constexpr array(T x_) : x(x_) {}
  int x;
};
template<array U> struct L;
template<> struct §(1)L<{1}>;
template<> struct §(2)L<{2}>;
)cpp");
    ZASSERT(compile());

    ZEXPECT(entity("1") != entity("2"));
}

ZEST_CASE(PlaceholderConstraint) {
    add_main("main.cpp", R"cpp(
template<auto N> struct §(1)M;
)cpp");
    ZASSERT(compile());

    Tester other;
    other.add_main("main.cpp", R"cpp(
template<typename T> concept C = requires {requires true;};
template<C auto N> struct §(2)M;
)cpp");
    ZASSERT(other.compile());

    ZEXPECT(entity("1") != entity_at(other, "main.cpp", "2"));
}

ZEST_CASE(DependentTemplateName) {
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
    ZASSERT(compile());

    ZEXPECT(entity("1") != entity("2"));

    Tester again;
    again.add_main("main.cpp", content);
    ZASSERT(again.compile());

    ZEXPECT(entity("1") == entity_at(again, "main.cpp", "1"));
    ZEXPECT(entity("2") == entity_at(again, "main.cpp", "2"));
}

ZEST_CASE(DeducingThis) {
    add_main("main.cpp", R"cpp(
class A {
public:
    template <typename Self>
    void §(1)foo(this Self&& s);

    template <typename Self>
    void §(2)foo(Self&& s);
};
)cpp");
    ZASSERT(compile("-std=c++23"));

    ZEXPECT(entity("1") != entity("2"));
}

ZEST_CASE(MacroPathDistinct) {
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
    ZASSERT(compile());

    ZEXPECT(macro_entity(*this, "a/x.h", "FOO") != macro_entity(*this, "b/x.h", "FOO"));
}

ZEST_CASE(NamespaceLambdasDistinct) {
    add_main("main.cpp", R"cpp(
auto §(l1)l1 = [](int) {};
auto §(l2)l2 = [](int) {};
void §(a1)accept(decltype(l1));
void §(a2)accept(decltype(l2));
)cpp");
    ZASSERT(compile());

    ZEXPECT(entity("a1") != entity("a2"));
}

ZEST_CASE(MemberRequiresDistinct) {
    add_main("main.cpp", R"cpp(
template<typename T> concept C = requires(T t) { t + 1; };
template<typename T> concept D = requires(T t) { t - 1; };
template<typename T> struct A {
    void §(1)f() requires C<T>;
    void §(2)f() requires D<T>;
};
)cpp");
    ZASSERT(compile());

    ZEXPECT(entity("1") != entity("2"));
}

ZEST_CASE(SpecializationReturnType) {
    add_main("main.cpp", R"cpp(
struct X { using A = int; using B = long; };
template<typename T> typename T::A g();
template<typename T> typename T::B g();
template<> X::A §(1)g<X>();
template<> X::B §(2)g<X>();
)cpp");
    ZASSERT(compile());

    ZEXPECT(entity("1") != entity("2"));
}

ZEST_CASE(TemplateFriendDefinition) {
    add_main("main.cpp", R"cpp(
template<typename T> struct X {
    friend void §(1)f(int);
};
void §(2)f(int) {}
)cpp");
    ZASSERT(compile());

    ZEXPECT(entity("1") == entity("2"));
}

ZEST_CASE(MemberVariableTemplate) {
    add_main("main.cpp", R"cpp(
struct S {
    template<typename T> static constexpr bool §(1)value = true;
};
)cpp");
    ZASSERT(compile());

    auto* decl = find_decl(*this, "main.cpp", "1");
    auto* wrapper = llvm::dyn_cast<clang::VarTemplateDecl>(decl);
    ZASSERT(wrapper != nullptr);
    ZEXPECT(unit->entity(wrapper) == unit->entity(wrapper->getTemplatedDecl()));
}

ZEST_CASE(MemberPointerOverloads) {
    add_main("main.cpp", R"cpp(
struct S { int m; int mf(); };
void §(1)p(int S::*);
void §(2)p(int (S::*)());
)cpp");
    ZASSERT(compile());

    ZEXPECT(entity("1") != entity("2"));
}

ZEST_CASE(ConversionRenamedParameter) {
    add_main("main.cpp", R"cpp(
template<typename T> struct A {
    §(1)operator T*();
};
template<typename U> A<U>::§(2)operator U*() { return nullptr; }
)cpp");
    ZASSERT(compile());

    ZEXPECT(entity("1") == entity("2"));
}

ZEST_CASE(NoexceptPointerOverloads) {
    add_main("main.cpp", R"cpp(
void §(1)q(void (*)());
void §(2)q(void (*)() noexcept);
)cpp");
    ZASSERT(compile());

    ZEXPECT(entity("1") != entity("2"));
}

ZEST_CASE(AnonymousNamespaceFiles) {
    llvm::StringRef content = R"cpp(
namespace { struct §(s)S {}; }
namespace { typedef enum { A } §(e)E; }
)cpp";
    add_main("a/x.cpp", content);
    ZASSERT(compile());

    Tester other;
    other.add_main("b/x.cpp", content);
    ZASSERT(other.compile());

    ZEXPECT(entity_at(*this, "a/x.cpp", "s") != entity_at(other, "b/x.cpp", "s"));
    ZEXPECT(entity_at(*this, "a/x.cpp", "e") != entity_at(other, "b/x.cpp", "e"));
}

ZEST_CASE(TopLevelConstParameter) {
    add_main("main.cpp", R"cpp(
void §(1)c(const int);
void §(2)c(int) {}
)cpp");
    ZASSERT(compile());

    ZEXPECT(entity("1") == entity("2"));
}

ZEST_CASE(DeclarationArgumentType) {
    add_main("main.cpp", R"cpp(
inline int a[1];
template<auto> struct X;
template<> struct §(1)X<a> {};
template<> struct §(2)X<&a> {};
)cpp");
    ZASSERT(compile());

    ZEXPECT(entity("1") != entity("2"));
}

ZEST_CASE(StructuralValuePath) {
    add_main("main.cpp", R"cpp(
union U { int a; int b; };
inline U u;
template<int*> struct X;
template<> struct §(1)X<&u.a> {};
template<> struct §(2)X<&u.b> {};
)cpp");
    ZASSERT(compile());

    ZEXPECT(entity("1") != entity("2"));
}

ZEST_CASE(ConstrainedFriendLexical) {
    add_main("main.cpp", R"cpp(
template<typename T> struct A {
    friend void §(1)h(A) requires (sizeof(T) > 1) {}
};
template<typename T> struct B {
    friend void §(2)h(A<T>) requires (sizeof(T) > 1) {}
};
)cpp");
    ZASSERT(compile());

    ZEXPECT(entity("1") != entity("2"));
}

ZEST_CASE(ExternCAcrossNamespaces) {
    add_main("main.cpp", R"cpp(
namespace A { extern "C" void §(1)f(); }
namespace B { extern "C" void §(2)f(); }
)cpp");
    ZASSERT(compile());

    ZEXPECT(entity("1") == entity("2"));
}

ZEST_CASE(IncludedLocals) {
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
    ZASSERT(compile());

    ZEXPECT(entity_at(*this, "a.inc", "x") != entity_at(*this, "b.inc", "x"));
}

ZEST_CASE(SpecializationMemberConstraint) {
    add_main("main.cpp", R"cpp(
template<typename T> struct S { static constexpr int n = 0; };
template<typename U> void §(1)f(U) requires (sizeof(U) == S<int>::n);
template<typename U> void §(2)f(U) requires (sizeof(U) == S<double>::n);
)cpp");
    ZASSERT(compile());

    ZEXPECT(entity("1") != entity("2"));
}

ZEST_CASE(OffsetofMembers) {
    add_main("main.cpp", R"cpp(
template<typename T> void §(1)f(T) requires (__builtin_offsetof(T, x) == 0);
template<typename T> void §(2)f(T) requires (__builtin_offsetof(T, y) == 0);
)cpp");
    ZASSERT(compile());

    ZEXPECT(entity("1") != entity("2"));
}

ZEST_CASE(ParenthesesDistinct) {
    add_main("main.cpp", R"cpp(
template<int N> void §(1)f() requires (N > 0);
template<int N> void §(2)f() requires ((N) > 0);
)cpp");
    ZASSERT(compile());

    ZEXPECT(entity("1") != entity("2"));
}

ZEST_CASE(DecltypeSizeofDistinct) {
    add_main("main.cpp", R"cpp(
template<typename T> auto §(1)f() -> decltype(sizeof(T));
template<typename T> auto §(2)f() -> __SIZE_TYPE__;
)cpp");
    ZASSERT(compile());

    ZEXPECT(entity("1") != entity("2"));
}

ZEST_CASE(ConceptResolvedThroughUsing) {
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
    ZASSERT(compile());

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
    ZASSERT(other.compile());

    ZEXPECT(entity_at(*this, "h.h", "f") != entity_at(other, "h.h", "f"));
}

ZEST_CASE(EarlierDecltypeNoEffect) {
    llvm::StringRef header = R"cpp(
template<typename T> requires C<T> void §(f)f();
)cpp";
    add_main("main.cpp", R"cpp(
template<typename T> concept C = true;
#include "h.h"
)cpp");
    add_file("h.h", header);
    ZASSERT(compile());

    Tester other;
    other.add_file("h.h", header);
    other.add_main("main.cpp", R"cpp(
template<typename T> concept C = true;
template<typename T> using Noise = decltype(::C<T>);
#include "h.h"
)cpp");
    ZASSERT(other.compile());

    ZEXPECT(entity_at(*this, "h.h", "f") == entity_at(other, "h.h", "f"));
}

ZEST_CASE(MacroRedefinition) {
    add_main("main.cpp", R"cpp(
#define FOO 1
#undef FOO
#define FOO 2
)cpp");
    ZASSERT(compile());

    std::vector<std::uint64_t> entities;
    for(auto& [fid, directive]: unit->directives()) {
        for(auto& macro: directive.macros) {
            if(macro.kind == MacroRef::Def) {
                entities.push_back(unit->entity(macro.macro));
            }
        }
    }
    ZASSERT(entities.size() == 2U);
    ZEXPECT(entities[0] != entities[1]);
}

ZEST_CASE(CopyDeductionCandidate) {
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
    ZASSERT(compile());

    Tester other;
    other.add_file("box.h", header);
    other.add_main("b.cpp", R"cpp(
#include "box.h"
Box<char> spelled = make();
)cpp");
    ZASSERT(other.compile());

    ZEXPECT(entity_at(*this, "box.h", "t") == entity_at(other, "box.h", "t"));
    ZEXPECT(entity_at(*this, "box.h", "d") == entity_at(other, "box.h", "d"));
}

ZEST_CASE(CopyDeductionCandidateRedeclared) {
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
    ZASSERT(compile());

    Tester other;
    other.add_file("box.h", header);
    other.add_main("b.cpp", R"cpp(
#include "box.h"
)cpp");
    ZASSERT(other.compile());

    ZEXPECT(entity_at(*this, "box.h", "t") == entity_at(other, "box.h", "t"));
}

ZEST_CASE(ComplexSubobjectArgument) {
    add_main("main.cpp", R"cpp(
inline _Complex int z;
template<int*> struct X;
template<> struct §(real)X<&__real__ z> {};
template<> struct §(imag)X<&__imag__ z> {};
)cpp");
    ZASSERT(compile());

    ZEXPECT(entity_at(*this, "main.cpp", "real") != entity_at(*this, "main.cpp", "imag"));
}

ZEST_CASE(ImplicitConstructorUse) {
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
    ZASSERT(compile());

    Tester other;
    other.add_file("s.h", header);
    other.add_main("b.cpp", R"cpp(
#include "s.h"
struct T : S { using S::S; };
)cpp");
    ZASSERT(other.compile());

    ZEXPECT(copy_constructor(*this) == copy_constructor(other));
}

ZEST_CASE(NestedMacroLambdas) {
    add_main("main.cpp", R"cpp(
#define L []{}
#define BOTH auto a = L; auto b = L;
BOTH
void §(f1)f(decltype(a));
void §(f2)f(decltype(b));
)cpp");
    ZASSERT(compile());

    ZEXPECT(entity_at(*this, "main.cpp", "f1") != entity_at(*this, "main.cpp", "f2"));
}

ZEST_CASE(TemplateNameSpelling) {
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
    ZASSERT(compile());

    Tester other;
    other.add_file("h.h", header);
    other.add_main("b.cpp", R"cpp(
#include "h.h"
)cpp");
    ZASSERT(other.compile());

    ZEXPECT(entity_at(*this, "h.h", "f") == entity_at(other, "h.h", "f"));
}

ZEST_CASE(ArrayFillerArgument) {
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
    ZASSERT(compile());

    Tester other;
    other.add_file("h.h", header);
    other.add_main("b.cpp", R"cpp(
#include "h.h"
)cpp");
    ZASSERT(other.compile());

    ZEXPECT(entity_at(*this, "h.h", "x") == entity_at(other, "h.h", "x"));
    ZEXPECT(entity_at(*this, "h.h", "f") == entity_at(other, "h.h", "f"));
}

ZEST_CASE(InstantiatedLambdas) {
    add_main("main.cpp", R"cpp(
template<class T> inline auto v = []{};
void §(f1)f(decltype(v<int>));
void §(f2)f(decltype(v<double>));
)cpp");
    ZASSERT(compile());

    ZEXPECT(entity_at(*this, "main.cpp", "f1") != entity_at(*this, "main.cpp", "f2"));
}

ZEST_CASE(LocalClassConstructors) {
    add_main("main.cpp", R"cpp(
void f() {
    struct §(s)S {};
    S a;
    S b(a);
    S c(static_cast<S&&>(a));
}
)cpp");
    ZASSERT(compile());

    auto* record = llvm::cast<clang::CXXRecordDecl>(find_decl(*this, "main.cpp", "s"));
    std::set<std::uint64_t> entities;
    for(auto* ctor: record->ctors()) {
        entities.insert(unit->entity(ctor));
    }
    ZEXPECT(entities.size() == 3U);
}

ZEST_CASE(MacroArgumentTwice) {
    add_main("main.cpp", R"cpp(
#define BOTH(X) auto a = X; auto b = X;
BOTH([]{})
void §(f1)f(decltype(a));
void §(f2)f(decltype(b));
)cpp");
    ZASSERT(compile());

    ZEXPECT(entity_at(*this, "main.cpp", "f1") != entity_at(*this, "main.cpp", "f2"));
}

ZEST_CASE(MacroArgumentPreExpanded) {
    add_main("main.cpp", R"cpp(
#define F void f(decltype([]{})) {}
#define TWO F F
#define ID(...) __VA_ARGS__
ID(TWO)
)cpp");
    ZASSERT(compile());

    std::set<std::uint64_t> entities;
    for(auto* decl: unit->tu()->decls()) {
        if(auto* function = llvm::dyn_cast<clang::FunctionDecl>(decl);
           function && function->getName() == "f") {
            entities.insert(unit->entity(decls::normalize(function)));
        }
    }
    ZEXPECT(entities.size() == 2U);
}

ZEST_CASE(CpuSpecificVersions) {
    triple = "x86_64-unknown-linux-gnu";
    add_main("main.cpp", R"cpp(
__attribute__((cpu_specific(generic))) void §(generic)f() {}
__attribute__((cpu_specific(pentium_4))) void §(pentium)f() {}
)cpp");
    ZASSERT(compile());

    ZEXPECT(entity_at(*this, "main.cpp", "generic") != entity_at(*this, "main.cpp", "pentium"));
}

ZEST_CASE(NoreturnRedeclarationOrder) {
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
    ZASSERT(compile());

    Tester other;
    other.add_file("plain.h", plain);
    other.add_file("attributed.h", attributed);
    other.add_main("b.cpp", R"cpp(
#include "attributed.h"
#include "plain.h"
)cpp");
    ZASSERT(other.compile());

    ZEXPECT(entity_at(*this, "plain.h", "f") == entity_at(other, "plain.h", "f"));
    ZEXPECT(entity_at(*this, "plain.h", "f") == entity_at(*this, "attributed.h", "f"));
}

ZEST_CASE(PastedTemplateParameters) {
    add_main("main.cpp", R"cpp(
#define PARAMS class T##1, class T##2
template<PARAMS> struct §(x)X { T1 a; T2 b; };
)cpp");
    ZASSERT(compile());

    auto* parameters = llvm::cast<clang::ClassTemplateDecl>(find_decl(*this, "main.cpp", "x"))
                           ->getTemplateParameters();
    ZASSERT(parameters->size() == 2U);
    ZEXPECT(unit->entity(parameters->getParam(0)) != unit->entity(parameters->getParam(1)));
}

ZEST_CASE(FileScopeTypedefs) {
    add_main("a.cpp", R"cpp(
typedef struct { int §(field)x; } §(state)State;
using §(alias)Result = int;
)cpp");
    ZASSERT(compile());

    Tester other;
    other.add_main("b.cpp", R"cpp(
typedef struct { int §(field)x; } §(state)State;
using §(alias)Result = int;
)cpp");
    ZASSERT(other.compile());

    for(auto marker: {"state", "alias", "field"}) {
        ZEXPECT(entity_at(*this, "a.cpp", marker) != entity_at(other, "b.cpp", marker));
    }
}

ZEST_CASE(CTagsPerFile) {
    llvm::StringRef header = R"c(
struct §(shared)shared { int x; };
struct §(opaque)opaque;
)c";
    add_file("forward.h", "struct opaque;\n");
    add_file("common.h", header);
    add_main("a.c", R"c(
#include "forward.h"
#include "common.h"
struct §(state)state { int x; };
)c");
    ZASSERT(compile("-std=c17"));

    Tester other;
    other.add_file("common.h", header);
    other.add_main("b.c", R"c(
#include "common.h"
struct §(state)state { int x; };
)c");
    ZASSERT(other.compile("-std=c17"));

    ZEXPECT(entity_at(*this, "a.c", "state") != entity_at(other, "b.c", "state"));
    ZEXPECT(entity_at(*this, "common.h", "shared") == entity_at(other, "common.h", "shared"));
    ZEXPECT(entity_at(*this, "common.h", "opaque") == entity_at(other, "common.h", "opaque"));
}

ZEST_CASE(CTagsInContext) {
    add_file("part.h", "struct ctx;\nstruct §(part)part { int x; };\n");
    add_main("host.c", R"c(
struct §(ctx)ctx { int x; };
#include "part.h"
)c");
    ZASSERT(compile("-std=c17"));

    /// The header compiled as its host sees it: the host's text before the
    /// include arrives as a fragment the compile -includes, named after
    /// the host by its opening #line marker — not by a #line the host's
    /// own text carries.
    std::string marker = "#line 1 \"";
    for(char c: unit->file_path(unit->main_file())) {
        if(c == '\\') {
            marker += '\\';
        }
        marker += c;
    }
    marker += "\"\n";

    Tester context;
    context.add_main("part.h", "struct §(ctx)ctx;\nstruct §(part)part { int x; };\n");
    context.prepare("-std=c17");
    auto prefix = TestVFS::path(".clice-prefix.h");
    context.params.add_synthesized({
        {prefix, marker + "struct ctx { int x; };\n#line 9 \"elsewhere.c\"\n"}
    });
    context.owned_args.insert(context.owned_args.end() - 1, {"-include", prefix});
    context.params.arguments.clear();
    for(auto& arg: context.owned_args) {
        context.params.arguments.push_back(arg.c_str());
    }
    ZASSERT(context.try_compile());

    ZEXPECT(entity_at(context, "part.h", "ctx") == entity_at(*this, "host.c", "ctx"));
    ZEXPECT(entity_at(context, "part.h", "part") == entity_at(*this, "part.h", "part"));
}

ZEST_CASE(CLinkageIgnoresTypes) {
    add_main("a.c", R"c(
typedef int wchar_t;
int §(f)f();
void §(g)g(wchar_t c);
)c");
    ZASSERT(compile("-std=c17"));

    Tester other;
    other.add_main("b.cpp", R"cpp(
extern "C" int §(f)f(void);
extern "C" void §(g)g(wchar_t c);
)cpp");
    ZASSERT(other.compile());

    ZEXPECT(entity_at(*this, "a.c", "f") == entity_at(other, "b.cpp", "f"));
    ZEXPECT(entity_at(*this, "a.c", "g") == entity_at(other, "b.cpp", "g"));
}

ZEST_CASE(OverloadableCFunctions) {
    add_main("a.c", R"c(
__attribute__((overloadable)) void §(i)f(int);
__attribute__((overloadable)) void §(d)f(double);
)c");
    ZASSERT(compile("-std=c17"));

    ZEXPECT(entity_at(*this, "a.c", "i") != entity_at(*this, "a.c", "d"));
}

ZEST_CASE(HeaderAcrossUnits) {
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
    ZASSERT(compile());

    Tester other;
    other.add_file("first.h", first);
    other.add_file("second.h", second);
    other.add_main("b.cpp", R"cpp(
#include "second.h"
#include "first.h"
)cpp");
    ZASSERT(other.compile());

    for(auto marker: markers) {
        ZEXPECT(entity_at(*this, "first.h", marker) == entity_at(other, "first.h", marker));
    }
    ZEXPECT(macro_entity(*this, "first.h", "MAC") == macro_entity(other, "first.h", "MAC"));
    ZEXPECT(entity_at(*this, "first.h", "ov") != entity_at(*this, "first.h", "ov2"));
}

ZEST_CASE(ParentChain) {
    add_main("main.cpp", R"cpp(
namespace §(ns)ns {
extern "C" { void §(c)c_linkage(); }
template <typename T> struct §(box)Box { void §(method)method(); };
template <> struct §(spec)Box<int> { void §(spec_method)method(); };
void §(fn)fn() { struct §(local)Local {}; }
namespace { void §(hidden)hidden(); }
}
void §(global)global();
)cpp");
    ZASSERT(compile());

    auto parent = [&](llvm::StringRef marker) {
        return unit->parent(find_decl(*this, "main.cpp", marker));
    };
    ZEXPECT(parent("global") == 0u);
    ZEXPECT(parent("ns") == 0u);
    ZEXPECT(parent("c") == 0u);
    ZEXPECT(parent("box") == entity("ns"));
    ZEXPECT(parent("method") == entity("box"));
    ZEXPECT(parent("spec_method") == entity("spec"));
    ZEXPECT(entity("spec") != entity("box"));
    ZEXPECT(parent("local") == entity("fn"));
    ZEXPECT(parent("hidden") == entity("ns"));
}

ZEST_CASE(ModuleEntity) {
    add_main("main.cpp", "int x;");
    ZASSERT(compile());
    Tester other;
    other.add_main("main.cpp", "int y;");
    ZASSERT(other.compile());

    ZEXPECT(unit->module_entity("foo") == other.unit->module_entity("foo"));
    ZEXPECT(unit->module_entity("foo") != unit->module_entity("foo:part"));
}

};  // ZEST_SUITE(identity)

}  // namespace

}  // namespace clice::testing
