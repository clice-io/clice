// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "test/tester.h"
#include "semantic/expr_hash.h"
#include "support/logging.h"

#include "llvm/ADT/DenseSet.h"
#include "clang/AST/DeclTemplate.h"
#include "clang/AST/ExprConcepts.h"
#include "clang/AST/RecursiveASTVisitor.h"

namespace clice::testing {
namespace {

using namespace clang;

struct PointerLeaves : ExprHashLeaves {
    llvm::FoldingSetNodeID& id;
    const ASTContext& context;

    PointerLeaves(llvm::FoldingSetNodeID& id, const ASTContext& context) :
        id(id), context(context) {}

    void add_decl(const Decl* decl) override {
        id.AddInteger(decl ? decl->getKind() : 0);

        if(decl) {
            if(const NonTypeTemplateParmDecl* nttp = dyn_cast<NonTypeTemplateParmDecl>(decl)) {
                id.AddInteger(nttp->getDepth());
                id.AddInteger(nttp->getIndex());
                id.AddBoolean(nttp->isParameterPack());

                add_type(context.getUnconstrainedType(nttp->getType()));
                return;
            }

            if(const ParmVarDecl* param = dyn_cast<ParmVarDecl>(decl)) {
                add_type(param->getType());
                id.AddInteger(param->getFunctionScopeDepth());
                id.AddInteger(param->getFunctionScopeIndex());
                return;
            }

            if(const TemplateTypeParmDecl* template_param = dyn_cast<TemplateTypeParmDecl>(decl)) {
                id.AddInteger(template_param->getDepth());
                id.AddInteger(template_param->getIndex());
                id.AddBoolean(template_param->isParameterPack());
                return;
            }

            if(const TemplateTemplateParmDecl* template_param =
                   dyn_cast<TemplateTemplateParmDecl>(decl)) {
                id.AddInteger(template_param->getDepth());
                id.AddInteger(template_param->getIndex());
                id.AddBoolean(template_param->isParameterPack());
                return;
            }
        }

        id.AddPointer(decl ? decl->getCanonicalDecl() : nullptr);
    }

    void add_type(QualType type) override {
        if(!type.isNull())
            type = context.getCanonicalType(type);

        id.AddPointer(type.getAsOpaquePtr());
    }

    void add_name(DeclarationName name, bool) override {
        id.AddPointer(name.getAsOpaquePtr());
    }

    void add_identifier(const IdentifierInfo* identifier) override {
        id.AddPointer(identifier);
    }

    void add_nested_name_specifier(NestedNameSpecifier specifier) override {
        specifier = specifier.getCanonical();
        specifier.Profile(id);
    }

    void add_template_name(TemplateName name) override {
        name = context.getCanonicalTemplateName(name);

        name.Profile(id);
    }

    void add_structural_value(QualType type, const APValue& value) override {
        add_type(type);
        value.Profile(id);
    }
};

struct Fidelity {
    ASTContext& context;
    llvm::DenseSet<const Stmt*> checked;
    llvm::DenseSet<unsigned> kinds;

    llvm::FoldingSetNodeID profile(const Stmt* stmt) {
        llvm::FoldingSetNodeID id;
        PointerLeaves leaves(id, context);
        ExprHasher(id, leaves).Visit(stmt);
        return id;
    }

    llvm::FoldingSetNodeID upstream(const Stmt* stmt) {
        llvm::FoldingSetNodeID id;
        stmt->Profile(id, context, /*Canonical=*/true, /*ProfileLambdaExpr=*/false);
        return id;
    }

    void check(const Stmt* stmt) {
        if(!stmt || !checked.insert(stmt).second) {
            return;
        }
        kinds.insert(stmt->getStmtClass());
        auto equal = profile(stmt) == upstream(stmt);
        if(!equal) {
            LOG_ERROR("Profile mismatch: {} at {}",
                      stmt->getStmtClassName(),
                      stmt->getBeginLoc().printToString(context.getSourceManager()));
        }
        EXPECT_TRUE(equal);
    }

    void compare(const Expr* lhs, const Expr* rhs, bool expected) {
        check(lhs);
        check(rhs);
        EXPECT_EQ(profile(lhs) == profile(rhs), expected);
        EXPECT_EQ(upstream(lhs) == upstream(rhs), expected);
    }
};

struct ConstraintSweep : RecursiveASTVisitor<ConstraintSweep> {
    Fidelity& fidelity;

    explicit ConstraintSweep(Fidelity& fidelity) : fidelity(fidelity) {}

    bool shouldVisitTemplateInstantiations() const {
        return false;
    }

    void template_parameters(const TemplateParameterList* parameters) {
        fidelity.check(parameters->getRequiresClause());
        for(auto* parameter: *parameters) {
            if(auto* type = dyn_cast<TemplateTypeParmDecl>(parameter)) {
                if(auto* constraint = type->getTypeConstraint()) {
                    fidelity.check(constraint->getImmediatelyDeclaredConstraint());
                }
            } else if(auto* value = dyn_cast<NonTypeTemplateParmDecl>(parameter)) {
                fidelity.check(value->getPlaceholderTypeConstraint());
            } else {
                template_parameters(
                    cast<TemplateTemplateParmDecl>(parameter)->getTemplateParameters());
            }
        }
    }

    bool VisitTemplateDecl(TemplateDecl* decl) {
        template_parameters(decl->getTemplateParameters());
        return true;
    }

    bool VisitDeclaratorDecl(DeclaratorDecl* decl) {
        for(unsigned i = 0; i < decl->getNumTemplateParameterLists(); i += 1) {
            template_parameters(decl->getTemplateParameterList(i));
        }
        return true;
    }

    bool VisitTagDecl(TagDecl* decl) {
        for(unsigned i = 0; i < decl->getNumTemplateParameterLists(); i += 1) {
            template_parameters(decl->getTemplateParameterList(i));
        }
        return true;
    }

    bool VisitFunctionDecl(FunctionDecl* decl) {
        fidelity.check(decl->getTrailingRequiresClause().ConstraintExpr);
        return true;
    }

    bool VisitDecltypeType(DecltypeType* type) {
        if(type->isDependentType()) {
            fidelity.check(type->getUnderlyingExpr());
        }
        return true;
    }

    bool VisitConceptDecl(ConceptDecl* decl) {
        fidelity.check(decl->getConstraintExpr());
        return true;
    }

    void template_argument(const TemplateArgument& argument) {
        if(argument.getKind() == TemplateArgument::Expression) {
            fidelity.check(argument.getAsExpr());
        } else if(argument.getKind() == TemplateArgument::Pack) {
            for(const auto& element: argument.pack_elements()) {
                template_argument(element);
            }
        }
    }

    bool VisitClassTemplatePartialSpecializationDecl(ClassTemplatePartialSpecializationDecl* decl) {
        template_parameters(decl->getTemplateParameters());
        for(const auto& argument: decl->getTemplateArgs().asArray()) {
            template_argument(argument);
        }
        return true;
    }
};

struct ProfileTree : RecursiveASTVisitor<ProfileTree> {
    Fidelity& fidelity;

    explicit ProfileTree(Fidelity& fidelity) : fidelity(fidelity) {}

    bool shouldVisitImplicitCode() const {
        return true;
    }

    bool shouldVisitTemplateInstantiations() const {
        return true;
    }

    bool VisitStmt(Stmt* stmt) {
        fidelity.check(stmt);
        return true;
    }
};

struct MarkedExpressions : RecursiveASTVisitor<MarkedExpressions> {
    llvm::StringMap<const Expr*> expressions;

    bool VisitTypedefNameDecl(TypedefNameDecl* decl) {
        if(decl->getName().starts_with("mark_")) {
            auto* type = cast<DecltypeType>(decl->getUnderlyingType());
            EXPECT_TRUE(expressions.try_emplace(decl->getName(), type->getUnderlyingExpr()).second);
        }
        return true;
    }

    bool VisitFunctionDecl(FunctionDecl* decl) {
        if(decl->getNameInfo().getAsString().starts_with("mark_")) {
            EXPECT_TRUE(
                expressions
                    .try_emplace(decl->getName(), decl->getTrailingRequiresClause().ConstraintExpr)
                    .second);
        }
        return true;
    }
};

bool has_errors(CompilationUnit& unit) {
    bool result = unit.context().getDiagnostics().hasErrorOccurred();
    if(result) {
        for(const auto& diagnostic: unit.diagnostics()) {
            LOG_ERROR("{}", diagnostic.message);
        }
    }
    return result;
}

Fidelity check_marked(Tester& tester) {
    MarkedExpressions markers;
    EXPECT_TRUE(markers.TraverseDecl(tester.unit->tu()));
    EXPECT_FALSE(markers.expressions.empty());
    Fidelity fidelity{tester.unit->context()};
    for(const auto& [name, expr]: markers.expressions) {
        EXPECT_TRUE(expr);
        ProfileTree tree(fidelity);
        EXPECT_TRUE(tree.TraverseStmt(const_cast<Expr*>(expr)));
    }
    return fidelity;
}

void expect_kinds(const Fidelity& fidelity, llvm::ArrayRef<Stmt::StmtClass> kinds) {
    for(auto kind: kinds) {
        if(!fidelity.kinds.contains(kind)) {
            LOG_ERROR("Missing handwritten statement class {}", static_cast<unsigned>(kind));
        }
        EXPECT_TRUE(fidelity.kinds.contains(kind));
    }
}

TEST_SUITE(expr_hash, Tester) {

TEST_CASE(StandardLibraryConstraints) {
    add_main("main.cpp", R"cpp(
#include <vector>
#include <string>
#include <ranges>
#include <algorithm>
#include <concepts>
#include <functional>
#include <memory>

template<std::integral auto N> requires (N > 0)
struct Bounded {
    template<std::regular T> auto get(T t) -> decltype(t + N) requires requires { t + N; };
};
template<class T, int N> struct Partial;
template<class T> struct Partial<T, sizeof(T)> {};
)cpp");
    ASSERT_TRUE(compile_driver("-std=c++20"));
    ASSERT_FALSE(has_errors(*unit));
    Fidelity fidelity{unit->context()};
    ConstraintSweep sweep(fidelity);
    ASSERT_TRUE(sweep.TraverseDecl(unit->tu()));
    LOG_INFO("expr_hash: checked {} distinct standard-library constraint expressions",
             fidelity.checked.size());
    EXPECT_GT(fidelity.checked.size(), 1500u);
};

TEST_CASE(HandwrittenExpressions) {
    add_main("main.cpp", R"cpp(
namespace std {
class type_info;
template<class T> class initializer_list {
    const T* begin;
    decltype(sizeof(0)) size;
};
struct source_location {
    struct __impl {
        const char* _M_file_name;
        const char* _M_function_name;
        unsigned _M_line;
        unsigned _M_column;
    };
};
}
struct Base { virtual ~Base(); int member; int method(int = 1); };
struct Derived : Base {};
struct Value { Value(int = 0); ~Value(); int member; };
struct Aggregate { int x; int y; };
struct Defaults { int field = 1; };
using mark_inherited = decltype([] {
    struct Child : Value { using Value::Value; };
    Child child(1);
});
using mark_default_init = decltype(Defaults{});
using mark_array_capture = decltype([] {
    int values[2] = {};
    auto closure = [values] {};
});
using mark_zero_init = decltype(new int());
using mark_cleanup = decltype([] { Value(1); });
struct Equal { bool operator==(const Equal&) const; };
int ordinary(int = 1);
void list(std::initializer_list<int>);
void variadic(...);
constexpr unsigned long long operator""_tag(unsigned long long value) { return value; }
template<class T> concept Any = true;
template<class T> concept Complete = requires { sizeof(T); };
template<class T, int N> concept Sized = sizeof(T) == N;
template<auto V> concept Valued = true;
template<template<class> class T> concept Templated = true;
template<class...> struct Types {};
template<template<class> class... Templates> concept TemplateList = true;
template<template<class> class... Templates> struct TemplatePacks {
    using mark_template_expansion = decltype(TemplateList<Templates...>);
    using mark_template_pack_size = decltype(sizeof...(Templates));
};
int global;
namespace names { int variable; }
using names::variable;

using mark_integer = decltype(42);
using mark_bit_integer = decltype(42__wb);
using mark_fixed = decltype(1.5hk);
using mark_unsigned = decltype(42u);
using mark_long = decltype(42LL);
using mark_character = decltype('x');
using mark_wide_character = decltype(L'x');
using mark_utf8_character = decltype(u8'x');
using mark_utf16_character = decltype(u'x');
using mark_utf32_character = decltype(U'x');
using mark_float = decltype(1.25f);
using mark_double = decltype(1.25);
using mark_long_double = decltype(1.25L);
using mark_imaginary = decltype(1.0i);
using mark_string = decltype("abc");
using mark_wide_string = decltype(L"abc");
using mark_utf8_string = decltype(u8"abc");
using mark_utf16_string = decltype(u"abc");
using mark_utf32_string = decltype(U"abc");
using mark_bool = decltype(true);
using mark_nullptr = decltype(nullptr);
using mark_gnu_null = decltype(__null);
using mark_user_literal = decltype(42_tag);
using mark_paren = decltype((42));
using mark_unary = decltype(-global);
using mark_binary = decltype(global + 1);
using mark_compound_assign = decltype(global += 1);
using mark_conditional = decltype(global ? 1 : 2);
using mark_binary_conditional = decltype(global ?: 2);
using mark_call = decltype(ordinary());
using mark_sizeof_type = decltype(sizeof(int));
using mark_sizeof_expr = decltype(sizeof(global));
using mark_alignof = decltype(alignof(int));
using mark_offsetof = decltype(__builtin_offsetof(Aggregate, y));
using mark_c_cast = decltype((long)global);
using mark_static_cast = decltype(static_cast<long>(global));
using mark_dynamic_cast = decltype(dynamic_cast<Derived*>((Base*)nullptr));
using mark_reinterpret_cast = decltype(reinterpret_cast<char*>(&global));
using mark_const_cast = decltype(const_cast<int*>((const int*)nullptr));
using mark_bit_cast = decltype(__builtin_bit_cast(float, global));
using mark_functional_cast = decltype(int(1.0));
using mark_scalar_init = decltype(int());
using mark_construct = decltype(Value(1));
using mark_temporary = decltype(Value{});
using mark_materialize = decltype(Value().member);
using mark_member_call = decltype(((Base*)nullptr)->method());
using mark_member = decltype(((Base*)nullptr)->member);
using mark_compound_literal = decltype((Aggregate){1, 2});
using mark_designated = decltype(Aggregate{.x = 1, .y = 2});
using mark_initializer_list = decltype(list({1, 2, 3}));
using mark_paren_init = decltype(Aggregate(1, 2));
using mark_new = decltype(new Value(1));
using mark_global_new = decltype(::new int[3]{});
using mark_delete = decltype(delete (Value*)nullptr);
using mark_array_delete = decltype(::delete[] (int*)nullptr);
using mark_throw = decltype(throw 1);
using mark_typeid_type = decltype(typeid(int));
using mark_typeid_expr = decltype(typeid(global));
using mark_trait = decltype(__is_same(int, long));
using mark_array_trait = decltype(__array_rank(int[3][4]));
using mark_array_extent = decltype(__array_extent(int[3][4], 1));
using mark_expression_trait = decltype(__is_lvalue_expr(global));
using mark_noexcept = decltype(noexcept(ordinary()));
using mark_concept = decltype(Complete<int>);
using mark_concept_integral = decltype(Sized<int, 4>);
using mark_concept_decl = decltype(Valued<&global>);
using mark_concept_nullptr = decltype(Valued<nullptr>);
using mark_concept_structural = decltype(Valued<1.5>);
using mark_concept_template = decltype(Templated<Types>);
using mark_requires = decltype(requires(int value) {
    typename Base;
    value + 1;
    { value + 1 } noexcept -> Any;
    requires Any<decltype(value)>;
});
using mark_choose = decltype(__builtin_choose_expr(true, 1, 2L));
using mark_generic = decltype(_Generic(global, int: 1, default: 2L));
using mark_atomic = decltype(__atomic_fetch_add(&global, 1, 5));
using mark_rewritten = decltype(Equal{} != Equal{});
using mark_paren_list = decltype([]<class T> {
    struct Construct { T member; Construct(): member(1, 2) {} };
});
using mark_lambda = decltype([value = 1](int param) { return value + param; });
using mark_source_location = decltype(__builtin_source_location());

using Vector = int __attribute__((ext_vector_type(4)));
Vector vector;
using mark_shuffle = decltype(__builtin_shufflevector(vector, vector, 0, 5));
using mark_convert_vector = decltype(__builtin_convertvector(vector, Vector));
using mark_vector_element = decltype(vector.x);
using Matrix = float __attribute__((matrix_type(2, 2)));
Matrix matrix;
using mark_matrix = decltype(matrix[0][1]);

template<class T, class... Pack> struct Dependent {
    T object;
    template<class U> int overloaded(U);
    int overloaded(int);
    using mark_dependent_ref = decltype(T::value);
    using mark_dependent_template_ref = decltype(T::template value<int>);
    using mark_dependent_member = decltype(((T*)nullptr)->member);
    using mark_dependent_member_template = decltype(((T*)nullptr)->template method<int>());
    using mark_unresolved_lookup = decltype(unknown(T{}));
    using mark_unresolved_member = decltype(((Dependent*)nullptr)->overloaded(T{}));
    using mark_unresolved_construct = decltype(T(1, 2));
    using mark_unresolved_list = decltype(T{1, 2});
    using mark_fold = decltype((sizeof(Pack) + ... + 0));
    using mark_pack = decltype(variadic(Pack{}...));
    using mark_sizeof_pack = decltype(sizeof...(Pack));
    using mark_dependent_offsetof = decltype(__builtin_offsetof(T, field[1]));
    using mark_dependent_requires = decltype(requires(T value) {
        typename T::type;
        value.member;
        { value.method() } noexcept -> Complete;
        requires Complete<typename T::type>;
    });
    using mark_operator = decltype(T{} + T{});
    using mark_subscript = decltype(((T*)nullptr)[1]);
    using mark_pseudo_destructor = decltype(((T*)nullptr)->~T());
    auto member() -> decltype(this->object);
    void body() {
        using mark_this = decltype(this);
        using mark_implicit_member = decltype(overloaded(T{}));
    }
};

void statements() {
    using mark_captured = decltype(({
#pragma clang __debug captured
        { int value = 1; }
        0;
    }));
    using mark_predefined = decltype(__func__);
    using mark_statement = decltype(({
        ;
        int value = 0;
        if(int condition = value) value = condition;
        else value = 1;
        switch(int condition = value) {
            case 0: value += 1; break;
            default: break;
        }
        while(int condition = value) { value -= condition; continue; }
        do { value += 1; } while(value < 1);
        for(int i = 0; i < 1; i += 1) value += i;
        int values[] = {1, 2};
        for(int element : values) value += element;
        goto label;
        label: value += 1;
        [[likely]] if(value) value += 1;
        void* address = &&label;
        goto *address;
        asm volatile("" : "+r"(value) : "r"(value) : "memory");
        try { throw value; } catch(int caught) { value = caught; }
        value;
    }));
    using mark_return = decltype([] { return 1; }());
    using mark_block = decltype(^{ return 1; });
    __builtin_va_list args;
    using mark_va_arg = decltype(__builtin_va_arg(args, int));
}
)cpp");
    prepare("-std=c++23");
    params.arguments.insert(
        params.arguments.end(),
        {"-fenable-matrix", "-fblocks", "-ffixed-point", "-fcxx-exceptions", "-fexceptions"});
    ASSERT_TRUE(try_compile());
    ASSERT_FALSE(has_errors(*unit));
    MarkedExpressions markers;
    ASSERT_TRUE(markers.TraverseDecl(unit->tu()));
    Fidelity fidelity{unit->context()};
    for(const auto& [name, expr]: markers.expressions) {
        ASSERT_TRUE(expr);
        ProfileTree tree(fidelity);
        ASSERT_TRUE(tree.TraverseStmt(const_cast<Expr*>(expr)));
    }
    LOG_INFO("expr_hash: checked {} handwritten markers, {} nodes, {} statement classes",
             markers.expressions.size(),
             fidelity.checked.size(),
             fidelity.kinds.size());
    EXPECT_GT(markers.expressions.size(), 100u);
    expect_kinds(fidelity,
                 {Stmt::AddrLabelExprClass,
                  Stmt::ArrayInitIndexExprClass,
                  Stmt::ArrayInitLoopExprClass,
                  Stmt::ArraySubscriptExprClass,
                  Stmt::ArrayTypeTraitExprClass,
                  Stmt::AtomicExprClass,
                  Stmt::AttributedStmtClass,
                  Stmt::BinaryConditionalOperatorClass,
                  Stmt::BinaryOperatorClass,
                  Stmt::BlockExprClass,
                  Stmt::BreakStmtClass,
                  Stmt::BuiltinBitCastExprClass,
                  Stmt::CStyleCastExprClass,
                  Stmt::CXXBindTemporaryExprClass,
                  Stmt::CXXBoolLiteralExprClass,
                  Stmt::CXXCatchStmtClass,
                  Stmt::CXXConstCastExprClass,
                  Stmt::CXXConstructExprClass,
                  Stmt::CXXDefaultArgExprClass,
                  Stmt::CXXDefaultInitExprClass,
                  Stmt::CXXDeleteExprClass,
                  Stmt::CXXDependentScopeMemberExprClass,
                  Stmt::CXXDynamicCastExprClass,
                  Stmt::CXXFoldExprClass,
                  Stmt::CXXForRangeStmtClass,
                  Stmt::CXXFunctionalCastExprClass,
                  Stmt::CXXInheritedCtorInitExprClass,
                  Stmt::CXXMemberCallExprClass,
                  Stmt::CXXNewExprClass,
                  Stmt::CXXNoexceptExprClass,
                  Stmt::CXXNullPtrLiteralExprClass,
                  Stmt::CXXOperatorCallExprClass,
                  Stmt::CXXParenListInitExprClass,
                  Stmt::CXXPseudoDestructorExprClass,
                  Stmt::CXXReinterpretCastExprClass,
                  Stmt::CXXRewrittenBinaryOperatorClass,
                  Stmt::CXXScalarValueInitExprClass,
                  Stmt::CXXStaticCastExprClass,
                  Stmt::CXXStdInitializerListExprClass,
                  Stmt::CXXTemporaryObjectExprClass,
                  Stmt::CXXThisExprClass,
                  Stmt::CXXThrowExprClass,
                  Stmt::CXXTryStmtClass,
                  Stmt::CXXTypeidExprClass,
                  Stmt::CXXUnresolvedConstructExprClass,
                  Stmt::CallExprClass,
                  Stmt::CaseStmtClass,
                  Stmt::CapturedStmtClass,
                  Stmt::CharacterLiteralClass,
                  Stmt::ChooseExprClass,
                  Stmt::CompoundAssignOperatorClass,
                  Stmt::CompoundLiteralExprClass,
                  Stmt::CompoundStmtClass,
                  Stmt::ConceptSpecializationExprClass,
                  Stmt::ConditionalOperatorClass,
                  Stmt::ConstantExprClass,
                  Stmt::ContinueStmtClass,
                  Stmt::ConvertVectorExprClass,
                  Stmt::DeclRefExprClass,
                  Stmt::DeclStmtClass,
                  Stmt::DefaultStmtClass,
                  Stmt::DependentScopeDeclRefExprClass,
                  Stmt::DesignatedInitExprClass,
                  Stmt::DoStmtClass,
                  Stmt::ExpressionTraitExprClass,
                  Stmt::ExtVectorElementExprClass,
                  Stmt::ExprWithCleanupsClass,
                  Stmt::FixedPointLiteralClass,
                  Stmt::FloatingLiteralClass,
                  Stmt::ForStmtClass,
                  Stmt::GCCAsmStmtClass,
                  Stmt::GNUNullExprClass,
                  Stmt::GenericSelectionExprClass,
                  Stmt::GotoStmtClass,
                  Stmt::IfStmtClass,
                  Stmt::ImaginaryLiteralClass,
                  Stmt::ImplicitCastExprClass,
                  Stmt::ImplicitValueInitExprClass,
                  Stmt::IndirectGotoStmtClass,
                  Stmt::InitListExprClass,
                  Stmt::IntegerLiteralClass,
                  Stmt::LabelStmtClass,
                  Stmt::LambdaExprClass,
                  Stmt::MaterializeTemporaryExprClass,
                  Stmt::MatrixSubscriptExprClass,
                  Stmt::MemberExprClass,
                  Stmt::NullStmtClass,
                  Stmt::OffsetOfExprClass,
                  Stmt::OpaqueValueExprClass,
                  Stmt::PackExpansionExprClass,
                  Stmt::ParenExprClass,
                  Stmt::PredefinedExprClass,
                  Stmt::RequiresExprClass,
                  Stmt::ReturnStmtClass,
                  Stmt::ShuffleVectorExprClass,
                  Stmt::SizeOfPackExprClass,
                  Stmt::SourceLocExprClass,
                  Stmt::StmtExprClass,
                  Stmt::StringLiteralClass,
                  Stmt::SwitchStmtClass,
                  Stmt::TypeTraitExprClass,
                  Stmt::UnaryExprOrTypeTraitExprClass,
                  Stmt::UnaryOperatorClass,
                  Stmt::UnresolvedLookupExprClass,
                  Stmt::UnresolvedMemberExprClass,
                  Stmt::UserDefinedLiteralClass,
                  Stmt::VAArgExprClass,
                  Stmt::WhileStmtClass,
                  Stmt::ParenListExprClass});
};

TEST_CASE(MicrosoftExpressions) {
    add_main("main.cpp", R"cpp(
struct _GUID { unsigned long a; unsigned short b, c; unsigned char d[8]; };
struct __declspec(uuid("12345678-1234-1234-1234-123456789abc")) Identified {};
using mark_uuid_type = decltype(__uuidof(Identified));
using mark_uuid_expr = decltype(__uuidof(Identified{}));
struct Properties {
    int get(); void put(int);
    int at(int); void put_at(int, int);
    __declspec(property(get=get, put=put)) int value;
    __declspec(property(get=at, put=put_at)) int values[];
};
Properties properties;
using mark_property = decltype(properties.value);
using mark_property_subscript = decltype(properties.values[0]);
using mark_pseudo_object = decltype(properties.value + 1);
void statements() {
    using mark_seh = decltype(({
        __try { __leave; } __except(1) {}
        __try {} __finally {}
        0;
    }));
}
template<class T> void dependent() {
    using mark_exists = decltype(({
        __if_exists(T::member) { int value = 0; }
        0;
    }));
}
)cpp");
    triple = "x86_64-pc-windows-msvc";
    ASSERT_TRUE(compile());
    ASSERT_FALSE(has_errors(*unit));
    auto fidelity = check_marked(*this);
    expect_kinds(fidelity,
                 {Stmt::CXXUuidofExprClass,
                  Stmt::MSPropertyRefExprClass,
                  Stmt::MSPropertySubscriptExprClass,
                  Stmt::PseudoObjectExprClass,
                  Stmt::MSDependentExistsStmtClass,
                  Stmt::SEHTryStmtClass,
                  Stmt::SEHExceptStmtClass,
                  Stmt::SEHFinallyStmtClass,
                  Stmt::SEHLeaveStmtClass});
    // clice does not link the target-specific parsers required to parse MS asm.
    auto& context = unit->context();
    auto location =
        context.getSourceManager().getLocForStartOfFile(context.getSourceManager().getMainFileID());
    auto* operand = IntegerLiteral::Create(context, llvm::APInt(32, 1), context.IntTy, location);
    auto* assembly = new (context) MSAsmStmt(context,
                                             location,
                                             location,
                                             false,
                                             true,
                                             {},
                                             0,
                                             1,
                                             {"r"},
                                             {operand},
                                             "",
                                             {},
                                             location);
    auto* body = CompoundStmt::Create(context, {assembly, operand}, {}, location, location);
    auto* expression = new (context) StmtExpr(body, context.IntTy, location, location, 0);
    ProfileTree tree(fidelity);
    ASSERT_TRUE(tree.TraverseStmt(expression));
    expect_kinds(fidelity, {Stmt::MSAsmStmtClass});
};

TEST_CASE(CoroutineExpressions) {
    add_main("main.cpp", R"cpp(
#include <coroutine>
struct Task {
    struct promise_type {
        Task get_return_object();
        std::suspend_never initial_suspend() noexcept;
        std::suspend_never final_suspend() noexcept;
        std::suspend_never yield_value(int);
        void return_void();
        void unhandled_exception();
    };
};
using mark_dependent_coroutine = decltype([]<class T>(T value) -> Task {
    co_await value;
    co_yield 1;
    co_return;
});
using mark_coroutine = decltype([]() -> Task {
    co_await std::suspend_never{};
    co_yield 1;
    co_return;
});
)cpp");
    ASSERT_TRUE(compile_driver("-std=c++20"));
    ASSERT_FALSE(has_errors(*unit));
    auto fidelity = check_marked(*this);
    expect_kinds(fidelity,
                 {Stmt::CoroutineBodyStmtClass,
                  Stmt::CoreturnStmtClass,
                  Stmt::CoawaitExprClass,
                  Stmt::DependentCoawaitExprClass,
                  Stmt::CoyieldExprClass});
};

TEST_CASE(LanguageExtensions) {
    Tester opencl;
    opencl.add_main("main.cpp", R"cpp(
using mark_as_type = decltype(__builtin_astype(1, float));
using mark_addrspace = decltype(addrspace_cast<__global int*>((__generic int*)nullptr));
)cpp");
    opencl.prepare("-cl-std=clc++2021");
    opencl.params.arguments.insert(opencl.params.arguments.end(), {"-x", "clcpp"});
    ASSERT_TRUE(opencl.try_compile());
    ASSERT_FALSE(has_errors(*opencl.unit));
    auto opencl_fidelity = check_marked(opencl);
    expect_kinds(opencl_fidelity, {Stmt::AsTypeExprClass, Stmt::CXXAddrspaceCastExprClass});

    {
        Tester fixture;
        fixture.add_main("main.cpp", R"cpp(
struct dim3 { dim3(unsigned); };
int cudaConfigureCall(dim3, dim3, unsigned = 0, void* = nullptr);
__attribute__((global)) void kernel();
using mark_cuda_call = decltype(kernel<<<1, 1>>>());
)cpp");
        fixture.prepare("-std=c++20");
        fixture.params.arguments.insert(fixture.params.arguments.end(), {"-x", "cuda"});
        ASSERT_TRUE(fixture.try_compile());
        ASSERT_FALSE(has_errors(*fixture.unit));
        auto fidelity = check_marked(fixture);
        expect_kinds(fidelity, {Stmt::CUDAKernelCallExprClass});
    }
    {
        Tester fixture;
        fixture.add_main("main.cpp", R"cpp(
class Kernel;
using mark_sycl_kernel = decltype([] {
    struct Launcher {
        [[clang::sycl_kernel_entry_point(Kernel)]] static void run() {}
    };
});
using mark_sycl_name = decltype(__builtin_sycl_unique_stable_name(int));
)cpp");
        fixture.prepare("-std=c++20");
        fixture.params.arguments.insert(fixture.params.arguments.end(), {"-fsycl-is-device"});
        ASSERT_TRUE(fixture.try_compile());
        ASSERT_FALSE(has_errors(*fixture.unit));
        auto fidelity = check_marked(fixture);
        expect_kinds(fidelity,
                     {Stmt::SYCLUniqueStableNameExprClass, Stmt::SYCLKernelCallStmtClass});
    }
    {
        Tester fixture;
        fixture.add_file("data.bin", std::string(400, 'a'));
        fixture.add_main("main.cpp", R"cpp(
using mark_embed = decltype((int[]){
#embed "data.bin"
});
)cpp");
        ASSERT_TRUE(fixture.compile("-std=c++2c"));
        ASSERT_FALSE(has_errors(*fixture.unit));
        auto fidelity = check_marked(fixture);
        expect_kinds(fidelity, {Stmt::EmbedExprClass});
    }
    {
        Tester fixture;
        fixture.add_main("main.cpp", R"cpp(
int ordinary(int);
using mark_recovery = decltype([] { ordinary(1, 2); });
)cpp");
        fixture.prepare();
        fixture.params.arguments.push_back("-frecovery-ast");
        ASSERT_TRUE(fixture.try_compile());
        ASSERT_TRUE(fixture.unit->context().getDiagnostics().hasErrorOccurred());
        auto fidelity = check_marked(fixture);
        expect_kinds(fidelity, {Stmt::RecoveryExprClass});
    }
};

TEST_CASE(PartialPackSubstitution) {
    add_main("main.cpp", R"cpp(
void consume(...);
template<int... N> struct Values {
    template<class... T> using mark_value_pack = decltype(consume((T{} + N)...));
};
template<class... T> struct Parameters {
    template<class... U> auto mark_function_pack(T... args) -> decltype(consume(U(args)...));
};
template struct Values<1, 2>;
template struct Parameters<int, int>;
template<class... T> void indexing(T... args) {
    using mark_pack_index = decltype(args...[0]);
}
void instantiate_index() { indexing(1, 2); }
)cpp");
    ASSERT_TRUE(compile("-std=c++2c"));
    ASSERT_FALSE(has_errors(*unit));
    Fidelity fidelity{unit->context()};
    ProfileTree tree(fidelity);
    ASSERT_TRUE(tree.TraverseDecl(unit->tu()));
    expect_kinds(fidelity,
                 {Stmt::SubstNonTypeTemplateParmPackExprClass,
                  Stmt::FunctionParmPackExprClass,
                  Stmt::PackIndexingExprClass});
};

TEST_CASE(EquivalencePairs) {
    add_main("main.cpp", R"cpp(
template<int N> void mark_nttp_a() requires (N > 0);
template<int Renamed> void mark_nttp_b() requires (Renamed > 0);
template<int N> void mark_parens_a() requires ((N) > 0);
template<int N> void mark_parens_b() requires (N > 0);
template<class T> auto parameter_a(T value) -> decltype(value) {
    using mark_param_a = decltype(value);
    using mark_type_a = decltype(sizeof(T));
}
template<class U> auto parameter_b(U renamed) -> decltype(renamed) {
    using mark_param_b = decltype(renamed);
    using mark_type_b = decltype(sizeof(U));
}
using Integer = int;
using mark_typedef_a = decltype(sizeof(Integer));
using mark_typedef_b = decltype(sizeof(int));
namespace named { int value; }
using named::value;
using mark_qualified_a = decltype(named::value);
using mark_qualified_b = decltype(value);
template<class... T> void mark_fold_a() requires ((sizeof(T) > 0) && ...);
class A;
void operator&&(A, A);
template<class... T> void mark_fold_b() requires ((sizeof(T) > 0) && ...);
using mark_literal = decltype(42);
template<int N> struct Wrapped { static constexpr int value = N; };
static_assert(Wrapped<42>::value == 42);
)cpp");
    ASSERT_TRUE(compile());
    ASSERT_FALSE(has_errors(*unit));
    MarkedExpressions markers;
    ASSERT_TRUE(markers.TraverseDecl(unit->tu()));
    Fidelity fidelity{unit->context()};
    for(auto name: {"nttp", "param", "type", "typedef", "qualified", "fold", "parens"}) {
        auto* lhs = markers.expressions.lookup(std::format("mark_{}_a", name));
        auto* rhs = markers.expressions.lookup(std::format("mark_{}_b", name));
        ASSERT_TRUE(lhs && rhs);
        fidelity.compare(lhs, rhs, llvm::StringRef(name) != "parens");
    }
    auto* fold_a = cast<CXXFoldExpr>(markers.expressions.lookup("mark_fold_a"));
    auto* fold_b = cast<CXXFoldExpr>(markers.expressions.lookup("mark_fold_b"));
    EXPECT_FALSE(fold_a->getCallee());
    EXPECT_TRUE(fold_b->getCallee());

    auto* literal = const_cast<Expr*>(markers.expressions.lookup("mark_literal"));
    ASSERT_TRUE(literal);
    auto* constant = ConstantExpr::Create(unit->context(), literal);
    fidelity.compare(constant, literal, true);
    ClassTemplateDecl* wrapped = nullptr;
    for(auto* decl: unit->tu()->decls()) {
        if(auto* candidate = dyn_cast<ClassTemplateDecl>(decl);
           candidate && candidate->getName() == "Wrapped") {
            wrapped = candidate;
        }
    }
    ASSERT_TRUE(wrapped);
    auto* specialization = *wrapped->specializations().begin();
    const SubstNonTypeTemplateParmExpr* substitution = nullptr;
    for(auto* decl: specialization->decls()) {
        if(auto* value = dyn_cast<VarDecl>(decl); value && value->getName() == "value") {
            substitution = dyn_cast<SubstNonTypeTemplateParmExpr>(value->getInit());
        }
    }
    ASSERT_TRUE(substitution);
    fidelity.compare(substitution, substitution->getReplacement(), true);
};

};  // TEST_SUITE(expr_hash)

}  // namespace
}  // namespace clice::testing
