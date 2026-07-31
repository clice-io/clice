#include "test/test.h"
#include "test/tester.h"
#include "semantic/semantics.h"

#include "clang/AST/RecursiveASTVisitor.h"

namespace clice::testing {

namespace {

struct InputFinder : clang::RecursiveASTVisitor<InputFinder> {
    CompilationUnitRef unit;
    clang::QualType input;
    clang::QualType expect;

    using Base = clang::RecursiveASTVisitor<InputFinder>;

    InputFinder(CompilationUnitRef unit) : unit(unit) {}

    bool TraverseDecl(clang::Decl* decl) {
        if(decl && (llvm::isa<clang::TranslationUnitDecl>(decl) ||
                    unit.file_id(decl->getLocation()) == unit.interested_file())) {
            Base::TraverseDecl(decl);
        }

        return true;
    }

    bool VisitTypedefNameDecl(const clang::TypedefNameDecl* decl) {
        if(decl->getName() == "input") {
            input = decl->getUnderlyingType();
        }

        if(decl->getName() == "expect") {
            expect = decl->getUnderlyingType();
        }

        return true;
    }
};

TEST_SUITE(TemplateResolver, Tester) {

void run(llvm::StringRef code) {
    add_main("main.cpp", code);
    ASSERT_TRUE(compile());

    InputFinder finder(*unit);
    finder.TraverseAST(unit->context());

    auto input = unit->resolver().resolve(finder.input);
    auto target = finder.expect;
    ASSERT_FALSE(input.isNull() || target.isNull());
    EXPECT_EQ(input.getCanonicalType(), target.getCanonicalType());
}

TEST_CASE(TypeParameterType) {
    run(R"code(
        template <typename T>
        struct A {
            using type = T;
        };

        template <typename X>
        struct test {
            using input = typename A<X>::type;
            using expect = X;
        };
    )code");
}

TEST_CASE(SingleLevel) {
    run(R"code(
        template <typename... Ts>
        struct type_list {};

        template <typename T>
        struct A {
            using type = type_list<T>;
        };

        template <typename X>
        struct test {
            using input = typename A<X>::type;
            using expect = type_list<X>;
        };
    )code");
}

TEST_CASE(SingleLevelNotDependent) {
    run(R"code(
        template <typename T>
        struct A {
            using type = int;
        };

        template <typename X>
        struct test {
            using input = typename A<X>::type;
            using expect = int;
        };
    )code");
}

TEST_CASE(MultiLevel) {
    run(R"code(
        template <typename... Ts>
        struct type_list {};

        template <typename T1>
        struct A {
            using type = type_list<T1>;
        };

        template <typename T2>
        struct B {
            using type = typename A<T2>::type;
        };

        template <typename T3>
        struct C {
            using type = typename B<T3>::type;
        };

        template <typename X>
        struct test {
            using input = typename C<X>::type;
            using expect = type_list<X>;
        };
    )code");
}

TEST_CASE(MultiLevelNotDependent) {
    run(R"code(
        template <typename T1>
        struct A {
            using type = int;
        };

        template <typename T2>
        struct B {
            using type = typename A<T2>::type;
        };

        template <typename T3>
        struct C {
            using type = typename B<T3>::type;
        };

        template <typename X>
        struct test {
            using input = typename C<X>::type;
            using expect = int;
        };
    )code");
}

TEST_CASE(ArgumentDependent) {
    run(R"code(
        template <typename... Ts>
        struct type_list {};

        template <typename T1>
        struct A {
            using type = T1;
        };

        template <typename T2>
        struct B {
            using type = type_list<T2>;
        };

        template <typename X>
        struct test {
            using input = typename B<typename A<X>::type>::type;
            using expect = type_list<X>;
        };
    )code");
}

TEST_CASE(AliasArgument) {
    run(R"code(
        template <typename... Ts>
        struct type_list {};

        template <typename T1>
        struct A {
            using type = T1;
        };

        template <typename T2>
        struct B {
            using base = A<T2>;
            using type = type_list<typename base::type>;
        };

        template <typename X>
        struct test {
            using input = typename B<X>::type;
            using expect = type_list<X>;
        };
    )code");
}

TEST_CASE(AliasDependent) {
    run(R"code(
        template <typename... Ts>
        struct type_list {};

        template <typename T1>
        struct A {
            using type = type_list<T1>;
        };

        template <typename T2>
        struct B {
            using base = A<T2>;
            using type = typename base::type;
        };

        template <typename X>
        struct test {
            using input = typename B<X>::type;
            using expect = type_list<X>;
        };
    )code");
}

TEST_CASE(AliasTemplate) {
    run(R"code(
        template <typename... Ts>
        struct type_list {};

        template <typename T1, typename U1>
        struct A {
            using type = type_list<T1, U1>;
        };

        template <typename T2>
        struct B {
            template <typename U2>
            using type = typename A<T2, U2>::type;
        };

        template <typename X, typename Y>
        struct test {
            using input = typename B<X>::template type<Y>;
            using expect = type_list<X, Y>;
        };
    )code");
}

TEST_CASE(BaseDependent) {
    run(R"code(
        template <typename... Ts>
        struct type_list {};

        template <typename T1>
        struct A {
            using type = type_list<T1>;
        };

        template <typename U2>
        struct B : A<U2> {};

        template <typename X>
        struct test {
            using input = typename B<X>::type;
            using expect = type_list<X>;
        };
    )code");
}

TEST_CASE(MultiNested) {
    run(R"code(
        template <typename... Ts>
        struct type_list {};

        template <typename T1>
        struct A {
            using self = A<T1>;
            using type = type_list<T1>;
        };

        template <typename X>
        struct test {
            using input = typename A<X>::self::self::self::self::self::type;
            using expect = type_list<X>;
        };
    )code");
}

TEST_CASE(OuterDependentMemberClass) {
    run(R"code(
        template <typename... Ts>
        struct type_list {};

        template <typename T1>
        struct A {
            template <typename T2>
            struct B {
                template <typename T3>
                struct C {
                    using type = type_list<T1, T2, T3>;
                };
            };
        };

        template <typename X, typename Y, typename Z>
        struct test {
            using input = typename A<X>::template B<Y>::template C<Z>::type;
            using expect = type_list<X, Y, Z>;
        };
    )code");
}

TEST_CASE(InnerDependentMemberClass) {
    run(R"code(
        template <typename... Ts>
        struct type_list {};

        template <typename T>
        struct test {
            template <int N, typename U>
            struct B {
                using type = type_list<U, T>;
            };

            using input = typename B<1, T>::type;
            using expect = type_list<T, T>;
        };
    )code");
}

TEST_CASE(InnerPartialMember) {
    run(R"code(
        template <typename... Ts>
        struct type_list {};

        template <typename T, typename U>
        struct test {};

        template <typename T>
        struct test<T, T> {
            template <int N, typename U>
            struct A {
                using type = type_list<U, T>;
            };

            using input = typename A<1, T>::type;
            using expect = type_list<T, T>;
        };
    )code");
}

TEST_CASE(PartialSpecialization) {
    run(R"code(
        template <typename... Ts>
        struct type_list {};

        template <typename T1>
        struct A {};

        template <typename U2>
        struct B {};

        template <typename U2, template <typename...> typename HKT>
        struct B<HKT<U2>> {
            using type = type_list<U2>;
        };

        template <typename X>
        struct test {
            using input = typename B<A<X>>::type;
            using expect = type_list<X>;
        };
    )code");
}

TEST_CASE(PartialDefaultArgument) {
    run(R"code(
        template <typename T, typename U = T>
        struct X {};

        template <typename T>
        struct X<T, T> {
            using type = T;
        };

        template <typename T>
        struct test {
            using input = typename X<T>::type;
            using expect = T;
        };
    )code");
}

TEST_CASE(DefaultArgument) {
    run(R"code(
        template <typename... Ts>
        struct type_list {};

        template <typename T1>
        struct A {
            using type = type_list<T1>;
        };

        template <typename U1, typename U2 = A<U1>>
        struct B {
            using type = typename U2::type;
        };

        template <typename X>
        struct test {
            using input = typename B<X>::type;
            using expect = type_list<X>;
        };
    )code");
}

TEST_CASE(TemplateTemplateReplace) {
    run(R"code(
        template <typename T>
        struct box {};

        template <typename A, typename U>
        struct replace_first {};

        template <template <typename, typename...> typename TT,
                  typename U,
                  typename T,
                  typename... Ts>
        struct replace_first<TT<T, Ts...>, U> {
            using type = TT<U, Ts...>;
        };

        template <typename X>
        struct test {
            using input = typename replace_first<box<X>, int>::type;
            using expect = box<int>;
        };
    )code");
}

TEST_CASE(SfinaeRebindPresent) {
    run(R"code(
        template <typename... Ts>
        using void_t = void;

        template <typename T>
        struct alloc {
            template <typename U>
            struct rebind {
                using other = alloc<U>;
            };
        };

        template <typename A, typename U, typename = void>
        struct rebind_helper {
            using type = int;
        };

        template <typename A, typename U>
        struct rebind_helper<A, U, void_t<typename A::template rebind<U>::other>> {
            using type = typename A::template rebind<U>::other;
        };

        template <typename X>
        struct test {
            using input = typename rebind_helper<alloc<X>, float>::type;
            using expect = alloc<float>;
        };
    )code");
}

TEST_CASE(SfinaeRebindAbsent) {
    run(R"code(
        template <typename... Ts>
        using void_t = void;

        template <typename T>
        struct plain {};

        template <typename A, typename U>
        struct replace_first {};

        template <template <typename, typename...> typename TT,
                  typename U,
                  typename T,
                  typename... Ts>
        struct replace_first<TT<T, Ts...>, U> {
            using type = TT<U, Ts...>;
        };

        template <typename A, typename U, typename = void>
        struct rebind_helper {
            using type = typename replace_first<A, U>::type;
        };

        template <typename A, typename U>
        struct rebind_helper<A, U, void_t<typename A::template rebind<U>::other>> {
            using type = typename A::template rebind<U>::other;
        };

        template <typename X>
        struct test {
            using input = typename rebind_helper<plain<X>, int>::type;
            using expect = plain<int>;
        };
    )code");
}

TEST_CASE(NttpDefaultArgument) {
    run(R"code(
        template <typename T, int N = 0>
        struct S {
            using type = T;
        };

        template <typename X>
        struct test {
            using input = typename S<X>::type;
            using expect = X;
        };
    )code");
}

TEST_CASE(NttpArraySize) {
    run(R"code(
        template <typename T, unsigned long N>
        struct S {
            using type = T[N];
        };

        template <typename X>
        struct test {
            using input = typename S<X, 3>::type;
            using expect = X[3];
        };
    )code");
}

TEST_CASE(MultiElementPack) {
    run(R"code(
        template <typename... Ts>
        struct type_list {};

        template <typename... Us>
        struct A {
            using type = type_list<int, Us...>;
        };

        template <typename X, typename Y>
        struct test {
            using input = typename A<X, Y>::type;
            using expect = type_list<int, X, Y>;
        };
    )code");
}

TEST_CASE(PackExpansion) {
    run(R"code(
        template <typename... Ts>
        struct type_list {};

        template <typename U, typename... Us>
        struct X {
            using type = type_list<Us...>;
        };

        template <typename... Ts>
        struct test {
            using input = typename X<int, Ts...>::type;
            using expect = type_list<Ts...>;
        };
    )code");
}

TEST_CASE(BasePackExpansion) {
    run(R"code(
        template <typename... Ts>
        struct type_list {};

        template <typename U, typename... Us>
        struct X {
            using type = type_list<Us...>;
        };

        template <typename... Us>
        struct Y : X<int, Us...> {};

        template <typename... Ts>
        struct test {
            using input = typename Y<Ts...>::type;
            using expect = type_list<Ts...>;
        };
    )code");
}

TEST_CASE(RecursiveBaseClass) {
    // Regression test: callback_traits<F> inherits callback_traits<decltype(&F::operator())>,
    // creating infinite recursion through lookupInBases. CTD cycle detection must bail out.
    // We set input = expect because the resolver cannot fully resolve this pattern;
    // the test verifies it doesn't crash or hang.
    run(R"code(
        template <typename F>
        struct callback_traits : callback_traits<decltype(&F::operator())> {};

        template <typename R, typename C, typename... Args>
        struct callback_traits<R (C::*)(Args...) const> {
            using result_type = R;
        };

        template <typename F>
        struct test {
            using input = typename callback_traits<F>::result_type;
            using expect = typename callback_traits<F>::result_type;
        };
    )code");
}

TEST_CASE(PointerType) {
    run(R"code(
        template <typename T>
        struct A {
            using type = T*;
        };

        template <typename X>
        struct test {
            using input = typename A<X>::type;
            using expect = X*;
        };
    )code");
}

TEST_CASE(ReferenceType) {
    run(R"code(
        template <typename T>
        struct A {
            using type = T&;
        };

        template <typename X>
        struct test {
            using input = typename A<X>::type;
            using expect = X&;
        };
    )code");
}

TEST_CASE(ConstQualified) {
    run(R"code(
        template <typename T>
        struct A {
            using type = const T;
        };

        template <typename X>
        struct test {
            using input = typename A<X>::type;
            using expect = const X;
        };
    )code");
}

// TODO: Outer<int> is non-dependent, TransformNestedNameSpecifierLoc
// doesn't trigger our heuristic lookup for non-dependent qualifiers.
// TEST_CASE(NestedClassTemplate) { ... }

TEST_CASE(MultipleInheritance) {
    run(R"code(
        template <typename... Ts>
        struct type_list {};

        template <typename T>
        struct Base1 {
            using type1 = type_list<T>;
        };

        template <typename T>
        struct Base2 {
            using type2 = T;
        };

        template <typename T>
        struct Derived : Base1<T>, Base2<T> {};

        template <typename X>
        struct test {
            using input = typename Derived<X>::type1;
            using expect = type_list<X>;
        };
    )code");
}

TEST_CASE(SecondBaseInheritance) {
    run(R"code(
        template <typename T>
        struct Base1 {
            using type1 = int;
        };

        template <typename T>
        struct Base2 {
            using type2 = T;
        };

        template <typename T>
        struct Derived : Base1<T>, Base2<T> {};

        template <typename X>
        struct test {
            using input = typename Derived<X>::type2;
            using expect = X;
        };
    )code");
}

TEST_CASE(TypedefChain) {
    // Deep typedef chain that SubstituteOnly must expand
    run(R"code(
        template <typename T>
        struct A {
            using step1 = T;
            using step2 = step1;
            using step3 = step2;
            using type = step3;
        };

        template <typename X>
        struct test {
            using input = typename A<X>::type;
            using expect = X;
        };
    )code");
}

TEST_CASE(DependentBaseTypedef) {
    // Base class type depends on template parameter through alias
    run(R"code(
        template <typename... Ts>
        struct type_list {};

        template <typename T>
        struct Base {
            using value_type = T;
        };

        template <typename T>
        struct Derived {
            using base = Base<T>;
            using type = typename base::value_type;
        };

        template <typename X>
        struct test {
            using input = typename Derived<X>::type;
            using expect = X;
        };
    )code");
}

TEST_CASE(CRTPPattern) {
    // Common CRTP pattern
    run(R"code(
        template <typename Derived>
        struct Base {
            using derived_type = Derived;
        };

        template <typename T>
        struct Impl : Base<Impl<T>> {
            using type = T;
        };

        template <typename X>
        struct test {
            using input = typename Impl<X>::type;
            using expect = X;
        };
    )code");
}

// TODO: NTTP partial specialization matching not yet supported.
// checkTemplateArguments only fills default TemplateTypeParmDecl args.
// TEST_CASE(NonTypeTemplateParam) { ... }

TEST_CASE(IdentityAlias) {
    // Alias template that forwards type unchanged
    run(R"code(
        template <typename T>
        using identity = T;

        template <typename T>
        struct A {
            using type = identity<T>;
        };

        template <typename X>
        struct test {
            using input = typename A<X>::type;
            using expect = X;
        };
    )code");
}

TEST_CASE(ConditionalType) {
    // Partial specialization as conditional
    run(R"code(
        template <bool B, typename T, typename F>
        struct conditional {
            using type = T;
        };

        template <typename T, typename F>
        struct conditional<false, T, F> {
            using type = F;
        };

        template <typename X>
        struct test {
            using input = typename conditional<true, X, int>::type;
            using expect = X;
        };
    )code");
}

// TODO: Same as NonTypeTemplateParam — partial specialization on `false`
// requires NTTP matching which is not yet supported.
// TEST_CASE(ConditionalTypeFalse) { ... }

// TODO: Template template parameter deduction not yet supported.
// TEST_CASE(TemplateTemplateParam) { ... }

TEST_CASE(DependentReturnType) {
    // Resolve through a struct that wraps a function return type pattern
    run(R"code(
        template <typename T>
        struct remove_reference {
            using type = T;
        };

        template <typename T>
        struct remove_reference<T&> {
            using type = T;
        };

        template <typename T>
        struct remove_reference<T&&> {
            using type = T;
        };

        template <typename X>
        struct test {
            using input = typename remove_reference<X&>::type;
            using expect = X;
        };
    )code");
}

TEST_CASE(RvalueRefRemoval) {
    run(R"code(
        template <typename T>
        struct remove_reference {
            using type = T;
        };

        template <typename T>
        struct remove_reference<T&> {
            using type = T;
        };

        template <typename T>
        struct remove_reference<T&&> {
            using type = T;
        };

        template <typename X>
        struct test {
            using input = typename remove_reference<X&&>::type;
            using expect = X;
        };
    )code");
}

TEST_CASE(AddPointer) {
    run(R"code(
        template <typename T>
        struct add_pointer {
            using type = T*;
        };

        template <typename T>
        struct add_pointer<T&> {
            using type = T*;
        };

        template <typename X>
        struct test {
            using input = typename add_pointer<X&>::type;
            using expect = X*;
        };
    )code");
}

// TODO: enable_if<true, X> requires NTTP partial specialization matching.
// TEST_CASE(EnableIfLike) { ... }

TEST_CASE(NestedLookup) {
    // Two levels of dependent lookup: A<T>::B<T>::type
    run(R"code(
        template <typename... Ts>
        struct type_list {};

        template <typename T>
        struct A {
            template <typename U>
            struct B {
                using type = type_list<T, U>;
            };
        };

        template <typename X, typename Y>
        struct test {
            using input = typename A<X>::template B<Y>::type;
            using expect = type_list<X, Y>;
        };
    )code");
}

TEST_CASE(IndirectBaseClass) {
    // Member found through two levels of inheritance
    run(R"code(
        template <typename T>
        struct GrandBase {
            using type = T;
        };

        template <typename T>
        struct Middle : GrandBase<T> {};

        template <typename T>
        struct Top : Middle<T> {};

        template <typename X>
        struct test {
            using input = typename Top<X>::type;
            using expect = X;
        };
    )code");
}

TEST_CASE(SelfReferentialAlias) {
    // Type alias that refers back to the same class (like iterator::self)
    run(R"code(
        template <typename T>
        struct Wrapper {
            using self = Wrapper<T>;
            using type = T;
        };

        template <typename X>
        struct test {
            using input = typename Wrapper<X>::self::self::type;
            using expect = X;
        };
    )code");
}

TEST_CASE(VoidSpecialization) {
    run(R"code(
        template <typename T>
        struct A {
            using type = T;
        };

        template <>
        struct A<void> {
            using type = int;
        };

        template <typename X>
        struct test {
            using input = typename A<X>::type;
            using expect = X;
        };
    )code");
}

TEST_CASE(DependentSizedArray) {
    run(R"code(
        template <typename T>
        struct A {
            using type = T;
            using pointer = type*;
        };

        template <typename X>
        struct test {
            using input = typename A<X>::pointer;
            using expect = X*;
        };
    )code");
}

TEST_CASE(MultiplePacks) {
    // Two separate pack parameters
    run(R"code(
        template <typename... Ts>
        struct type_list {};

        template <typename T, typename... Us>
        struct A {
            using type = type_list<T, Us...>;
        };

        template <typename X, typename... Ys>
        struct test {
            using input = typename A<X, Ys...>::type;
            using expect = type_list<X, Ys...>;
        };
    )code");
}

TEST_CASE(ConstPointerMember) {
    run(R"code(
        template <typename T>
        struct A {
            using type = const T*;
        };

        template <typename X>
        struct test {
            using input = typename A<X>::type;
            using expect = const X*;
        };
    )code");
}

TEST_CASE(PointerConstMember) {
    run(R"code(
        template <typename T>
        struct A {
            using type = T* const;
        };

        template <typename X>
        struct test {
            using input = typename A<X>::type;
            using expect = X* const;
        };
    )code");
}

TEST_CASE(ConstRefTypedef) {
    run(R"code(
        template <typename T>
        struct A {
            using c = const T;
            using type = c&;
        };

        template <typename X>
        struct test {
            using input = typename A<X>::type;
            using expect = const X&;
        };
    )code");
}

TEST_CASE(PointerChainRef) {
    run(R"code(
        template <typename T>
        struct A {
            using p = T*;
            using pp = p*;
            using type = pp&;
        };

        template <typename X>
        struct test {
            using input = typename A<X>::type;
            using expect = X**&;
        };
    )code");
}

TEST_CASE(TypedefChainFourTemplates) {
    run(R"code(
        template <typename... Ts>
        struct type_list {};

        template <typename T1>
        struct A {
            using type = type_list<T1>;
        };

        template <typename T2>
        struct B {
            using type = typename A<T2>::type;
        };

        template <typename T3>
        struct C {
            using type = typename B<T3>::type;
        };

        template <typename T4>
        struct D {
            using type = typename C<T4>::type;
        };

        template <typename X>
        struct test {
            using input = typename D<X>::type;
            using expect = type_list<X>;
        };
    )code");
}

TEST_CASE(NonDependentBase) {
    run(R"code(
        struct Base {
            using type = int;
        };

        template <typename T>
        struct Derived : Base {};

        template <typename X>
        struct test {
            using input = typename Derived<X>::type;
            using expect = int;
        };
    )code");
}

TEST_CASE(ThreeLevelInheritance) {
    run(R"code(
        template <typename... Ts>
        struct type_list {};

        template <typename T>
        struct L1 {
            using type = type_list<T>;
        };

        template <typename T>
        struct L2 : L1<T> {};

        template <typename T>
        struct L3 : L2<T> {};

        template <typename T>
        struct L4 : L3<T> {};

        template <typename X>
        struct test {
            using input = typename L4<X>::type;
            using expect = type_list<X>;
        };
    )code");
}

TEST_CASE(InjectedClassName) {
    run(R"code(
        template <typename T>
        struct A {
            using type = T;
            using self = A;
        };

        template <typename X>
        struct test {
            using input = typename A<X>::self::type;
            using expect = X;
        };
    )code");
}

TEST_CASE(NestedMemberDepthMix) {
    run(R"code(
        template <typename... Ts>
        struct type_list {};

        template <typename T>
        struct Outer {
            using outer_type = T*;

            template <typename U>
            struct Inner {
                using type = type_list<outer_type, U>;
            };
        };

        template <typename X, typename Y>
        struct test {
            using input = typename Outer<X>::template Inner<Y>::type;
            using expect = type_list<X*, Y>;
        };
    )code");
}

TEST_CASE(DefaultArgEarlierParam) {
    run(R"code(
        template <typename T, typename U = T*>
        struct A {
            using type = U;
        };

        template <typename X>
        struct test {
            using input = typename A<X>::type;
            using expect = X*;
        };
    )code");
}

TEST_CASE(AliasTemplateChain) {
    run(R"code(
        template <typename T>
        using first = T;

        template <typename T>
        using second = first<T>;

        template <typename T>
        struct A {
            using type = second<T>;
        };

        template <typename X>
        struct test {
            using input = typename A<X>::type;
            using expect = X;
        };
    )code");
}

TEST_CASE(ClassTemplateAliasTarget) {
    run(R"code(
        template <typename T>
        struct Impl {
            using type = T;
        };

        template <typename T>
        using Alias = Impl<T>;

        template <typename X>
        struct test {
            using input = typename Alias<X>::type;
            using expect = X;
        };
    )code");
}

TEST_CASE(PackLeadingFixed) {
    run(R"code(
        template <typename... Ts>
        struct type_list {};

        template <typename A, typename B, typename... Rest>
        struct pick {
            using type = type_list<Rest..., A, B>;
        };

        template <typename X, typename Y, typename Z, typename W>
        struct test {
            using input = typename pick<X, Y, Z, W>::type;
            using expect = type_list<Z, W, X, Y>;
        };
    )code");
}

TEST_CASE(EmptyPackDeduced) {
    run(R"code(
        template <typename... Ts>
        struct type_list {};

        template <typename A, typename... Rest>
        struct pick {
            using type = type_list<A, Rest...>;
        };

        template <typename X>
        struct test {
            using input = typename pick<X>::type;
            using expect = type_list<X>;
        };
    )code");
}

TEST_CASE(PackThroughTwoLayers) {
    run(R"code(
        template <typename... Ts>
        struct type_list {};

        template <typename... Us>
        struct Inner {
            using type = type_list<Us...>;
        };

        template <typename... Vs>
        struct Outer {
            using type = typename Inner<Vs...>::type;
        };

        template <typename... Ts>
        struct test {
            using input = typename Outer<Ts...>::type;
            using expect = type_list<Ts...>;
        };
    )code");
}

TEST_CASE(PackDefaultInterplay) {
    run(R"code(
        template <typename... Ts>
        struct type_list {};

        template <typename T, typename U = int, typename... Vs>
        struct A {
            using type = type_list<T, U, Vs...>;
        };

        template <typename X>
        struct test {
            using input = typename A<X>::type;
            using expect = type_list<X, int>;
        };
    )code");
}

TEST_CASE(MixedPackElements) {
    run(R"code(
        template <typename... Ts>
        struct type_list {};

        template <typename... Us>
        struct A {
            using type = type_list<Us...>;
        };

        template <typename X>
        struct test {
            using input = typename A<int, X, float>::type;
            using expect = type_list<int, X, float>;
        };
    )code");
}

TEST_CASE(NttpPassthrough) {
    run(R"code(
        template <typename T, int N>
        struct box {};

        template <typename T, int N>
        struct S {
            using type = box<T, N>;
        };

        template <typename X>
        struct test {
            using input = typename S<X, 5>::type;
            using expect = box<X, 5>;
        };
    )code");
}

TEST_CASE(NttpBoolValue) {
    run(R"code(
        template <typename T, bool B>
        struct box {};

        template <typename T, bool B>
        struct S {
            using type = box<T, B>;
        };

        template <typename X>
        struct test {
            using input = typename S<X, true>::type;
            using expect = box<X, true>;
        };
    )code");
}

TEST_CASE(NttpEnumValue) {
    run(R"code(
        enum Color { Red, Green, Blue };

        template <typename T, Color C>
        struct box {};

        template <typename T, Color C>
        struct S {
            using type = box<T, C>;
        };

        template <typename X>
        struct test {
            using input = typename S<X, Green>::type;
            using expect = box<X, Green>;
        };
    )code");
}

TEST_CASE(MultiDimArray) {
    run(R"code(
        template <typename T, unsigned long N>
        struct S {
            using type = T[N][3];
        };

        template <typename X>
        struct test {
            using input = typename S<X, 4>::type;
            using expect = X[4][3];
        };
    )code");
}

TEST_CASE(ArrayOfPointer) {
    run(R"code(
        template <typename T, unsigned long N>
        struct S {
            using type = T*[N];
        };

        template <typename X>
        struct test {
            using input = typename S<X, 2>::type;
            using expect = X*[2];
        };
    )code");
}

TEST_CASE(RebindTrailingArgs) {
    run(R"code(
        template <typename A, typename B, typename C>
        struct triple {};

        template <typename Old, typename New>
        struct replace_first {};

        template <template <typename...> typename TT,
                  typename New,
                  typename T,
                  typename... Rest>
        struct replace_first<TT<T, Rest...>, New> {
            using type = TT<New, Rest...>;
        };

        template <typename X, typename Y>
        struct test {
            using input = typename replace_first<triple<int, X, Y>, float>::type;
            using expect = triple<float, X, Y>;
        };
    )code");
}

TEST_CASE(TemplatePackExtract) {
    run(R"code(
        template <typename... Ts>
        struct type_list {};

        template <typename T>
        struct wrap {};

        template <template <typename...> typename TT, typename... Us>
        struct wrap<TT<Us...>> {
            using type = TT<Us...>;
        };

        template <typename X, typename Y>
        struct test {
            using input = typename wrap<type_list<X, Y>>::type;
            using expect = type_list<X, Y>;
        };
    )code");
}

TEST_CASE(TemplateSwapArgs) {
    run(R"code(
        template <typename A, typename B>
        struct pair {};

        template <typename T>
        struct first_of {};

        template <template <typename, typename> typename TT, typename A, typename B>
        struct first_of<TT<A, B>> {
            using type = TT<B, A>;
        };

        template <typename X, typename Y>
        struct test {
            using input = typename first_of<pair<X, Y>>::type;
            using expect = pair<Y, X>;
        };
    )code");
}

TEST_CASE(PointerCvOrdering) {
    run(R"code(
        template <typename... Ts>
        struct type_list {};

        template <typename T>
        struct trait {
            using type = type_list<T>;
        };

        template <typename T>
        struct trait<T*> {
            using type = type_list<T, T>;
        };

        template <typename T>
        struct trait<const T*> {
            using type = type_list<T, T, T>;
        };

        template <typename X>
        struct test {
            using input = typename trait<const X*>::type;
            using expect = type_list<X, X, X>;
        };
    )code");
}

TEST_CASE(PointerDoubleOrdering) {
    run(R"code(
        template <typename... Ts>
        struct type_list {};

        template <typename T>
        struct rank {
            using type = type_list<T>;
        };

        template <typename T>
        struct rank<T*> {
            using type = type_list<T, T>;
        };

        template <typename T>
        struct rank<T**> {
            using type = type_list<T, T, T>;
        };

        template <typename X>
        struct test {
            using input = typename rank<X**>::type;
            using expect = type_list<X, X, X>;
        };
    )code");
}

TEST_CASE(RefPartialOrdering) {
    run(R"code(
        template <typename... Ts>
        struct type_list {};

        template <typename T>
        struct classify {
            using type = type_list<T>;
        };

        template <typename T>
        struct classify<T*> {
            using type = type_list<T, T>;
        };

        template <typename T>
        struct classify<T&> {
            using type = type_list<T, T, T>;
        };

        template <typename X>
        struct test {
            using input = typename classify<X*>::type;
            using expect = type_list<X, X>;
        };
    )code");
}

TEST_CASE(PartialSecondArg) {
    run(R"code(
        template <typename... Ts>
        struct type_list {};

        template <typename A, typename B>
        struct combine {
            using type = A;
        };

        template <typename A, typename B>
        struct combine<A, B*> {
            using type = type_list<A, B>;
        };

        template <typename X, typename Y>
        struct test {
            using input = typename combine<X, Y*>::type;
            using expect = type_list<X, Y>;
        };
    )code");
}

TEST_CASE(TemplateIdPartial) {
    run(R"code(
        template <typename... Ts>
        struct type_list {};

        template <typename T>
        struct vec {};

        template <typename T>
        struct unwrap {
            using type = T;
        };

        template <typename T>
        struct unwrap<vec<T>> {
            using type = type_list<T>;
        };

        template <typename X>
        struct test {
            using input = typename unwrap<vec<X>>::type;
            using expect = type_list<X>;
        };
    )code");
}

TEST_CASE(DetectorMemberPresent) {
    run(R"code(
        template <typename... Ts>
        using void_t = void;

        template <typename... Ts>
        struct type_list {};

        template <typename T, typename = void>
        struct detect {
            using type = int;
        };

        template <typename T>
        struct detect<T, void_t<typename T::element>> {
            using type = typename T::element;
        };

        template <typename T>
        struct has_elem {
            using element = type_list<T>;
        };

        template <typename X>
        struct test {
            using input = typename detect<has_elem<X>>::type;
            using expect = type_list<X>;
        };
    )code");
}

TEST_CASE(DetectorMemberAbsent) {
    run(R"code(
        template <typename... Ts>
        using void_t = void;

        template <typename... Ts>
        struct type_list {};

        template <typename T, typename = void>
        struct detect {
            using type = type_list<T>;
        };

        template <typename T>
        struct detect<T, void_t<typename T::element>> {
            using type = typename T::element;
        };

        template <typename T>
        struct no_elem {
            using other = int;
        };

        template <typename X>
        struct test {
            using input = typename detect<no_elem<X>>::type;
            using expect = type_list<no_elem<X>>;
        };
    )code");
}

TEST_CASE(DetectorViaBase) {
    run(R"code(
        template <typename... Ts>
        using void_t = void;

        template <typename... Ts>
        struct type_list {};

        template <typename T, typename = void>
        struct detect {
            using type = int;
        };

        template <typename T>
        struct detect<T, void_t<typename T::element>> {
            using type = typename T::element;
        };

        template <typename T>
        struct elem_base {
            using element = type_list<T>;
        };

        template <typename T>
        struct elem_derived : elem_base<T> {};

        template <typename X>
        struct test {
            using input = typename detect<elem_derived<X>>::type;
            using expect = type_list<X>;
        };
    )code");
}

TEST_CASE(NestedRebindDetector) {
    run(R"code(
        template <typename... Ts>
        using void_t = void;

        template <typename T>
        struct alloc {
            template <typename U>
            struct rebind {
                using other = alloc<U>;
            };
        };

        template <typename A, typename U, typename = void>
        struct rebind_inner {
            using type = int;
        };

        template <typename A, typename U>
        struct rebind_inner<A, U, void_t<typename A::template rebind<U>::other>> {
            using type = typename A::template rebind<U>::other;
        };

        template <typename A, typename U>
        struct rebind_outer {
            using type = typename rebind_inner<A, U>::type;
        };

        template <typename X>
        struct test {
            using input = typename rebind_outer<alloc<X>, float>::type;
            using expect = alloc<float>;
        };
    )code");
}

TEST_CASE(PartialMemberViaBase) {
    run(R"code(
        template <typename... Ts>
        struct type_list {};

        template <typename T>
        struct holder {
            using type = type_list<T>;
        };

        template <typename T>
        struct picker {
            using type = int;
        };

        template <typename T>
        struct picker<T*> : holder<T> {};

        template <typename X>
        struct test {
            using input = typename picker<X*>::type;
            using expect = type_list<X>;
        };
    )code");
}

TEST_CASE(CrtpPartialSpec) {
    run(R"code(
        template <typename... Ts>
        struct type_list {};

        template <typename D>
        struct facade {
            using type = typename D::value;
        };

        template <typename T>
        struct widget;

        template <typename T>
        struct widget<T*> : facade<widget<T*>> {
            using value = type_list<T>;
        };

        template <typename X>
        struct test {
            using input = typename widget<X*>::type;
            using expect = type_list<X>;
        };
    )code");
}

TEST_CASE(InheritanceThroughPartial) {
    run(R"code(
        template <typename... Ts>
        struct type_list {};

        template <typename T>
        struct groot {
            using type = type_list<T>;
        };

        template <typename T>
        struct mid {
            using type = int;
        };

        template <typename T>
        struct mid<T*> : groot<T> {};

        template <typename T>
        struct topc : mid<T*> {};

        template <typename X>
        struct test {
            using input = typename topc<X>::type;
            using expect = type_list<X>;
        };
    )code");
}

TEST_CASE(RecursivePointerPeel) {
    run(R"code(
        template <typename T>
        struct strip {
            using type = T;
        };

        template <typename T>
        struct strip<T*> {
            using type = typename strip<T>::type;
        };

        template <typename X>
        struct test {
            using input = typename strip<X***>::type;
            using expect = X;
        };
    )code");
}

TEST_CASE(StructuredPackDeduce) {
    run(R"code(
        template <typename... Ts>
        struct type_list {};

        template <typename T>
        struct box {};

        template <typename T>
        struct unbox {};

        template <typename... Us>
        struct unbox<type_list<box<Us>...>> {
            using type = type_list<Us...>;
        };

        template <typename X, typename Y>
        struct test {
            using input = typename unbox<type_list<box<X>, box<Y>>>::type;
            using expect = type_list<X, Y>;
        };
    )code");
}

TEST_CASE(PackZipDeduce) {
    run(R"code(
        template <typename... Ts>
        struct type_list {};

        template <typename A, typename B>
        struct pair {};

        template <typename T>
        struct split {};

        template <typename... As, typename... Bs>
        struct split<type_list<pair<As, Bs>...>> {
            using type = type_list<As..., Bs...>;
        };

        template <typename X, typename Y>
        struct test {
            using input = typename split<type_list<pair<X, int>, pair<Y, float>>>::type;
            using expect = type_list<X, Y, int, float>;
        };
    )code");
}

TEST_CASE(UnresolvedMemberLookup) {
    add_main("main.cpp", R"code(
        template <typename T>
        struct S {
            int foo(int);
            int foo(char);

            void call(T t) {
                foo(t);
            }
        };
    )code");
    ASSERT_TRUE(compile());

    struct Finder : clang::RecursiveASTVisitor<Finder> {
        const clang::UnresolvedMemberExpr* expr = nullptr;

        bool VisitUnresolvedMemberExpr(clang::UnresolvedMemberExpr* e) {
            expr = e;
            return true;
        }
    } finder;

    finder.TraverseAST(unit->context());
    ASSERT_TRUE(finder.expr != nullptr);

    auto members = unit->resolver().lookup(finder.expr);
    EXPECT_EQ(std::ranges::distance(members), 2);
}

TEST_CASE(NamespaceOverloadLookup) {
    add_main("main.cpp", R"code(
        namespace ns {
            template <typename T>
            int f(T);

            int f(int);
        }

        template <typename T>
        void g(T t) {
            ns::f<T>(t);
        }
    )code");
    ASSERT_TRUE(compile());

    struct Finder : clang::RecursiveASTVisitor<Finder> {
        const clang::UnresolvedLookupExpr* expr = nullptr;

        bool VisitUnresolvedLookupExpr(clang::UnresolvedLookupExpr* e) {
            expr = e;
            return true;
        }
    } finder;

    finder.TraverseAST(unit->context());
    ASSERT_TRUE(finder.expr != nullptr);

    auto members = unit->resolver().lookup(finder.expr);
    EXPECT_EQ(std::ranges::distance(members), 2);
}

TEST_CASE(RecursiveDetectorProbe) {
    run(R"code(
        template <typename... Ts>
        using void_t = void;

        template <typename... Ts>
        struct type_list {};

        template <typename T, typename = void>
        struct wrap {
            using type = type_list<T>;
        };

        template <typename T>
        struct wrap<T*, void_t<typename wrap<T>::type>> {
            using type = typename wrap<T>::type;
        };

        template <typename X>
        struct test {
            using input = typename wrap<X*>::type;
            using expect = type_list<X>;
        };
    )code");
}

TEST_CASE(CallArityFilter) {
    add_main("main.cpp", R"code(
        template <typename T>
        struct S {
            int foo(int);
            int foo(int, int);
            int foo(T);

            void call(T t) {
                foo(t);
            }
        };
    )code");
    ASSERT_TRUE(compile());

    struct Finder : clang::RecursiveASTVisitor<Finder> {
        const clang::CallExpr* expr = nullptr;

        bool VisitCallExpr(clang::CallExpr* e) {
            if(llvm::isa<clang::UnresolvedMemberExpr>(e->getCallee()->IgnoreParenImpCasts())) {
                expr = e;
            }
            return true;
        }
    } finder;

    finder.TraverseAST(unit->context());
    ASSERT_TRUE(finder.expr != nullptr);

    /// One argument: `foo(int, int)` is filtered out, both single-parameter
    /// overloads survive.
    auto candidates = unit->resolver().lookup(finder.expr);
    EXPECT_EQ(candidates.size(), 2u);
}

TEST_CASE(PackElementCache) {
    /// Each element of `box<Ts>::type...` resolves the same dependent-name
    /// node under a different binding; a node-keyed cache hit would repeat
    /// the first element for every later one.
    run(R"code(
        template <typename T>
        struct box {
            using type = T;
        };

        template <typename... Ts>
        struct type_list {};

        template <typename... Ts>
        struct A {
            using type = type_list<typename box<Ts>::type...>;
        };

        template <typename X>
        struct test {
            using input = typename A<X, int>::type;
            using expect = type_list<X, int>;
        };
    )code");
}

TEST_CASE(InheritedDefaultArgument) {
    run(R"code(
        template <typename T, typename U = T>
        struct A;

        template <typename T, typename U>
        struct A {
            using type = U;
        };

        template <typename X>
        struct test {
            using input = typename A<X>::type;
            using expect = X;
        };
    )code");
}

TEST_CASE(VoidReference) {
    /// `A<void, X>::type` would be `void&` — ill-formed; the resolver must
    /// degrade rather than fabricate the reference.
    add_main("main.cpp", R"code(
        template <typename T, typename X>
        struct A {
            using type = T&;
        };

        template <typename X>
        struct test {
            using input = typename A<void, X>::type;
            using expect = void;
        };
    )code");
    ASSERT_TRUE(compile());

    InputFinder finder(*unit);
    finder.TraverseAST(unit->context());

    auto input = unit->resolver().resolve(finder.input);
    ASSERT_FALSE(input.isNull());
    if(input->isReferenceType()) {
        EXPECT_FALSE(input->getPointeeType()->isVoidType());
    }
}

TEST_CASE(ValuePackSplice) {
    run(R"code(
        template <int... Ns>
        struct values {};

        template <int... Ns>
        struct A {
            using type = values<Ns...>;
        };

        template <int X>
        struct test {
            using input = typename A<1, X>::type;
            using expect = values<1, X>;
        };
    )code");
}

TEST_CASE(FunctionParamPack) {
    run(R"code(
        template <typename... Ts>
        struct A {
            using type = void(Ts...);
        };

        template <typename X>
        struct test {
            using input = typename A<X, int>::type;
            using expect = void(X, int);
        };
    )code");
}

TEST_CASE(DependentArrayBound) {
    run(R"code(
        template <typename T>
        struct trait {
            using type = void;
        };

        template <typename T, unsigned long N>
        struct trait<T[N]> {
            using type = T;
        };

        template <typename X, unsigned long M>
        struct test {
            using input = typename trait<X[M]>::type;
            using expect = X;
        };
    )code");
}

TEST_CASE(RepeatedPackElement) {
    /// `pair<Ts, Ts>...` requires both occurrences to agree in each element;
    /// the mismatching first pair must fall back to the primary.
    run(R"code(
        template <typename A, typename B>
        struct pair {};

        template <typename... Ts>
        struct type_list {};

        template <typename T>
        struct trait {
            using type = void;
        };

        template <typename... Ts>
        struct trait<type_list<pair<Ts, Ts>...>> {
            using type = int;
        };

        template <typename X, typename Y>
        struct test {
            using input = typename trait<type_list<pair<X, Y>, pair<Y, Y>>>::type;
            using expect = void;
        };
    )code");
}

TEST_CASE(TemplatePackExpansion) {
    run(R"code(
        template <typename T>
        struct box {};

        template <typename... Ts>
        struct type_list {};

        template <template <typename> class... Fs>
        struct A {
            using type = type_list<Fs<int>...>;
        };

        template <template <typename> class TX>
        struct test {
            using input = typename A<TX, box>::type;
            using expect = type_list<TX<int>, box<int>>;
        };
    )code");
}

TEST_CASE(ReferenceCollapse) {
    run(R"code(
        template <typename T>
        struct R {
            using type = T&;
        };

        template <typename X>
        struct test {
            using input = typename R<X&&>::type;
            using expect = X&;
        };
    )code");
}

TEST_CASE(PointerToReference) {
    /// `P<X&>::type` would be `X& *` — ill-formed. The resolver must leave
    /// the name unresolved rather than fabricate a malformed pointer node.
    add_main("main.cpp", R"code(
        template <typename T>
        struct P {
            using type = T*;
        };

        template <typename X>
        struct test {
            using input = typename P<X&>::type;
            using expect = void;
        };
    )code");
    ASSERT_TRUE(compile());

    InputFinder finder(*unit);
    finder.TraverseAST(unit->context());

    auto input = unit->resolver().resolve(finder.input);
    ASSERT_FALSE(input.isNull());
    EXPECT_TRUE(input->isDependentType());
    /// Degrading returns the written `T*`; fabricating `X& *` would not.
    if(input->isPointerType()) {
        EXPECT_FALSE(input->getPointeeType()->isReferenceType());
    }
}

TEST_CASE(UnboundArrayPattern) {
    run(R"code(
        template <typename T>
        struct trait {
            using type = void;
        };

        template <typename T>
        struct trait<T[]> {
            using type = T;
        };

        template <typename X>
        struct test {
            using input = typename trait<X[]>::type;
            using expect = X;
        };
    )code");
}

TEST_CASE(ConstMethodPattern) {
    /// The const-qualified member-function partial must not swallow a
    /// non-const member function pointer.
    run(R"code(
        template <typename T>
        struct trait {
            using type = void;
        };

        template <typename R, typename C, typename... As>
        struct trait<R (C::*)(As...) const> {
            using type = R;
        };

        template <typename X>
        struct test {
            using input = typename trait<int (X::*)(char)>::type;
            using expect = void;
        };
    )code");
}

TEST_CASE(RepeatedValuePattern) {
    run(R"code(
        template <int A, int B>
        struct P {
            using type = void;
        };

        template <int N>
        struct P<N, N> {
            using type = int;
        };

        template <int X>
        struct test {
            using input = typename P<X, X>::type;
            using expect = int;
        };
    )code");
}

TEST_CASE(TemplatePackDeduce) {
    run(R"code(
        template <typename T>
        struct box {};

        template <template <typename> class... TTs>
        struct A {
            using type = int;
        };

        template <template <typename> class TX>
        struct test {
            using input = typename A<TX, box>::type;
            using expect = int;
        };
    )code");
}

TEST_CASE(NoexceptFunctionPattern) {
    /// The partial pins `noexcept`; a plain function type must keep the
    /// primary rather than matching with the specification ignored.
    run(R"code(
        template <typename T>
        struct trait {
            using type = void;
        };

        template <typename R, typename... As>
        struct trait<R(As...) noexcept> {
            using type = R;
        };

        template <typename X>
        struct test {
            using input = typename trait<X(int)>::type;
            using expect = void;
        };
    )code");
}

TEST_CASE(UnderlyingTypeTransform) {
    run(R"code(
        enum class E : short {};

        template <typename T>
        struct wrap {
            using held = E;
        };

        template <typename T>
        struct A {
            using type = __underlying_type(typename wrap<T>::held);
        };

        template <typename X>
        struct test {
            using input = typename A<X>::type;
            using expect = short;
        };
    )code");
}

TEST_CASE(DependentScopeInconclusive) {
    /// `A<X>::foo` is unknowable while X is: the primary lacks `foo` but the
    /// pointer partial declares it, so the probe must stay Unknown and keep
    /// the detector partial.
    run(R"code(
        template <typename... Ts>
        using void_t = void;

        template <typename T>
        struct A {};

        template <typename T>
        struct A<T*> {
            using foo = int;
        };

        template <typename T, typename = void>
        struct detect {
            using type = void;
        };

        template <typename T>
        struct detect<T, void_t<typename A<T>::foo>> {
            using type = int;
        };

        template <typename X>
        struct test {
            using input = typename detect<X>::type;
            using expect = int;
        };
    )code");
}

TEST_CASE(AliasDefaultArgument) {
    run(R"code(
        template <typename T, typename U>
        struct pair {};

        template <typename T, typename U = int>
        using alias_pair = pair<T, U>;

        template <template <typename> class TT, typename A>
        struct apply {
            using type = TT<A>;
        };

        template <typename X>
        struct test {
            using input = typename apply<alias_pair, X>::type;
            using expect = pair<X, int>;
        };
    )code");
}

TEST_CASE(FunctionTypePattern) {
    run(R"code(
        template <typename T>
        struct trait {
            using type = void;
        };

        template <typename R, typename... As>
        struct trait<R(As...)> {
            using type = R;
        };

        template <typename X>
        struct test {
            using input = typename trait<X(int)>::type;
            using expect = X;
        };
    )code");
}

TEST_CASE(MemberPointerPattern) {
    run(R"code(
        template <typename T>
        struct trait {
            using type = void;
        };

        template <typename R, typename C>
        struct trait<R C::*> {
            using type = R;
        };

        template <typename X>
        struct test {
            using input = typename trait<int X::*>::type;
            using expect = int;
        };
    )code");
}

TEST_CASE(MemberPointerRewrite) {
    run(R"code(
        template <typename T>
        struct A {
            using type = T A::*;
        };

        template <typename X>
        struct test {
            using input = typename A<X>::type;
            using expect = X A<X>::*;
        };
    )code");
}

TEST_CASE(PackPatternExpansion) {
    run(R"code(
        template <typename T>
        struct box {};

        template <typename... Ts>
        struct type_list {};

        template <typename... Us>
        struct A {
            using type = type_list<box<Us>...>;
        };

        template <typename X>
        struct test {
            using input = typename A<X, int>::type;
            using expect = type_list<box<X>, box<int>>;
        };
    )code");
}

TEST_CASE(OuterParamMismatch) {
    /// The partial's pattern `pair<O, U>` pins Outer's own parameter: for a
    /// mismatching first element it must not match, keeping the primary.
    run(R"code(
        template <typename A, typename B>
        struct pair {};

        template <typename O>
        struct Outer {
            template <typename T>
            struct Inner {
                using type = void;
            };

            template <typename U>
            struct Inner<pair<O, U>> {
                using type = U;
            };
        };

        template <typename X, typename Y>
        struct test {
            using input = typename Outer<X>::template Inner<pair<Y, int>>::type;
            using expect = void;
        };
    )code");
}

TEST_CASE(OuterParamMatch) {
    run(R"code(
        template <typename A, typename B>
        struct pair {};

        template <typename O>
        struct Outer {
            template <typename T>
            struct Inner {
                using type = void;
            };

            template <typename U>
            struct Inner<pair<O, U>> {
                using type = U;
            };
        };

        template <typename X>
        struct test {
            using input = typename Outer<X>::template Inner<pair<X, int>>::type;
            using expect = int;
        };
    )code");
}

TEST_CASE(AliasTemplateHead) {
    /// A template template parameter bound to an alias template: after head
    /// substitution the rebuilt specialization names an alias, so its aliased
    /// type must be computed — assertion-enabled clang aborts otherwise.
    run(R"code(
        template <typename T, typename U>
        struct pair {};

        template <typename T, typename U>
        using alias_pair = pair<T, U>;

        template <template <typename, typename> class TT, typename A, typename B>
        struct apply {
            using type = TT<A, B>;
        };

        template <typename X>
        struct test {
            using input = typename apply<alias_pair, X, int>::type;
            using expect = pair<X, int>;
        };
    )code");
}

TEST_CASE(AliasPackExpanded) {
    /// `listify<Us...>` carries no aliased type (its arguments hold an
    /// unexpanded pack), so rewriting the pack away must compute the aliased
    /// type before the specialization can be rebuilt — assertion-enabled
    /// clang aborts otherwise.
    run(R"code(
        template <typename... Ts>
        struct type_list {};

        template <typename... Ts>
        using listify = type_list<Ts...>;

        template <typename... Us>
        struct A {
            using type = listify<Us...>;
        };

        template <typename X>
        struct test {
            using input = typename A<X, int>::type;
            using expect = type_list<X, int>;
        };
    )code");
}

TEST_CASE(RedeclSplitDefinition) {
    /// `A<X>` is parsed while only the forward declaration is visible, so the
    /// DTST's TemplateName points at the redecl whose parameter list differs
    /// from the defining declaration that owns `type`.
    run(R"code(
        template <typename T>
        struct A;

        template <typename X>
        struct test {
            using input = typename A<X>::type;
            using expect = X*;
        };

        template <typename T>
        struct A {
            using type = T*;
        };
    )code");
}

TEST_CASE(QualifiedCallArity) {
    add_main("main.cpp", R"code(
        template <typename T>
        struct base {
            static int foo(int);
            static int foo(int, int);
            static int foo(T);
        };

        template <typename T>
        struct S {
            void call(T t) {
                base<T>::foo(t);
            }
        };
    )code");
    ASSERT_TRUE(compile());

    struct Finder : clang::RecursiveASTVisitor<Finder> {
        const clang::CallExpr* expr = nullptr;

        bool VisitCallExpr(clang::CallExpr* e) {
            if(llvm::isa<clang::DependentScopeDeclRefExpr>(e->getCallee()->IgnoreParenImpCasts())) {
                expr = e;
            }
            return true;
        }
    } finder;

    finder.TraverseAST(unit->context());
    ASSERT_TRUE(finder.expr != nullptr);

    /// One argument: `foo(int, int)` is filtered out, both single-parameter
    /// overloads survive.
    auto candidates = unit->resolver().lookup(finder.expr);
    EXPECT_EQ(candidates.size(), 2u);
}

TEST_CASE(PackArgumentCall) {
    /// `foo(us...)` may expand to any arity; the filter must keep every
    /// overload.
    add_main("main.cpp", R"code(
        template <typename T>
        struct S {
            int foo(int);
            int foo(int, int);

            template <typename... Us>
            void call(Us... us) {
                foo(us...);
            }
        };
    )code");
    ASSERT_TRUE(compile());

    struct Finder : clang::RecursiveASTVisitor<Finder> {
        const clang::CallExpr* expr = nullptr;

        bool VisitCallExpr(clang::CallExpr* e) {
            if(llvm::isa<clang::UnresolvedMemberExpr>(e->getCallee()->IgnoreParenImpCasts())) {
                expr = e;
            }
            return true;
        }
    } finder;

    finder.TraverseAST(unit->context());
    ASSERT_TRUE(finder.expr != nullptr);

    auto candidates = unit->resolver().lookup(finder.expr);
    EXPECT_EQ(candidates.size(), 2u);
}

TEST_CASE(AtomicReference) {
    /// `_Atomic(int&)` does not exist; the resolver must degrade.
    add_main("main.cpp", R"code(
        template <typename T, typename U>
        struct A {
            using type = _Atomic(T);
        };

        template <typename X>
        struct test {
            using input = typename A<int&, X>::type;
            using expect = void;
        };
    )code");
    ASSERT_TRUE(compile());

    InputFinder finder(*unit);
    finder.TraverseAST(unit->context());

    auto input = unit->resolver().resolve(finder.input);
    ASSERT_FALSE(input.isNull());
    if(auto AT = input->getAs<clang::AtomicType>()) {
        EXPECT_FALSE(AT->getValueType()->isReferenceType());
    }
}

TEST_CASE(ConstrainedPartial) {
    /// `requires false` cannot be evaluated here; the structurally matching
    /// partial is unverifiable and the member must stay unresolved.
    add_main("main.cpp", R"code(
        template <typename T>
        struct P {
            using type = void;
        };

        template <typename T>
            requires false
        struct P<T*> {
            using type = int;
        };

        template <typename X>
        struct test {
            using input = typename P<X*>::type;
            using expect = void;
        };
    )code");
    ASSERT_TRUE(compile());

    InputFinder finder(*unit);
    finder.TraverseAST(unit->context());

    auto input = unit->resolver().resolve(finder.input);
    ASSERT_FALSE(input.isNull());
    EXPECT_TRUE(input->isDependentType());
    EXPECT_FALSE(input->isBuiltinType());
}

TEST_CASE(BoolArrayBound) {
    /// `bool N` cannot represent the bound 2, so the primary applies.
    run(R"code(
        template <typename T>
        struct trait {
            using type = void;
        };

        template <bool N, typename U>
        struct trait<U[N]> {
            using type = U;
        };

        template <typename X>
        struct test {
            using input = typename trait<X[2]>::type;
            using expect = void;
        };
    )code");
}

TEST_CASE(PackPrefixArity) {
    /// `TT` requires two fixed parameters before its pack; the unary
    /// template cannot supply them, so the primary applies.
    run(R"code(
        template <typename T>
        struct unary {};

        template <template <typename...> class G>
        struct token {};

        template <typename T, typename U>
        struct P {
            using type = void;
        };

        template <template <typename, typename, typename...> class TT, typename U>
        struct P<token<TT>, U> {
            using type = int;
        };

        template <typename X>
        struct test {
            using input = typename P<token<unary>, X>::type;
            using expect = void;
        };
    )code");
}

TEST_CASE(TemplateMemberProbe) {
    /// `typename T::foo` cannot name an unspecialized class template; the
    /// probe must fail and keep the primary.
    run(R"code(
        template <typename... Ts>
        using void_t = void;

        template <typename T, typename = void>
        struct detect {
            using type = void;
        };

        template <typename T>
        struct detect<T, void_t<typename T::foo>> {
            using type = int;
        };

        template <typename T>
        struct wrap {
            template <typename U>
            struct foo {};
        };

        template <typename X>
        struct test {
            using input = typename detect<wrap<X>>::type;
            using expect = void;
        };
    )code");
}

TEST_CASE(ArrowWithoutPointer) {
    /// `p->value` on a class with no `operator->` is ill-formed; the lookup
    /// must produce no candidates instead of pretending it was a dot access.
    add_main("main.cpp", R"code(
        template <typename T>
        struct P {
            int value;

            void f(P p) {
                p->value;
            }
        };
    )code");
    ASSERT_TRUE(compile());

    struct Finder : clang::RecursiveASTVisitor<Finder> {
        const clang::CXXDependentScopeMemberExpr* expr = nullptr;

        bool VisitCXXDependentScopeMemberExpr(clang::CXXDependentScopeMemberExpr* e) {
            if(e->isArrow()) {
                expr = e;
            }
            return true;
        }
    } finder;

    finder.TraverseAST(unit->context());
    ASSERT_TRUE(finder.expr != nullptr);

    auto candidates = unit->resolver().lookup(finder.expr);
    EXPECT_TRUE(candidates.empty());
}

TEST_CASE(MixedPackCandidate) {
    /// A type pack cannot cover `mixed`'s non-type slot, so the primary
    /// applies even though one type argument would satisfy the defaults.
    run(R"code(
        template <typename T, int N = 0>
        struct mixed {};

        template <typename T>
        struct trait {
            using type = void;
        };

        template <template <typename...> class TT, typename... Us>
        struct trait<TT<Us...>> {
            using type = int;
        };

        template <typename X>
        struct test {
            using input = typename trait<mixed<X>>::type;
            using expect = void;
        };
    )code");
}

TEST_CASE(AtomicRewrite) {
    run(R"code(
        template <typename T>
        struct A {
            using type = _Atomic(T);
        };

        template <typename X>
        struct test {
            using input = typename A<X>::type;
            using expect = _Atomic(X);
        };
    )code");
}

TEST_CASE(DependentBaseProbe) {
    /// `D<T>` inherits from the dependent `T`: `foo` may appear after
    /// instantiation, so the probe stays Unknown and keeps the detector.
    run(R"code(
        template <typename... Ts>
        using void_t = void;

        template <typename T>
        struct D : T {};

        template <typename T, typename = void>
        struct detect {
            using type = void;
        };

        template <typename T>
        struct detect<T, void_t<typename D<T>::foo>> {
            using type = int;
        };

        template <typename X>
        struct test {
            using input = typename detect<X>::type;
            using expect = int;
        };
    )code");
}

TEST_CASE(NegativeArrayBound) {
    /// `U[-1]` is ill-formed; the resolver must degrade rather than
    /// fabricate an array from the wrapped-around extent.
    add_main("main.cpp", R"code(
        template <int N, typename U>
        struct A {
            using type = U[N];
        };

        template <typename X>
        struct test {
            using input = typename A<-1, X>::type;
            using expect = void;
        };
    )code");
    ASSERT_TRUE(compile());

    InputFinder finder(*unit);
    finder.TraverseAST(unit->context());

    auto input = unit->resolver().resolve(finder.input);
    ASSERT_FALSE(input.isNull());
    EXPECT_FALSE(input->isConstantArrayType());
}

TEST_CASE(DecayedParameter) {
    run(R"code(
        template <typename T>
        struct A {
            using type = void(T[]);
        };

        template <typename X>
        struct test {
            using input = typename A<X>::type;
            using expect = void(X*);
        };
    )code");
}

TEST_CASE(RedeclAliasForward) {
    /// The dependent name is formed through an alias declared while only the
    /// forward declaration of `A` was visible.
    run(R"code(
        template <typename T>
        struct A;

        template <typename T>
        using ref = A<T>;

        template <typename X>
        struct test {
            using input = typename ref<X>::type;
            using expect = X*;
        };

        template <typename T>
        struct A {
            using type = T*;
        };
    )code");
}

TEST_CASE(CompoundValueDegrade) {
    /// TODO(nttp-expr): compound NTTP expressions are not substituted; this
    /// pins that they degrade cleanly (assertion-enabled clang would abort
    /// on a fabricated node) rather than crash.
    add_main("main.cpp", R"code(
        template <int N>
        struct value {};

        template <int N>
        struct A {
            using type = value<N + 1>;
        };

        template <int X>
        struct test {
            using input = typename A<X>::type;
            using expect = void;
        };
    )code");
    ASSERT_TRUE(compile());

    InputFinder finder(*unit);
    finder.TraverseAST(unit->context());

    auto input = unit->resolver().resolve(finder.input);
    ASSERT_FALSE(input.isNull());
    EXPECT_TRUE(input->isDependentType());
}

TEST_CASE(TypedValueMismatch) {
    /// Value deduction requires the parameter and argument types to agree:
    /// the bool partial never matches an int argument.
    run(R"code(
        template <auto V>
        struct A {
            using type = void;
        };

        template <bool B>
        struct A<B> {
            using type = int;
        };

        template <int X>
        struct test {
            using input = typename A<X>::type;
            using expect = void;
        };
    )code");
}

TEST_CASE(PrivateMemberProbe) {
    /// Access participates in SFINAE: a private `secret` fails the probe
    /// and keeps the primary.
    run(R"code(
        template <typename... Ts>
        using void_t = void;

        template <typename T, typename = void>
        struct detect {
            using type = void;
        };

        template <typename T>
        struct detect<T, void_t<typename T::secret>> {
            using type = int;
        };

        template <typename T>
        class wrap {
            using secret = int;
        };

        template <typename X>
        struct test {
            using input = typename detect<wrap<X>>::type;
            using expect = void;
        };
    )code");
}

TEST_CASE(UnboundedArgument) {
    /// The bounded pattern `U[N]` must not absorb the unbounded `X[]`.
    run(R"code(
        template <int N, typename B>
        struct P {
            using type = void;
        };

        template <int N, typename U>
        struct P<N, U[N]> {
            using type = U;
        };

        template <typename X>
        struct test {
            using input = typename P<3, X[]>::type;
            using expect = void;
        };
    )code");
}

TEST_CASE(EmptyPackConflict) {
    /// `Ts` deduces `{X}` from the first argument; the empty `tuple<>` in
    /// the second is a cardinality conflict, so the primary applies.
    run(R"code(
        template <typename... Ts>
        struct list {};

        template <typename... Ts>
        struct tuple {};

        template <typename A, typename B>
        struct P {
            using type = void;
        };

        template <typename... Ts>
        struct P<list<Ts...>, tuple<Ts...>> {
            using type = int;
        };

        template <typename X>
        struct test {
            using input = typename P<list<X>, tuple<>>::type;
            using expect = void;
        };
    )code");
}

TEST_CASE(AmbiguousPartials) {
    /// `trait<X*, Y*>` matches both partials and neither dominates: real
    /// instantiation is ambiguous, so the member must stay unresolved.
    add_main("main.cpp", R"code(
        template <typename A, typename B>
        struct trait {
            using type = void;
        };

        template <typename T, typename U>
        struct trait<T*, U> {
            using type = int;
        };

        template <typename T, typename U>
        struct trait<T, U*> {
            using type = char;
        };

        template <typename X, typename Y>
        struct test {
            using input = typename trait<X*, Y*>::type;
            using expect = void;
        };
    )code");
    ASSERT_TRUE(compile());

    InputFinder finder(*unit);
    finder.TraverseAST(unit->context());

    auto input = unit->resolver().resolve(finder.input);
    ASSERT_FALSE(input.isNull());
    EXPECT_TRUE(input->isDependentType());
    EXPECT_FALSE(input->isVoidType() || input->isBuiltinType());
}

TEST_CASE(DependentNameArgument) {
    run(R"code(
        template <template <typename> class TT>
        struct apply {};

        template <typename T>
        struct A {
            using type = apply<T::template tmpl>;
        };

        template <typename X>
        struct test {
            using input = typename A<X>::type;
            using expect = apply<X::template tmpl>;
        };
    )code");
}

TEST_CASE(TemplateExpansionSplice) {
    run(R"code(
        template <typename T>
        struct box {};

        template <template <typename> class... Gs>
        struct tlist {};

        template <template <typename> class... Fs>
        struct A {
            using type = tlist<Fs...>;
        };

        template <template <typename> class TX>
        struct test {
            using input = typename A<TX, box>::type;
            using expect = tlist<TX, box>;
        };
    )code");
}

TEST_CASE(NonTrailingSuffix) {
    /// `void(Ts..., int)` has a fixed suffix: the expansion's length follows
    /// from the arity, so `void(X, int)` matches with `Ts = {X}` while
    /// `void(X)` (missing the suffix) falls back to the primary.
    run(R"code(
        template <typename... Ts>
        struct type_list {};

        template <typename A, typename B>
        struct trait {
            using type = void;
        };

        template <typename... Ts>
        struct trait<type_list<Ts...>, void(Ts..., int)> {
            using type = int;
        };

        template <typename X>
        struct test {
            using input = typename trait<type_list<X>, void(X, int)>::type;
            using expect = int;
        };
    )code");
}

TEST_CASE(ArgumentPackMismatch) {
    /// The argument-side expansion `void(Us*...)` only produces pointer
    /// parameters; the fixed `void(int)` partial can never match it.
    run(R"code(
        template <typename A, typename B>
        struct trait {
            using type = void;
        };

        template <typename T>
        struct trait<T, void(int)> {
            using type = int;
        };

        template <typename... Us>
        struct test {
            using input = typename trait<int, void(Us*...)>::type;
            using expect = void;
        };
    )code");
}

TEST_CASE(AttributedRewrite) {
    run(R"code(
        template <typename T>
        struct A {
            using type = T __attribute__((address_space(1)))*;
        };

        template <typename X>
        struct test {
            using input = typename A<X>::type;
            using expect = X __attribute__((address_space(1)))*;
        };
    )code");
}

TEST_CASE(AutoArrayBound) {
    run(R"code(
        template <typename T>
        struct trait {
            using type = void;
        };

        template <typename U, auto N>
        struct trait<U[N]> {
            using type = U;
        };

        template <typename X>
        struct test {
            using input = typename trait<X[3]>::type;
            using expect = X;
        };
    )code");
}

TEST_CASE(ConstReferenceDrop) {
    /// cv-qualifiers applied to a substituted reference are ignored.
    run(R"code(
        template <typename T>
        struct A {
            using type = const T;
        };

        template <typename X>
        struct test {
            using input = typename A<X&>::type;
            using expect = X&;
        };
    )code");
}

TEST_CASE(ValueMemberProbe) {
    /// `typename T::value` requires a type; a static data member named
    /// `value` must fail the probe and keep the primary.
    run(R"code(
        template <typename... Ts>
        using void_t = void;

        template <typename T, typename = void>
        struct detect {
            using type = void;
        };

        template <typename T>
        struct detect<T, void_t<typename T::value>> {
            using type = int;
        };

        template <typename T>
        struct wrap {
            static constexpr int value = 1;
        };

        template <typename X>
        struct test {
            using input = typename detect<wrap<X>>::type;
            using expect = void;
        };
    )code");
}

TEST_CASE(NoexceptSubstitute) {
    /// The leading X offsets P's parameter index so the unsubstituted `B`
    /// cannot accidentally compare canonically equal.
    run(R"code(
        template <bool B>
        struct A {
            using type = void() noexcept(B);
        };

        template <typename X, bool P>
        struct test {
            using input = typename A<P>::type;
            using expect = void() noexcept(P);
        };
    )code");
}

TEST_CASE(NoexceptDeduce) {
    run(R"code(
        template <bool B>
        struct flag {};

        template <typename T>
        struct trait {
            using type = void;
        };

        template <bool B>
        struct trait<void() noexcept(B)> {
            using type = flag<B>;
        };

        template <typename X, bool P>
        struct test {
            using input = typename trait<void() noexcept(P)>::type;
            using expect = flag<P>;
        };
    )code");
}

TEST_CASE(TemplateArityMismatch) {
    /// `binary` needs two arguments; a unary template template parameter
    /// cannot accept it, so the primary applies.
    run(R"code(
        template <typename A, typename B>
        struct binary {};

        template <typename T>
        struct trait {
            using type = void;
        };

        template <template <typename> class TT, typename... Us>
        struct trait<TT<Us...>> {
            using type = int;
        };

        template <typename X>
        struct test {
            using input = typename trait<binary<X, int>>::type;
            using expect = void;
        };
    )code");
}

TEST_CASE(VoidFunctionParam) {
    /// `A<void, X>::type` would be a function taking a void parameter; the
    /// resolver must degrade rather than fabricate it.
    add_main("main.cpp", R"code(
        template <typename T, typename U>
        struct A {
            using type = void(T);
        };

        template <typename X>
        struct test {
            using input = typename A<void, X>::type;
            using expect = void;
        };
    )code");
    ASSERT_TRUE(compile());

    InputFinder finder(*unit);
    finder.TraverseAST(unit->context());

    auto input = unit->resolver().resolve(finder.input);
    ASSERT_FALSE(input.isNull());
    if(auto FPT = input->getAs<clang::FunctionProtoType>()) {
        for(auto param: FPT->getParamTypes()) {
            EXPECT_FALSE(param->isVoidType());
        }
    }
}

TEST_CASE(VoidMemberPointee) {
    /// `void C::*` is not a valid member pointer; degrade.
    add_main("main.cpp", R"code(
        struct C {};

        template <typename T, typename U>
        struct A {
            using type = T C::*;
        };

        template <typename X>
        struct test {
            using input = typename A<void, X>::type;
            using expect = void;
        };
    )code");
    ASSERT_TRUE(compile());

    InputFinder finder(*unit);
    finder.TraverseAST(unit->context());

    auto input = unit->resolver().resolve(finder.input);
    ASSERT_FALSE(input.isNull());
    if(auto MPT = input->getAs<clang::MemberPointerType>()) {
        EXPECT_FALSE(MPT->getPointeeType()->isVoidType());
    }
}

TEST_CASE(ArraySizeForward) {
    run(R"code(
        template <typename T, unsigned long N>
        struct S {
            using type = T[N];
        };

        template <typename X, unsigned long M>
        struct test {
            using input = typename S<X, M>::type;
            using expect = X[M];
        };
    )code");
}

TEST_CASE(RepeatedValuePack) {
    run(R"code(
        template <int A, int B>
        struct pairv {};

        template <typename... Ts>
        struct type_list {};

        template <typename T>
        struct trait {
            using type = void;
        };

        template <int... Ns>
        struct trait<type_list<pairv<Ns, Ns>...>> {
            using type = int;
        };

        template <int X>
        struct test {
            using input = typename trait<type_list<pairv<X, X>>>::type;
            using expect = int;
        };
    )code");
}

TEST_CASE(SurplusArguments) {
    /// A variadic template template parameter bound to a one-parameter
    /// template, applied with two arguments: rebuilding `target<X, int>`
    /// would fabricate an invalid specialization — it must degrade.
    add_main("main.cpp", R"code(
        template <typename T>
        struct target {};

        template <template <typename...> class TT, typename A>
        struct apply {
            using type = TT<A, int>;
        };

        template <typename X>
        struct test {
            using input = typename apply<target, X>::type;
            using expect = void;
        };
    )code");
    ASSERT_TRUE(compile());

    InputFinder finder(*unit);
    finder.TraverseAST(unit->context());

    auto input = unit->resolver().resolve(finder.input);
    ASSERT_FALSE(input.isNull());
    if(auto TST = input->getAs<clang::TemplateSpecializationType>()) {
        auto TD = TST->getTemplateName().getAsTemplateDecl();
        EXPECT_FALSE(TD && TD->getName() == "target" && TST->template_arguments().size() > 1);
    }
}

TEST_CASE(TemplateDefaultForward) {
    run(R"code(
        template <typename T>
        struct box {};

        template <template <typename> class TT, template <typename> class U = TT>
        struct A {
            template <typename X>
            using type = U<X>;
        };

        template <typename X>
        struct test {
            using input = typename A<box>::template type<X>;
            using expect = box<X>;
        };
    )code");
}

TEST_CASE(NonTrailingPack) {
    /// A non-trailing pack expansion is non-deduced: the partial must not
    /// greedily swallow the trailing `int` and mis-select itself.
    run(R"code(
        template <typename... Ts>
        struct type_list {};

        template <typename A, typename B>
        struct trait {
            using type = void;
        };

        template <typename... Ts>
        struct trait<type_list<Ts...>, void(Ts..., int)> {
            using type = int;
        };

        template <typename X>
        struct test {
            using input = typename trait<type_list<X>, void(X)>::type;
            using expect = void;
        };
    )code");
}

TEST_CASE(FunctionReturnArray) {
    /// `A<int[2], X>::type` would be an array-returning function type; the
    /// resolver must degrade rather than fabricate it.
    add_main("main.cpp", R"code(
        template <typename T, typename U>
        struct A {
            using type = T();
        };

        template <typename X>
        struct test {
            using input = typename A<int[2], X>::type;
            using expect = void;
        };
    )code");
    ASSERT_TRUE(compile());

    InputFinder finder(*unit);
    finder.TraverseAST(unit->context());

    auto input = unit->resolver().resolve(finder.input);
    ASSERT_FALSE(input.isNull());
    if(auto FPT = input->getAs<clang::FunctionProtoType>()) {
        EXPECT_FALSE(FPT->getReturnType()->isArrayType());
    }
}

TEST_CASE(DefaultsInCanonical) {
    /// A class template bound through a template template parameter: the
    /// rebuilt specialization's canonical arguments must include the bound
    /// template's defaults, or it never compares equal to `target<X>`.
    run(R"code(
        template <typename T, typename U = int>
        struct target {};

        template <template <typename> class TT, typename A>
        struct apply {
            using type = TT<A>;
        };

        template <typename X>
        struct test {
            using input = typename apply<target, X>::type;
            using expect = target<X>;
        };
    )code");
}

TEST_CASE(ValuePackExpansion) {
    run(R"code(
        template <int N>
        struct value {};

        template <typename... Ts>
        struct type_list {};

        template <int... Ns>
        struct A {
            using type = type_list<value<Ns>...>;
        };

        template <int X>
        struct test {
            using input = typename A<X, 2>::type;
            using expect = type_list<value<X>, value<2>>;
        };
    )code");
}

TEST_CASE(AutoValueType) {
    /// `auto` parameter identity includes the value's type: the int-literal
    /// partial must not swallow a long argument.
    run(R"code(
        template <auto A, auto B>
        struct trait {
            using type = void;
        };

        template <auto B>
        struct trait<1, B> {
            using type = int;
        };

        template <auto Y>
        struct test {
            using input = typename trait<1L, Y>::type;
            using expect = void;
        };
    )code");
}

TEST_CASE(DependentTypedValue) {
    /// `N` is typed by the earlier parameter `T`: its argument must stay an
    /// expression until `T` is known — normalizing it against the dependent
    /// type would violate integral-argument invariants.
    run(R"code(
        template <typename T, T N>
        struct A {
            using type = T;
        };

        template <typename X>
        struct test {
            using input = typename A<X, 1>::type;
            using expect = X;
        };
    )code");
}

TEST_CASE(ParamDecayAdjust) {
    run(R"code(
        template <typename T, typename U>
        struct A {
            using type = void(T);
        };

        template <typename X>
        struct test {
            using input = typename A<int[2], X>::type;
            using expect = void(int*);
        };
    )code");
}

TEST_CASE(DependentValueDefault) {
    run(R"code(
        template <int N>
        struct value {};

        template <int N, int M = N>
        struct A {
            using type = value<M>;
        };

        template <int X>
        struct test {
            using input = typename A<X>::type;
            using expect = value<X>;
        };
    )code");
}

TEST_CASE(ExplicitObjectArity) {
    add_main("main.cpp", R"code(
        template <typename T>
        struct S {
            int foo(this S&, int);
            int foo(this S&, int, int);

            void call(T t) {
                this->foo(t);
            }
        };
    )code");
    ASSERT_TRUE(compile("-std=c++23"));

    struct Finder : clang::RecursiveASTVisitor<Finder> {
        const clang::CallExpr* expr = nullptr;

        bool VisitCallExpr(clang::CallExpr* e) {
            if(llvm::isa<clang::UnresolvedMemberExpr>(e->getCallee()->IgnoreParenImpCasts())) {
                expr = e;
            }
            return true;
        }
    } finder;

    finder.TraverseAST(unit->context());
    ASSERT_TRUE(finder.expr != nullptr);

    /// One written argument plus the unspelled object: only the
    /// two-parameter explicit-object overload survives.
    auto candidates = unit->resolver().lookup(finder.expr);
    EXPECT_EQ(candidates.size(), 1u);
}

TEST_CASE(CallArityOccurrences) {
    /// The production occurrence path must apply the arity filter: the
    /// two-argument overload never appears for a one-argument call.
    add_main("main.cpp", R"code(
        template <typename T>
        struct S {
            int foo(int);
            int foo(int, int);
            int foo(T);

            void call(T t) {
                foo(t);
            }
        };
    )code");
    ASSERT_TRUE(compile());

    auto semantics = Semantics::build(*unit);
    auto entries = semantics.node_entries();

    bool found = false;
    for(std::uint32_t i = 0; i < entries.size(); i += 1) {
        if(entries[i].node.get<clang::UnresolvedMemberExpr>()) {
            auto occurrences = resolve_occurrences(semantics, i, &unit->resolver());
            EXPECT_EQ(occurrences.size(), 2);
            found = true;
        }
    }
    EXPECT_TRUE(found);
}

TEST_CASE(ConditionalFalseType) {
    run(R"code(
        template <bool B, typename T, typename F>
        struct pick {
            using type = T;
        };

        template <typename T, typename F>
        struct pick<false, T, F> {
            using type = F;
        };

        template <typename X>
        struct test {
            using input = typename pick<false, int, X>::type;
            using expect = X;
        };
    )code");
}

TEST_CASE(TemplateThroughLayer) {
    run(R"code(
        template <typename... Ts>
        struct type_list {};

        template <template <typename...> typename TT, typename... Us>
        struct apply {
            using type = TT<Us...>;
        };

        template <template <typename...> typename TT, typename... Us>
        struct indirect {
            using type = typename apply<TT, Us...>::type;
        };

        template <typename X, typename Y>
        struct test {
            using input = typename indirect<type_list, X, Y>::type;
            using expect = type_list<X, Y>;
        };
    )code");
}

TEST_CASE(StandardMap) {
    add_main("main.cpp", R"code(
        #include <map>

        template <typename K, typename V>
        struct test {
            using input = typename std::map<K, V>::mapped_type;
            using expect = V;
        };
    )code");
    ASSERT_TRUE(compile_driver());

    InputFinder finder(*unit);
    finder.TraverseAST(unit->context());

    auto input = unit->resolver().resolve(finder.input);
    auto target = finder.expect;
    ASSERT_FALSE(input.isNull() || target.isNull());
    EXPECT_EQ(input.getCanonicalType(), target.getCanonicalType());
}

TEST_CASE(StandardString) {
    add_main("main.cpp", R"code(
        #include <string>

        template <typename T>
        struct test {
            using input = typename std::basic_string<T>::value_type;
            using expect = T;
        };
    )code");
    ASSERT_TRUE(compile_driver());

    InputFinder finder(*unit);
    finder.TraverseAST(unit->context());

    auto input = unit->resolver().resolve(finder.input);
    auto target = finder.expect;
    ASSERT_FALSE(input.isNull() || target.isNull());
    EXPECT_EQ(input.getCanonicalType(), target.getCanonicalType());
}

TEST_CASE(Standard) {
    add_main("main.cpp", R"code(
        #include <vector>

        template <typename T>
        struct test {
            using input = typename std::vector<T>::reference;
            using expect = T&;
        };
    )code");
    ASSERT_TRUE(compile_driver());

    InputFinder finder(*unit);
    finder.TraverseAST(unit->context());

    auto input = unit->resolver().resolve(finder.input);
    auto target = finder.expect;
    ASSERT_FALSE(input.isNull() || target.isNull());
    EXPECT_EQ(input.getCanonicalType(), target.getCanonicalType());
};

TEST_CASE(DependentTemplateHead) {
    /// A template template argument that is itself a dependent name (libc++
    /// binds `_Alloc::template rebind` this way): the rebuilt head must be a
    /// dependent specialization, never a TemplateSpecializationType —
    /// assertion-enabled clang aborts on the latter.
    add_main("main.cpp", R"code(
        template <template <typename> class TT>
        struct apply {
            using type = TT<int>;
        };

        template <typename T>
        struct test {
            using input = typename apply<T::template tmpl>::type;
            using expect = void;
        };
    )code");
    ASSERT_TRUE(compile());

    InputFinder finder(*unit);
    finder.TraverseAST(unit->context());

    auto input = unit->resolver().resolve(finder.input);
    ASSERT_FALSE(input.isNull());
    EXPECT_TRUE(input->isDependentType());
}

TEST_CASE(BrokenCodeSweep) {
    /// Error-recovery ASTs are the resolver's everyday hostile input in an
    /// LSP; resolving anything in a broken TU must degrade, never crash.
    add_main("main.cpp", R"code(
        template <typename T>
        struct A {
            using type = typename T::missing;
            using bad = typename unknown_ns::thing<T>::nested;
            using worse = typename A<typename T::gone>::type;
            T member;

            void f() {
                auto x = undeclared_function(member);
                member.no_such_member();
            }
        };

        template <typename T>
        struct A<T*>;

        template <typename... Ts>
        struct B {
            using type = typename A<mistyped<Ts...>>::type;
            using other = typename B::unknown;
        };

        struct C : A<not_a_type> {
            using inherited = typename A<not_a_type>::type;
        };
    )code");
    prepare();
    ASSERT_TRUE(try_compile());

    struct Sweeper : clang::RecursiveASTVisitor<Sweeper> {
        types::TemplateResolver& resolver;
        unsigned visited = 0;

        Sweeper(types::TemplateResolver& resolver) : resolver(resolver) {}

        bool VisitTypedefNameDecl(clang::TypedefNameDecl* decl) {
            auto type = decl->getUnderlyingType();
            if(!type.isNull() && type->isDependentType()) {
                resolver.resolve(type);
                visited += 1;
            }
            return true;
        }

        bool VisitCXXDependentScopeMemberExpr(clang::CXXDependentScopeMemberExpr* expr) {
            resolver.lookup(expr);
            visited += 1;
            return true;
        }

        bool VisitDependentScopeDeclRefExpr(clang::DependentScopeDeclRefExpr* expr) {
            resolver.lookup(expr);
            visited += 1;
            return true;
        }
    } sweeper(unit->resolver());

    sweeper.TraverseAST(unit->context());
    EXPECT_TRUE(sweeper.visited > 0);
}

TEST_CASE(StandardSweep) {
    /// Crash-safety sweep: feed every dependent typedef and dependent
    /// expression in a libstdc++-heavy TU through the resolver. Results are
    /// irrelevant — under the assertion-enabled ASan Debug build, surviving
    /// the sweep is the assertion.
    add_main("main.cpp", R"code(
        #include <functional>
        #include <map>
        #include <memory>
        #include <vector>
    )code");
    ASSERT_TRUE(compile_driver());

    struct Sweeper : clang::RecursiveASTVisitor<Sweeper> {
        types::TemplateResolver& resolver;
        unsigned visited = 0;

        Sweeper(types::TemplateResolver& resolver) : resolver(resolver) {}

        bool VisitTypedefNameDecl(clang::TypedefNameDecl* decl) {
            auto type = decl->getUnderlyingType();
            if(!type.isNull() && type->isDependentType()) {
                resolver.resolve(type);
                visited += 1;
            }
            return true;
        }

        bool VisitCXXDependentScopeMemberExpr(clang::CXXDependentScopeMemberExpr* expr) {
            resolver.lookup(expr);
            visited += 1;
            return true;
        }

        bool VisitDependentScopeDeclRefExpr(clang::DependentScopeDeclRefExpr* expr) {
            resolver.lookup(expr);
            visited += 1;
            return true;
        }
    } sweeper(unit->resolver());

    sweeper.TraverseAST(unit->context());
    EXPECT_TRUE(sweeper.visited > 0);
}

};  // TEST_SUITE(TemplateResolver)

}  // namespace

}  // namespace clice::testing
