#include "Test/Test.h"
#include "Test/Tester.h"

#include "clang/AST/RecursiveASTVisitor.h"

namespace clice::testing {

namespace {

struct InputFinder : clang::RecursiveASTVisitor<InputFinder> {
    CompilationUnit& unit;
    clang::QualType input;
    clang::QualType expect;

    using Base = clang::RecursiveASTVisitor<InputFinder>;

    InputFinder(CompilationUnit& unit) : unit(unit) {}

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

TEST_SUITE(TemplateResolver) {

void run(llvm::StringRef code) {
    Tester tester;
    tester.add_main("main.cpp", code);
    ASSERT_TRUE(tester.compile());

    InputFinder finder(*tester.unit);
    finder.TraverseAST(tester.unit->context());

    auto input = tester.unit->resolver().resolve(finder.input);
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

TEST_CASE(InnerDependentPartialMemberClass) {
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

TEST_CASE(Standard) {
    run(R"code(
        #include <vector>

        template <typename T>
        struct test {
            using input = typename std::vector<T>::reference;
            using expect = T&;
        };
    )code");
};

};  // TEST_SUITE(TemplateResolver)

}  // namespace

}  // namespace clice::testing
