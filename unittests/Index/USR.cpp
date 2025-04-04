#include "Support/Logger.h"
#include "Test/CTest.h"
#include "Index/USR.h"
#include "clang/AST/DeclBase.h"
#include "clang/AST/RecursiveASTVisitor.h"

namespace clice::testing {

namespace {

struct USRInfo {
    llvm::SmallString<128> USR;
    uint32_t offset;
    clang::Decl* decl;
};

struct GetUSRVisitor : public clang::RecursiveASTVisitor<GetUSRVisitor> {
    bool VisitDecl(clang::Decl* decl) {
        auto offset = [&]() -> std::optional<std::uint32_t> {
            auto loc = decl->getLocation();
            if(!loc.isValid())
                return std::nullopt;

            auto& SM = decl->getASTContext().getSourceManager();
            auto [_, offset] = SM.getDecomposedLoc(loc);
            return offset;
        }();

        auto USR = [&]() -> std::optional<llvm::SmallString<128>> {
            llvm::SmallString<128> buffer;
            if(!clice::index::generateUSRForDecl(decl, buffer)) {
                return buffer;
            }
            return std::nullopt;
        }();

        if(offset.has_value() && USR.has_value()) {
            USRs[*offset] = USRInfo{*USR, *offset, decl};
            // log::info("USR: {} at {}:{}", USR->str(), pos->line, pos->character);
        }

        return true;
    }

    std::map<std::uint32_t, USRInfo> USRs;
};

class USRTester : public Tester {
    using Tester::Tester;

public:
    void run(llvm::StringRef standard = "-std=c++20") {
        Tester::compile(standard);

        GetUSRVisitor visitor;
        visitor.TraverseDecl(AST->tu());
        USRs = std::move(visitor.USRs);
    }

    llvm::StringRef lookupUSR(llvm::StringRef key) {
        auto iter = USRs.find(offset(key));
        if(iter == USRs.end()) {
            log::fatal("USR not found for key: {}", key);
        }
        return iter->second.USR;
    }

    std::map<uint32_t, USRInfo> USRs;
};

TEST(Index, USRTemplateClassRequireClause) {
    llvm::StringRef content = R"cpp(
#include <concepts>
template <typename T> struct A;

template <typename T> requires (__is_same(T, float))
struct $(1)A<T>;

template <typename T> requires (__is_same(T, int))
struct $(2)A<T>;

template <typename T> requires (std::same_as<T, double>)
struct $(3)A<T> {};
)cpp";

    USRTester tester("main.cpp", content);
    tester.run();

    auto usr1 = tester.lookupUSR("1");
    auto usr2 = tester.lookupUSR("2");
    auto usr3 = tester.lookupUSR("3");

    EXPECT_NE(usr1, usr2);
    EXPECT_NE(usr1, usr3);
    EXPECT_NE(usr2, usr3);
}

TEST(Index, USRTemplateClassRequireClauseComplex) {
    llvm::StringRef content = R"cpp(
template <typename T> struct A;

template <typename T> requires (__is_same(T, float))
struct $(1)A<T>;

using FLOAT = float;
template <typename T> requires (__is_same(T, FLOAT))
struct $(2)A<T>;

template <typename T> requires (__is_same(T, A<int>))
struct $(11)A<T>;
template <typename T> requires (__is_same(T, A<float>))
struct $(12)A<T>;
template <typename T> requires (__is_same(T, A<FLOAT>))
struct $(13)A<T>;
)cpp";

    USRTester tester("main.cpp", content);
    tester.run();

    auto usr1 = tester.lookupUSR("1");
    auto usr2 = tester.lookupUSR("2");

    EXPECT_EQ(usr1, usr2);

    auto usr11 = tester.lookupUSR("11");
    auto usr12 = tester.lookupUSR("12");
    auto usr13 = tester.lookupUSR("13");

    EXPECT_NE(usr11, usr12);
    EXPECT_EQ(usr12, usr13);
}

TEST(Index, USRTemplateClassConceptConstraint) {
    llvm::StringRef content = R"cpp(
template<typename T>
concept C = requires(T t) { true; };
template<typename T, typename U>
struct A;

template<typename T, C U>
struct $(1)A<T, U>;

template<C T, typename U>
struct $(2)A<T, U>;
)cpp";

    USRTester tester("main.cpp", content);
    tester.run();

    auto usr1 = tester.lookupUSR("1");
    auto usr2 = tester.lookupUSR("2");

    EXPECT_NE(usr1, usr2);
}

TEST(Index, USRTemplateClassConceptConstraintPack) {
    llvm::StringRef content1 = R"cpp(
template<typename... Ts>
struct $(1)A;
)cpp";

    llvm::StringRef content2 = R"cpp(
template<typename T>
concept C = requires(T t) { true; };
template<C... Ts>
struct $(2)A;
)cpp";

    USRTester tester1("main.cpp", content1);
    tester1.run();
    USRTester tester2("main.cpp", content2);
    tester2.run();

    auto usr1 = tester1.lookupUSR("1");
    auto usr2 = tester2.lookupUSR("2");

    EXPECT_NE(usr1, usr2);
}

TEST(Index, USRTemplateArgumentExpr) {
    llvm::StringRef content = R"cpp(
template <typename T, int N> struct C;

template <int N> struct $(1)C<float, N>;
template <int M> struct $(2)C<float, M>;
template <char c> struct $(3)C<float, c>;
)cpp";

    USRTester tester("main.cpp", content);
    tester.run();

    auto usr1 = tester.lookupUSR("1");
    auto usr2 = tester.lookupUSR("2");
    auto usr3 = tester.lookupUSR("3");

    EXPECT_EQ(usr1, usr2);
    EXPECT_NE(usr1, usr3);
}

TEST(Index, USRTemplateFunctionRequireClause) {
    llvm::StringRef content = R"cpp(
template<typename T>
void $(1)func(T t) requires (sizeof(T) == 4) {};

template<typename T>
void $(2)func(T t) requires (sizeof(T) == 8) {};
)cpp";

    USRTester tester("main.cpp", content);
    tester.run();

    auto usr1 = tester.lookupUSR("1");
    auto usr2 = tester.lookupUSR("2");

    EXPECT_NE(usr1, usr2);
}

TEST(Index, USRTemplateVarConceptConstraint) {
    llvm::StringRef content1 = R"cpp(
template <typename T>
constexpr T $(1)pi = 3.14;
)cpp";

    llvm::StringRef content2 = R"cpp(
template <typename T>
concept integral = requires (T t) { t + 1; };
template <integral T>
constexpr T $(2)pi = 3;
)cpp";

    USRTester tester1("main.cpp", content1);
    tester1.run();
    USRTester tester2("main.cpp", content2);
    tester2.run();

    auto usr1 = tester1.lookupUSR("1");
    auto usr2 = tester2.lookupUSR("2");

    EXPECT_NE(usr1, usr2);
}

TEST(Index, USRCTAD) {
    llvm::StringRef content = R"cpp(
template<typename T>
struct array {};
template<typename U, array arr>
struct $(1)L;
)cpp";

    USRTester tester("main.cpp", content);
    tester.run();

    auto usr1 = tester.lookupUSR("1");

    EXPECT_TRUE(usr1.contains("array"));
}

TEST(Index, USRTemplateParamObject) {
    llvm::StringRef content = R"cpp(
template<typename T> struct array {
  constexpr array(T x_) : x(x_) {}
  int x;
};
template<array U> struct L;
template<> struct $(1)L<{1}>;
template<> struct $(2)L<{2}>;
)cpp";

    USRTester tester("main.cpp", content);
    tester.run();

    auto usr1 = tester.lookupUSR("1");
    auto usr2 = tester.lookupUSR("2");

    EXPECT_NE(usr1, usr2);
}

TEST(Index, USRNTTPConstraint) {
    llvm::StringRef content1 = R"cpp(
template<auto N> struct $(1)M;
)cpp";

    llvm::StringRef content2 = R"cpp(
template<typename T> concept C = requires {requires true;};
template<C auto N> struct $(2)M;
)cpp";

    USRTester tester1("main.cpp", content1);
    tester1.run();
    USRTester tester2("main.cpp", content2);
    tester2.run();

    auto usr1 = tester1.lookupUSR("1");
    auto usr2 = tester2.lookupUSR("2");

    EXPECT_NE(usr1, usr2);
}

TEST(Index, USRDependentTemplateName) {
    llvm::StringRef content = R"cpp(
template <typename MetaFun>
struct X {
    template <template <typename, typename> class Tmpl>
    class Y;

    template <>
    class $(1)Y<MetaFun::template apply>;

    template <>
    class $(2)Y<MetaFun::template apply2>;
};
)cpp";

    USRTester tester("main.cpp", content);
    tester.run();

    auto usr1 = tester.lookupUSR("1");
    auto usr2 = tester.lookupUSR("2");

    EXPECT_NE(usr1, usr2);
}

TEST(Index, USRDeducingThis) {
    llvm::StringRef content = R"cpp(
class A {
public:
    template <typename Self>
    void $(1)foo(this Self&& s);

    template <typename Self>
    void $(2)foo(Self&& s);
};
)cpp";

    USRTester tester("main.cpp", content);
    tester.run("-std=c++23");

    auto usr1 = tester.lookupUSR("1");
    auto usr2 = tester.lookupUSR("2");

    EXPECT_NE(usr1, usr2);
}

}  // namespace

}  // namespace clice::testing
