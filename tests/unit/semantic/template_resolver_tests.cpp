#include "test/test.h"
#include "test/tester.h"

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

// --- Robustness tests for edge cases found during stress testing ---

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

    auto old_level = clice::logging::options.level;
    clice::logging::options.level = clice::logging::Level::debug;

    auto input = unit->resolver().resolve(finder.input);
    auto target = finder.expect;

    clice::logging::options.level = old_level;

    llvm::errs() << "resolved: " << input << "\n";
    llvm::errs() << "target: " << target << "\n";
    ASSERT_FALSE(input.isNull() || target.isNull());
    EXPECT_EQ(input.getCanonicalType(), target.getCanonicalType());
};

};  // TEST_SUITE(TemplateResolver)

}  // namespace

}  // namespace clice::testing
