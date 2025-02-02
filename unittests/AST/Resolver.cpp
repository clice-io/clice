#include "Test/CTest.h"
#include "clang/AST/RecursiveASTVisitor.h"

namespace clice::testing {

namespace {

void run(llvm::StringRef code, std::source_location current = std::source_location::current()) {
    Tester tester("main.cpp", code);
    tester.run();

    struct Run : clang::RecursiveASTVisitor<Run> {
        ASTInfo& info;
        clang::QualType input;
        clang::QualType expect;

        using Base = clang::RecursiveASTVisitor<Run>;

        Run(ASTInfo& info) : info(info) {}

        bool TraverseDecl(clang::Decl* decl) {
            if(decl && (llvm::isa<clang::TranslationUnitDecl>(decl) ||
                        info.srcMgr().isInMainFile(decl->getLocation()))) {
                return Base::TraverseDecl(decl);
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

    Run run{*tester.info};
    run.TraverseAST(tester.info->context());

    auto input = tester.info->resolver().resolve(run.input);
    auto expect = run.expect;

    EXPECT_EQ(input.isNull(), false);
    EXPECT_EQ(expect.isNull(), false);

    EXPECT_EQ(input.getCanonicalType(), expect.getCanonicalType());
}

TEST(TemplateResolver, TypeParameterType) {
    run(R"cpp(
template <typename T>
struct A {
    using type = T;
};

template <typename X>
struct test {
    using input = typename A<X>::type;
    using expect = X;
};
)cpp");
}

TEST(TemplateResolver, SingleLevel) {
    run(R"cpp(
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
)cpp");
}

TEST(TemplateResolver, SingleLevelNotDependent) {
    run(R"cpp(
template <typename T>
struct A {
    using type = int;
};

template <typename X>
struct test {
    using input = typename A<X>::type;
    using expect = int;
};
)cpp");
}

TEST(TemplateResolver, MultiLevel) {
    run(R"cpp(
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
)cpp");
}

TEST(TemplateResolver, MultiLevelNotDependent) {
    run(R"cpp(
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
)cpp");
}

TEST(TemplateResolver, ArgumentDependent) {
    run(R"cpp(
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
)cpp");
}

TEST(TemplateResolver, AliasArgument) {
    run(R"cpp(
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
)cpp");
}

TEST(TemplateResolver, AliasDependent) {
    run(R"cpp(
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
)cpp");
}

TEST(TemplateResolver, AliasTemplate) {
    run(R"cpp(
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
)cpp");
}

TEST(TemplateResolver, BaseDependent) {
    run(R"cpp(
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
)cpp");
}

TEST(TemplateResolver, MultiNested) {
    run(R"cpp(
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
)cpp");
}

TEST(TemplateResolver, OuterDependentMemberClass) {
    run(R"cpp(
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
)cpp");
}

TEST(TemplateResolver, InnerDependentMemberClass) {
    run(R"cpp(
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
)cpp");
}

TEST(TemplateResolver, InnerDependentPartialMemberClass) {
    run(R"cpp(
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
)cpp");
}

TEST(TemplateResolver, PartialSpecialization) {
    run(R"cpp(
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
)cpp");
}

TEST(TemplateResolver, PartialDefaultArgument) {
    run(R"cpp(
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
)cpp");
}

TEST(TemplateResolver, DefaultArgument) {
    run(R"cpp(
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
)cpp");
}

TEST(TemplateResolver, PackExpansion) {
    run(R"cpp(
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
)cpp");
}

TEST(TemplateResolver, BasePackExpansion) {
    run(R"cpp(
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
)cpp");
}

TEST(TemplateResolver, Standard) {
    run(R"cpp(
#include <vector>

template <typename T>
struct test {
    using input = typename std::vector<T>::reference;
    using expect = T&;
};
)cpp");
}

}  // namespace

}  // namespace clice::testing

