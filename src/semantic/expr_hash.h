#pragma once

/// Expression profiling for declaration identity: a port of clang's
/// StmtProfiler (clang/lib/AST/StmtProfile.cpp, LLVM 22.1.8) restricted to
/// the canonical profile Sema uses to decide template equivalence
/// (isSameConstraintExpr, isSameTemplateParameterList), with every
/// pointer-valued leaf routed through ExprHashLeaves. Upstream feeds raw
/// pointers to the FoldingSetNodeID, which is why Stmt::Profile cannot serve
/// as a cross-process hash; the walk itself is pointer-free.
///
/// Kept: the C and C++ statement and expression visitors. Dropped: the
/// OpenMP, OpenACC and Objective-C visitors (those nodes fall back to
/// VisitStmt: statement class plus children), the non-canonical branches,
/// and the ProfileLambdaExpr branch (a lambda profiles as its closure
/// declaration, C++20 [temp.over.link]p5). Upstream changes are ported by
/// reading StmtProfile.cpp's commit history between the two LLVM versions,
/// not by diffing the files.

#include "llvm/ADT/FoldingSet.h"
#include "clang/AST/APValue.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/DeclarationName.h"
#include "clang/AST/NestedNameSpecifier.h"
#include "clang/AST/StmtVisitor.h"
#include "clang/AST/TemplateBase.h"
#include "clang/AST/TemplateName.h"

namespace clice {

/// The leaves of an expression profile: what the walk cannot express as
/// integers and strings. ExprHasher calls these at exactly the points where
/// clang's StmtProfilerWithPointers adds a pointer, so an implementation
/// that adds the same pointers reproduces clang's canonical profile bit for
/// bit (the port's fidelity test), and the identity module's implementation
/// makes the profile stable across processes and translation units.
class ExprHashLeaves {
public:
    virtual ~ExprHashLeaves() = default;

    /// A referenced declaration, possibly null. Upstream encodes template
    /// parameters and function parameters by depth and index before falling
    /// back to the canonical declaration's pointer; every implementation
    /// keeps that split.
    virtual void add_decl(const clang::Decl* decl) = 0;

    /// A referenced type, possibly null; profiled canonically.
    virtual void add_type(clang::QualType type) = 0;

    /// A name that is not resolved to a declaration (dependent member and
    /// scope names). `treat_as_decl` mirrors upstream's flag.
    virtual void add_name(clang::DeclarationName name, bool treat_as_decl) = 0;

    /// An identifier outside any declaration or type, possibly null.
    virtual void add_identifier(const clang::IdentifierInfo* identifier) = 0;

    virtual void add_nested_name_specifier(clang::NestedNameSpecifier specifier) = 0;

    virtual void add_template_name(clang::TemplateName name) = 0;

    /// A structural-value template argument (class-type and floating-point
    /// non-type template parameters). Upstream profiles the APValue itself,
    /// which embeds the addresses of lvalue bases.
    virtual void add_structural_value(clang::QualType type, const clang::APValue& value) = 0;
};

/// The walk. One instance per expression: construct, Visit the root, then
/// hash the id's data.
class ExprHasher : public clang::ConstStmtVisitor<ExprHasher> {
public:
    ExprHasher(llvm::FoldingSetNodeID& id,
               const clang::ASTContext& context,
               ExprHashLeaves& leaves) : id(id), context(context), leaves(leaves) {}

    /// Statement class plus children: the default for every node without a
    /// visitor of its own.
    void VisitStmt(const clang::Stmt* stmt);

    void VisitTemplateArguments(const clang::TemplateArgumentLoc* arguments, unsigned count);

    void VisitTemplateArgument(const clang::TemplateArgument& argument);

    /// The kept upstream visitors, in StmtProfile.cpp's order.
    void VisitDeclStmt(const clang::DeclStmt* stmt);
    void VisitNullStmt(const clang::NullStmt* stmt);
    void VisitCompoundStmt(const clang::CompoundStmt* stmt);
    void VisitCaseStmt(const clang::CaseStmt* stmt);
    void VisitDefaultStmt(const clang::DefaultStmt* stmt);
    void VisitLabelStmt(const clang::LabelStmt* stmt);
    void VisitAttributedStmt(const clang::AttributedStmt* stmt);
    void VisitIfStmt(const clang::IfStmt* stmt);
    void VisitSwitchStmt(const clang::SwitchStmt* stmt);
    void VisitWhileStmt(const clang::WhileStmt* stmt);
    void VisitDoStmt(const clang::DoStmt* stmt);
    void VisitForStmt(const clang::ForStmt* stmt);
    void VisitGotoStmt(const clang::GotoStmt* stmt);
    void VisitIndirectGotoStmt(const clang::IndirectGotoStmt* stmt);
    void VisitContinueStmt(const clang::ContinueStmt* stmt);
    void VisitBreakStmt(const clang::BreakStmt* stmt);
    void VisitReturnStmt(const clang::ReturnStmt* stmt);
    void VisitDeferStmt(const clang::DeferStmt* stmt);
    void VisitGCCAsmStmt(const clang::GCCAsmStmt* stmt);
    void VisitMSAsmStmt(const clang::MSAsmStmt* stmt);
    void VisitCXXCatchStmt(const clang::CXXCatchStmt* stmt);
    void VisitCXXTryStmt(const clang::CXXTryStmt* stmt);
    void VisitCXXForRangeStmt(const clang::CXXForRangeStmt* stmt);
    void VisitMSDependentExistsStmt(const clang::MSDependentExistsStmt* stmt);
    void VisitSEHTryStmt(const clang::SEHTryStmt* stmt);
    void VisitSEHFinallyStmt(const clang::SEHFinallyStmt* stmt);
    void VisitSEHExceptStmt(const clang::SEHExceptStmt* stmt);
    void VisitSEHLeaveStmt(const clang::SEHLeaveStmt* stmt);
    void VisitCapturedStmt(const clang::CapturedStmt* stmt);
    void VisitSYCLKernelCallStmt(const clang::SYCLKernelCallStmt* stmt);
    void VisitExpr(const clang::Expr* expr);
    void VisitConstantExpr(const clang::ConstantExpr* expr);
    void VisitDeclRefExpr(const clang::DeclRefExpr* expr);
    void VisitSYCLUniqueStableNameExpr(const clang::SYCLUniqueStableNameExpr* expr);
    void VisitPredefinedExpr(const clang::PredefinedExpr* expr);
    void VisitIntegerLiteral(const clang::IntegerLiteral* expr);
    void VisitFixedPointLiteral(const clang::FixedPointLiteral* expr);
    void VisitCharacterLiteral(const clang::CharacterLiteral* expr);
    void VisitFloatingLiteral(const clang::FloatingLiteral* expr);
    void VisitImaginaryLiteral(const clang::ImaginaryLiteral* expr);
    void VisitStringLiteral(const clang::StringLiteral* expr);
    void VisitParenExpr(const clang::ParenExpr* expr);
    void VisitParenListExpr(const clang::ParenListExpr* expr);
    void VisitUnaryOperator(const clang::UnaryOperator* expr);
    void VisitOffsetOfExpr(const clang::OffsetOfExpr* expr);
    void VisitArraySubscriptExpr(const clang::ArraySubscriptExpr* expr);
    void VisitMatrixSingleSubscriptExpr(const clang::MatrixSingleSubscriptExpr* expr);
    void VisitMatrixSubscriptExpr(const clang::MatrixSubscriptExpr* expr);
    void VisitArraySectionExpr(const clang::ArraySectionExpr* expr);
    void VisitCallExpr(const clang::CallExpr* expr);
    void VisitMemberExpr(const clang::MemberExpr* expr);
    void VisitCompoundLiteralExpr(const clang::CompoundLiteralExpr* expr);
    void VisitCastExpr(const clang::CastExpr* expr);
    void VisitImplicitCastExpr(const clang::ImplicitCastExpr* expr);
    void VisitExplicitCastExpr(const clang::ExplicitCastExpr* expr);
    void VisitCStyleCastExpr(const clang::CStyleCastExpr* expr);
    void VisitBinaryOperator(const clang::BinaryOperator* expr);
    void VisitConditionalOperator(const clang::ConditionalOperator* expr);
    void VisitBinaryConditionalOperator(const clang::BinaryConditionalOperator* expr);
    void VisitAddrLabelExpr(const clang::AddrLabelExpr* expr);
    void VisitStmtExpr(const clang::StmtExpr* expr);
    void VisitShuffleVectorExpr(const clang::ShuffleVectorExpr* expr);
    void VisitConvertVectorExpr(const clang::ConvertVectorExpr* expr);
    void VisitChooseExpr(const clang::ChooseExpr* expr);
    void VisitGNUNullExpr(const clang::GNUNullExpr* expr);
    void VisitVAArgExpr(const clang::VAArgExpr* expr);
    void VisitInitListExpr(const clang::InitListExpr* expr);
    void VisitDesignatedInitExpr(const clang::DesignatedInitExpr* expr);
    void VisitDesignatedInitUpdateExpr(const clang::DesignatedInitUpdateExpr* expr);
    void VisitArrayInitLoopExpr(const clang::ArrayInitLoopExpr* expr);
    void VisitArrayInitIndexExpr(const clang::ArrayInitIndexExpr* expr);
    void VisitNoInitExpr(const clang::NoInitExpr* expr);
    void VisitImplicitValueInitExpr(const clang::ImplicitValueInitExpr* expr);
    void VisitExtVectorElementExpr(const clang::ExtVectorElementExpr* expr);
    void VisitBlockExpr(const clang::BlockExpr* expr);
    void VisitGenericSelectionExpr(const clang::GenericSelectionExpr* expr);
    void VisitPseudoObjectExpr(const clang::PseudoObjectExpr* expr);
    void VisitAtomicExpr(const clang::AtomicExpr* expr);
    void VisitConceptSpecializationExpr(const clang::ConceptSpecializationExpr* expr);
    void VisitRequiresExpr(const clang::RequiresExpr* expr);
    void VisitCXXOperatorCallExpr(const clang::CXXOperatorCallExpr* expr);
    void VisitCXXRewrittenBinaryOperator(const clang::CXXRewrittenBinaryOperator* expr);
    void VisitCXXMemberCallExpr(const clang::CXXMemberCallExpr* expr);
    void VisitCUDAKernelCallExpr(const clang::CUDAKernelCallExpr* expr);
    void VisitAsTypeExpr(const clang::AsTypeExpr* expr);
    void VisitCXXNamedCastExpr(const clang::CXXNamedCastExpr* expr);
    void VisitCXXStaticCastExpr(const clang::CXXStaticCastExpr* expr);
    void VisitCXXDynamicCastExpr(const clang::CXXDynamicCastExpr* expr);
    void VisitCXXConstCastExpr(const clang::CXXConstCastExpr* expr);
    void VisitBuiltinBitCastExpr(const clang::BuiltinBitCastExpr* expr);
    void VisitCXXAddrspaceCastExpr(const clang::CXXAddrspaceCastExpr* expr);
    void VisitUserDefinedLiteral(const clang::UserDefinedLiteral* expr);
    void VisitCXXBoolLiteralExpr(const clang::CXXBoolLiteralExpr* expr);
    void VisitCXXNullPtrLiteralExpr(const clang::CXXNullPtrLiteralExpr* expr);
    void VisitCXXStdInitializerListExpr(const clang::CXXStdInitializerListExpr* expr);
    void VisitCXXTypeidExpr(const clang::CXXTypeidExpr* expr);
    void VisitCXXUuidofExpr(const clang::CXXUuidofExpr* expr);
    void VisitMSPropertyRefExpr(const clang::MSPropertyRefExpr* expr);
    void VisitMSPropertySubscriptExpr(const clang::MSPropertySubscriptExpr* expr);
    void VisitCXXThisExpr(const clang::CXXThisExpr* expr);
    void VisitCXXThrowExpr(const clang::CXXThrowExpr* expr);
    void VisitCXXDefaultArgExpr(const clang::CXXDefaultArgExpr* expr);
    void VisitCXXDefaultInitExpr(const clang::CXXDefaultInitExpr* expr);
    void VisitCXXBindTemporaryExpr(const clang::CXXBindTemporaryExpr* expr);
    void VisitCXXConstructExpr(const clang::CXXConstructExpr* expr);
    void VisitCXXInheritedCtorInitExpr(const clang::CXXInheritedCtorInitExpr* expr);
    void VisitCXXFunctionalCastExpr(const clang::CXXFunctionalCastExpr* expr);
    void VisitCXXDeleteExpr(const clang::CXXDeleteExpr* expr);
    void VisitCXXNewExpr(const clang::CXXNewExpr* expr);
    void VisitOverloadExpr(const clang::OverloadExpr* expr);
    void VisitTypeTraitExpr(const clang::TypeTraitExpr* expr);
    void VisitArrayTypeTraitExpr(const clang::ArrayTypeTraitExpr* expr);
    void VisitExpressionTraitExpr(const clang::ExpressionTraitExpr* expr);
    void VisitDependentScopeDeclRefExpr(const clang::DependentScopeDeclRefExpr* expr);
    void VisitExprWithCleanups(const clang::ExprWithCleanups* expr);
    void VisitCXXUnresolvedConstructExpr(const clang::CXXUnresolvedConstructExpr* expr);
    void VisitCXXDependentScopeMemberExpr(const clang::CXXDependentScopeMemberExpr* expr);
    void VisitUnresolvedMemberExpr(const clang::UnresolvedMemberExpr* expr);
    void VisitCXXNoexceptExpr(const clang::CXXNoexceptExpr* expr);
    void VisitPackExpansionExpr(const clang::PackExpansionExpr* expr);
    void VisitSizeOfPackExpr(const clang::SizeOfPackExpr* expr);
    void VisitPackIndexingExpr(const clang::PackIndexingExpr* expr);
    void VisitSubstNonTypeTemplateParmPackExpr(const clang::SubstNonTypeTemplateParmPackExpr* expr);
    void VisitSubstNonTypeTemplateParmExpr(const clang::SubstNonTypeTemplateParmExpr* expr);
    void VisitFunctionParmPackExpr(const clang::FunctionParmPackExpr* expr);
    void VisitMaterializeTemporaryExpr(const clang::MaterializeTemporaryExpr* expr);
    void VisitCXXFoldExpr(const clang::CXXFoldExpr* expr);
    void VisitCXXParenListInitExpr(const clang::CXXParenListInitExpr* expr);
    void VisitLambdaExpr(const clang::LambdaExpr* expr);
    void VisitCoroutineBodyStmt(const clang::CoroutineBodyStmt* stmt);
    void VisitCoreturnStmt(const clang::CoreturnStmt* stmt);
    void VisitCoawaitExpr(const clang::CoawaitExpr* expr);
    void VisitDependentCoawaitExpr(const clang::DependentCoawaitExpr* expr);
    void VisitCoyieldExpr(const clang::CoyieldExpr* expr);
    void VisitOpaqueValueExpr(const clang::OpaqueValueExpr* expr);
    void VisitSourceLocExpr(const clang::SourceLocExpr* expr);
    void VisitEmbedExpr(const clang::EmbedExpr* expr);
    void VisitRecoveryExpr(const clang::RecoveryExpr* expr);
    void VisitHLSLOutArgExpr(const clang::HLSLOutArgExpr* expr);

private:
    friend class clang::StmtVisitorBase<llvm::make_const_ptr, ExprHasher>;

    void VisitUnaryExprOrTypeTraitExpr(const clang::UnaryExprOrTypeTraitExpr* expr);
    void VisitCXXPseudoDestructorExpr(const clang::CXXPseudoDestructorExpr* expr);

    /// The statement class alone; upstream's VisitStmtNoChildren.
    void visit_stmt_class(const clang::Stmt* stmt);

    llvm::FoldingSetNodeID& id;
    const clang::ASTContext& context;
    ExprHashLeaves& leaves;
};

}  // namespace clice
