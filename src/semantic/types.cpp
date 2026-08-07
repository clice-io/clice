/// Parts of this file (deduced_type and its visitor) are ported from
/// clangd's AST.cpp (llvmorg-21.1.8), part of the LLVM project, licensed
/// under Apache License v2.0 with LLVM Exceptions. See
/// https://llvm.org/LICENSE.txt for license information.

#include "semantic/types.h"

#include "semantic/decls.h"
#include "semantic/resolver.h"

#include "clang/AST/ASTContext.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/DeclTemplate.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/AST/Type.h"

namespace clice::types {

namespace {

template <typename Ty>
    requires requires(Ty* T) { T->getDecl(); }
const clang::NamedDecl* decl_of_impl(const Ty* T) {
    return T->getDecl();
}

const clang::NamedDecl* decl_of_impl(const void* T) {
    return nullptr;
}

}  // namespace

auto decls_of(clang::QualType type, TemplateResolver* resolver)
    -> llvm::SmallVector<const clang::NamedDecl*, 1> {
    if(type.isNull()) {
        return {};
    }

    /// Dependent names carry no declaration structurally; resolve them
    /// heuristically when a resolver is available.
    if(const auto* DNT = type->getAs<clang::DependentNameType>()) {
        llvm::SmallVector<const clang::NamedDecl*, 1> result;
        if(resolver) {
            for(auto* target: resolver->lookup(DNT)) {
                result.push_back(target);
            }
        }
        return result;
    }

    /// using typename B<T>::type — the unresolved-using declaration itself
    /// first, then the resolver's candidates for what it imports.
    if(const auto* UUT = type->getAs<clang::UnresolvedUsingType>()) {
        llvm::SmallVector<const clang::NamedDecl*, 1> result;
        result.push_back(UUT->getDecl());
        if(resolver) {
            if(auto* UD = llvm::dyn_cast<clang::UnresolvedUsingTypenameDecl>(UUT->getDecl())) {
                for(auto* target: resolver->lookup(UD)) {
                    result.push_back(target);
                }
            }
        }
        return result;
    }

    if(auto TST = type->getAs<clang::TemplateSpecializationType>()) {
        auto decl = TST->getTemplateName().getAsTemplateDecl();

        /// A dependent template name (`T::template rebind<U>`) has no
        /// declaration structurally; resolve it heuristically.
        if(!decl) {
            llvm::SmallVector<const clang::NamedDecl*, 1> result;
            if(resolver) {
                for(auto* target: resolver->lookup(TST)) {
                    result.push_back(target);
                }
            }
            return result;
        }

        if(type->isDependentType()) {
            return {decl};
        }

        /// For a template specialization type, the template name is possibly a `ClassTemplateDecl`
        ///  `TypeAliasTemplateDecl` or `TemplateTemplateParmDecl` and `BuiltinTemplateDecl`.
        if(llvm::isa<clang::TypeAliasTemplateDecl>(decl)) {
            return {decl->getTemplatedDecl()};
        }

        if(llvm::isa<clang::TemplateTemplateParmDecl, clang::BuiltinTemplateDecl>(decl)) {
            return {decl};
        }

        if(const auto* record = TST->getAsCXXRecordDecl()) {
            if(const auto* pattern = decls::instantiated_from(record)) {
                return {pattern};
            }
        }
        return {};
    }

    const clang::NamedDecl* decl = nullptr;
    switch(type->getTypeClass()) {
#define ABSTRACT_TYPE(TY, BASE)
#define TYPE(TY, BASE)                                                                             \
    case clang::Type::TY: decl = decl_of_impl(llvm::cast<clang::TY##Type>(type)); break;
#include "clang/AST/TypeNodes.inc"
    }

    if(decl) {
        return {decl};
    }
    return {};
}

auto decl_of(clang::QualType type, TemplateResolver* resolver) -> const clang::NamedDecl* {
    auto decls = decls_of(type, resolver);
    return decls.empty() ? nullptr : decls.front();
}

auto unwrap(clang::TypeLoc type, bool unwrap_function_type) -> clang::TypeLoc {
    while(true) {
        if(auto qualified = type.getAs<clang::QualifiedTypeLoc>()) {
            type = qualified.getUnqualifiedLoc();
        } else if(auto reference = type.getAs<clang::ReferenceTypeLoc>()) {
            type = reference.getPointeeLoc();
        } else if(auto pointer = type.getAs<clang::PointerTypeLoc>()) {
            type = pointer.getPointeeLoc();
        } else if(auto paren = type.getAs<clang::ParenTypeLoc>()) {
            type = paren.getInnerLoc();
        } else if(auto array = type.getAs<clang::ConstantArrayTypeLoc>()) {
            type = array.getElementLoc();
        } else if(auto proto = type.getAs<clang::FunctionProtoTypeLoc>();
                  proto && unwrap_function_type) {
            type = proto.getReturnLoc();
        } else {
            break;
        }
    }
    return type;
}

auto declared_type(const clang::TypeDecl* decl) -> clang::QualType {
    assert(decl);
    clang::ASTContext& context = decl->getASTContext();
    if(const auto* spec = llvm::dyn_cast<clang::ClassTemplateSpecializationDecl>(decl)) {
        if(const auto* args = spec->getTemplateArgsAsWritten()) {
            return context.getTemplateSpecializationType(
                clang::ElaboratedTypeKeyword::None,
                clang::TemplateName(spec->getSpecializedTemplate()),
                args->arguments(),
                /*CanonicalArgs=*/{});
        }
    }
    return context.getTypeDeclType(decl);
}

namespace {

/// Returns the TemplateTypeParmTypeLoc of the implicit template type
/// parameter introduced by an `auto` typed function parameter, if any.
auto contained_auto_param_type(clang::TypeLoc type) -> clang::TemplateTypeParmTypeLoc {
    if(auto qualified = type.getAs<clang::QualifiedTypeLoc>()) {
        return contained_auto_param_type(qualified.getUnqualifiedLoc());
    }

    if(llvm::isa<clang::PointerType, clang::ReferenceType, clang::ParenType>(type.getTypePtr())) {
        return contained_auto_param_type(type.getNextTypeLoc());
    }

    if(auto function = type.getAs<clang::FunctionTypeLoc>()) {
        return contained_auto_param_type(function.getReturnLoc());
    }

    if(auto param = type.getAs<clang::TemplateTypeParmTypeLoc>()) {
        if(param.getTypePtr()->getDecl()->isImplicit()) {
            return param;
        }
    }

    return {};
}

/// Computes the deduced type at a given location by visiting the relevant
/// nodes. We use this to display the actual type when hovering over an "auto"
/// keyword or "decltype()" expression.
/// FIXME: This could have been a lot simpler by visiting AutoTypeLocs but it
/// seems that the AutoTypeLocs that can be visited along with their AutoType do
/// not have the deduced type set. Instead, we have to go to the appropriate
/// DeclaratorDecl/FunctionDecl and work our back to the AutoType that does have
/// a deduced type set. The AST should be improved to simplify this scenario.
class DeducedTypeVisitor : public clang::RecursiveASTVisitor<DeducedTypeVisitor> {
public:
    DeducedTypeVisitor(clang::SourceLocation searched_location) :
        searched_location(searched_location) {}

    /// Handle auto initializers:
    /// - auto i = 1;
    /// - decltype(auto) i = 1;
    /// - auto& i = 1;
    /// - auto* i = &a;
    bool VisitDeclaratorDecl(clang::DeclaratorDecl* decl) {
        if(!decl->getTypeSourceInfo() ||
           !decl->getTypeSourceInfo()->getTypeLoc().getContainedAutoTypeLoc() ||
           decl->getTypeSourceInfo()->getTypeLoc().getContainedAutoTypeLoc().getNameLoc() !=
               searched_location) {
            return true;
        }

        if(auto* type = decl->getType()->getContainedAutoType()) {
            deduced = type->desugar();
        }
        return true;
    }

    /// Handle auto return types:
    /// - auto foo() {}
    /// - auto& foo() {}
    /// - auto foo() -> int {}
    /// - auto foo() -> decltype(1+1) {}
    /// - operator auto() const { return 10; }
    bool VisitFunctionDecl(clang::FunctionDecl* decl) {
        if(!decl->getTypeSourceInfo()) {
            return true;
        }

        /// Loc of auto in return type (c++14).
        auto location = decl->getReturnTypeSourceRange().getBegin();

        /// Loc of "auto" in operator auto().
        if(location.isInvalid() && llvm::isa<clang::CXXConversionDecl>(decl)) {
            location = decl->getTypeSourceInfo()->getTypeLoc().getBeginLoc();
        }

        /// Loc of "auto" in function with trailing return type (c++11).
        if(location.isInvalid()) {
            location = decl->getSourceRange().getBegin();
        }

        if(location != searched_location) {
            return true;
        }

        const clang::AutoType* type = decl->getReturnType()->getContainedAutoType();
        if(type && !type->getDeducedType().isNull()) {
            deduced = type->getDeducedType();
        } else if(auto* decltype_type =
                      llvm::dyn_cast<clang::DecltypeType>(decl->getReturnType())) {
            /// auto in a trailing return type just points to a DecltypeType
            /// and getContainedAutoType does not unwrap it.
            if(!decltype_type->getUnderlyingType().isNull()) {
                deduced = decltype_type->getUnderlyingType();
            }
        } else if(!decl->getReturnType().isNull()) {
            deduced = decl->getReturnType();
        }
        return true;
    }

    /// Handle non-auto decltype, e.g.:
    /// - auto foo() -> decltype(expr) {}
    /// - decltype(expr);
    bool VisitDecltypeTypeLoc(clang::DecltypeTypeLoc type_loc) {
        if(type_loc.getBeginLoc() != searched_location) {
            return true;
        }

        /// A DecltypeType's underlying type can be another DecltypeType! E.g.
        ///   int I = 0;
        ///   decltype(I) J = I;
        ///   decltype(J) K = J;
        const auto* type = llvm::dyn_cast<clang::DecltypeType>(type_loc.getTypePtr());
        while(type && !type->getUnderlyingType().isNull()) {
            deduced = type->getUnderlyingType();
            type = llvm::dyn_cast<clang::DecltypeType>(deduced.getTypePtr());
        }
        return true;
    }

    /// Handle functions/lambdas with `auto` typed parameters.
    /// We deduce the type if there's exactly one instantiation visible.
    bool VisitParmVarDecl(clang::ParmVarDecl* param) {
        if(!param->getType()->isDependentType()) {
            return true;
        }

        /// 'auto' here does not name an AutoType, but an implicit template param.
        clang::TemplateTypeParmTypeLoc auto_loc =
            contained_auto_param_type(param->getTypeSourceInfo()->getTypeLoc());
        if(auto_loc.isNull() || auto_loc.getNameLoc() != searched_location) {
            return true;
        }

        /// We expect the TTP to be attached to this function template.
        /// Find the template and the param index.
        auto* templated = llvm::dyn_cast<clang::FunctionDecl>(param->getDeclContext());
        if(!templated) {
            return true;
        }

        auto* template_decl = templated->getDescribedFunctionTemplate();
        if(!template_decl) {
            return true;
        }

        int index = param_index(*template_decl, *auto_loc.getDecl());
        if(index < 0) {
            assert(false && "auto TTP is not from enclosing function?");
            return true;
        }

        /// Now find the instantiation and the deduced template type arg.
        auto* instantiation =
            llvm::dyn_cast_or_null<clang::FunctionDecl>(decls::only_instantiation(templated));
        if(!instantiation) {
            return true;
        }

        const auto* args = instantiation->getTemplateSpecializationArgs();
        if(args->size() != template_decl->getTemplateParameters()->size()) {
            /// No weird variadic stuff.
            return true;
        }

        deduced = args->get(index).getAsType();
        return true;
    }

    static int param_index(const clang::TemplateDecl& template_decl, clang::NamedDecl& param) {
        int index = 0;
        for(auto* decl: *template_decl.getTemplateParameters()) {
            if(&param == decl) {
                return index;
            }
            index += 1;
        }
        return -1;
    }

    clang::SourceLocation searched_location;

    clang::QualType deduced;
};

}  // namespace

auto deduced_type(clang::ASTContext& context, clang::SourceLocation loc)
    -> std::optional<clang::QualType> {
    if(!loc.isValid()) {
        return std::nullopt;
    }

    DeducedTypeVisitor visitor(loc);
    visitor.TraverseAST(context);
    if(visitor.deduced.isNull()) {
        return std::nullopt;
    }
    return visitor.deduced;
}

}  // namespace clice::types
