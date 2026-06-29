/// Ported from clangd's FindTarget.cpp (llvmorg-21.1.8), part of the LLVM
/// project, licensed under Apache License v2.0 with LLVM Exceptions.
/// See https://llvm.org/LICENSE.txt for license information.

#include "semantic/find_target.h"

#include <cassert>
#include <cstddef>
#include <utility>

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/ErrorHandling.h"
#include "clang/AST/ASTConcept.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/ASTTypeTraits.h"
#include "clang/AST/Decl.h"
#include "clang/AST/DeclBase.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/DeclObjC.h"
#include "clang/AST/DeclTemplate.h"
#include "clang/AST/DeclarationName.h"
#include "clang/AST/Expr.h"
#include "clang/AST/ExprCXX.h"
#include "clang/AST/ExprConcepts.h"
#include "clang/AST/ExprObjC.h"
#include "clang/AST/NestedNameSpecifier.h"
#include "clang/AST/StmtVisitor.h"
#include "clang/AST/TemplateBase.h"
#include "clang/AST/Type.h"
#include "clang/AST/TypeLoc.h"
#include "clang/AST/TypeVisitor.h"
#include "clang/Basic/Specifiers.h"
#include "clang/Sema/HeuristicResolver.h"

namespace clice::ast {

namespace {

const clang::NamedDecl* template_pattern(const clang::NamedDecl* decl) {
    if(const auto* record = llvm::dyn_cast<clang::CXXRecordDecl>(decl)) {
        if(const auto* result = record->getTemplateInstantiationPattern()) {
            return result;
        }
        /// getTemplateInstantiationPattern returns null if the Specialization is
        /// incomplete (e.g. the type didn't need to be complete), fall back to
        /// the primary template.
        if(record->getTemplateSpecializationKind() == clang::TSK_Undeclared) {
            if(const auto* spec = llvm::dyn_cast<clang::ClassTemplateSpecializationDecl>(record)) {
                return spec->getSpecializedTemplate()->getTemplatedDecl();
            }
        }
    } else if(const auto* function = llvm::dyn_cast<clang::FunctionDecl>(decl)) {
        return function->getTemplateInstantiationPattern();
    } else if(const auto* var = llvm::dyn_cast<clang::VarDecl>(decl)) {
        /// Hmm: getTIP returns its arg if it's not an instantiation?!
        clang::VarDecl* pattern = var->getTemplateInstantiationPattern();
        return (pattern == decl) ? nullptr : pattern;
    } else if(const auto* enum_decl = llvm::dyn_cast<clang::EnumDecl>(decl)) {
        return enum_decl->getInstantiatedFromMemberEnum();
    } else if(llvm::isa<clang::FieldDecl>(decl) || llvm::isa<clang::TypedefNameDecl>(decl)) {
        if(const auto* parent = llvm::dyn_cast<clang::NamedDecl>(decl->getDeclContext())) {
            if(const auto* parent_pattern =
                   llvm::dyn_cast_or_null<clang::DeclContext>(template_pattern(parent))) {
                for(const clang::NamedDecl* base_decl:
                    parent_pattern->lookup(decl->getDeclName())) {
                    if(!base_decl->isImplicit() && base_decl->getKind() == decl->getKind()) {
                        return base_decl;
                    }
                }
            }
        }
    } else if(const auto* enum_constant = llvm::dyn_cast<clang::EnumConstantDecl>(decl)) {
        if(const auto* enum_decl =
               llvm::dyn_cast<clang::EnumDecl>(enum_constant->getDeclContext())) {
            if(const clang::EnumDecl* pattern = enum_decl->getInstantiatedFromMemberEnum()) {
                for(const clang::NamedDecl* base_constant:
                    pattern->lookup(enum_constant->getDeclName())) {
                    return base_constant;
                }
            }
        }
    }
    return nullptr;
}

/// Returns true if the `TypedefNameDecl` should not be reported.
bool should_skip_typedef(const clang::TypedefNameDecl* typedef_decl) {
    /// These should be treated as keywords rather than decls - the typedef is
    /// an odd implementation detail.
    if(typedef_decl == typedef_decl->getASTContext().getObjCInstanceTypeDecl() ||
       typedef_decl == typedef_decl->getASTContext().getObjCIdDecl()) {
        return true;
    }
    return false;
}

/// TargetFinder locates the entities that an AST node refers to.
///
/// Typically this is (possibly) one declaration and (possibly) one type, but
/// may be more:
///  - for ambiguous nodes like OverloadExpr
///  - if we want to include e.g. both typedefs and the underlying type
///
/// This is organized as a set of mutually recursive helpers for particular node
/// types, but for most nodes this is a short walk rather than a deep traversal.
///
/// It's tempting to do e.g. typedef resolution as a second normalization step,
/// after finding the 'primary' decl etc. But we do this monolithically instead
/// because:
///  - normalization may require these traversals again (e.g. unwrapping a
///    typedef reveals a decltype which must be traversed)
///  - it doesn't simplify that much, e.g. the first stage must still be able
///    to yield multiple decls to handle OverloadExpr
///  - there are cases where it's required for correctness. e.g:
///      template<class X> using pvec = vector<x*>; pvec<int> x;
///    There's no Decl `pvec<int>`, we must choose `pvec<X>` or `vector<int*>`
///    and both are lossy. We must know upfront what the caller ultimately
///    wants.
struct TargetFinder {
    using RelSet = DeclRelationSet;
    using Rel = DeclRelation;

private:
    const clang::HeuristicResolver* resolver;
    llvm::SmallDenseMap<const clang::NamedDecl*, std::pair<RelSet, /*InsertionOrder*/ std::size_t>>
        decls;
    llvm::SmallDenseMap<const clang::Decl*, RelSet> seen;

    void report(const clang::NamedDecl* decl, RelSet flags) {
        auto it = decls.try_emplace(decl, std::make_pair(flags, decls.size()));
        /// If already exists, update the flags.
        if(!it.second) {
            it.first->second.first |= flags;
        }
    }

public:
    TargetFinder(const clang::HeuristicResolver* resolver) : resolver(resolver) {}

    llvm::SmallVector<std::pair<const clang::NamedDecl*, RelSet>, 1> take_decls() const {
        using ValTy = std::pair<const clang::NamedDecl*, RelSet>;
        llvm::SmallVector<ValTy, 1> result;
        result.resize(decls.size());
        for(const auto& elem: decls) {
            result[elem.second.second] = {elem.first, elem.second.first};
        }
        return result;
    }

    void add(const clang::Decl* dcl, RelSet flags) {
        const auto* decl = llvm::dyn_cast_or_null<clang::NamedDecl>(dcl);
        if(!decl) {
            return;
        }

        /// Avoid recursion (which can arise in the presence of heuristic
        /// resolution of dependent names) by exiting early if we have
        /// already seen this decl with all flags in `flags`.
        auto res = seen.try_emplace(decl);
        if(!res.second && res.first->second.contains(flags)) {
            return;
        }
        res.first->second |= flags;

        if(const auto* using_directive = llvm::dyn_cast<clang::UsingDirectiveDecl>(decl)) {
            decl = using_directive->getNominatedNamespaceAsWritten();
        }

        if(const auto* typedef_decl = llvm::dyn_cast<clang::TypedefNameDecl>(decl)) {
            add(typedef_decl->getUnderlyingType(), flags | Rel::Underlying);
            flags |= Rel::Alias;  /// continue with the alias.
        } else if(const auto* using_decl = llvm::dyn_cast<clang::UsingDecl>(decl)) {
            /// no Underlying as this is a non-renaming alias.
            for(const clang::UsingShadowDecl* shadow: using_decl->shadows()) {
                add(shadow->getUnderlyingDecl(), flags);
            }
            flags |= Rel::Alias;  /// continue with the alias.
        } else if(const auto* using_enum = llvm::dyn_cast<clang::UsingEnumDecl>(decl)) {
            /// UsingEnumDecl is not an alias at all, just a reference.
            decl = using_enum->getEnumDecl();
        } else if(const auto* namespace_alias = llvm::dyn_cast<clang::NamespaceAliasDecl>(decl)) {
            add(namespace_alias->getUnderlyingDecl(), flags | Rel::Underlying);
            flags |= Rel::Alias;  /// continue with the alias
        } else if(const auto* unresolved_using =
                      llvm::dyn_cast<clang::UnresolvedUsingValueDecl>(decl)) {
            if(resolver) {
                for(const clang::NamedDecl* target:
                    resolver->resolveUsingValueDecl(unresolved_using)) {
                    add(target, flags);  /// no Underlying as this is a non-renaming alias
                }
            }
            flags |= Rel::Alias;  /// continue with the alias
        } else if(llvm::isa<clang::UnresolvedUsingTypenameDecl>(decl)) {
            /// FIXME: improve common dependent scope using name lookup in
            /// primary templates.
            flags |= Rel::Alias;
        } else if(const auto* shadow = llvm::dyn_cast<clang::UsingShadowDecl>(decl)) {
            /// Include the introducing UsingDecl, but don't traverse it. This
            /// may end up including *all* shadows, which we don't want.
            /// Don't apply this logic to UsingEnumDecl, which can't easily be
            /// conflated with the aliases it introduces.
            if(llvm::isa<clang::UsingDecl>(shadow->getIntroducer())) {
                report(shadow->getIntroducer(), flags | Rel::Alias);
            }
            /// Shadow decls are synthetic and not themselves interesting.
            /// Record the underlying decl instead, if allowed.
            decl = shadow->getTargetDecl();
        } else if(const auto* deduction_guide =
                      llvm::dyn_cast<clang::CXXDeductionGuideDecl>(decl)) {
            decl = deduction_guide->getDeducedTemplate();
        } else if(const auto* impl = llvm::dyn_cast<clang::ObjCImplementationDecl>(decl)) {
            /// Treat ObjC{Interface,Implementation}Decl as if they were a
            /// decl/def pair as long as the interface isn't implicit.
            if(const auto* interface = impl->getClassInterface()) {
                if(const auto* definition = interface->getDefinition()) {
                    if(!definition->isImplicitInterfaceDecl()) {
                        decl = definition;
                    }
                }
            }
        } else if(const auto* category_impl = llvm::dyn_cast<clang::ObjCCategoryImplDecl>(decl)) {
            /// Treat ObjC{Category,CategoryImpl}Decl as if they were a decl/def
            /// pair.
            decl = category_impl->getCategoryDecl();
        }
        if(!decl) {
            return;
        }

        if(const clang::Decl* pattern = template_pattern(decl)) {
            assert(pattern != decl);
            add(pattern, flags | Rel::TemplatePattern);
            /// Now continue with the instantiation.
            flags |= Rel::TemplateInstantiation;
        }

        report(decl, flags);
    }

    void add(const clang::Stmt* stmt, RelSet flags) {
        if(!stmt) {
            return;
        }

        struct Visitor : public clang::ConstStmtVisitor<Visitor> {
            TargetFinder& outer;
            RelSet flags;

            Visitor(TargetFinder& outer, RelSet flags) : outer(outer), flags(flags) {}

            void VisitCallExpr(const clang::CallExpr* expr) {
                outer.add(expr->getCalleeDecl(), flags);
            }

            void VisitConceptSpecializationExpr(const clang::ConceptSpecializationExpr* expr) {
                outer.add(expr->getConceptReference(), flags);
            }

            void VisitDeclRefExpr(const clang::DeclRefExpr* expr) {
                const clang::Decl* decl = expr->getDecl();
                /// UsingShadowDecl allows us to record the UsingDecl.
                /// getFoundDecl() returns the wrong thing in other cases
                /// (templates).
                if(auto* shadow = llvm::dyn_cast<clang::UsingShadowDecl>(expr->getFoundDecl())) {
                    decl = shadow;
                }
                outer.add(decl, flags);
            }

            void VisitMemberExpr(const clang::MemberExpr* expr) {
                const clang::Decl* decl = expr->getMemberDecl();
                if(auto* shadow =
                       llvm::dyn_cast<clang::UsingShadowDecl>(expr->getFoundDecl().getDecl())) {
                    decl = shadow;
                }
                outer.add(decl, flags);
            }

            void VisitOverloadExpr(const clang::OverloadExpr* expr) {
                for(auto* decl: expr->decls()) {
                    outer.add(decl, flags);
                }
            }

            void VisitSizeOfPackExpr(const clang::SizeOfPackExpr* expr) {
                outer.add(expr->getPack(), flags);
            }

            void VisitCXXConstructExpr(const clang::CXXConstructExpr* expr) {
                outer.add(expr->getConstructor(), flags);
            }

            void VisitDesignatedInitExpr(const clang::DesignatedInitExpr* expr) {
                for(const clang::DesignatedInitExpr::Designator& designator:
                    llvm::reverse(expr->designators())) {
                    if(designator.isFieldDesignator()) {
                        outer.add(designator.getFieldDecl(), flags);
                        /// We don't know which designator was intended, we
                        /// assume the outer.
                        break;
                    }
                }
            }

            void VisitGotoStmt(const clang::GotoStmt* stmt) {
                if(auto* label = stmt->getLabel()) {
                    outer.add(label, flags);
                }
            }

            void VisitLabelStmt(const clang::LabelStmt* stmt) {
                if(auto* label = stmt->getDecl()) {
                    outer.add(label, flags);
                }
            }

            void VisitCXXDependentScopeMemberExpr(const clang::CXXDependentScopeMemberExpr* expr) {
                if(outer.resolver) {
                    for(const clang::NamedDecl* decl: outer.resolver->resolveMemberExpr(expr)) {
                        outer.add(decl, flags);
                    }
                }
            }

            void VisitDependentScopeDeclRefExpr(const clang::DependentScopeDeclRefExpr* expr) {
                if(outer.resolver) {
                    for(const clang::NamedDecl* decl: outer.resolver->resolveDeclRefExpr(expr)) {
                        outer.add(decl, flags);
                    }
                }
            }

            void VisitObjCIvarRefExpr(const clang::ObjCIvarRefExpr* expr) {
                outer.add(expr->getDecl(), flags);
            }

            void VisitObjCMessageExpr(const clang::ObjCMessageExpr* expr) {
                outer.add(expr->getMethodDecl(), flags);
            }

            void VisitObjCPropertyRefExpr(const clang::ObjCPropertyRefExpr* expr) {
                if(expr->isExplicitProperty()) {
                    outer.add(expr->getExplicitProperty(), flags);
                } else {
                    if(expr->isMessagingGetter()) {
                        outer.add(expr->getImplicitPropertyGetter(), flags);
                    }
                    if(expr->isMessagingSetter()) {
                        outer.add(expr->getImplicitPropertySetter(), flags);
                    }
                }
            }

            void VisitObjCProtocolExpr(const clang::ObjCProtocolExpr* expr) {
                outer.add(expr->getProtocol(), flags);
            }

            void VisitOpaqueValueExpr(const clang::OpaqueValueExpr* expr) {
                outer.add(expr->getSourceExpr(), flags);
            }

            void VisitPseudoObjectExpr(const clang::PseudoObjectExpr* expr) {
                outer.add(expr->getSyntacticForm(), flags);
            }

            void VisitCXXNewExpr(const clang::CXXNewExpr* expr) {
                outer.add(expr->getOperatorNew(), flags);
            }

            void VisitCXXDeleteExpr(const clang::CXXDeleteExpr* expr) {
                outer.add(expr->getOperatorDelete(), flags);
            }

            void VisitCXXRewrittenBinaryOperator(const clang::CXXRewrittenBinaryOperator* expr) {
                outer.add(expr->getDecomposedForm().InnerBinOp, flags);
            }
        };

        Visitor(*this, flags).Visit(stmt);
    }

    void add(clang::QualType type, RelSet flags) {
        if(type.isNull()) {
            return;
        }

        struct Visitor : public clang::TypeVisitor<Visitor> {
            TargetFinder& outer;
            RelSet flags;

            Visitor(TargetFinder& outer, RelSet flags) : outer(outer), flags(flags) {}

            void VisitTagType(const clang::TagType* type) {
                outer.add(type->getAsTagDecl(), flags);
            }

            void VisitUsingType(const clang::UsingType* type) {
                outer.add(type->getDecl(), flags);
            }

            void VisitInjectedClassNameType(const clang::InjectedClassNameType* type) {
                outer.add(type->getDecl(), flags);
            }

            void VisitDecltypeType(const clang::DecltypeType* type) {
                outer.add(type->getUnderlyingType(), flags | Rel::Underlying);
            }

            void VisitDeducedType(const clang::DeducedType* type) {
                /// FIXME: In practice this doesn't work: the AutoType you find
                /// inside TypeLoc never has a deduced type.
                /// https://llvm.org/PR42914
                outer.add(type->getDeducedType(), flags);
            }

            void VisitUnresolvedUsingType(const clang::UnresolvedUsingType* type) {
                outer.add(type->getDecl(), flags);
            }

            void VisitDeducedTemplateSpecializationType(
                const clang::DeducedTemplateSpecializationType* type) {
                if(const auto* shadow = type->getTemplateName().getAsUsingShadowDecl()) {
                    outer.add(shadow, flags);
                }

                /// FIXME: This is a workaround for https://llvm.org/PR42914,
                /// which is causing type->getDeducedType() to be empty. We
                /// fall back to the template pattern and miss the instantiation
                /// even when it's known in principle. Once that bug is fixed,
                /// the following code can be removed (the existing handling in
                /// VisitDeducedType() is sufficient).
                if(auto* template_decl = type->getTemplateName().getAsTemplateDecl()) {
                    outer.add(template_decl->getTemplatedDecl(), flags | Rel::TemplatePattern);
                }
            }

            void VisitDependentNameType(const clang::DependentNameType* type) {
                if(outer.resolver) {
                    for(const clang::NamedDecl* decl:
                        outer.resolver->resolveDependentNameType(type)) {
                        outer.add(decl, flags);
                    }
                }
            }

            void VisitTypedefType(const clang::TypedefType* type) {
                if(should_skip_typedef(type->getDecl())) {
                    return;
                }
                outer.add(type->getDecl(), flags);
            }

            void VisitTemplateSpecializationType(const clang::TemplateSpecializationType* type) {
                /// Have to handle these case-by-case.

                /// For dependent template specializations, try heuristic resolution.
                if(type->isDependentType() && outer.resolver) {
                    for(const clang::NamedDecl* decl:
                        outer.resolver->resolveTemplateSpecializationType(type)) {
                        outer.add(decl, flags);
                    }
                }

                if(const auto* shadow = type->getTemplateName().getAsUsingShadowDecl()) {
                    outer.add(shadow, flags);
                }

                /// templated type aliases: there's no specialized/instantiated
                /// using decl to point to. So try to find a decl for the
                /// underlying type (after substitution), and failing that point
                /// to the (templated) using decl.
                if(type->isTypeAlias()) {
                    outer.add(type->getAliasedType(), flags | Rel::Underlying);
                    /// Don't *traverse* the alias, which would result in
                    /// traversing the template of the underlying type.

                    clang::TemplateDecl* template_decl =
                        type->getTemplateName().getAsTemplateDecl();
                    /// Builtin templates e.g. __make_integer_seq,
                    /// __type_pack_element are such that they don't have alias
                    /// *decls*. Even then, we still traverse their desugared
                    /// *types* so that instantiated decls are collected.
                    if(llvm::isa<clang::BuiltinTemplateDecl>(template_decl)) {
                        return;
                    }
                    outer.report(template_decl->getTemplatedDecl(),
                                 flags | Rel::Alias | Rel::TemplatePattern);
                }
                /// specializations of template template parameters aren't
                /// instantiated into decls, so they must refer to the parameter
                /// itself.
                else if(const auto* param = llvm::dyn_cast_or_null<clang::TemplateTemplateParmDecl>(
                            type->getTemplateName().getAsTemplateDecl())) {
                    outer.add(param, flags);
                }
                /// class template specializations have a (specialized)
                /// CXXRecordDecl.
                else if(const clang::CXXRecordDecl* record = type->getAsCXXRecordDecl()) {
                    outer.add(record, flags);  /// add(Decl) will despecialize if needed.
                } else {
                    /// fallback: the (un-specialized) declaration from primary
                    /// template.
                    if(auto* template_decl = type->getTemplateName().getAsTemplateDecl()) {
                        outer.add(template_decl->getTemplatedDecl(), flags | Rel::TemplatePattern);
                    }
                }
            }

            void VisitSubstTemplateTypeParmType(const clang::SubstTemplateTypeParmType* type) {
                outer.add(type->getReplacementType(), flags);
            }

            void VisitTemplateTypeParmType(const clang::TemplateTypeParmType* type) {
                outer.add(type->getDecl(), flags);
            }

            void VisitObjCInterfaceType(const clang::ObjCInterfaceType* type) {
                outer.add(type->getDecl(), flags);
            }
        };

        Visitor(*this, flags).Visit(type.getTypePtr());
    }

    void add(clang::NestedNameSpecifier nns, RelSet flags) {
        if(!nns) {
            return;
        }

        using Kind = clang::NestedNameSpecifier::Kind;
        switch(nns.getKind()) {
            case Kind::Namespace: add(nns.getAsNamespaceAndPrefix().Namespace, flags); return;
            case Kind::Type: add(clang::QualType(nns.getAsType(), 0), flags); return;
            case Kind::Global: return;
            case Kind::MicrosoftSuper: add(nns.getAsRecordDecl(), flags); return;
            case Kind::Null: return;
        }
        llvm_unreachable("unhandled NestedNameSpecifier::Kind");
    }

    void add(const clang::CXXCtorInitializer* init, RelSet flags) {
        if(!init) {
            return;
        }

        if(init->isAnyMemberInitializer()) {
            add(init->getAnyMember(), flags);
        }
        /// Constructor calls contain a TypeLoc node, so we don't handle them
        /// here.
    }

    void add(const clang::TemplateArgument& arg, RelSet flags) {
        /// Only used for template template arguments.
        /// For type and non-type template arguments, SelectionTree
        /// will hit a more specific node (e.g. a TypeLoc or a
        /// DeclRefExpr).
        if(arg.getKind() == clang::TemplateArgument::Template ||
           arg.getKind() == clang::TemplateArgument::TemplateExpansion) {
            if(clang::TemplateDecl* template_decl =
                   arg.getAsTemplateOrTemplatePattern().getAsTemplateDecl()) {
                report(template_decl, flags);
            }
            if(const auto* shadow = arg.getAsTemplateOrTemplatePattern().getAsUsingShadowDecl()) {
                add(shadow, flags);
            }
        }
    }

    void add(const clang::ConceptReference* ref, RelSet flags) {
        add(ref->getNamedConcept(), flags);
    }
};

}  // namespace

auto all_target_decls(const clang::DynTypedNode& node, const clang::HeuristicResolver* resolver)
    -> llvm::SmallVector<std::pair<const clang::NamedDecl*, DeclRelationSet>, 1> {
    TargetFinder finder(resolver);
    DeclRelationSet flags;
    if(const auto* decl = node.get<clang::Decl>()) {
        finder.add(decl, flags);
    } else if(const auto* stmt = node.get<clang::Stmt>()) {
        finder.add(stmt, flags);
    } else if(const auto* nns_loc = node.get<clang::NestedNameSpecifierLoc>()) {
        finder.add(nns_loc->getNestedNameSpecifier(), flags);
    } else if(const auto* nns = node.get<clang::NestedNameSpecifier>()) {
        finder.add(*nns, flags);
    } else if(const auto* type_loc = node.get<clang::TypeLoc>()) {
        finder.add(type_loc->getType(), flags);
    } else if(const auto* type = node.get<clang::QualType>()) {
        finder.add(*type, flags);
    } else if(const auto* init = node.get<clang::CXXCtorInitializer>()) {
        finder.add(init, flags);
    } else if(const auto* arg_loc = node.get<clang::TemplateArgumentLoc>()) {
        finder.add(arg_loc->getArgument(), flags);
    } else if(const auto* base = node.get<clang::CXXBaseSpecifier>()) {
        finder.add(base->getTypeSourceInfo()->getType(), flags);
    } else if(const auto* protocol_loc = node.get<clang::ObjCProtocolLoc>()) {
        finder.add(protocol_loc->getProtocol(), flags);
    } else if(const auto* ref = node.get<clang::ConceptReference>()) {
        finder.add(ref, flags);
    }
    return finder.take_decls();
}

auto target_decl(const clang::DynTypedNode& node,
                 DeclRelationSet mask,
                 const clang::HeuristicResolver* resolver)
    -> llvm::SmallVector<const clang::NamedDecl*, 1> {
    llvm::SmallVector<const clang::NamedDecl*, 1> result;
    for(const auto& entry: all_target_decls(node, resolver)) {
        if(!(entry.second & ~mask)) {
            result.push_back(entry.first);
        }
    }
    return result;
}

auto explicit_reference_targets(clang::DynTypedNode node,
                                DeclRelationSet mask,
                                const clang::HeuristicResolver* resolver)
    -> llvm::SmallVector<const clang::NamedDecl*, 1> {
    assert(!(mask & (DeclRelation::TemplatePattern | DeclRelation::TemplateInstantiation)) &&
           "explicit_reference_targets handles templates on its own");
    auto decls = all_target_decls(node, resolver);

    /// We prefer to return template instantiation, but fallback to template
    /// pattern if instantiation is not available.
    mask |= DeclRelation::TemplatePattern | DeclRelation::TemplateInstantiation;

    llvm::SmallVector<const clang::NamedDecl*, 1> template_patterns;
    llvm::SmallVector<const clang::NamedDecl*, 1> targets;
    bool seen_template_instantiations = false;
    for(auto& decl: decls) {
        if(decl.second & ~mask) {
            continue;
        }
        if(decl.second & DeclRelation::TemplatePattern) {
            template_patterns.push_back(decl.first);
            continue;
        }
        if(decl.second & DeclRelation::TemplateInstantiation) {
            seen_template_instantiations = true;
        }
        targets.push_back(decl.first);
    }
    if(!seen_template_instantiations) {
        targets.insert(targets.end(), template_patterns.begin(), template_patterns.end());
    }
    return targets;
}

}  // namespace clice::ast
