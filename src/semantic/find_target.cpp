#include "semantic/find_target.h"

#include <cassert>
#include <string>
#include <unordered_set>
#include <utility>

#include "semantic/ast_utility.h"
#include "semantic/resolver.h"

#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/raw_ostream.h"
#include "clang/AST/ASTConcept.h"
#include "clang/AST/ASTTypeTraits.h"
#include "clang/AST/Decl.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/DeclObjC.h"
#include "clang/AST/DeclTemplate.h"
#include "clang/AST/Expr.h"
#include "clang/AST/ExprCXX.h"
#include "clang/AST/ExprConcepts.h"
#include "clang/AST/ExprObjC.h"
#include "clang/AST/PrettyPrinter.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/AST/StmtVisitor.h"
#include "clang/AST/TemplateBase.h"
#include "clang/AST/Type.h"
#include "clang/AST/TypeLoc.h"
#include "clang/AST/TypeLocVisitor.h"
#include "clang/AST/TypeVisitor.h"
#include "clang/Basic/LangOptions.h"
#include "clang/Lex/Lexer.h"

// #include <cstdint>
// #include "clang/AST/ExprConcepts.h"

namespace clice {
namespace {

using Targets = llvm::SmallVector<const clang::NamedDecl*, 1>;

bool has_unmasked_relations(DeclRelationSet relations, DeclRelationSet mask) {
    return (relations.bits & ~mask.bits) != 0;
}

bool should_skip_typedef(const clang::TypedefNameDecl* decl) {
    // These should be treated as keywords rather than decls. The typedef is an
    // odd implementation detail.
    return decl == decl->getASTContext().getObjCInstanceTypeDecl() ||
           decl == decl->getASTContext().getObjCIdDecl();
}

void append_target(Targets& targets, const clang::NamedDecl* decl) {
    if(decl) {
        targets.push_back(decl);
    }
}

template <typename Range>
void append_targets(Targets& targets, const Range& decls) {
    for(const auto* decl: decls) {
        append_target(targets, decl);
    }
}

const clang::NamedDecl* get_template_pattern(const clang::NamedDecl* decl) {
    if(const auto* record = llvm::dyn_cast<clang::CXXRecordDecl>(decl)) {
        if(const auto* pattern = record->getTemplateInstantiationPattern()) {
            return pattern;
        }

        // getTemplateInstantiationPattern() returns null if the specialization
        // is incomplete, e.g. the type did not need to be complete. Fall back
        // to the primary template.
        if(record->getTemplateSpecializationKind() == clang::TSK_Undeclared) {
            if(const auto* specialization =
                   llvm::dyn_cast<clang::ClassTemplateSpecializationDecl>(record)) {
                return specialization->getSpecializedTemplate()->getTemplatedDecl();
            }
        }
    } else if(const auto* function = llvm::dyn_cast<clang::FunctionDecl>(decl)) {
        return function->getTemplateInstantiationPattern();
    } else if(const auto* variable = llvm::dyn_cast<clang::VarDecl>(decl)) {
        // Hmm: getTemplateInstantiationPattern() returns its argument if it is
        // not an instantiation.
        clang::VarDecl* pattern = variable->getTemplateInstantiationPattern();
        return pattern == decl ? nullptr : pattern;
    } else if(const auto* enumeration = llvm::dyn_cast<clang::EnumDecl>(decl)) {
        return enumeration->getInstantiatedFromMemberEnum();
    } else if(llvm::isa<clang::FieldDecl, clang::TypedefNameDecl>(decl)) {
        if(const auto* parent = llvm::dyn_cast<clang::NamedDecl>(decl->getDeclContext())) {
            if(const auto* pattern =
                   llvm::dyn_cast_or_null<clang::DeclContext>(get_template_pattern(parent))) {
                for(const auto* base_decl: pattern->lookup(decl->getDeclName())) {
                    if(!base_decl->isImplicit() && base_decl->getKind() == decl->getKind()) {
                        return base_decl;
                    }
                }
            }
        }
    } else if(const auto* enumerator = llvm::dyn_cast<clang::EnumConstantDecl>(decl)) {
        if(const auto* enumeration =
               llvm::dyn_cast<clang::EnumDecl>(enumerator->getDeclContext())) {
            if(const auto* pattern = enumeration->getInstantiatedFromMemberEnum()) {
                for(const auto* base_decl: pattern->lookup(enumerator->getDeclName())) {
                    return base_decl;
                }
            }
        }
    }

    return nullptr;
}

// TargetFinder locates the declarations that a node may refer to.
//
// Most nodes resolve to a single declaration, but some resolve to multiple
// targets:
// - ambiguous nodes such as overload sets
// - aliases where both the written alias and the underlying declaration matter
// - template references where both the instantiated decl and its pattern can
//   be relevant depending on what the caller wants to surface
//
// This is intentionally structured as a mutually-recursive normalization walk
// rather than "find a primary decl, then normalize later". Unwrapping aliases,
// following template instantiations, and handling dependent code all need the
// same traversal machinery, and splitting that into phases tends to either
// duplicate work or lose information about the written name.
struct TargetFinder {
    using RelSet = DeclRelationSet;
    using Rel = DeclRelation;

    TemplateResolver* Resolver;
    llvm::DenseMap<const clang::NamedDecl*, std::pair<RelSet, size_t>> Decls;
    llvm::DenseMap<const clang::Decl*, RelSet> Seen;

    explicit TargetFinder(TemplateResolver* resolver) : Resolver(resolver) {}

    void report(const clang::NamedDecl* decl, RelSet flags) {
        auto [iter, inserted] = Decls.try_emplace(decl, std::make_pair(flags, Decls.size()));
        // If already present, merge any newly discovered relations.
        if(!inserted) {
            iter->second.first |= flags;
        }
    }

    auto take_decls() const -> llvm::SmallVector<std::pair<const clang::NamedDecl*, RelSet>, 1> {
        using Value = std::pair<const clang::NamedDecl*, RelSet>;

        llvm::SmallVector<Value, 1> result(Decls.size());
        for(const auto& [decl, info]: Decls) {
            result[info.second] = {decl, info.first};
        }

        return result;
    }

    void add(const clang::Decl* declaration, RelSet flags) {
        auto* decl = llvm::dyn_cast_or_null<clang::NamedDecl>(declaration);
        if(!decl) {
            return;
        }

        auto [iter, inserted] = Seen.try_emplace(decl);
        // Heuristic resolution of dependent names can re-enter the same decl.
        // Once we've seen it with all requested flags, further traversal does
        // not add information and only risks recursion.
        if(!inserted && iter->second.contains(flags)) {
            return;
        }
        iter->second |= flags;

        if(const auto* directive = llvm::dyn_cast<clang::UsingDirectiveDecl>(decl)) {
            decl = directive->getNominatedNamespaceAsWritten();
        }

        if(const auto* typedef_decl = llvm::dyn_cast<clang::TypedefNameDecl>(decl)) {
            add(typedef_decl->getUnderlyingType(), flags | Rel::Underlying);
            flags |= Rel::Alias;
        } else if(const auto* using_decl = llvm::dyn_cast<clang::UsingDecl>(decl)) {
            // UsingDecl is a non-renaming alias: keep the written alias, but do
            // not mark the shadow targets as "Underlying".
            for(const auto* shadow: using_decl->shadows()) {
                add(shadow->getUnderlyingDecl(), flags);
            }
            flags |= Rel::Alias;
        } else if(const auto* using_enum = llvm::dyn_cast<clang::UsingEnumDecl>(decl)) {
            // UsingEnumDecl is not an alias at all, just a reference.
            decl = using_enum->getEnumDecl();
        } else if(const auto* namespace_alias = llvm::dyn_cast<clang::NamespaceAliasDecl>(decl)) {
            add(namespace_alias->getUnderlyingDecl(), flags | Rel::Underlying);
            flags |= Rel::Alias;
        } else if(const auto* unresolved_using =
                      llvm::dyn_cast<clang::UnresolvedUsingValueDecl>(decl)) {
            if(Resolver) {
                for(const auto* target: Resolver->lookup(unresolved_using)) {
                    add(target, flags);
                }
            }
            flags |= Rel::Alias;
        } else if(llvm::isa<clang::UnresolvedUsingTypenameDecl>(decl)) {
            // We still want the written alias to survive filtering even when we
            // cannot reliably resolve the dependent target.
            // FIXME: improve common dependent scope using name lookup in primary
            // templates.
            flags |= Rel::Alias;
        } else if(const auto* shadow = llvm::dyn_cast<clang::UsingShadowDecl>(decl)) {
            // Include the introducing UsingDecl, but do not traverse it. That
            // could pick up all shadows, which is not what we want.
            // Shadow decls themselves are synthetic. Record the underlying decl
            // instead, while still preserving the written alias target.
            if(llvm::isa<clang::UsingDecl>(shadow->getIntroducer())) {
                report(shadow->getIntroducer(), flags | Rel::Alias);
            }
            decl = shadow->getTargetDecl();
        } else if(const auto* guide = llvm::dyn_cast<clang::CXXDeductionGuideDecl>(decl)) {
            decl = guide->getDeducedTemplate();
        } else if(const auto* implementation =
                      llvm::dyn_cast<clang::ObjCImplementationDecl>(decl)) {
            // Treat ObjCInterface/ObjCImplementation as a decl/def pair as
            // long as the interface is not implicit.
            if(const auto* interface_decl = implementation->getClassInterface()) {
                if(const auto* definition = interface_decl->getDefinition()) {
                    if(!definition->isImplicitInterfaceDecl()) {
                        decl = definition;
                    }
                }
            }
        } else if(const auto* category_impl = llvm::dyn_cast<clang::ObjCCategoryImplDecl>(decl)) {
            // Treat ObjCCategory/ObjCCategoryImpl as a decl/def pair.
            decl = category_impl->getCategoryDecl();
        }

        if(!decl) {
            return;
        }

        if(const clang::NamedDecl* pattern = get_template_pattern(decl)) {
            assert(pattern != decl);
            add(pattern, flags | Rel::TemplatePattern);
            // Now continue with the instantiation. explicit_reference_targets()
            // will prefer it, but can fall back to the pattern when needed.
            flags |= Rel::TemplateInstantiation;
        }

        report(decl, flags);
    }

    void add(const clang::Stmt* statement, RelSet flags) {
        if(!statement) {
            return;
        }

        struct Visitor : clang::ConstStmtVisitor<Visitor> {
            TargetFinder& Outer;
            RelSet Flags;

            Visitor(TargetFinder& outer, RelSet flags) : Outer(outer), Flags(flags) {}

            void VisitCallExpr(const clang::CallExpr* expr) {
                Outer.add(expr->getCalleeDecl(), Flags);
            }

            void VisitConceptSpecializationExpr(const clang::ConceptSpecializationExpr* expr) {
                Outer.add(expr->getConceptReference(), Flags);
            }

            void VisitDeclRefExpr(const clang::DeclRefExpr* expr) {
                const clang::Decl* decl = expr->getDecl();
                // UsingShadowDecl lets us recover the introducing UsingDecl.
                // getFoundDecl() points at the wrong entity in other cases,
                // notably templates, so only use it for shadows.
                if(auto* shadow = llvm::dyn_cast<clang::UsingShadowDecl>(expr->getFoundDecl())) {
                    decl = shadow;
                }
                Outer.add(decl, Flags);
            }

            void VisitMemberExpr(const clang::MemberExpr* expr) {
                const clang::Decl* decl = expr->getMemberDecl();
                if(auto* shadow =
                       llvm::dyn_cast<clang::UsingShadowDecl>(expr->getFoundDecl().getDecl())) {
                    decl = shadow;
                }
                Outer.add(decl, Flags);
            }

            void VisitOverloadExpr(const clang::OverloadExpr* expr) {
                for(const auto* decl: expr->decls()) {
                    Outer.add(decl, Flags);
                }
            }

            void VisitSizeOfPackExpr(const clang::SizeOfPackExpr* expr) {
                Outer.add(expr->getPack(), Flags);
            }

            void VisitCXXConstructExpr(const clang::CXXConstructExpr* expr) {
                Outer.add(expr->getConstructor(), Flags);
            }

            void VisitDesignatedInitExpr(const clang::DesignatedInitExpr* expr) {
                for(const auto& designator: llvm::reverse(expr->designators())) {
                    if(designator.isFieldDesignator()) {
                        Outer.add(designator.getFieldDecl(), Flags);
                        // We do not know which designator was intended, so we
                        // assume the outer one.
                        break;
                    }
                }
            }

            void VisitGotoStmt(const clang::GotoStmt* statement) {
                Outer.add(statement->getLabel(), Flags);
            }

            void VisitLabelStmt(const clang::LabelStmt* statement) {
                Outer.add(statement->getDecl(), Flags);
            }

            void VisitCXXDependentScopeMemberExpr(const clang::CXXDependentScopeMemberExpr* expr) {
                if(!Outer.Resolver) {
                    return;
                }
                for(const auto* decl:
                    Outer.Resolver->lookup(const_cast<clang::CXXDependentScopeMemberExpr*>(expr))) {
                    Outer.add(decl, Flags);
                }
            }

            void VisitDependentScopeDeclRefExpr(const clang::DependentScopeDeclRefExpr* expr) {
                if(!Outer.Resolver) {
                    return;
                }
                for(const auto* decl: Outer.Resolver->lookup(expr)) {
                    Outer.add(decl, Flags);
                }
            }

            void VisitObjCIvarRefExpr(const clang::ObjCIvarRefExpr* expr) {
                Outer.add(expr->getDecl(), Flags);
            }

            void VisitObjCMessageExpr(const clang::ObjCMessageExpr* expr) {
                Outer.add(expr->getMethodDecl(), Flags);
            }

            void VisitObjCPropertyRefExpr(const clang::ObjCPropertyRefExpr* expr) {
                if(expr->isExplicitProperty()) {
                    Outer.add(expr->getExplicitProperty(), Flags);
                    return;
                }

                if(expr->isMessagingGetter()) {
                    Outer.add(expr->getImplicitPropertyGetter(), Flags);
                }
                if(expr->isMessagingSetter()) {
                    Outer.add(expr->getImplicitPropertySetter(), Flags);
                }
            }

            void VisitObjCProtocolExpr(const clang::ObjCProtocolExpr* expr) {
                Outer.add(expr->getProtocol(), Flags);
            }

            void VisitOpaqueValueExpr(const clang::OpaqueValueExpr* expr) {
                Outer.add(expr->getSourceExpr(), Flags);
            }

            void VisitPseudoObjectExpr(const clang::PseudoObjectExpr* expr) {
                Outer.add(expr->getSyntacticForm(), Flags);
            }

            void VisitCXXNewExpr(const clang::CXXNewExpr* expr) {
                Outer.add(expr->getOperatorNew(), Flags);
            }

            void VisitCXXDeleteExpr(const clang::CXXDeleteExpr* expr) {
                Outer.add(expr->getOperatorDelete(), Flags);
            }

            void VisitCXXRewrittenBinaryOperator(const clang::CXXRewrittenBinaryOperator* expr) {
                Outer.add(expr->getDecomposedForm().InnerBinOp, Flags);
            }
        };

        Visitor(*this, flags).Visit(statement);
    }

    void add(clang::QualType type, RelSet flags) {
        if(type.isNull()) {
            return;
        }

        struct Visitor : clang::TypeVisitor<Visitor> {
            TargetFinder& Outer;
            RelSet Flags;

            Visitor(TargetFinder& outer, RelSet flags) : Outer(outer), Flags(flags) {}

            void VisitTagType(const clang::TagType* type) {
                Outer.add(type->getAsTagDecl(), Flags);
            }

            void VisitElaboratedType(const clang::ElaboratedType* type) {
                Outer.add(type->desugar(), Flags);
            }

            void VisitUsingType(const clang::UsingType* type) {
                Outer.add(type->getFoundDecl(), Flags);
            }

            void VisitInjectedClassNameType(const clang::InjectedClassNameType* type) {
                Outer.add(type->getDecl(), Flags);
            }

            void VisitDecltypeType(const clang::DecltypeType* type) {
                Outer.add(type->getUnderlyingType(), Flags | Rel::Underlying);
            }

            void VisitDeducedType(const clang::DeducedType* type) {
                // FIXME: In practice this often does not work. The AutoType
                // inside TypeLoc frequently has no deduced type.
                // https://llvm.org/PR42914
                Outer.add(type->getDeducedType(), Flags);
            }

            void VisitUnresolvedUsingType(const clang::UnresolvedUsingType* type) {
                Outer.add(type->getDecl(), Flags);
            }

            void VisitDeducedTemplateSpecializationType(
                const clang::DeducedTemplateSpecializationType* type) {
                if(const auto* shadow = type->getTemplateName().getAsUsingShadowDecl()) {
                    Outer.add(shadow, Flags);
                }

                // FIXME: Work around https://llvm.org/PR42914. Clang may leave
                // getDeducedType() empty here, so we fall back to the template
                // pattern and miss the concrete instantiation even when it is
                // known in principle.
                if(const auto* template_decl = type->getTemplateName().getAsTemplateDecl()) {
                    Outer.add(template_decl->getTemplatedDecl(), Flags | Rel::TemplatePattern);
                }
            }

            void VisitDependentNameType(const clang::DependentNameType* type) {
                if(!Outer.Resolver) {
                    return;
                }
                for(const auto* decl: Outer.Resolver->lookup(type)) {
                    Outer.add(decl, Flags);
                }
            }

            void VisitDependentTemplateSpecializationType(
                const clang::DependentTemplateSpecializationType* type) {
                if(!Outer.Resolver) {
                    return;
                }
                for(const auto* decl: Outer.Resolver->lookup(type)) {
                    Outer.add(decl, Flags);
                }
            }

            void VisitTypedefType(const clang::TypedefType* type) {
                if(should_skip_typedef(type->getDecl())) {
                    return;
                }
                Outer.add(type->getDecl(), Flags);
            }

            void VisitTemplateSpecializationType(const clang::TemplateSpecializationType* type) {
                // These have to be handled case by case.
                if(const auto* shadow = type->getTemplateName().getAsUsingShadowDecl()) {
                    Outer.add(shadow, Flags);
                }

                if(type->isTypeAlias()) {
                    // Specialized alias templates such as `valias<int>` have
                    // no concrete using-decl to point at. Record the
                    // substituted underlying type, then separately preserve the
                    // alias pattern so callers can prefer the written alias.
                    Outer.add(type->getAliasedType(), Flags | Rel::Underlying);

                    // Do not traverse the alias itself: that would immediately
                    // recurse into the underlying template.
                    if(auto* template_decl = type->getTemplateName().getAsTemplateDecl()) {
                        // Builtin templates do not have alias decls. We still
                        // traverse their desugared types above so instantiated
                        // decls can be collected.
                        if(llvm::isa_and_nonnull<clang::BuiltinTemplateDecl>(template_decl)) {
                            return;
                        }
                        Outer.report(template_decl->getTemplatedDecl(),
                                     Flags | Rel::Alias | Rel::TemplatePattern);
                    }
                } else if(const auto* parameter =
                              llvm::dyn_cast_or_null<clang::TemplateTemplateParmDecl>(
                                  type->getTemplateName().getAsTemplateDecl())) {
                    // Template-template parameter specializations are not
                    // instantiated into decls, so they refer to the parameter
                    // itself.
                    Outer.add(parameter, Flags);
                } else if(const auto* record = type->getAsCXXRecordDecl()) {
                    // Class template specializations have their own
                    // specialized CXXRecordDecl.
                    Outer.add(record, Flags);
                } else if(auto* template_decl = type->getTemplateName().getAsTemplateDecl()) {
                    // Fallback to the unspecialized primary template decl.
                    Outer.add(template_decl->getTemplatedDecl(), Flags | Rel::TemplatePattern);
                }
            }

            void VisitSubstTemplateTypeParmType(const clang::SubstTemplateTypeParmType* type) {
                Outer.add(type->getReplacementType(), Flags);
            }

            void VisitTemplateTypeParmType(const clang::TemplateTypeParmType* type) {
                Outer.add(type->getDecl(), Flags);
            }

            void VisitObjCInterfaceType(const clang::ObjCInterfaceType* type) {
                Outer.add(type->getDecl(), Flags);
            }
        };

        Visitor(*this, flags).Visit(type.getTypePtr());
    }

    void add(const clang::NestedNameSpecifier* specifier, RelSet flags) {
        if(!specifier) {
            return;
        }

        switch(specifier->getKind()) {
            case clang::NestedNameSpecifier::Namespace:
                add(specifier->getAsNamespace(), flags);
                return;
            case clang::NestedNameSpecifier::NamespaceAlias:
                add(specifier->getAsNamespaceAlias(), flags);
                return;
            case clang::NestedNameSpecifier::Identifier:
                if(Resolver) {
                    for(const auto* decl:
                        Resolver->lookup(specifier->getPrefix(), specifier->getAsIdentifier())) {
                        add(decl, flags);
                    }
                }
                return;
            case clang::NestedNameSpecifier::TypeSpec:
                add(clang::QualType(specifier->getAsType(), 0), flags);
                return;
            case clang::NestedNameSpecifier::Global:
                // This would ideally target the translation unit decl, but
                // Clang does not expose a pointer to it here.
                return;
            case clang::NestedNameSpecifier::Super:
                add(specifier->getAsRecordDecl(), flags);
                return;
        }
    }

    void add(const clang::CXXCtorInitializer* initializer, RelSet flags) {
        if(!initializer) {
            return;
        }

        if(initializer->isAnyMemberInitializer()) {
            add(initializer->getAnyMember(), flags);
        }
        // Constructor calls already carry a TypeLoc, so they are handled
        // elsewhere.
    }

    void add(const clang::TemplateArgument& argument, RelSet flags) {
        // Only used for template-template arguments. Type and non-type
        // arguments are visited through more specific nodes such as TypeLoc or
        // DeclRefExpr.
        if(argument.getKind() != clang::TemplateArgument::Template &&
           argument.getKind() != clang::TemplateArgument::TemplateExpansion) {
            return;
        }

        if(auto* template_decl = argument.getAsTemplateOrTemplatePattern().getAsTemplateDecl()) {
            report(template_decl, flags);
        }
        if(const auto* shadow = argument.getAsTemplateOrTemplatePattern().getAsUsingShadowDecl()) {
            add(shadow, flags);
        }
    }

    void add(const clang::ConceptReference* reference, RelSet flags) {
        add(reference->getNamedConcept(), flags);
    }
};

}  // namespace

auto all_target_decls(const clang::DynTypedNode& node, TemplateResolver* resolver)
    -> llvm::SmallVector<TargetDecl, 1> {
    TargetFinder finder(resolver);
    DeclRelationSet flags;

    if(const auto* decl = node.get<clang::Decl>()) {
        finder.add(decl, flags);
    } else if(const auto* stmt = node.get<clang::Stmt>()) {
        finder.add(stmt, flags);
    } else if(const auto* specifier = node.get<clang::NestedNameSpecifierLoc>()) {
        finder.add(specifier->getNestedNameSpecifier(), flags);
    } else if(const auto* specifier = node.get<clang::NestedNameSpecifier>()) {
        finder.add(specifier, flags);
    } else if(const auto* type_loc = node.get<clang::TypeLoc>()) {
        finder.add(type_loc->getType(), flags);
    } else if(const auto* type = node.get<clang::QualType>()) {
        finder.add(*type, flags);
    } else if(const auto* initializer = node.get<clang::CXXCtorInitializer>()) {
        finder.add(initializer, flags);
    } else if(const auto* argument = node.get<clang::TemplateArgumentLoc>()) {
        finder.add(argument->getArgument(), flags);
    } else if(const auto* base = node.get<clang::CXXBaseSpecifier>()) {
        finder.add(base->getTypeSourceInfo()->getType(), flags);
    } else if(const auto* protocol = node.get<clang::ObjCProtocolLoc>()) {
        finder.add(protocol->getProtocol(), flags);
    } else if(const auto* concept_ref = node.get<clang::ConceptReference>()) {
        finder.add(concept_ref, flags);
    }

    auto decls = finder.take_decls();
    llvm::SmallVector<TargetDecl, 1> result;
    result.reserve(decls.size());
    for(const auto& [decl, relations]: decls) {
        result.push_back(TargetDecl{
            .Decl = decl,
            .Relations = relations,
        });
    }
    return result;
}

namespace {

// Returns the declarations that should be attached to a reference written in
// source code.
//
// Template handling is slightly special:
// - prefer concrete instantiations when available
// - otherwise fall back to the template pattern
// - preserve alias targets when the caller asks for them
Targets explicit_reference_targets(clang::DynTypedNode node,
                                   DeclRelationSet mask,
                                   TemplateResolver* resolver) {
    auto decls = all_target_decls(node, resolver);

    mask |= DeclRelation::TemplatePattern;
    mask |= DeclRelation::TemplateInstantiation;

    Targets template_patterns;
    Targets targets;
    bool seen_template_instantiations = false;

    for(const auto& decl: decls) {
        if(has_unmasked_relations(decl.Relations, mask)) {
            continue;
        }

        if(decl.Relations.contains(DeclRelation::TemplatePattern)) {
            template_patterns.push_back(decl.Decl);
            continue;
        }

        if(decl.Relations.contains(DeclRelation::TemplateInstantiation)) {
            seen_template_instantiations = true;
        }

        targets.push_back(decl.Decl);
    }

    if(!seen_template_instantiations) {
        targets.append(template_patterns.begin(), template_patterns.end());
    }

    return targets;
}

Targets explicit_reference_targets(clang::DynTypedNode node, TemplateResolver* resolver) {
    return explicit_reference_targets(node, DeclRelationSet(), resolver);
}

Targets explicit_reference_targets(clang::QualType type, TemplateResolver* resolver) {
    return explicit_reference_targets(clang::DynTypedNode::create(type), resolver);
}

Targets explicit_reference_targets(const clang::NestedNameSpecifier* specifier,
                                   TemplateResolver* resolver) {
    if(!specifier) {
        return {};
    }
    return explicit_reference_targets(clang::DynTypedNode::create(*specifier), resolver);
}

void maybe_add_named_decl_reference(const clang::NamedDecl* decl,
                                    llvm::SmallVectorImpl<ReferenceLoc>& refs) {
    // TemplateDecl wrappers share their source range with the underlying
    // declaration, which will be visited separately.
    if(llvm::isa<clang::ClassTemplateDecl,
                 clang::FunctionTemplateDecl,
                 clang::VarTemplateDecl,
                 clang::TypeAliasTemplateDecl>(decl)) {
        return;
    }

    // FIXME: decide how to surface destructors when we need them.
    if(llvm::isa<clang::CXXDestructorDecl>(decl)) {
        return;
    }

    // Anonymous decls have name locations that point outside an actual name
    // token, and downstream clients are not prepared for that.
    if(decl->getDeclName().isIdentifier() && !decl->getDeclName().getAsIdentifierInfo()) {
        return;
    }

    refs.push_back(ReferenceLoc{
        .Qualifier = ast::get_qualifier_loc(decl),
        .NameLoc = decl->getLocation(),
        .IsDecl = true,
        .Targets = {decl},
    });
}

llvm::SmallVector<ReferenceLoc> ref_in_type_loc(clang::TypeLoc type_loc,
                                                TemplateResolver* resolver);

llvm::SmallVector<ReferenceLoc> ref_in_decl(const clang::Decl* decl, TemplateResolver* resolver) {
    llvm::SmallVector<ReferenceLoc> refs;

    if(const auto* using_directive = llvm::dyn_cast<clang::UsingDirectiveDecl>(decl)) {
        // Keep this as a non-declaration reference: `using namespace` has no
        // declaration name of its own.
        refs.push_back(ReferenceLoc{
            .Qualifier = using_directive->getQualifierLoc(),
            .NameLoc = using_directive->getIdentLocation(),
            .Targets = {using_directive->getNominatedNamespaceAsWritten()},
        });
        return refs;
    }

    if(const auto* using_decl = llvm::dyn_cast<clang::UsingDecl>(decl)) {
        // `using ns::identifier;` is itself a reference, not a declaration of
        // `identifier`.
        refs.push_back(ReferenceLoc{
            .Qualifier = using_decl->getQualifierLoc(),
            .NameLoc = using_decl->getLocation(),
            // Keep the written alias target and drop the desugared underlying
            // decls from the final explicit reference.
            .Targets = explicit_reference_targets(clang::DynTypedNode::create(*using_decl),
                                                  DeclRelation::Underlying,
                                                  resolver),
        });
        return refs;
    }

    if(llvm::isa<clang::UsingEnumDecl>(decl)) {
        // `using enum ns::E` is covered by the embedded TypeLoc. Avoid the
        // default declaration reference.
        return refs;
    }

    if(const auto* namespace_alias = llvm::dyn_cast<clang::NamespaceAliasDecl>(decl)) {
        // `namespace Foo = Target;` contributes two references: the declared
        // alias name and the referenced namespace on the right-hand side.
        maybe_add_named_decl_reference(namespace_alias, refs);
        refs.push_back(ReferenceLoc{
            .Qualifier = namespace_alias->getQualifierLoc(),
            .NameLoc = namespace_alias->getTargetNameLoc(),
            .Targets = {namespace_alias->getAliasedNamespace()},
        });
        return refs;
    }

    if(const auto* deduction_guide = llvm::dyn_cast<clang::CXXDeductionGuideDecl>(decl)) {
        // The written class name in a deduction guide refers to the class
        // template rather than the guide decl itself.
        refs.push_back(ReferenceLoc{
            .Qualifier = deduction_guide->getQualifierLoc(),
            .NameLoc = deduction_guide->getNameInfo().getLoc(),
            .Targets = {deduction_guide->getDeducedTemplate()},
        });
        return refs;
    }

    if(const auto* objc_method = llvm::dyn_cast<clang::ObjCMethodDecl>(decl)) {
        // Objective-C selectors may span several tokens; we can only report
        // the first one.
        refs.push_back(ReferenceLoc{
            .NameLoc = objc_method->getSelectorStartLoc(),
            .IsDecl = true,
            .Targets = {objc_method},
        });
        return refs;
    }

    if(const auto* objc_category = llvm::dyn_cast<clang::ObjCCategoryDecl>(decl)) {
        // getLocation() points at the extended class location, not the
        // category name.
        refs.push_back(ReferenceLoc{
            .NameLoc = objc_category->getLocation(),
            .Targets = {objc_category->getClassInterface()},
        });
        refs.push_back(ReferenceLoc{
            .NameLoc = objc_category->getCategoryNameLoc(),
            .IsDecl = true,
            .Targets = {objc_category},
        });
        return refs;
    }

    if(const auto* objc_category_impl = llvm::dyn_cast<clang::ObjCCategoryImplDecl>(decl)) {
        refs.push_back(ReferenceLoc{
            .NameLoc = objc_category_impl->getLocation(),
            .Targets = {objc_category_impl->getClassInterface()},
        });
        refs.push_back(ReferenceLoc{
            .NameLoc = objc_category_impl->getCategoryNameLoc(),
            .Targets = {objc_category_impl->getCategoryDecl()},
        });
        refs.push_back(ReferenceLoc{
            .NameLoc = objc_category_impl->getCategoryNameLoc(),
            .IsDecl = true,
            .Targets = {objc_category_impl},
        });
        return refs;
    }

    if(const auto* objc_impl = llvm::dyn_cast<clang::ObjCImplementationDecl>(decl)) {
        refs.push_back(ReferenceLoc{
            .NameLoc = objc_impl->getLocation(),
            .Targets = {objc_impl->getClassInterface()},
        });
        refs.push_back(ReferenceLoc{
            .NameLoc = objc_impl->getLocation(),
            .IsDecl = true,
            .Targets = {objc_impl},
        });
        return refs;
    }

    if(const auto* named = llvm::dyn_cast<clang::NamedDecl>(decl)) {
        maybe_add_named_decl_reference(named, refs);
    }

    return refs;
}

llvm::SmallVector<ReferenceLoc> ref_in_stmt(const clang::Stmt* stmt, TemplateResolver* resolver) {
    struct Visitor : clang::ConstStmtVisitor<Visitor> {
        TemplateResolver* resolver;
        // FIXME: handle more complicated cases such as additional ObjC forms
        // and designated initializers.
        llvm::SmallVector<ReferenceLoc> refs;

        explicit Visitor(TemplateResolver* resolver) : resolver(resolver) {}

        void VisitDeclRefExpr(const clang::DeclRefExpr* expr) {
            refs.push_back(ReferenceLoc{
                .Qualifier = expr->getQualifierLoc(),
                .NameLoc = expr->getNameInfo().getLoc(),
                .Targets = {expr->getFoundDecl()},
            });
        }

        void VisitDependentScopeDeclRefExpr(const clang::DependentScopeDeclRefExpr* expr) {
            Targets targets =
                explicit_reference_targets(clang::DynTypedNode::create(*expr), resolver);

            refs.push_back(ReferenceLoc{
                .Qualifier = expr->getQualifierLoc(),
                .NameLoc = expr->getNameInfo().getLoc(),
                .IsDecl = false,
                .Targets = std::move(targets),
            });
        }

        void VisitMemberExpr(const clang::MemberExpr* expr) {
            // Skip destructor calls to avoid duplication: the corresponding
            // TypeLoc is visited separately.
            if(llvm::isa<clang::CXXDestructorDecl>(expr->getFoundDecl().getDecl())) {
                return;
            }

            refs.push_back(ReferenceLoc{
                .Qualifier = expr->getQualifierLoc(),
                .NameLoc = expr->getMemberNameInfo().getLoc(),
                .IsDecl = false,
                .Targets = {expr->getFoundDecl()},
            });
        }

        void VisitCXXDependentScopeMemberExpr(const clang::CXXDependentScopeMemberExpr* expr) {
            refs.push_back(ReferenceLoc{
                .Qualifier = expr->getQualifierLoc(),
                .NameLoc = expr->getMemberNameInfo().getLoc(),
                .IsDecl = false,
                .Targets = explicit_reference_targets(clang::DynTypedNode::create(*expr), resolver),
            });
        }

        void VisitOverloadExpr(const clang::OverloadExpr* expr) {
            Targets targets;
            for(const auto* decl: expr->decls()) {
                append_target(targets, decl);
            }
            refs.push_back(ReferenceLoc{
                .Qualifier = expr->getQualifierLoc(),
                .NameLoc = expr->getNameInfo().getLoc(),
                .IsDecl = false,
                .Targets = std::move(targets),
            });
        }

        void VisitSizeOfPackExpr(const clang::SizeOfPackExpr* expr) {
            refs.push_back(ReferenceLoc{
                .NameLoc = expr->getPackLoc(),
                .IsDecl = false,
                .Targets = {expr->getPack()},
            });
        }

        void VisitObjCPropertyRefExpr(const clang::ObjCPropertyRefExpr* expr) {
            refs.push_back(ReferenceLoc{
                .NameLoc = expr->getLocation(),
                .IsDecl = false,
                // Select the getter, setter, or @property depending on the
                // syntactic form.
                .Targets = explicit_reference_targets(clang::DynTypedNode::create(*expr), resolver),
            });
        }

        void VisitObjCIvarRefExpr(const clang::ObjCIvarRefExpr* expr) {
            refs.push_back(ReferenceLoc{
                .NameLoc = expr->getLocation(),
                .IsDecl = false,
                .Targets = {expr->getDecl()},
            });
        }

        void VisitObjCMessageExpr(const clang::ObjCMessageExpr* expr) {
            // Objective-C selectors may span several tokens; we can only report
            // the first one.
            refs.push_back(ReferenceLoc{
                .NameLoc = expr->getSelectorStartLoc(),
                .IsDecl = false,
                .Targets = {expr->getMethodDecl()},
            });
        }

        void VisitDesignatedInitExpr(const clang::DesignatedInitExpr* expr) {
            for(const auto& designator: expr->designators()) {
                if(!designator.isFieldDesignator()) {
                    continue;
                }

                refs.push_back(ReferenceLoc{
                    .NameLoc = designator.getFieldLoc(),
                    .IsDecl = false,
                    .Targets = {designator.getFieldDecl()},
                });
            }
        }

        void VisitGotoStmt(const clang::GotoStmt* stmt) {
            refs.push_back(ReferenceLoc{
                .NameLoc = stmt->getLabelLoc(),
                .IsDecl = false,
                .Targets = {stmt->getLabel()},
            });
        }

        void VisitLabelStmt(const clang::LabelStmt* stmt) {
            refs.push_back(ReferenceLoc{
                .NameLoc = stmt->getIdentLoc(),
                .IsDecl = true,
                .Targets = {stmt->getDecl()},
            });
        }
    };

    Visitor visitor(resolver);
    visitor.Visit(stmt);
    return visitor.refs;
}

llvm::SmallVector<ReferenceLoc> ref_in_type_loc(clang::TypeLoc type_loc,
                                                TemplateResolver* resolver) {
    struct Visitor : clang::TypeLocVisitor<Visitor> {
        TemplateResolver* resolver;
        llvm::SmallVector<ReferenceLoc> refs;

        explicit Visitor(TemplateResolver* resolver) : resolver(resolver) {}

        void VisitUnresolvedUsingTypeLoc(clang::UnresolvedUsingTypeLoc loc) {
            refs.push_back(ReferenceLoc{
                .NameLoc = loc.getNameLoc(),
                .IsDecl = false,
                .Targets = {loc.getDecl()},
            });
        }

        void VisitElaboratedTypeLoc(clang::ElaboratedTypeLoc loc) {
            // We only learn the qualifier from the ElaboratedTypeLoc. The
            // underlying reference details come from the inner TypeLoc.
            size_t initial_size = refs.size();
            Visit(loc.getNamedTypeLoc().getUnqualifiedLoc());
            size_t new_size = refs.size();

            // Attach the qualifier to any refs produced by the inner visit.
            for(size_t i = initial_size; i < new_size; ++i) {
                assert(!refs[i].Qualifier.hasQualifier() && "qualifier already set");
                refs[i].Qualifier = loc.getQualifierLoc();
            }
        }

        void VisitUsingTypeLoc(clang::UsingTypeLoc loc) {
            refs.push_back(ReferenceLoc{
                .NameLoc = loc.getLocalSourceRange().getBegin(),
                .IsDecl = false,
                .Targets = {loc.getFoundDecl()},
            });
        }

        void VisitTagTypeLoc(clang::TagTypeLoc loc) {
            refs.push_back(ReferenceLoc{
                .NameLoc = loc.getNameLoc(),
                .IsDecl = false,
                .Targets = {loc.getDecl()},
            });
        }

        void VisitTemplateTypeParmTypeLoc(clang::TemplateTypeParmTypeLoc loc) {
            refs.push_back(ReferenceLoc{
                .NameLoc = loc.getNameLoc(),
                .IsDecl = false,
                .Targets = {loc.getDecl()},
            });
        }

        void VisitTemplateSpecializationTypeLoc(clang::TemplateSpecializationTypeLoc loc) {
            refs.push_back(ReferenceLoc{
                .Qualifier = clang::NestedNameSpecifierLoc(),
                .NameLoc = loc.getTemplateNameLoc(),
                .IsDecl = false,
                // We must preserve written alias templates here. For
                // `valias<int>`, explicit_reference_targets() may see both the
                // alias pattern and the desugared underlying type, but the
                // source reference should prefer the alias name that was
                // actually written.
                .Targets = explicit_reference_targets(clang::DynTypedNode::create(loc.getType()),
                                                      DeclRelation::Alias,
                                                      resolver),
            });
        }

        void VisitDependentTemplateSpecializationTypeLoc(
            clang::DependentTemplateSpecializationTypeLoc loc) {
            refs.push_back(ReferenceLoc{
                .Qualifier = loc.getQualifierLoc(),
                .NameLoc = loc.getTemplateNameLoc(),
                .Targets = explicit_reference_targets(clang::DynTypedNode::create(loc.getType()),
                                                      resolver),
            });
        }

        void VisitDeducedTemplateSpecializationTypeLoc(
            clang::DeducedTemplateSpecializationTypeLoc loc) {
            refs.push_back(ReferenceLoc{
                .Qualifier = clang::NestedNameSpecifierLoc(),
                .NameLoc = loc.getNameLoc(),
                // Same as template aliases above: keep the written alias name
                // if there is one.
                .Targets = explicit_reference_targets(clang::DynTypedNode::create(loc.getType()),
                                                      DeclRelation::Alias,
                                                      resolver),
            });
        }

        void VisitDependentNameTypeLoc(clang::DependentNameTypeLoc loc) {
            refs.push_back(ReferenceLoc{
                .Qualifier = loc.getQualifierLoc(),
                .NameLoc = loc.getNameLoc(),
                .Targets = explicit_reference_targets(clang::DynTypedNode::create(loc.getType()),
                                                      resolver),
            });
        }

        void VisitInjectedClassNameTypeLoc(clang::InjectedClassNameTypeLoc loc) {
            refs.push_back(ReferenceLoc{
                .Qualifier = clang::NestedNameSpecifierLoc(),
                // todo: compile error
                // .Qualifier = loc.getQualifierLoc(),
                .NameLoc = loc.getNameLoc(),
                .Targets = {loc.getDecl()},
            });
        }

        void VisitTypedefTypeLoc(clang::TypedefTypeLoc loc) {
            if(should_skip_typedef(loc.getTypedefNameDecl())) {
                return;
            }

            refs.push_back(ReferenceLoc{
                // .Qualifier = loc.getQualifierLoc(),
                .Qualifier = clang::NestedNameSpecifierLoc(),
                .NameLoc = loc.getNameLoc(),
                .Targets = {loc.getTypedefNameDecl()},
            });
        }

        void VisitObjCInterfaceTypeLoc(clang::ObjCInterfaceTypeLoc loc) {
            refs.push_back(ReferenceLoc{
                .Qualifier = clang::NestedNameSpecifierLoc(),
                .NameLoc = loc.getNameLoc(),
                .Targets = {loc.getIFaceDecl()},
            });
        }
    };

    Visitor visitor(resolver);
    visitor.Visit(type_loc.getUnqualifiedLoc());
    return visitor.refs;
}

class ExplicitReferenceCollector : public clang::RecursiveASTVisitor<ExplicitReferenceCollector> {
public:
    ExplicitReferenceCollector(llvm::function_ref<void(ReferenceLoc)> out,
                               TemplateResolver* resolver) : out(out), resolver(resolver) {}

    bool VisitTypeLoc(clang::TypeLoc type_loc) {
        if(type_locs_to_skip.contains(type_loc.getBeginLoc())) {
            return true;
        }
        visit_node(clang::DynTypedNode::create(type_loc));
        return true;
    }

    bool TraverseElaboratedTypeLoc(clang::ElaboratedTypeLoc loc) {
        clang::TypeLoc inner = loc.getNamedTypeLoc().getUnqualifiedLoc();
        // ElaboratedTypeLoc reports the actual reference through its inner
        // TypeLoc. If both are visited independently we either duplicate the
        // reference or lose the qualifier carried by the outer node.
        if(loc.getBeginLoc() == inner.getBeginLoc()) {
            return clang::RecursiveASTVisitor<ExplicitReferenceCollector>::TraverseTypeLoc(inner);
        }
        type_locs_to_skip.insert(inner.getBeginLoc());
        return clang::RecursiveASTVisitor<ExplicitReferenceCollector>::TraverseElaboratedTypeLoc(
            loc);
    }

    bool VisitStmt(clang::Stmt* stmt) {
        visit_node(clang::DynTypedNode::create(*stmt));
        return true;
    }

    bool TraverseOpaqueValueExpr(clang::OpaqueValueExpr* expr) {
        visit_node(clang::DynTypedNode::create(*expr));
        // Not clear why the source expression is skipped by default...
        // FIXME: can we just make RecursiveASTVisitor do this?
        return clang::RecursiveASTVisitor<ExplicitReferenceCollector>::TraverseStmt(
            expr->getSourceExpr());
    }

    bool TraversePseudoObjectExpr(clang::PseudoObjectExpr* expr) {
        visit_node(clang::DynTypedNode::create(*expr));
        // Traverse only the syntactic form to find written references. The
        // semantic form contains substantial duplication.
        return clang::RecursiveASTVisitor<ExplicitReferenceCollector>::TraverseStmt(
            expr->getSyntacticForm());
    }

    bool TraverseTemplateArgumentLoc(clang::TemplateArgumentLoc argument) {
        // There is no corresponding Visit* hook. TemplateArgumentLoc is also
        // the only way to recover locations for template-template parameter
        // references.
        switch(argument.getArgument().getKind()) {
            case clang::TemplateArgument::Template:
            case clang::TemplateArgument::TemplateExpansion: {
                report_reference(
                    ReferenceLoc{
                        .Qualifier = argument.getTemplateQualifierLoc(),
                        .NameLoc = argument.getTemplateNameLoc(),
                        .Targets = {argument.getArgument()
                                        .getAsTemplateOrTemplatePattern()
                                        .getAsTemplateDecl()},
                    },
                    clang::DynTypedNode::create(argument.getArgument()));
                break;
            }
            case clang::TemplateArgument::Declaration:
                break;  // FIXME: can this actually happen in TemplateArgumentLoc?
            case clang::TemplateArgument::Integral:
            case clang::TemplateArgument::Null:
            case clang::TemplateArgument::NullPtr: break;  // no references.
            case clang::TemplateArgument::Pack:
            case clang::TemplateArgument::Type:
            case clang::TemplateArgument::Expression:
            case clang::TemplateArgument::StructuralValue:
                break;  // Handled by VisitType and VisitExpression.

            default: break;
        }

        return clang::RecursiveASTVisitor<ExplicitReferenceCollector>::TraverseTemplateArgumentLoc(
            argument);
    }

    bool VisitDecl(clang::Decl* decl) {
        visit_node(clang::DynTypedNode::create(*decl));
        return true;
    }

    bool TraverseNestedNameSpecifierLoc(clang::NestedNameSpecifierLoc loc) {
        if(!loc.getNestedNameSpecifier()) {
            return true;
        }

        // There is no corresponding Visit* hook for NestedNameSpecifierLoc.
        visit_node(clang::DynTypedNode::create(loc));
        // Inner TypeLoc nodes do not know their qualifier, so skip them and
        // keep the richer reference from the NestedNameSpecifierLoc.
        if(clang::TypeLoc type_loc = loc.getTypeLoc()) {
            type_locs_to_skip.insert(type_loc.getBeginLoc());
        }

        return clang::RecursiveASTVisitor<
            ExplicitReferenceCollector>::TraverseNestedNameSpecifierLoc(loc);
    }

    bool TraverseObjCProtocolLoc(clang::ObjCProtocolLoc loc) {
        visit_node(clang::DynTypedNode::create(loc));
        return true;
    }

    bool TraverseConstructorInitializer(clang::CXXCtorInitializer* init) {
        visit_node(clang::DynTypedNode::create(*init));
        return clang::RecursiveASTVisitor<
            ExplicitReferenceCollector>::TraverseConstructorInitializer(init);
    }

    bool VisitConceptReference(clang::ConceptReference* reference) {
        visit_node(clang::DynTypedNode::create(*reference));
        return true;
    }

private:
#if 0
    static std::string reference_key(const ReferenceLoc& ref) {
        std::string key;
        llvm::raw_string_ostream os(key);
        os << ref.NameLoc.getRawEncoding() << '|';
        os << ref.IsDecl << '|';
        if(ref.Qualifier) {
            os << ref.Qualifier.getBeginLoc().getRawEncoding() << ':';
            ref.Qualifier.getNestedNameSpecifier()->print(
                os,
                clang::PrintingPolicy(clang::LangOptions()));
        }
        os << '|';

        llvm::SmallVector<std::uintptr_t> targets;
        targets.reserve(ref.Targets.size());
        for(const auto* target: ref.Targets) {
            targets.push_back(reinterpret_cast<std::uintptr_t>(target));
        }
        llvm::sort(targets);
        for(auto target: targets) {
            os << target << ',';
        }
        return key;
    }
#endif

    /// Obtain information about a reference directly written by \p node. This
    /// does not recurse into children.
    ///
    /// Individual fields of ReferenceLoc may be empty:
    /// - implicit AST nodes can lack usable source locations
    /// - dependent code can have no resolved targets
    ///
    /// Declarations themselves are not treated as references, but a
    /// declaration node can still contain references in its spelling, such as
    /// `namespace foo = std`.
    llvm::SmallVector<ReferenceLoc> explicit_reference_of(clang::DynTypedNode node) {
        if(const auto* decl = node.get<clang::Decl>()) {
            return ref_in_decl(decl, resolver);
        }

        if(const auto* stmt = node.get<clang::Stmt>()) {
            return ref_in_stmt(stmt, resolver);
        }

        if(const auto* nested_name = node.get<clang::NestedNameSpecifierLoc>()) {
            if(clang::TypeLoc type_loc = nested_name->getTypeLoc()) {
                return ref_in_type_loc(type_loc, resolver);
            }

            return {
                ReferenceLoc{
                             .Qualifier = nested_name->getPrefix(),
                             .NameLoc = nested_name->getLocalBeginLoc(),
                             // DeclRelation::Alias ensures we do not lose namespace
                    // aliases such as `alias::foo`.
                    .Targets = explicit_reference_targets(
                        clang::DynTypedNode::create(*nested_name->getNestedNameSpecifier()),
                             DeclRelation::Alias,
                             resolver),
                             }
            };
        }

        if(const auto* type_loc = node.get<clang::TypeLoc>()) {
            return ref_in_type_loc(*type_loc, resolver);
        }

        if(const auto* initializer = node.get<clang::CXXCtorInitializer>()) {
            if(initializer->isAnyMemberInitializer()) {
                return {
                    ReferenceLoc{
                                 .NameLoc = initializer->getMemberLocation(),
                                 .IsDecl = false,
                                 .Targets = {initializer->getAnyMember()},
                                 }
                };
            }
            // Other type initializers, such as base initializers, are handled
            // by visiting the corresponding TypeLoc.
        }

        if(const auto* protocol_loc = node.get<clang::ObjCProtocolLoc>()) {
            return {
                ReferenceLoc{
                             .NameLoc = protocol_loc->getLocation(),
                             .IsDecl = false,
                             .Targets = {protocol_loc->getProtocol()},
                             }
            };
        }

        if(const auto* concept_ref = node.get<clang::ConceptReference>()) {
            return {
                ReferenceLoc{
                             .Qualifier = concept_ref->getNestedNameSpecifierLoc(),
                             .NameLoc = concept_ref->getConceptNameLoc(),
                             .IsDecl = false,
                             .Targets = {concept_ref->getNamedConcept()},
                             }
            };
        }

        // Other node kinds do not carry enough source location information to
        // form a ReferenceLoc.
        return {};
    }

    void visit_node(clang::DynTypedNode node) {
        for(auto& ref: explicit_reference_of(node)) {
            report_reference(std::move(ref), node);
        }
    }

    void report_reference(ReferenceLoc&& ref, clang::DynTypedNode) {
        // Strip null targets that can arise from invalid code.
        llvm::erase(ref.Targets, nullptr);
        // Only report references that are actually written in source. If we
        // cannot recover a location, skip the node.
        if(ref.NameLoc.isInvalid()) {
            // dlog("invalid location at node {0}", nodeToString(N));
            return;
        }
        // todo: clangd didn't deduplicate this.
        // if(!seen_references.insert(reference_key(ref)).second) {
        //     return;
        // }
        out(std::move(ref));
    }

    llvm::function_ref<void(ReferenceLoc)> out;
    TemplateResolver* resolver;
    // TypeLocs starting at these locations are skipped because a richer
    // enclosing node already reported the corresponding reference.
    llvm::DenseSet<clang::SourceLocation> type_locs_to_skip;
    std::unordered_set<std::string> seen_references;
};

std::string target_name(const clang::NamedDecl& decl) {
    std::string result;
    llvm::raw_string_ostream os(result);
    decl.printQualifiedName(os);
    os << ast::print_template_specialization_args(&decl);
    return result;
}

}  // namespace

void explicit_references(const clang::Stmt* stmt,
                         llvm::function_ref<void(ReferenceLoc)> out,
                         TemplateResolver* resolver) {
    assert(stmt);
    ExplicitReferenceCollector(out, resolver).TraverseStmt(const_cast<clang::Stmt*>(stmt));
}

void explicit_references(const clang::Decl* decl,
                         llvm::function_ref<void(ReferenceLoc)> out,
                         TemplateResolver* resolver) {
    assert(decl);
    ExplicitReferenceCollector(out, resolver).TraverseDecl(const_cast<clang::Decl*>(decl));
}

void explicit_references(const clang::ASTContext& ast,
                         llvm::function_ref<void(ReferenceLoc)> out,
                         TemplateResolver* resolver) {
    ExplicitReferenceCollector(out, resolver).TraverseAST(const_cast<clang::ASTContext&>(ast));
}

llvm::raw_ostream& operator<<(llvm::raw_ostream& os, ReferenceLoc ref) {
    os << "targets = {";
    llvm::SmallVector<std::string> targets;
    for(const auto* target: ref.Targets) {
        targets.push_back(target_name(*target));
    }
    llvm::sort(targets);
    os << llvm::join(targets, ", ");
    os << "}";
    if(ref.Qualifier) {
        os << ", qualifier = '";
        ref.Qualifier.getNestedNameSpecifier()->print(os,
                                                      clang::PrintingPolicy(clang::LangOptions()));
        os << "'";
    }
    if(ref.IsDecl) {
        os << ", decl";
    }
    return os;
}

}  // namespace clice
