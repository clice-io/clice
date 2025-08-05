#include "Test/Tester.h"
#include "AST/Selection.h"
#include "clang/Lex/Lexer.h"

namespace clice::testing {

namespace {

using namespace clang;

static unsigned getTokenLengthAtLoc(SourceLocation Loc,
                                    const SourceManager& SM,
                                    const LangOptions& LangOpts) {
    clang::Token TheTok;
    if(clang::Lexer::getRawToken(Loc, TheTok, SM, LangOpts))
        return 0;
    // FIXME: Here we check whether the token at the location is a greatergreater
    // (>>) token and consider it as a single greater (>). This is to get it
    // working for templates but it isn't correct for the right shift operator. We
    // can avoid this by using half open char ranges in getFileRange() but getting
    // token ending is not well supported in macroIDs.
    if(TheTok.is(tok::greatergreater))
        return 1;

    return TheTok.getLength();
}

// Returns location of the starting of the token at a given EndLoc
static SourceLocation getLocForTokenBegin(SourceLocation EndLoc,
                                          const SourceManager& SM,
                                          const LangOptions& LangOpts) {
    return EndLoc.getLocWithOffset(-(signed)getTokenLengthAtLoc(EndLoc, SM, LangOpts));
}

// Returns location of the last character of the token at a given loc
static SourceLocation getLocForTokenEnd(SourceLocation BeginLoc,
                                        const SourceManager& SM,
                                        const LangOptions& LangOpts) {
    unsigned Len = getTokenLengthAtLoc(BeginLoc, SM, LangOpts);
    return BeginLoc.getLocWithOffset(Len ? Len - 1 : 0);
}

// Converts a char source range to a token range.
static SourceRange toTokenRange(CharSourceRange Range,
                                const SourceManager& SM,
                                const LangOptions& LangOpts) {
    if(!Range.isTokenRange())
        Range.setEnd(getLocForTokenBegin(Range.getEnd(), SM, LangOpts));
    return Range.getAsRange();
}

// Returns the union of two token ranges.
// To find the maximum of the Ends of the ranges, we compare the location of the
// last character of the token.
static SourceRange unionTokenRange(SourceRange R1,
                                   SourceRange R2,
                                   const SourceManager& SM,
                                   const LangOptions& LangOpts) {
    SourceLocation Begin =
        SM.isBeforeInTranslationUnit(R1.getBegin(), R2.getBegin()) ? R1.getBegin() : R2.getBegin();
    SourceLocation End = SM.isBeforeInTranslationUnit(getLocForTokenEnd(R1.getEnd(), SM, LangOpts),
                                                      getLocForTokenEnd(R2.getEnd(), SM, LangOpts))
                             ? R2.getEnd()
                             : R1.getEnd();
    return SourceRange(Begin, End);
}

bool isValidFileRange(const SourceManager& Mgr, SourceRange R) {
    if(!R.getBegin().isValid() || !R.getEnd().isValid())
        return false;

    FileID BeginFID;
    size_t BeginOffset = 0;
    std::tie(BeginFID, BeginOffset) = Mgr.getDecomposedLoc(R.getBegin());

    FileID EndFID;
    size_t EndOffset = 0;
    std::tie(EndFID, EndOffset) = Mgr.getDecomposedLoc(R.getEnd());

    return BeginFID.isValid() && BeginFID == EndFID && BeginOffset <= EndOffset;
}

SourceLocation includeHashLoc(FileID IncludedFile, const SourceManager& SM) {
    assert(SM.getLocForEndOfFile(IncludedFile).isFileID());
    FileID IncludingFile;
    unsigned Offset;
    std::tie(IncludingFile, Offset) = SM.getDecomposedExpansionLoc(SM.getIncludeLoc(IncludedFile));
    bool Invalid = false;
    llvm::StringRef Buf = SM.getBufferData(IncludingFile, &Invalid);
    if(Invalid)
        return SourceLocation();
    // Now buf is "...\n#include <foo>\n..."
    // and Offset points here:   ^
    // Rewind to the preceding # on the line.
    assert(Offset < Buf.size());
    for(;; --Offset) {
        if(Buf[Offset] == '#')
            return SM.getComposedLoc(IncludingFile, Offset);
        if(Buf[Offset] == '\n' || Offset == 0)  // no hash, what's going on?
            return SourceLocation();
    }
}

// Given a range whose endpoints may be in different expansions or files,
// tries to find a range within a common file by following up the expansion and
// include location in each.
static SourceRange rangeInCommonFile(SourceRange R,
                                     const SourceManager& SM,
                                     const LangOptions& LangOpts) {
    // Fast path for most common cases.
    if(SM.isWrittenInSameFile(R.getBegin(), R.getEnd()))
        return R;
    // Record the stack of expansion locations for the beginning, keyed by FileID.
    llvm::DenseMap<FileID, SourceLocation> BeginExpansions;
    for(SourceLocation Begin = R.getBegin(); Begin.isValid();
        Begin = Begin.isFileID() ? includeHashLoc(SM.getFileID(Begin), SM)
                                 : SM.getImmediateExpansionRange(Begin).getBegin()) {
        BeginExpansions[SM.getFileID(Begin)] = Begin;
    }
    // Move up the stack of expansion locations for the end until we find the
    // location in BeginExpansions with that has the same file id.
    for(SourceLocation End = R.getEnd(); End.isValid();
        End = End.isFileID()
                  ? includeHashLoc(SM.getFileID(End), SM)
                  : toTokenRange(SM.getImmediateExpansionRange(End), SM, LangOpts).getEnd()) {
        auto It = BeginExpansions.find(SM.getFileID(End));
        if(It != BeginExpansions.end()) {
            if(SM.getFileOffset(It->second) > SM.getFileOffset(End))
                return SourceLocation();
            return {It->second, End};
        }
    }
    return SourceRange();
}

// Find an expansion range (not necessarily immediate) the ends of which are in
// the same file id.
static SourceRange getExpansionTokenRangeInSameFile(SourceLocation Loc,
                                                    const SourceManager& SM,
                                                    const LangOptions& LangOpts) {
    return rangeInCommonFile(toTokenRange(SM.getImmediateExpansionRange(Loc), SM, LangOpts),
                             SM,
                             LangOpts);
}

// Returns the file range for a given Location as a Token Range
// This is quite similar to getFileLoc in SourceManager as both use
// getImmediateExpansionRange and getImmediateSpellingLoc (for macro IDs).
// However:
// - We want to maintain the full range information as we move from one file to
//   the next. getFileLoc only uses the BeginLoc of getImmediateExpansionRange.
// - We want to split '>>' tokens as the lexer parses the '>>' in nested
//   template instantiations as a '>>' instead of two '>'s.
// There is also getExpansionRange but it simply calls
// getImmediateExpansionRange on the begin and ends separately which is wrong.
static SourceRange getTokenFileRange(SourceLocation Loc,
                                     const SourceManager& SM,
                                     const LangOptions& LangOpts) {
    SourceRange FileRange = Loc;
    while(!FileRange.getBegin().isFileID()) {
        if(SM.isMacroArgExpansion(FileRange.getBegin())) {
            FileRange = unionTokenRange(SM.getImmediateSpellingLoc(FileRange.getBegin()),
                                        SM.getImmediateSpellingLoc(FileRange.getEnd()),
                                        SM,
                                        LangOpts);
            assert(SM.isWrittenInSameFile(FileRange.getBegin(), FileRange.getEnd()));
        } else {
            SourceRange ExpansionRangeForBegin =
                getExpansionTokenRangeInSameFile(FileRange.getBegin(), SM, LangOpts);
            SourceRange ExpansionRangeForEnd =
                getExpansionTokenRangeInSameFile(FileRange.getEnd(), SM, LangOpts);
            if(ExpansionRangeForBegin.isInvalid() || ExpansionRangeForEnd.isInvalid())
                return SourceRange();
            assert(SM.isWrittenInSameFile(ExpansionRangeForBegin.getBegin(),
                                          ExpansionRangeForEnd.getBegin()) &&
                   "Both Expansion ranges should be in same file.");
            FileRange = unionTokenRange(ExpansionRangeForBegin, ExpansionRangeForEnd, SM, LangOpts);
        }
    }
    return FileRange;
}

std::optional<SourceRange> toHalfOpenFileRange(const SourceManager& SM,
                                               const LangOptions& LangOpts,
                                               SourceRange R) {
    SourceRange R1 = getTokenFileRange(R.getBegin(), SM, LangOpts);
    if(!isValidFileRange(SM, R1))
        return std::nullopt;

    SourceRange R2 = getTokenFileRange(R.getEnd(), SM, LangOpts);
    if(!isValidFileRange(SM, R2))
        return std::nullopt;

    SourceRange Result = rangeInCommonFile(unionTokenRange(R1, R2, SM, LangOpts), SM, LangOpts);
    unsigned TokLen = getTokenLengthAtLoc(Result.getEnd(), SM, LangOpts);
    // Convert from closed token range to half-open (char) range
    Result.setEnd(Result.getEnd().getLocWithOffset(TokLen));
    if(!isValidFileRange(SM, Result))
        return std::nullopt;

    return Result;
}

}  // namespace

void dump_diagnostics() {}

void select_right(llvm::StringRef code, auto&& callback, LocationChain chain = LocationChain()) {
    Tester tester;
    tester.add_main("main.cpp", code);
    ASSERT_TRUE(tester.compile(), chain);
    /// ASSERT_TRUE(tester.unit->diagnostics().empty(), chain);

    auto points = tester.nameless_points();
    ASSERT_TRUE(points.size() >= 1, chain);

    LocalSourceRange selected_range;
    selected_range.begin = points[0];
    selected_range.end = points.size() == 2 ? points[1] : points[0];
    auto tree = SelectionTree::create_right(*tester.unit, selected_range);
    callback(tester, tree);
}

void EXPECT_SELECT(llvm::StringRef code, const char* kind, LocationChain chain = LocationChain()) {
    select_right(
        code,
        [&](Tester& tester, SelectionTree& tree) {
            auto node = tree.common_ancestor();
            if(!kind) {
                ASSERT_FALSE(node, chain);
            } else {
                ASSERT_TRUE(node, chain);
                auto range2 = toHalfOpenFileRange(tester.unit->context().getSourceManager(),
                                                  tester.unit->lang_options(),
                                                  node->source_range());
                LocalSourceRange range = {
                    tester.unit->file_offset(range2->getBegin()),
                    tester.unit->file_offset(range2->getEnd()),
                };

                /// llvm::outs() << tree << "\n";
                /// tree.print(llvm::outs(), *node, 2);

                ASSERT_EQ(node->kind(), llvm::StringRef(kind), chain);
                ASSERT_EQ(range, tester.range(), chain);
            }
        },
        chain);
}

TEST(Selection, Expressions) {
    EXPECT_SELECT(R"(
          struct AAA { struct BBB { static int ccc(); };};
          int x = @[AAA::BBB::c$c$c]();
          )",
                  "DeclRefExpr");

    EXPECT_SELECT(R"(
          struct AAA { struct BBB { static int ccc(); };};
          int x = @[AAA::BBB::ccc($)];
          )",
                  "CallExpr");

    EXPECT_SELECT(R"(
          struct S {
            int foo() const;
            int bar() { return @[f$oo](); }
          };
          )",
                  "MemberExpr");

    EXPECT_SELECT(R"(void foo() { @[$foo](); })", "DeclRefExpr");
    EXPECT_SELECT(R"(void foo() { @[f$oo](); })", "DeclRefExpr");
    EXPECT_SELECT(R"(void foo() { @[fo$o](); })", "DeclRefExpr");

    EXPECT_SELECT(R"(void foo() { @[foo$] (); })", "DeclRefExpr");

    EXPECT_SELECT(R"(void foo() { @[foo$()]; })", "CallExpr");
    EXPECT_SELECT(R"(void foo() { @[foo$()]; /*comment*/$})", "CallExpr");
    EXPECT_SELECT(R"(const int x = 1, y = 2; int array[ @[$x] ][10][y];)", "DeclRefExpr");
    EXPECT_SELECT(R"(const int x = 1, y = 2; int array[x][10][ @[$y] ];)", "DeclRefExpr");
    EXPECT_SELECT(R"(void func(int x) { int v_array[ @[$x] ][10]; })", "DeclRefExpr");
    EXPECT_SELECT(R"(
        int a;
        decltype(@[$a] + a) b;
        )",
                  "DeclRefExpr");

    EXPECT_SELECT(R"(
        void func() { @[__$func__]; }
        )",
                  "PredefinedExpr");
}

TEST(Selection, Literals) {
    EXPECT_SELECT(R"(
          auto lambda = [](const char*){ return 0; };
          int x = lambda(@["y$"]);
          )",
                  "StringLiteral");

    EXPECT_SELECT(R"(int x = @[42]$;)", "IntegerLiteral");
    EXPECT_SELECT(R"(const int x = 1, y = 2; int array[x][ @[$10] ][y];)", "IntegerLiteral");

    EXPECT_SELECT(R"(
          struct Foo{};
          Foo operator""_ud(unsigned long long);
          Foo x = @[$12_ud];
          )",
                  "UserDefinedLiteral");
}

TEST(Selection, ControlFlow) {
    EXPECT_SELECT(R"(
          void foo() { @[if (1$11) { return; } else {$ }]} }
          )",
                  "IfStmt");

    EXPECT_SELECT(R"(int bar; void foo() @[{ foo (); }]$)", "CompoundStmt");

    /// FIXME:
    /// EXPECT_SELECT(R"(
    ///     /*error-ok*/
    ///     void func() @[{^])",
    ///               "CompoundStmt");

    EXPECT_SELECT(R"(
          struct Str {
            const char *begin();
            const char *end();
          };
          Str makeStr(const char*);
          void loop() {
            for (const char C : @[mak$eStr("foo"$)])
              ;
          }
          )",
                  "CallExpr");
}

TEST(Selection, Declarations) {
    /// FIXME: how to handle this?
    /// EXPECT_SELECT(R"(
    ///      #define TARGET void foo()
    ///      @[TAR$GET{ return; }]
    ///      )",
    ///              "FunctionDecl");

    EXPECT_SELECT(R"(@[$void foo$()];)", "FunctionDecl");
    EXPECT_SELECT(R"(@[void $foo()];)", "FunctionDecl");

    EXPECT_SELECT(R"(
          struct S { S(const char*); };
          @[S s $= "foo"];
          )",
                  "VarDecl");

    EXPECT_SELECT(R"(
          struct S { S(const char*); };
          @[S $s = "foo"];
          )",
                  "VarDecl");

    EXPECT_SELECT(R"(
          @[void (*$S)(int) = nullptr];
          )",
                  "VarDecl");

    EXPECT_SELECT(R"(@[int $a], b;)", "VarDecl");
    EXPECT_SELECT(R"(@[int a, $b];)", "VarDecl");
    EXPECT_SELECT(R"(@[struct {int x;} $y];)", "VarDecl");
    EXPECT_SELECT(R"(struct foo { @[int has$h<:32:>]; };)", "FieldDecl");
    EXPECT_SELECT(R"(struct {@[int $x];} y;)", "FieldDecl");

    EXPECT_SELECT(R"(
        void test(int bar) {
          auto l = [ $@[foo = bar] ] { };
        })",
                  "VarDecl");
}

TEST(Selection, Types) {
    EXPECT_SELECT(R"(
          struct AAA { struct BBB { static int ccc(); };};
          int x = AAA::@[B$B$B]::ccc();
          )",
                  "RecordTypeLoc");
    EXPECT_SELECT(R"(
          struct AAA { struct BBB { static int ccc(); };};
          int x = AAA::@[B$BB$]::ccc();
          )",
                  "RecordTypeLoc");
    EXPECT_SELECT(R"(
          struct Foo {};
          struct Bar : private @[Fo$o] {};
          )",
                  "RecordTypeLoc");
    EXPECT_SELECT(R"(
          struct Foo {};
          struct Bar : @[Fo$o] {};
          )",
                  "RecordTypeLoc");
    EXPECT_SELECT(R"(@[$void] (*S)(int) = nullptr;)", "BuiltinTypeLoc");
    /// EXPECT_SELECT(R"(@[void (*S)$(int)] = nullptr;)", "FunctionProtoTypeLoc");
    EXPECT_SELECT(R"(@[void ($*S)(int)] = nullptr;)", "PointerTypeLoc");
    /// EXPECT_SELECT(R"(@[void $(*S)(int)] = nullptr;)", "ParenTypeLoc");
    EXPECT_SELECT(R"(@[$void] foo();)", "BuiltinTypeLoc");
    EXPECT_SELECT(R"(@[void foo$()];)", "FunctionProtoTypeLoc");
    EXPECT_SELECT(R"(const int x = 1, y = 2; @[i$nt] array[x][10][y];)", "BuiltinTypeLoc");
    EXPECT_SELECT(R"(int (*getFunc(@[do$uble]))(int);)", "BuiltinTypeLoc");
    EXPECT_SELECT(R"(class X{}; @[int X::$*]y[10];)", "MemberPointerTypeLoc");
    EXPECT_SELECT(R"(const @[a$uto] x = 42;)", "AutoTypeLoc");
    /// EXPECT_SELECT(R"(@[decltype$(1)] b;)", "DecltypeTypeLoc");
    EXPECT_SELECT(R"(@[de$cltype(a$uto)] a = 1;)", "AutoTypeLoc");
    EXPECT_SELECT(R"(
        typedef int Foo;
        enum Bar : @[Fo$o] {};
      )",
                  "TypedefTypeLoc");
    EXPECT_SELECT(R"(
        typedef int Foo;
        enum Bar : @[Fo$o];
      )",
                  "TypedefTypeLoc");
}

TEST(Selection, CXXFeatures) {
    EXPECT_SELECT(R"(
          template <typename T>
          int x = @[T::$U::]ccc();
          )",
                  "NestedNameSpecifierLoc");
    EXPECT_SELECT(R"(
          struct Foo {};
          struct Bar : @[v$ir$tual private Foo] {};
          )",
                  "CXXBaseSpecifier");
    EXPECT_SELECT(R"(
          struct X { X(int); };
          class Y {
            X x;
            Y() : @[$x(4)] {}
          };
          )",
                  "CXXCtorInitializer");
    EXPECT_SELECT(R"(@[st$ruct {int x;}] y;)", "CXXRecordDecl");
    EXPECT_SELECT(R"(struct foo { @[op$erator int()]; };)", "CXXConversionDecl");
    EXPECT_SELECT(R"(struct foo { @[$~foo()]; };)", "CXXDestructorDecl");
    EXPECT_SELECT(R"(struct foo { @[~$foo()]; };)", "CXXDestructorDecl");
    EXPECT_SELECT(R"(struct foo { @[fo$o(){}] };)", "CXXConstructorDecl");
    EXPECT_SELECT(R"(
        struct S1 { void f(); };
        struct S2 { S1 * operator->(); };
        void test(S2 s2) {
          s2@[-$>]f();
        }
      )",
                  "DeclRefExpr");  // Test for overloaded operator->
}

TEST(Selection, UsingEnum) {
    EXPECT_SELECT(R"(
        namespace ns { enum class A {}; };
        using enum ns::@[$A];
        )",
                  "EnumTypeLoc");
    EXPECT_SELECT(R"(
        namespace ns { enum class A {}; using B = A; };
        using enum ns::@[$B];
        )",
                  "TypedefTypeLoc");
    EXPECT_SELECT(R"(
        namespace ns { enum class A {}; };
        using enum @[$ns::]A;
        )",
                  "NestedNameSpecifierLoc");
    EXPECT_SELECT(R"(
        namespace ns { enum class A {}; };
        @[using $enum ns::A];
        )",
                  "UsingEnumDecl");
    EXPECT_SELECT(R"(
        namespace ns { enum class A {}; };
        @[$using enum ns::A];
        )",
                  "UsingEnumDecl");
}

TEST(Selection, Templates) {
    EXPECT_SELECT(R"(template<typename ...T> void foo(@[T*$...]x);)", "PackExpansionTypeLoc");
    EXPECT_SELECT(R"(template<typename ...T> void foo(@[$T]*...x);)", "TemplateTypeParmTypeLoc");
    EXPECT_SELECT(R"(template <typename T> void foo() { @[$T] t; })", "TemplateTypeParmTypeLoc");
    EXPECT_SELECT(R"(
          template <class T> struct Foo {};
          template <@[template<class> class /*cursor here*/$U]>
            struct Foo<U<int>*> {};
          )",
                  "TemplateTemplateParmDecl");
    EXPECT_SELECT(R"(template <class T> struct foo { ~foo<@[$T]>(){} };)",
                  "TemplateTypeParmTypeLoc");
    EXPECT_SELECT(R"(
        template <typename> class Vector {};
        template <template <typename> class Container> class A {};
        A<@[V$ector]> a;
      )",
                  "TemplateArgumentLoc");
}

TEST(Selection, Concepts) {
    EXPECT_SELECT(R"(
        template <class> concept C = true;
        auto x = @[$C<int>];
      )",
                  "ConceptReference");
    EXPECT_SELECT(R"(
        template <class> concept C = true;
        @[$C] auto x = 0;
      )",
                  "ConceptReference");
    EXPECT_SELECT(R"(
        template <class> concept C = true;
        void foo(@[$C] auto x) {}
      )",
                  "ConceptReference");
    EXPECT_SELECT(R"(
        template <class> concept C = true;
        template <@[$C] x> int i = 0;
      )",
                  "ConceptReference");
    EXPECT_SELECT(R"(
        namespace ns { template <class> concept C = true; }
        auto x = @[ns::$C<int>];
      )",
                  "ConceptReference");
    EXPECT_SELECT(R"(
        template <typename T, typename K>
        concept D = true;
        template <typename T> void g(D<@[$T]> auto abc) {}
      )",
                  "TemplateTypeParmTypeLoc");
}

TEST(Selection, Attributes) {
    EXPECT_SELECT(R"(
        void f(int * __attribute__((@[no$nnull])) );
      )",
                  "NonNullAttr");
    EXPECT_SELECT(R"(
        // Digraph syntax for attributes to avoid accidental annotations.
        class [[gsl::Owner( @[in$t] )]] X{};
      )",
                  "BuiltinTypeLoc");
}

TEST(Selection, Macros) {
    EXPECT_SELECT(R"(
            int x(int);
            #define M(foo) x(foo)
            int a = 42;
            int b = M(@[$a]);
            )",
                  "DeclRefExpr");
    EXPECT_SELECT(R"(
            void foo();
            #define CALL_FUNCTION(X) X()
            void bar() { CALL_FUNCTION(@[f$o$o]); }
            )",
                  "DeclRefExpr");
    EXPECT_SELECT(R"(
            void foo();
            #define CALL_FUNCTION(X) X()
            void bar() { @[CALL_FUNC$TION(fo$o)]; }
            )",
                  "CallExpr");
    EXPECT_SELECT(R"(
            void foo();
            #define CALL_FUNCTION(X) X()
            void bar() { @[C$ALL_FUNC$TION(foo)]; }
            )",
                  "CallExpr");
}

TEST(Selection, NullOrInvalid) {
    EXPECT_SELECT(R"(
              void foo();
              #$define CALL_FUNCTION(X) X($)
              void bar() { CALL_FUNCTION(foo); }
              )",
                  nullptr);
    EXPECT_SELECT(R"(
              void foo();
              #define CALL_FUNCTION(X) X()
              void bar() { CALL_FUNCTION(foo$)$; }
              )",
                  nullptr);
    EXPECT_SELECT(R"(
              namespace ns {
              #if 0
              void fo$o() {}
              #endif
              }
              )",
                  nullptr);
    EXPECT_SELECT(R"(co$nst auto x = 42;)", nullptr);
    EXPECT_SELECT(R"($)", nullptr);
    EXPECT_SELECT(R"(int x = 42;$)", nullptr);
    EXPECT_SELECT(R"($int x; int y;$)", nullptr);
    EXPECT_SELECT(R"(void foo() { @[foo$] (); })",
                  "DeclRefExpr");  // Technically valid, but tricky
}

TEST(Selection, InjectedClassName) {
    llvm::StringRef code = "struct $X { int x; };";
    select_right(code, [](Tester& tester, SelectionTree& tree) {
        auto ancestor = tree.common_ancestor();
        ASSERT_EQ(ancestor->kind(), "CXXRecordDecl");
        auto* D = dyn_cast<clang::CXXRecordDecl>(ancestor->get<clang::Decl>());
        ASSERT_FALSE(D->isInjectedClassName());
    });
}

TEST(Selection, Metrics) {
    llvm::StringRef code = R"cpp(
    // error-ok: testing behavior on recovery expression
    int foo();
    int foo(int, int);
    int x = fo^o(42);
  )cpp";

    /// FIXME:
}

TEST(Selection, Selected) {
    /// FIXME:
}

TEST(Selection, PathologicalPreprocessor) {
    /// FIXME:
}

TEST(Selection, IncludedFile) {
    /// FIXME:
    Tester tester;
    llvm::StringRef code = R"(
#[expand.inc]
while (0)

#[main.cpp]
void test() {
#include "exp$and.inc"
  break;
}
)";
    tester.add_files("main.cpp", code);
    ASSERT_TRUE(tester.compile());

    auto point = tester.nameless_points("main.cpp")[0];
    auto tree = SelectionTree::create_right(*tester.unit, {point, point});
    ASSERT_TRUE(tester.unit->diagnostics().empty());

    ASSERT_EQ(tree.common_ancestor(), nullptr);
}

TEST(Selection, Implicit) {
    llvm::StringRef code = R"cpp(
    struct S { S(const char*); };
    int f(S);
    int x = f("$");
  )cpp";

    select_right(code, [](Tester& tester, SelectionTree& tree) {
        auto ancestor = tree.common_ancestor();
        EXPECT_EQ(ancestor->kind(), "StringLiteral");
        EXPECT_EQ(ancestor->parent->kind(), "ImplicitCastExpr");
        EXPECT_EQ(ancestor->parent->parent->kind(), "CXXConstructExpr");

        auto implicit = ancestor->parent->parent->parent;
        EXPECT_EQ(implicit->kind(), "ImplicitCastExpr");
        EXPECT_EQ(implicit->parent->kind(), "CallExpr");
        EXPECT_EQ(ancestor, &implicit->ignore_implicit());
        EXPECT_EQ(&ancestor->outer_implicit(), implicit);
    });
}

TEST(Selection, DeclContextIsLexical) {
    llvm::StringRef code = R"cpp(
namespace a { 
    void f$oo(); 
} 

void a::foo() { }
  )cpp";

    select_right(code, [](Tester& tester, SelectionTree& tree) {
        auto ancestor = tree.common_ancestor();
        EXPECT_FALSE(ancestor->decl_context().isTranslationUnit());
    });

    code = R"cpp(
namespace a { 
    void foo(); 
} 

void a::f$oo() { }
  )cpp";

    select_right(code, [](Tester& tester, SelectionTree& tree) {
        auto ancestor = tree.common_ancestor();
        EXPECT_TRUE(ancestor->decl_context().isTranslationUnit());
    });
}

TEST(Selection, DeclContextLambda) {
    llvm::StringRef code = R"cpp(
void foo();
auto lambda = [] {
  return $foo();
};
  )cpp";

    select_right(code, [](Tester& tester, SelectionTree& tree) {
        auto ancestor = tree.common_ancestor();
        EXPECT_TRUE(ancestor->decl_context().isFunctionOrMethod());
    });
}

TEST(Selection, UsingConcepts) {
    llvm::StringRef code = R"cpp(
namespace ns {
template <typename T>
concept Foo = true;
}

using ns::Foo;

template <Fo$o... T, Fo$o auto U>
auto Func(Fo$o auto V) -> Fo$o decltype(auto) {
  Fo$o auto W = V;
  return W;
}
  )cpp";

    Tester tester;
    tester.add_main("main.cpp", code);
    ASSERT_TRUE(tester.compile());

    auto points = tester.nameless_points();

    for(auto point: tester.nameless_points()) {
        auto tree = SelectionTree::create_right(*tester.unit, {point, point});
        auto* C = tree.common_ancestor()->get<clang::ConceptReference>();
        EXPECT_TRUE(C && C->getFoundDecl() &&
                    C->getFoundDecl()->getKind() == clang::Decl::UsingShadow);
    }
}

}  // namespace clice::testing
