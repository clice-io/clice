module clice;

import :semantic.selection;
import :unittests.utils.tester;
import :unittests.utils;

namespace clice {

std::ostringstream& operator<<(std::ostringstream& os, const LocalSourceRange& range) {
    os << range.begin << " - " << range.end;
    return os;
}

}  // namespace clice

namespace clice::testing {

ZEST_SUITE(SelectionTree, Tester) {

template <typename Callback>
void select_right(llvm::StringRef code, Callback&& callback) {
    clear();
    add_focus("main.cpp", code);
    ZASSERT(compile());
    /// ZASSERT(unit->diagnostics().empty());

    auto points = nameless_points();
    ZASSERT(!points.empty());

    LocalSourceRange selected_range;
    selected_range.begin = points[0];
    selected_range.end = points.size() == 2 ? points[1] : points[0];
    auto tree = SelectionTree::create_right(*unit, selected_range);
    std::forward<Callback>(callback)(tree);
}

void EXPECT_SELECT(llvm::StringRef code, const char* kind) {
    select_right(code, [&](SelectionTree& tree) {
        auto node = tree.common_ancestor();
        if(!kind) {
            ZASSERT(!node);
            return;
        }

        ZASSERT(node);
        ZASSERT(node->kind() == llvm::StringRef(kind));
        ZASSERT(unit->focus_file().written_range(node->source_range()) == range());
    });
}

ZEST_CASE(Expressions) {
    EXPECT_SELECT(R"(
        struct AAA { struct BBB { static int ccc(); };};
        int x = §⟦AAA::BBB::c§c§c⟧();
    )",
                  "DeclRefExpr");

    EXPECT_SELECT(R"(
        struct AAA { struct BBB { static int ccc(); };};
        int x = §⟦AAA::BBB::ccc(§)⟧;
    )",
                  "CallExpr");

    EXPECT_SELECT(R"(
        struct S {
          int foo() const;
          int bar() { return §⟦f§oo⟧(); }
        };
    )",
                  "MemberExpr");

    EXPECT_SELECT(R"(void foo() { §⟦§foo⟧(); })", "DeclRefExpr");
    EXPECT_SELECT(R"(void foo() { §⟦f§oo⟧(); })", "DeclRefExpr");
    EXPECT_SELECT(R"(void foo() { §⟦fo§o⟧(); })", "DeclRefExpr");

    EXPECT_SELECT(R"(void foo() { §⟦foo§⟧ (); })", "DeclRefExpr");

    EXPECT_SELECT(R"(void foo() { §⟦foo§()()⟧; })", "CallExpr");
    EXPECT_SELECT(R"(void foo() { §⟦foo§()()⟧; /*comment*/§})", "CallExpr");
    EXPECT_SELECT(R"(const int x = 1, y = 2; int array[ §⟦§x⟧ ][10][y];)", "DeclRefExpr");
    EXPECT_SELECT(R"(const int x = 1, y = 2; int array[x][10][ §⟦§y⟧ ];)", "DeclRefExpr");
    EXPECT_SELECT(R"(void func(int x) { int v_array[ §⟦§x⟧ ][10]; })", "DeclRefExpr");
    EXPECT_SELECT(R"(
        int a;
        decltype(§⟦§a⟧ + a) b;
    )",
                  "DeclRefExpr");

    EXPECT_SELECT(R"(
        void func() { §⟦__§func__⟧; }
    )",
                  "PredefinedExpr");
}

ZEST_CASE(Literals) {
    EXPECT_SELECT(R"(
        auto lambda = [](const char*){ return 0; };
        int x = lambda(§⟦"y§"⟧);
    )",
                  "StringLiteral");

    EXPECT_SELECT(R"(int x = §⟦42⟧§;)", "IntegerLiteral");
    EXPECT_SELECT(R"(const int x = 1, y = 2; int array[x][ §⟦§10⟧ ][y];)", "IntegerLiteral");

    EXPECT_SELECT(R"(
        struct Foo{};
        Foo operator""_ud(unsigned long long);
        Foo x = §⟦§12_ud⟧;
    )",
                  "UserDefinedLiteral");
}

ZEST_CASE(ControlFlow) {
    EXPECT_SELECT(R"(
        void foo() { §⟦if (1§11) { return; } else {§ }⟧} }
    )",
                  "IfStmt");

    EXPECT_SELECT(R"(int bar; void foo() §⟦{ foo (); }⟧§)", "CompoundStmt");

    EXPECT_SELECT(R"(
        /*error-ok*/
        void func() §⟦{§⟧)",
                  "CompoundStmt");

    EXPECT_SELECT(R"(
        struct Str {
          const char *begin();
          const char *end();
        };
        Str makeStr(const char*);
        void loop() {
          for (const char C : §⟦mak§eStr("foo"§)⟧)
            ;
        }
    )",
                  "CallExpr");
}

ZEST_CASE(Declarations) {
    EXPECT_SELECT(R"(
         #define TARGET void foo()
         §⟦TAR§GET{ return; }⟧
         )",
                  "FunctionDecl");

    EXPECT_SELECT(R"(§⟦§void foo§()()⟧;)", "FunctionDecl");
    EXPECT_SELECT(R"(§⟦void §foo()⟧;)", "FunctionDecl");

    EXPECT_SELECT(R"(
        struct S { S(const char*); };
        §⟦S s §= "foo"⟧;
    )",
                  "VarDecl");

    EXPECT_SELECT(R"(
        struct S { S(const char*); };
        §⟦S §s = "foo"⟧;
    )",
                  "VarDecl");

    EXPECT_SELECT(R"(
        §⟦void (*§S)(int) = nullptr⟧;
    )",
                  "VarDecl");

    EXPECT_SELECT(R"(§⟦int §a⟧, b;)", "VarDecl");
    EXPECT_SELECT(R"(§⟦int a, §b⟧;)", "VarDecl");
    EXPECT_SELECT(R"(§⟦struct {int x;} §y⟧;)", "VarDecl");
    EXPECT_SELECT(R"(struct foo { §⟦int has§h<:32:>⟧; };)", "FieldDecl");
    EXPECT_SELECT(R"(struct {§⟦int §x⟧;} y;)", "FieldDecl");

    EXPECT_SELECT(R"(
        void test(int bar) {
        auto l = [ §§⟦foo = bar⟧ ] { };
    })",
                  "VarDecl");
}

ZEST_CASE(Types) {
    EXPECT_SELECT(R"(
        struct AAA { struct BBB { static int ccc(); };};
        int x = AAA::§⟦B§B§B⟧::ccc();
    )",
                  "RecordTypeLoc");
    EXPECT_SELECT(R"(
        struct AAA { struct BBB { static int ccc(); };};
        int x = AAA::§⟦B§BB§⟧::ccc();
    )",
                  "RecordTypeLoc");
    EXPECT_SELECT(R"(
        struct Foo {};
        struct Bar : private §⟦Fo§o⟧ {};
    )",
                  "RecordTypeLoc");
    EXPECT_SELECT(R"(
        struct Foo {};
        struct Bar : §⟦Fo§o⟧ {};
    )",
                  "RecordTypeLoc");
    EXPECT_SELECT(R"(§⟦§void⟧ (*S)(int) = nullptr;)", "BuiltinTypeLoc");
    /// EXPECT_SELECT(R"(§⟦void (*S)§()(int)⟧ = nullptr;)", "FunctionProtoTypeLoc");
    EXPECT_SELECT(R"(§⟦void (§*S)(int)⟧ = nullptr;)", "PointerTypeLoc");
    /// EXPECT_SELECT(R"(§⟦void §()(*S)(int)⟧ = nullptr;)", "ParenTypeLoc");
    EXPECT_SELECT(R"(§⟦§void⟧ foo();)", "BuiltinTypeLoc");
    EXPECT_SELECT(R"(§⟦void foo§()()⟧;)", "FunctionProtoTypeLoc");
    EXPECT_SELECT(R"(const int x = 1, y = 2; §⟦i§nt⟧ array[x][10][y];)", "BuiltinTypeLoc");
    EXPECT_SELECT(R"(int (*getFunc(§⟦do§uble⟧))(int);)", "BuiltinTypeLoc");
    EXPECT_SELECT(R"(class X{}; §⟦int X::§*⟧y[10];)", "MemberPointerTypeLoc");
    EXPECT_SELECT(R"(const §⟦a§uto⟧ x = 42;)", "AutoTypeLoc");
    /// EXPECT_SELECT(R"(§⟦decltype§()(1)⟧ b;)", "DecltypeTypeLoc");
    EXPECT_SELECT(R"(§⟦de§cltype(a§uto)⟧ a = 1;)", "AutoTypeLoc");
    EXPECT_SELECT(R"(
        typedef int Foo;
        enum Bar : §⟦Fo§o⟧ {};
    )",
                  "TypedefTypeLoc");
    EXPECT_SELECT(R"(
        typedef int Foo;
        enum Bar : §⟦Fo§o⟧;
    )",
                  "TypedefTypeLoc");
}

ZEST_CASE(CXXFeatures) {
    /// A dependent qualifier component (`T::U`) is a DependentNameType;
    /// the cursor's innermost node owns its name token.
    EXPECT_SELECT(R"(
          template <typename T>
          int x = T::§⟦§U⟧::ccc();
          )",
                  "DependentNameTypeLoc");
    EXPECT_SELECT(R"(
          struct Foo {};
          struct Bar : §⟦v§ir§tual private Foo⟧ {};
          )",
                  "CXXBaseSpecifier");
    EXPECT_SELECT(R"(
          struct X { X(int); };
          class Y {
            X x;
            Y() : §⟦§x(4)⟧ {}
          };
          )",
                  "CXXCtorInitializer");
    EXPECT_SELECT(R"(§⟦st§ruct {int x;}⟧ y;)", "CXXRecordDecl");
    EXPECT_SELECT(R"(struct foo { §⟦op§erator int()⟧; };)", "CXXConversionDecl");
    EXPECT_SELECT(R"(struct foo { §⟦§~foo()⟧; };)", "CXXDestructorDecl");
    EXPECT_SELECT(R"(struct foo { §⟦~§foo()⟧; };)", "CXXDestructorDecl");
    EXPECT_SELECT(R"(struct foo { §⟦fo§o(){}⟧ };)", "CXXConstructorDecl");
    EXPECT_SELECT(R"(
        struct S1 { void f(); };
        struct S2 { S1 * operator->(); };
        void test(S2 s2) {
          s2§⟦-§>⟧f();
        }
      )",
                  "DeclRefExpr");  // Test for overloaded operator->
}

ZEST_CASE(UsingEnum) {
    EXPECT_SELECT(R"(
        namespace ns { enum class A {}; };
        using enum ns::§⟦§A⟧;
        )",
                  "EnumTypeLoc");
    EXPECT_SELECT(R"(
        namespace ns { enum class A {}; using B = A; };
        using enum ns::§⟦§B⟧;
        )",
                  "TypedefTypeLoc");
    EXPECT_SELECT(R"(
        namespace ns { enum class A {}; };
        using enum §⟦§ns::⟧A;
        )",
                  "NestedNameSpecifierLoc");
    EXPECT_SELECT(R"(
        namespace ns { enum class A {}; };
        §⟦using §enum ns::A⟧;
        )",
                  "UsingEnumDecl");
    EXPECT_SELECT(R"(
        namespace ns { enum class A {}; };
        §⟦§using enum ns::A⟧;
        )",
                  "UsingEnumDecl");
}

ZEST_CASE(Templates) {
    EXPECT_SELECT(R"(template<typename ...T> void foo(§⟦T*§...⟧x);)", "PackExpansionTypeLoc");
    EXPECT_SELECT(R"(template<typename ...T> void foo(§⟦§T⟧*...x);)", "TemplateTypeParmTypeLoc");
    EXPECT_SELECT(R"(template <typename T> void foo() { §⟦§T⟧ t; })", "TemplateTypeParmTypeLoc");
    EXPECT_SELECT(R"(
          template <class T> struct Foo {};
          template <§⟦template<class> class /*cursor here*/§U⟧>
            struct Foo<U<int>*> {};
          )",
                  "TemplateTemplateParmDecl");
    EXPECT_SELECT(R"(template <class T> struct foo { ~foo<§⟦§T⟧>(){} };)",
                  "TemplateTypeParmTypeLoc");
    EXPECT_SELECT(R"(
        template <typename> class Vector {};
        template <template <typename> class Container> class A {};
        A<§⟦V§ector⟧> a;
      )",
                  "TemplateArgumentLoc");
}

ZEST_CASE(Concepts) {
    EXPECT_SELECT(R"(
        template <class> concept C = true;
        auto x = §⟦§C<int>⟧;
      )",
                  "ConceptReference");
    EXPECT_SELECT(R"(
        template <class> concept C = true;
        §⟦§C⟧ auto x = 0;
      )",
                  "ConceptReference");
    EXPECT_SELECT(R"(
        template <class> concept C = true;
        void foo(§⟦§C⟧ auto x) {}
      )",
                  "ConceptReference");
    EXPECT_SELECT(R"(
        template <class> concept C = true;
        template <§⟦§C⟧ x> int i = 0;
      )",
                  "ConceptReference");
    EXPECT_SELECT(R"(
        namespace ns { template <class> concept C = true; }
        auto x = §⟦ns::§C<int>⟧;
      )",
                  "ConceptReference");
    EXPECT_SELECT(R"(
        template <typename T, typename K>
        concept D = true;
        template <typename T> void g(D<§⟦§T⟧> auto abc) {}
      )",
                  "TemplateTypeParmTypeLoc");
}

ZEST_CASE(Attributes) {
    EXPECT_SELECT(R"(
        void f(int * __attribute__((§⟦no§nnull⟧)) );
      )",
                  "NonNullAttr");
    EXPECT_SELECT(R"(
        // Digraph syntax for attributes to avoid accidental annotations.
        class [[gsl::Owner( §⟦in§t⟧ )]] X{};
      )",
                  "BuiltinTypeLoc");
}

ZEST_CASE(PartialCoverage) {
    select_right(R"(void foo(int); void bar() { foo§()(1§()); })", [&](SelectionTree& tree) {
        auto* node = tree.common_ancestor();
        ZASSERT(node != nullptr);
        ZASSERT(node->kind() == std::string("CallExpr"));
        // The call owns `(` and `)`, and only `(` is selected.
        ZASSERT(node->selected == SelectionTree::Partial);
    });

    select_right(R"(void foo(int); void bar() { foo§()(1)§(); })", [&](SelectionTree& tree) {
        auto* node = tree.common_ancestor();
        ZASSERT(node != nullptr);
        ZASSERT(node->kind() == std::string("CallExpr"));
        ZASSERT(node->selected == SelectionTree::Complete);
    });
}

ZEST_CASE(Macros) {
    EXPECT_SELECT(R"(
            int x(int);
            #define M(foo) x(foo)
            int a = 42;
            int b = M(§⟦§a⟧);
            )",
                  "DeclRefExpr");
    EXPECT_SELECT(R"(
            void foo();
            #define CALL_FUNCTION(X) X()
            void bar() { CALL_FUNCTION(§⟦f§o§o⟧); }
            )",
                  "DeclRefExpr");
    EXPECT_SELECT(R"(
            void foo();
            #define CALL_FUNCTION(X) X()
            void bar() { §⟦CALL_FUNC§TION(fo§o)⟧; }
            )",
                  "CallExpr");
    EXPECT_SELECT(R"(
            void foo();
            #define CALL_FUNCTION(X) X()
            void bar() { §⟦C§ALL_FUNC§TION(foo)⟧; }
            )",
                  "CallExpr");
}

ZEST_CASE(NullOrInvalid) {
    EXPECT_SELECT(R"(
              void foo();
              #§define CALL_FUNCTION(X) X(§)
              void bar() { CALL_FUNCTION(foo); }
              )",
                  nullptr);
    EXPECT_SELECT(R"(
              void foo();
              #define CALL_FUNCTION(X) X()
              void bar() { CALL_FUNCTION(foo§)§; }
              )",
                  nullptr);
    EXPECT_SELECT(R"(
              namespace ns {
              #if 0
              void fo§o() {}
              #endif
              }
              )",
                  nullptr);
    EXPECT_SELECT(R"(co§nst auto x = 42;)", nullptr);
    EXPECT_SELECT(R"(§)", nullptr);
    EXPECT_SELECT(R"(int x = 42;§)", nullptr);
    EXPECT_SELECT(R"(§int x; int y;§)", nullptr);
    EXPECT_SELECT(R"(void foo() { §⟦foo§⟧ (); })",
                  "DeclRefExpr");  // Technically valid, but tricky
}

ZEST_CASE(InjectedClassName) {
    llvm::StringRef code = "struct §X { int x; };";
    select_right(code, [&](SelectionTree& tree) {
        auto ancestor = tree.common_ancestor();
        ZASSERT(ancestor->kind() == llvm::StringRef("CXXRecordDecl"));
        auto* D = dyn_cast<clang::CXXRecordDecl>(ancestor->get<clang::Decl>());
        ZASSERT(!D->isInjectedClassName());
    });
}

ZEST_CASE(PathologicalPreprocessor) {
    llvm::StringRef code = R"(
#[expand.inc]
MACRO

#[main.cpp]
#define MACRO while(1)
void test() {
#include "expand.inc"
    br§eak;
}
)";
    add_files("main.cpp", code);
    ZASSERT(compile());

    auto points = nameless_points("main.cpp");
    ZASSERT(!points.empty());
    auto tree = SelectionTree::create_right(*unit, {points[0], points[0]});
    ZASSERT(unit->diagnostics().empty());

    auto ancestor = tree.common_ancestor();
    ZASSERT(ancestor);
    ZASSERT(ancestor->kind() == llvm::StringRef("BreakStmt"));
    ZASSERT(ancestor->parent->kind() == llvm::StringRef("WhileStmt"));
}

ZEST_CASE(IncludedFile) {
    llvm::StringRef code = R"(
#[expand.inc]
while (0)

#[main.cpp]
void test() {
#include "exp§and.inc"
  break;
}
)";
    add_files("main.cpp", code);
    ZASSERT(compile());

    auto points = nameless_points("main.cpp");
    ZASSERT(!points.empty());
    auto point = points[0];
    auto tree = SelectionTree::create_right(*unit, {point, point});
    ZASSERT(unit->diagnostics().empty());

    ZASSERT(tree.common_ancestor() == nullptr);
}

ZEST_CASE(Implicit) {
    llvm::StringRef code = R"cpp(
    struct S { S(const char*); };
    int f(S);
    int x = f("§");
  )cpp";

    select_right(code, [&](SelectionTree& tree) {
        auto ancestor = tree.common_ancestor();
        ZASSERT(ancestor->kind() == llvm::StringRef("StringLiteral"));
        ZASSERT(ancestor->parent->kind() == llvm::StringRef("ImplicitCastExpr"));
        ZASSERT(ancestor->parent->parent->kind() == llvm::StringRef("CXXConstructExpr"));

        auto implicit = ancestor->parent->parent->parent;
        ZASSERT(implicit->kind() == llvm::StringRef("ImplicitCastExpr"));
        ZASSERT(implicit->parent->kind() == llvm::StringRef("CallExpr"));
        ZASSERT(ancestor == &implicit->ignore_implicit());
        ZASSERT(&ancestor->outer_implicit() == implicit);
    });
}

ZEST_CASE(DeclContextIsLexical) {
    llvm::StringRef code = R"cpp(
namespace a {
    void f§oo();
}

void a::foo() { }
  )cpp";

    select_right(code, [&](SelectionTree& tree) {
        auto ancestor = tree.common_ancestor();
        ZASSERT(!ancestor->decl_context().isTranslationUnit());
    });

    code = R"cpp(
namespace a {
    void foo();
}

void a::f§oo() { }
  )cpp";

    select_right(code, [&](SelectionTree& tree) {
        auto ancestor = tree.common_ancestor();
        ZASSERT(ancestor->decl_context().isTranslationUnit());
    });
}

ZEST_CASE(DeclContextLambda) {
    llvm::StringRef code = R"cpp(
void foo();
auto lambda = [] {
  return §foo();
};
  )cpp";

    select_right(code, [&](SelectionTree& tree) {
        auto ancestor = tree.common_ancestor();
        ZASSERT(ancestor->decl_context().isFunctionOrMethod());
    });
}

ZEST_CASE(UsingConcepts) {
    llvm::StringRef code = R"cpp(
namespace ns {
template <typename T>
concept Foo = true;
}

using ns::Foo;

template <Fo§o... T, Fo§o auto U>
auto Func(Fo§o auto V) -> Fo§o decltype(auto) {
  Fo§o auto W = V;
  return W;
}
  )cpp";

    add_focus("main.cpp", code);
    ZASSERT(compile());

    for(auto point: nameless_points()) {
        auto tree = SelectionTree::create_right(*unit, {point, point});
        auto* ancestor = tree.common_ancestor();
        ZASSERT(ancestor);
        auto* C = ancestor->get<clang::ConceptReference>();
        ZASSERT(C);
        ZASSERT(C->getFoundDecl());
        ZASSERT(C->getFoundDecl()->getKind() == clang::Decl::UsingShadow);
    }
}

};  // ZEST_SUITE(SelectionTree)

}  // namespace clice::testing
