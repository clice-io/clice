// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "semantic/expr_hash.h"

#include <utility>

#include "clang/AST/DeclTemplate.h"
#include "clang/AST/ExprConcepts.h"

namespace clice {

using namespace clang;

void ExprHasher::visit_stmt_class(const Stmt* stmt) {
    id.AddInteger(stmt->getStmtClass());
}

void ExprHasher::VisitStmt(const Stmt* node) {
    assert(node && "Requires non-null Stmt pointer");

    visit_stmt_class(node);

    for(const Stmt* sub_stmt: node->children()) {
        if(sub_stmt)
            Visit(sub_stmt);
        else
            id.AddInteger(0);
    }
}

void ExprHasher::VisitDeclStmt(const DeclStmt* node) {
    VisitStmt(node);
    for(const auto* decl: node->decls())
        leaves.add_decl(decl);
}

void ExprHasher::VisitNullStmt(const NullStmt* node) {
    VisitStmt(node);
}

void ExprHasher::VisitCompoundStmt(const CompoundStmt* node) {
    VisitStmt(node);
}

void ExprHasher::VisitCaseStmt(const CaseStmt* node) {
    VisitStmt(node);
}

void ExprHasher::VisitDefaultStmt(const DefaultStmt* node) {
    VisitStmt(node);
}

void ExprHasher::VisitLabelStmt(const LabelStmt* node) {
    VisitStmt(node);
    leaves.add_decl(node->getDecl());
}

void ExprHasher::VisitAttributedStmt(const AttributedStmt* node) {
    VisitStmt(node);
}

void ExprHasher::VisitIfStmt(const IfStmt* node) {
    VisitStmt(node);
    leaves.add_decl(node->getConditionVariable());
}

void ExprHasher::VisitSwitchStmt(const SwitchStmt* node) {
    VisitStmt(node);
    leaves.add_decl(node->getConditionVariable());
}

void ExprHasher::VisitWhileStmt(const WhileStmt* node) {
    VisitStmt(node);
    leaves.add_decl(node->getConditionVariable());
}

void ExprHasher::VisitDoStmt(const DoStmt* node) {
    VisitStmt(node);
}

void ExprHasher::VisitForStmt(const ForStmt* node) {
    VisitStmt(node);
}

void ExprHasher::VisitGotoStmt(const GotoStmt* node) {
    VisitStmt(node);
    leaves.add_decl(node->getLabel());
}

void ExprHasher::VisitIndirectGotoStmt(const IndirectGotoStmt* node) {
    VisitStmt(node);
}

void ExprHasher::VisitContinueStmt(const ContinueStmt* node) {
    VisitStmt(node);
}

void ExprHasher::VisitBreakStmt(const BreakStmt* node) {
    VisitStmt(node);
}

void ExprHasher::VisitReturnStmt(const ReturnStmt* node) {
    VisitStmt(node);
}

void ExprHasher::VisitDeferStmt(const DeferStmt* node) {
    VisitStmt(node);
}

void ExprHasher::VisitGCCAsmStmt(const GCCAsmStmt* node) {
    VisitStmt(node);
    id.AddBoolean(node->isVolatile());
    id.AddBoolean(node->isSimple());
    VisitExpr(node->getAsmStringExpr());
    id.AddInteger(node->getNumOutputs());
    for(unsigned i = 0, count = node->getNumOutputs(); i != count; i += 1) {
        id.AddString(node->getOutputName(i));
        VisitExpr(node->getOutputConstraintExpr(i));
    }
    id.AddInteger(node->getNumInputs());
    for(unsigned i = 0, count = node->getNumInputs(); i != count; i += 1) {
        id.AddString(node->getInputName(i));
        VisitExpr(node->getInputConstraintExpr(i));
    }
    id.AddInteger(node->getNumClobbers());
    for(unsigned i = 0, count = node->getNumClobbers(); i != count; i += 1)
        VisitExpr(node->getClobberExpr(i));
    id.AddInteger(node->getNumLabels());
    for(auto* label: node->labels())
        leaves.add_decl(label->getLabel());
}

void ExprHasher::VisitMSAsmStmt(const MSAsmStmt* node) {
    VisitStmt(node);
}

void ExprHasher::VisitCXXCatchStmt(const CXXCatchStmt* node) {
    VisitStmt(node);
    leaves.add_type(node->getCaughtType());
}

void ExprHasher::VisitCXXTryStmt(const CXXTryStmt* node) {
    VisitStmt(node);
}

void ExprHasher::VisitCXXForRangeStmt(const CXXForRangeStmt* node) {
    VisitStmt(node);
}

void ExprHasher::VisitMSDependentExistsStmt(const MSDependentExistsStmt* node) {
    VisitStmt(node);
    id.AddBoolean(node->isIfExists());
    leaves.add_nested_name_specifier(node->getQualifierLoc().getNestedNameSpecifier());
    leaves.add_name(node->getNameInfo().getName(), false);
}

void ExprHasher::VisitSEHTryStmt(const SEHTryStmt* node) {
    VisitStmt(node);
}

void ExprHasher::VisitSEHFinallyStmt(const SEHFinallyStmt* node) {
    VisitStmt(node);
}

void ExprHasher::VisitSEHExceptStmt(const SEHExceptStmt* node) {
    VisitStmt(node);
}

void ExprHasher::VisitSEHLeaveStmt(const SEHLeaveStmt* node) {
    VisitStmt(node);
}

void ExprHasher::VisitCapturedStmt(const CapturedStmt* node) {
    VisitStmt(node);
}

void ExprHasher::VisitSYCLKernelCallStmt(const SYCLKernelCallStmt* node) {
    VisitStmt(node);
}

void ExprHasher::VisitExpr(const Expr* node) {
    VisitStmt(node);
}

void ExprHasher::VisitConstantExpr(const ConstantExpr* node) {
    Visit(node->getSubExpr());
}

void ExprHasher::VisitDeclRefExpr(const DeclRefExpr* node) {
    VisitExpr(node);
    leaves.add_decl(node->getDecl());
}

void ExprHasher::VisitSYCLUniqueStableNameExpr(const SYCLUniqueStableNameExpr* node) {
    VisitExpr(node);
    leaves.add_type(node->getTypeSourceInfo()->getType());
}

void ExprHasher::VisitPredefinedExpr(const PredefinedExpr* node) {
    VisitExpr(node);
    id.AddInteger(llvm::to_underlying(node->getIdentKind()));
}

void ExprHasher::VisitIntegerLiteral(const IntegerLiteral* node) {
    VisitExpr(node);
    node->getValue().Profile(id);

    QualType type = node->getType().getCanonicalType();
    id.AddInteger(type->getTypeClass());
    if(auto bit_int_type = type->getAs<BitIntType>())
        bit_int_type->Profile(id);
    else
        id.AddInteger(type->castAs<BuiltinType>()->getKind());
}

void ExprHasher::VisitFixedPointLiteral(const FixedPointLiteral* node) {
    VisitExpr(node);
    node->getValue().Profile(id);
    id.AddInteger(node->getType()->castAs<BuiltinType>()->getKind());
}

void ExprHasher::VisitCharacterLiteral(const CharacterLiteral* node) {
    VisitExpr(node);
    id.AddInteger(llvm::to_underlying(node->getKind()));
    id.AddInteger(node->getValue());
}

void ExprHasher::VisitFloatingLiteral(const FloatingLiteral* node) {
    VisitExpr(node);
    node->getValue().Profile(id);
    id.AddBoolean(node->isExact());
    id.AddInteger(node->getType()->castAs<BuiltinType>()->getKind());
}

void ExprHasher::VisitImaginaryLiteral(const ImaginaryLiteral* node) {
    VisitExpr(node);
}

void ExprHasher::VisitStringLiteral(const StringLiteral* node) {
    VisitExpr(node);
    id.AddString(node->getBytes());
    id.AddInteger(llvm::to_underlying(node->getKind()));
}

void ExprHasher::VisitParenExpr(const ParenExpr* node) {
    VisitExpr(node);
}

void ExprHasher::VisitParenListExpr(const ParenListExpr* node) {
    VisitExpr(node);
}

void ExprHasher::VisitUnaryOperator(const UnaryOperator* node) {
    VisitExpr(node);
    id.AddInteger(node->getOpcode());
}

void ExprHasher::VisitOffsetOfExpr(const OffsetOfExpr* node) {
    leaves.add_type(node->getTypeSourceInfo()->getType());
    unsigned n = node->getNumComponents();
    for(unsigned i = 0; i < n; i += 1) {
        const OffsetOfNode& component = node->getComponent(i);
        id.AddInteger(component.getKind());
        switch(component.getKind()) {
            case OffsetOfNode::Array:
                // Expressions handled below.
                break;

            case OffsetOfNode::Field: leaves.add_decl(component.getField()); break;

            case OffsetOfNode::Identifier: leaves.add_identifier(component.getFieldName()); break;

            case OffsetOfNode::Base:
                // These nodes are implicit, and therefore don't need profiling.
                break;
        }
    }

    VisitExpr(node);
}

void ExprHasher::VisitUnaryExprOrTypeTraitExpr(const UnaryExprOrTypeTraitExpr* node) {
    VisitExpr(node);
    id.AddInteger(node->getKind());
    if(node->isArgumentType())
        leaves.add_type(node->getArgumentType());
}

void ExprHasher::VisitArraySubscriptExpr(const ArraySubscriptExpr* node) {
    VisitExpr(node);
}

void ExprHasher::VisitMatrixSingleSubscriptExpr(const MatrixSingleSubscriptExpr* node) {
    VisitExpr(node);
}

void ExprHasher::VisitMatrixSubscriptExpr(const MatrixSubscriptExpr* node) {
    VisitExpr(node);
}

void ExprHasher::VisitArraySectionExpr(const ArraySectionExpr* node) {
    VisitExpr(node);
}

void ExprHasher::VisitCallExpr(const CallExpr* node) {
    VisitExpr(node);
}

void ExprHasher::VisitMemberExpr(const MemberExpr* node) {
    VisitExpr(node);
    leaves.add_decl(node->getMemberDecl());
    id.AddBoolean(node->isArrow());
}

void ExprHasher::VisitCompoundLiteralExpr(const CompoundLiteralExpr* node) {
    VisitExpr(node);
    id.AddBoolean(node->isFileScope());
}

void ExprHasher::VisitCastExpr(const CastExpr* node) {
    VisitExpr(node);
}

void ExprHasher::VisitImplicitCastExpr(const ImplicitCastExpr* node) {
    VisitCastExpr(node);
    id.AddInteger(node->getValueKind());
}

void ExprHasher::VisitExplicitCastExpr(const ExplicitCastExpr* node) {
    VisitCastExpr(node);
    leaves.add_type(node->getTypeAsWritten());
}

void ExprHasher::VisitCStyleCastExpr(const CStyleCastExpr* node) {
    VisitExplicitCastExpr(node);
}

void ExprHasher::VisitBinaryOperator(const BinaryOperator* node) {
    VisitExpr(node);
    id.AddInteger(node->getOpcode());
}

void ExprHasher::VisitConditionalOperator(const ConditionalOperator* node) {
    VisitExpr(node);
}

void ExprHasher::VisitBinaryConditionalOperator(const BinaryConditionalOperator* node) {
    VisitExpr(node);
}

void ExprHasher::VisitAddrLabelExpr(const AddrLabelExpr* node) {
    VisitExpr(node);
    leaves.add_decl(node->getLabel());
}

void ExprHasher::VisitStmtExpr(const StmtExpr* node) {
    VisitExpr(node);
}

void ExprHasher::VisitShuffleVectorExpr(const ShuffleVectorExpr* node) {
    VisitExpr(node);
}

void ExprHasher::VisitConvertVectorExpr(const ConvertVectorExpr* node) {
    VisitExpr(node);
}

void ExprHasher::VisitChooseExpr(const ChooseExpr* node) {
    VisitExpr(node);
}

void ExprHasher::VisitGNUNullExpr(const GNUNullExpr* node) {
    VisitExpr(node);
}

void ExprHasher::VisitVAArgExpr(const VAArgExpr* node) {
    VisitExpr(node);
}

void ExprHasher::VisitInitListExpr(const InitListExpr* node) {
    if(node->getSyntacticForm()) {
        VisitInitListExpr(node->getSyntacticForm());
        return;
    }

    VisitExpr(node);
}

void ExprHasher::VisitDesignatedInitExpr(const DesignatedInitExpr* node) {
    VisitExpr(node);
    id.AddBoolean(node->usesGNUSyntax());
    for(const DesignatedInitExpr::Designator& decl: node->designators()) {
        if(decl.isFieldDesignator()) {
            id.AddInteger(0);
            leaves.add_name(decl.getFieldName(), false);
            continue;
        }

        if(decl.isArrayDesignator()) {
            id.AddInteger(1);
        } else {
            assert(decl.isArrayRangeDesignator());
            id.AddInteger(2);
        }
        id.AddInteger(decl.getArrayIndex());
    }
}

// Seems that if VisitInitListExpr() only works on the syntactic form of an
// InitListExpr, then a DesignatedInitUpdateExpr is not encountered.
void ExprHasher::VisitDesignatedInitUpdateExpr(const DesignatedInitUpdateExpr*) {
    std::unreachable();
}

void ExprHasher::VisitArrayInitLoopExpr(const ArrayInitLoopExpr* node) {
    VisitExpr(node);
}

void ExprHasher::VisitArrayInitIndexExpr(const ArrayInitIndexExpr* node) {
    VisitExpr(node);
}

void ExprHasher::VisitNoInitExpr(const NoInitExpr*) {
    std::unreachable();
}

void ExprHasher::VisitImplicitValueInitExpr(const ImplicitValueInitExpr* node) {
    VisitExpr(node);
}

void ExprHasher::VisitExtVectorElementExpr(const ExtVectorElementExpr* node) {
    VisitExpr(node);
    leaves.add_name(&node->getAccessor(), false);
}

void ExprHasher::VisitBlockExpr(const BlockExpr* node) {
    VisitExpr(node);
    leaves.add_decl(node->getBlockDecl());
}

void ExprHasher::VisitGenericSelectionExpr(const GenericSelectionExpr* node) {
    VisitExpr(node);
    for(const GenericSelectionExpr::ConstAssociation association: node->associations()) {
        QualType type = association.getType();
        if(type.isNull())
            id.AddPointer(nullptr);
        else
            leaves.add_type(type);
        VisitExpr(association.getAssociationExpr());
    }
}

void ExprHasher::VisitPseudoObjectExpr(const PseudoObjectExpr* node) {
    VisitExpr(node);
    for(PseudoObjectExpr::const_semantics_iterator i = node->semantics_begin(),
                                                   e = node->semantics_end();
        i != e;
        ++i)
        // Normally, we would not profile the source expressions of OVEs.
        if(const OpaqueValueExpr* opaque_value = dyn_cast<OpaqueValueExpr>(*i))
            Visit(opaque_value->getSourceExpr());
}

void ExprHasher::VisitAtomicExpr(const AtomicExpr* node) {
    VisitExpr(node);
    id.AddInteger(node->getOp());
}

void ExprHasher::VisitConceptSpecializationExpr(const ConceptSpecializationExpr* node) {
    VisitExpr(node);
    leaves.add_decl(node->getNamedConcept());
    for(const TemplateArgument& argument: node->getTemplateArguments())
        VisitTemplateArgument(argument);
}

void ExprHasher::VisitRequiresExpr(const RequiresExpr* node) {
    VisitExpr(node);
    id.AddInteger(node->getLocalParameters().size());
    for(ParmVarDecl* local_parameter: node->getLocalParameters())
        leaves.add_decl(local_parameter);
    id.AddInteger(node->getRequirements().size());
    for(concepts::Requirement* requirement: node->getRequirements()) {
        if(auto* type_requirement = dyn_cast<concepts::TypeRequirement>(requirement)) {
            id.AddInteger(concepts::Requirement::RK_Type);
            id.AddBoolean(type_requirement->isSubstitutionFailure());
            if(!type_requirement->isSubstitutionFailure())
                leaves.add_type(type_requirement->getType()->getType());
        } else if(auto* expr_requirement = dyn_cast<concepts::ExprRequirement>(requirement)) {
            id.AddInteger(concepts::Requirement::RK_Compound);
            id.AddBoolean(expr_requirement->isExprSubstitutionFailure());
            if(!expr_requirement->isExprSubstitutionFailure())
                Visit(expr_requirement->getExpr());
            // C++2a [expr.prim.req.compound]p1 Example:
            //    [...] The compound-requirement in C1 requires that x++ is a valid
            //    expression. It is equivalent to the simple-requirement x++; [...]
            // We therefore do not profile isSimple() here.
            id.AddBoolean(expr_requirement->getNoexceptLoc().isValid());
            const concepts::ExprRequirement::ReturnTypeRequirement& return_requirement =
                expr_requirement->getReturnTypeRequirement();
            if(return_requirement.isEmpty()) {
                id.AddInteger(0);
            } else if(return_requirement.isTypeConstraint()) {
                id.AddInteger(1);
                Visit(return_requirement.getTypeConstraint()->getImmediatelyDeclaredConstraint());
            } else {
                assert(return_requirement.isSubstitutionFailure());
                id.AddInteger(2);
            }
        } else {
            id.AddInteger(concepts::Requirement::RK_Nested);
            auto* nested_requirement = cast<concepts::NestedRequirement>(requirement);
            id.AddBoolean(nested_requirement->hasInvalidConstraint());
            if(!nested_requirement->hasInvalidConstraint())
                Visit(nested_requirement->getConstraintExpr());
        }
    }
}

static Stmt::StmtClass decode_operator_call(const CXXOperatorCallExpr* node,
                                            UnaryOperatorKind& unary_op,
                                            BinaryOperatorKind& binary_op,
                                            unsigned& num_args) {
    switch(node->getOperator()) {
        case OO_None:
        case OO_New:
        case OO_Delete:
        case OO_Array_New:
        case OO_Array_Delete:
        case OO_Arrow:
        case OO_Conditional:
        case NUM_OVERLOADED_OPERATORS: std::unreachable();

        case OO_Plus:
            if(num_args == 1) {
                unary_op = UO_Plus;
                return Stmt::UnaryOperatorClass;
            }

            binary_op = BO_Add;
            return Stmt::BinaryOperatorClass;

        case OO_Minus:
            if(num_args == 1) {
                unary_op = UO_Minus;
                return Stmt::UnaryOperatorClass;
            }

            binary_op = BO_Sub;
            return Stmt::BinaryOperatorClass;

        case OO_Star:
            if(num_args == 1) {
                unary_op = UO_Deref;
                return Stmt::UnaryOperatorClass;
            }

            binary_op = BO_Mul;
            return Stmt::BinaryOperatorClass;

        case OO_Slash: binary_op = BO_Div; return Stmt::BinaryOperatorClass;

        case OO_Percent: binary_op = BO_Rem; return Stmt::BinaryOperatorClass;

        case OO_Caret: binary_op = BO_Xor; return Stmt::BinaryOperatorClass;

        case OO_Amp:
            if(num_args == 1) {
                unary_op = UO_AddrOf;
                return Stmt::UnaryOperatorClass;
            }

            binary_op = BO_And;
            return Stmt::BinaryOperatorClass;

        case OO_Pipe: binary_op = BO_Or; return Stmt::BinaryOperatorClass;

        case OO_Tilde: unary_op = UO_Not; return Stmt::UnaryOperatorClass;

        case OO_Exclaim: unary_op = UO_LNot; return Stmt::UnaryOperatorClass;

        case OO_Equal: binary_op = BO_Assign; return Stmt::BinaryOperatorClass;

        case OO_Less: binary_op = BO_LT; return Stmt::BinaryOperatorClass;

        case OO_Greater: binary_op = BO_GT; return Stmt::BinaryOperatorClass;

        case OO_PlusEqual: binary_op = BO_AddAssign; return Stmt::CompoundAssignOperatorClass;

        case OO_MinusEqual: binary_op = BO_SubAssign; return Stmt::CompoundAssignOperatorClass;

        case OO_StarEqual: binary_op = BO_MulAssign; return Stmt::CompoundAssignOperatorClass;

        case OO_SlashEqual: binary_op = BO_DivAssign; return Stmt::CompoundAssignOperatorClass;

        case OO_PercentEqual: binary_op = BO_RemAssign; return Stmt::CompoundAssignOperatorClass;

        case OO_CaretEqual: binary_op = BO_XorAssign; return Stmt::CompoundAssignOperatorClass;

        case OO_AmpEqual: binary_op = BO_AndAssign; return Stmt::CompoundAssignOperatorClass;

        case OO_PipeEqual: binary_op = BO_OrAssign; return Stmt::CompoundAssignOperatorClass;

        case OO_LessLess: binary_op = BO_Shl; return Stmt::BinaryOperatorClass;

        case OO_GreaterGreater: binary_op = BO_Shr; return Stmt::BinaryOperatorClass;

        case OO_LessLessEqual: binary_op = BO_ShlAssign; return Stmt::CompoundAssignOperatorClass;

        case OO_GreaterGreaterEqual:
            binary_op = BO_ShrAssign;
            return Stmt::CompoundAssignOperatorClass;

        case OO_EqualEqual: binary_op = BO_EQ; return Stmt::BinaryOperatorClass;

        case OO_ExclaimEqual: binary_op = BO_NE; return Stmt::BinaryOperatorClass;

        case OO_LessEqual: binary_op = BO_LE; return Stmt::BinaryOperatorClass;

        case OO_GreaterEqual: binary_op = BO_GE; return Stmt::BinaryOperatorClass;

        case OO_Spaceship: binary_op = BO_Cmp; return Stmt::BinaryOperatorClass;

        case OO_AmpAmp: binary_op = BO_LAnd; return Stmt::BinaryOperatorClass;

        case OO_PipePipe: binary_op = BO_LOr; return Stmt::BinaryOperatorClass;

        case OO_PlusPlus:
            unary_op = num_args == 1 ? UO_PreInc : UO_PostInc;
            num_args = 1;
            return Stmt::UnaryOperatorClass;

        case OO_MinusMinus:
            unary_op = num_args == 1 ? UO_PreDec : UO_PostDec;
            num_args = 1;
            return Stmt::UnaryOperatorClass;

        case OO_Comma: binary_op = BO_Comma; return Stmt::BinaryOperatorClass;

        case OO_ArrowStar: binary_op = BO_PtrMemI; return Stmt::BinaryOperatorClass;

        case OO_Subscript: return Stmt::ArraySubscriptExprClass;

        case OO_Call: return Stmt::CallExprClass;

        case OO_Coawait: unary_op = UO_Coawait; return Stmt::UnaryOperatorClass;
    }

    std::unreachable();
}

void ExprHasher::VisitCXXOperatorCallExpr(const CXXOperatorCallExpr* node) {
    if(node->isTypeDependent()) {
        // Type-dependent operator calls are profiled like their underlying
        // syntactic operator.
        //
        // An operator call to operator-> is always implicit, so just skip it. The
        // enclosing MemberExpr will profile the actual member access.
        if(node->getOperator() == OO_Arrow)
            return Visit(node->getArg(0));

        UnaryOperatorKind unary_op = UO_Extension;
        BinaryOperatorKind binary_op = BO_Comma;
        unsigned num_args = node->getNumArgs();
        Stmt::StmtClass stmt_class = decode_operator_call(node, unary_op, binary_op, num_args);

        id.AddInteger(stmt_class);
        for(unsigned i = 0; i != num_args; i += 1)
            Visit(node->getArg(i));
        if(stmt_class == Stmt::UnaryOperatorClass)
            id.AddInteger(unary_op);
        else if(stmt_class == Stmt::BinaryOperatorClass ||
                stmt_class == Stmt::CompoundAssignOperatorClass)
            id.AddInteger(binary_op);
        else
            assert(stmt_class == Stmt::ArraySubscriptExprClass ||
                   stmt_class == Stmt::CallExprClass);

        return;
    }

    VisitCallExpr(node);
    id.AddInteger(node->getOperator());
}

void ExprHasher::VisitCXXRewrittenBinaryOperator(const CXXRewrittenBinaryOperator* node) {
    // If a rewritten operator were ever to be type-dependent, we should profile
    // it following its syntactic operator.
    assert(!node->isTypeDependent() &&
           "resolved rewritten operator should never be type-dependent");
    id.AddBoolean(node->isReversed());
    VisitExpr(node->getSemanticForm());
}

void ExprHasher::VisitCXXMemberCallExpr(const CXXMemberCallExpr* node) {
    VisitCallExpr(node);
}

void ExprHasher::VisitCUDAKernelCallExpr(const CUDAKernelCallExpr* node) {
    VisitCallExpr(node);
}

void ExprHasher::VisitAsTypeExpr(const AsTypeExpr* node) {
    VisitExpr(node);
}

void ExprHasher::VisitCXXNamedCastExpr(const CXXNamedCastExpr* node) {
    VisitExplicitCastExpr(node);
}

void ExprHasher::VisitCXXStaticCastExpr(const CXXStaticCastExpr* node) {
    VisitCXXNamedCastExpr(node);
}

void ExprHasher::VisitCXXDynamicCastExpr(const CXXDynamicCastExpr* node) {
    VisitCXXNamedCastExpr(node);
}

void ExprHasher::VisitCXXConstCastExpr(const CXXConstCastExpr* node) {
    VisitCXXNamedCastExpr(node);
}

void ExprHasher::VisitBuiltinBitCastExpr(const BuiltinBitCastExpr* node) {
    VisitExpr(node);
    leaves.add_type(node->getTypeInfoAsWritten()->getType());
}

void ExprHasher::VisitCXXAddrspaceCastExpr(const CXXAddrspaceCastExpr* node) {
    VisitCXXNamedCastExpr(node);
}

void ExprHasher::VisitUserDefinedLiteral(const UserDefinedLiteral* node) {
    VisitCallExpr(node);
}

void ExprHasher::VisitCXXBoolLiteralExpr(const CXXBoolLiteralExpr* node) {
    VisitExpr(node);
    id.AddBoolean(node->getValue());
}

void ExprHasher::VisitCXXNullPtrLiteralExpr(const CXXNullPtrLiteralExpr* node) {
    VisitExpr(node);
}

void ExprHasher::VisitCXXStdInitializerListExpr(const CXXStdInitializerListExpr* node) {
    VisitExpr(node);
}

void ExprHasher::VisitCXXTypeidExpr(const CXXTypeidExpr* node) {
    VisitExpr(node);
    if(node->isTypeOperand())
        leaves.add_type(node->getTypeOperandSourceInfo()->getType());
}

void ExprHasher::VisitCXXUuidofExpr(const CXXUuidofExpr* node) {
    VisitExpr(node);
    if(node->isTypeOperand())
        leaves.add_type(node->getTypeOperandSourceInfo()->getType());
}

void ExprHasher::VisitMSPropertyRefExpr(const MSPropertyRefExpr* node) {
    VisitExpr(node);
    leaves.add_decl(node->getPropertyDecl());
}

void ExprHasher::VisitMSPropertySubscriptExpr(const MSPropertySubscriptExpr* node) {
    VisitExpr(node);
}

void ExprHasher::VisitCXXThisExpr(const CXXThisExpr* node) {
    VisitExpr(node);
    id.AddBoolean(node->isImplicit());
    id.AddBoolean(node->isCapturedByCopyInLambdaWithExplicitObjectParameter());
}

void ExprHasher::VisitCXXThrowExpr(const CXXThrowExpr* node) {
    VisitExpr(node);
}

void ExprHasher::VisitCXXDefaultArgExpr(const CXXDefaultArgExpr* node) {
    VisitExpr(node);
    leaves.add_decl(node->getParam());
}

void ExprHasher::VisitCXXDefaultInitExpr(const CXXDefaultInitExpr* node) {
    VisitExpr(node);
    leaves.add_decl(node->getField());
}

void ExprHasher::VisitCXXBindTemporaryExpr(const CXXBindTemporaryExpr* node) {
    VisitExpr(node);
    leaves.add_decl(node->getTemporary()->getDestructor());
}

void ExprHasher::VisitCXXConstructExpr(const CXXConstructExpr* node) {
    VisitExpr(node);
    leaves.add_decl(node->getConstructor());
    id.AddBoolean(node->isElidable());
}

void ExprHasher::VisitCXXInheritedCtorInitExpr(const CXXInheritedCtorInitExpr* node) {
    VisitExpr(node);
    leaves.add_decl(node->getConstructor());
}

void ExprHasher::VisitCXXFunctionalCastExpr(const CXXFunctionalCastExpr* node) {
    VisitExplicitCastExpr(node);
}

void ExprHasher::VisitLambdaExpr(const LambdaExpr* node) {
    // Do not recursively visit the children of this expression. Profiling the
    // body would result in unnecessary work, and is not safe to do during
    // deserialization.
    visit_stmt_class(node);

    // C++20 [temp.over.link]p5:
    //   Two lambda-expressions are never considered equivalent.
    leaves.add_decl(node->getLambdaClass());
}

void ExprHasher::VisitCXXDeleteExpr(const CXXDeleteExpr* node) {
    VisitExpr(node);
    id.AddBoolean(node->isGlobalDelete());
    id.AddBoolean(node->isArrayForm());
    leaves.add_decl(node->getOperatorDelete());
}

void ExprHasher::VisitCXXNewExpr(const CXXNewExpr* node) {
    VisitExpr(node);
    leaves.add_type(node->getAllocatedType());
    leaves.add_decl(node->getOperatorNew());
    leaves.add_decl(node->getOperatorDelete());
    id.AddBoolean(node->isArray());
    id.AddInteger(node->getNumPlacementArgs());
    id.AddBoolean(node->isGlobalNew());
    id.AddBoolean(node->isParenTypeId());
    id.AddInteger(llvm::to_underlying(node->getInitializationStyle()));
}

void ExprHasher::VisitCXXPseudoDestructorExpr(const CXXPseudoDestructorExpr* node) {
    VisitExpr(node);
    id.AddBoolean(node->isArrow());
    leaves.add_nested_name_specifier(node->getQualifier());
    id.AddBoolean(node->getScopeTypeInfo() != nullptr);
    if(node->getScopeTypeInfo())
        leaves.add_type(node->getScopeTypeInfo()->getType());
    id.AddBoolean(node->getDestroyedTypeInfo() != nullptr);
    if(node->getDestroyedTypeInfo())
        leaves.add_type(node->getDestroyedType());
    else
        leaves.add_identifier(node->getDestroyedTypeIdentifier());
}

void ExprHasher::VisitOverloadExpr(const OverloadExpr* node) {
    VisitExpr(node);
    bool describing_dependent_var_template =
        node->getNumDecls() == 1 && isa<VarTemplateDecl>(*node->decls_begin());
    if(describing_dependent_var_template) {
        leaves.add_decl(*node->decls_begin());
    } else {
        leaves.add_nested_name_specifier(node->getQualifier());
        leaves.add_name(node->getName(), /*treat_as_decl=*/true);
    }
    id.AddBoolean(node->hasExplicitTemplateArgs());
    if(node->hasExplicitTemplateArgs())
        VisitTemplateArguments(node->getTemplateArgs(), node->getNumTemplateArgs());
}

void ExprHasher::VisitTypeTraitExpr(const TypeTraitExpr* node) {
    VisitExpr(node);
    id.AddInteger(node->getTrait());
    id.AddInteger(node->getNumArgs());
    for(unsigned i = 0, count = node->getNumArgs(); i != count; i += 1)
        leaves.add_type(node->getArg(i)->getType());
}

void ExprHasher::VisitArrayTypeTraitExpr(const ArrayTypeTraitExpr* node) {
    VisitExpr(node);
    id.AddInteger(node->getTrait());
    leaves.add_type(node->getQueriedType());
}

void ExprHasher::VisitExpressionTraitExpr(const ExpressionTraitExpr* node) {
    VisitExpr(node);
    id.AddInteger(node->getTrait());
    VisitExpr(node->getQueriedExpression());
}

void ExprHasher::VisitDependentScopeDeclRefExpr(const DependentScopeDeclRefExpr* node) {
    VisitExpr(node);
    leaves.add_name(node->getDeclName(), false);
    leaves.add_nested_name_specifier(node->getQualifier());
    id.AddBoolean(node->hasExplicitTemplateArgs());
    if(node->hasExplicitTemplateArgs())
        VisitTemplateArguments(node->getTemplateArgs(), node->getNumTemplateArgs());
}

void ExprHasher::VisitExprWithCleanups(const ExprWithCleanups* node) {
    VisitExpr(node);
}

void ExprHasher::VisitCXXUnresolvedConstructExpr(const CXXUnresolvedConstructExpr* node) {
    VisitExpr(node);
    leaves.add_type(node->getTypeAsWritten());
    id.AddInteger(node->isListInitialization());
}

void ExprHasher::VisitCXXDependentScopeMemberExpr(const CXXDependentScopeMemberExpr* node) {
    id.AddBoolean(node->isImplicitAccess());
    if(!node->isImplicitAccess()) {
        VisitExpr(node);
        id.AddBoolean(node->isArrow());
    }
    leaves.add_nested_name_specifier(node->getQualifier());
    leaves.add_name(node->getMember(), false);
    id.AddBoolean(node->hasExplicitTemplateArgs());
    if(node->hasExplicitTemplateArgs())
        VisitTemplateArguments(node->getTemplateArgs(), node->getNumTemplateArgs());
}

void ExprHasher::VisitUnresolvedMemberExpr(const UnresolvedMemberExpr* node) {
    id.AddBoolean(node->isImplicitAccess());
    if(!node->isImplicitAccess()) {
        VisitExpr(node);
        id.AddBoolean(node->isArrow());
    }
    leaves.add_nested_name_specifier(node->getQualifier());
    leaves.add_name(node->getMemberName(), false);
    id.AddBoolean(node->hasExplicitTemplateArgs());
    if(node->hasExplicitTemplateArgs())
        VisitTemplateArguments(node->getTemplateArgs(), node->getNumTemplateArgs());
}

void ExprHasher::VisitCXXNoexceptExpr(const CXXNoexceptExpr* node) {
    VisitExpr(node);
}

void ExprHasher::VisitPackExpansionExpr(const PackExpansionExpr* node) {
    VisitExpr(node);
}

void ExprHasher::VisitSizeOfPackExpr(const SizeOfPackExpr* node) {
    VisitExpr(node);
    if(node->isPartiallySubstituted()) {
        auto arguments = node->getPartialArguments();
        id.AddInteger(arguments.size());
        for(const auto& argument: arguments)
            VisitTemplateArgument(argument);
    } else {
        leaves.add_decl(node->getPack());
        id.AddInteger(0);
    }
}

void ExprHasher::VisitPackIndexingExpr(const PackIndexingExpr* expr) {
    VisitExpr(expr->getIndexExpr());

    if(expr->expandsToEmptyPack() || expr->getExpressions().size() != 0) {
        id.AddInteger(expr->getExpressions().size());
        for(const Expr* sub: expr->getExpressions())
            Visit(sub);
    } else {
        VisitExpr(expr->getPackIdExpression());
    }
}

void ExprHasher::VisitSubstNonTypeTemplateParmPackExpr(
    const SubstNonTypeTemplateParmPackExpr* node) {
    VisitExpr(node);
    leaves.add_decl(node->getParameterPack());
    VisitTemplateArgument(node->getArgumentPack());
}

void ExprHasher::VisitSubstNonTypeTemplateParmExpr(const SubstNonTypeTemplateParmExpr* expr) {
    Visit(expr->getReplacement());
}

void ExprHasher::VisitFunctionParmPackExpr(const FunctionParmPackExpr* node) {
    VisitExpr(node);
    leaves.add_decl(node->getParameterPack());
    id.AddInteger(node->getNumExpansions());
    for(FunctionParmPackExpr::iterator i = node->begin(), end = node->end(); i != end; ++i)
        leaves.add_decl(*i);
}

void ExprHasher::VisitMaterializeTemporaryExpr(const MaterializeTemporaryExpr* node) {
    VisitExpr(node);
}

void ExprHasher::VisitCXXFoldExpr(const CXXFoldExpr* node) {
    visit_stmt_class(node);
    // The callee sub-expression is not part of how the expression is written,
    // so it's not added to the profile.
    //
    // Example:
    // template <typename... T> requires ((sizeof(T) > 0) && ...) void f() {}
    // class A;
    // void operator&&(A, A);
    // template <typename... T> requires ((sizeof(T) > 0) && ...) void f() {}
    //
    // Both definitions have identically written fold expressions, but semantic
    // analysis adds the overloaded operator to the second one.
    if(node->getLHS())
        Visit(node->getLHS());
    else
        id.AddInteger(0);
    if(node->getRHS())
        Visit(node->getRHS());
    else
        id.AddInteger(0);
    id.AddInteger(node->getOperator());
}

void ExprHasher::VisitCXXParenListInitExpr(const CXXParenListInitExpr* node) {
    VisitExpr(node);
}

void ExprHasher::VisitCoroutineBodyStmt(const CoroutineBodyStmt* node) {
    VisitStmt(node);
}

void ExprHasher::VisitCoreturnStmt(const CoreturnStmt* node) {
    VisitStmt(node);
}

void ExprHasher::VisitCoawaitExpr(const CoawaitExpr* node) {
    VisitExpr(node);
}

void ExprHasher::VisitDependentCoawaitExpr(const DependentCoawaitExpr* node) {
    VisitExpr(node);
}

void ExprHasher::VisitCoyieldExpr(const CoyieldExpr* node) {
    VisitExpr(node);
}

void ExprHasher::VisitOpaqueValueExpr(const OpaqueValueExpr* expr) {
    VisitExpr(expr);
}

void ExprHasher::VisitSourceLocExpr(const SourceLocExpr* expr) {
    VisitExpr(expr);
}

void ExprHasher::VisitEmbedExpr(const EmbedExpr* expr) {
    VisitExpr(expr);
}

void ExprHasher::VisitRecoveryExpr(const RecoveryExpr* expr) {
    VisitExpr(expr);
}

void ExprHasher::VisitTemplateArguments(const TemplateArgumentLoc* arguments, unsigned num_args) {
    id.AddInteger(num_args);
    for(unsigned i = 0; i != num_args; i += 1)
        VisitTemplateArgument(arguments[i].getArgument());
}

void ExprHasher::VisitTemplateArgument(const TemplateArgument& argument) {
    id.AddInteger(argument.getKind());
    switch(argument.getKind()) {
        case TemplateArgument::Null: break;

        case TemplateArgument::Type: leaves.add_type(argument.getAsType()); break;

        case TemplateArgument::Template:
        case TemplateArgument::TemplateExpansion:
            leaves.add_template_name(argument.getAsTemplateOrTemplatePattern());
            break;

        case TemplateArgument::Declaration:
            leaves.add_type(argument.getParamTypeForDecl());

            leaves.add_decl(argument.getAsDecl());
            break;

        case TemplateArgument::NullPtr: leaves.add_type(argument.getNullPtrType()); break;

        case TemplateArgument::Integral:
            leaves.add_type(argument.getIntegralType());
            argument.getAsIntegral().Profile(id);
            break;

        case TemplateArgument::StructuralValue:
            leaves.add_structural_value(argument.getStructuralValueType(),
                                        argument.getAsStructuralValue());
            break;

        case TemplateArgument::Expression: Visit(argument.getAsExpr()); break;

        case TemplateArgument::Pack:
            for(const auto& element: argument.pack_elements())
                VisitTemplateArgument(element);
            break;
    }
}

void ExprHasher::VisitHLSLOutArgExpr(const HLSLOutArgExpr* node) {
    VisitStmt(node);
}

}  // namespace clice
