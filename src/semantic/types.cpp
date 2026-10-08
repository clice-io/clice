module;

#include "modules/prelude.h"
/// Parts of this file (deduced_type and its helpers) are ported from
/// clangd's AST.cpp (llvmorg-21.1.8), part of the LLVM project, licensed
/// under Apache License v2.0 with LLVM Exceptions. See
/// https://llvm.org/LICENSE.txt for license information.

module clice;

import :semantic.decls;
import :semantic.resolver;
import :semantic.types;

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
    /// Sugar spelling no name of its own gives way to the type it wraps.
    while(!type.isNull()) {
        const clang::Type* written = type.getTypePtr();
        if(auto* subst = llvm::dyn_cast<clang::SubstTemplateTypeParmType>(written)) {
            type = subst->getReplacementType();
        } else if(auto* paren = llvm::dyn_cast<clang::ParenType>(written)) {
            type = paren->getInnerType();
        } else if(auto* attributed = llvm::dyn_cast<clang::AttributedType>(written)) {
            type = attributed->getModifiedType();
        } else if(auto* macro = llvm::dyn_cast<clang::MacroQualifiedType>(written)) {
            type = macro->getUnderlyingType();
        } else {
            break;
        }
    }

    if(type.isNull()) {
        return {};
    }

    /// A type alias is what the type names, whatever it stands for.
    if(const auto* TT = llvm::dyn_cast<clang::TypedefType>(type.getTypePtr())) {
        return {TT->getDecl()};
    }

    /// using ns::Widget; — the written name imports the type it names.
    if(const auto* UT = llvm::dyn_cast<clang::UsingType>(type.getTypePtr())) {
        return {UT->getDecl()->getTargetDecl()};
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

auto unwrap(clang::QualType type) -> clang::QualType {
    while(!type.isNull()) {
        const clang::Type* written = type.getTypePtr();
        if(auto* paren = llvm::dyn_cast<clang::ParenType>(written)) {
            type = paren->getInnerType();
        } else if(auto* adjusted = llvm::dyn_cast<clang::AdjustedType>(written)) {
            type = adjusted->getOriginalType();
        } else if(auto* pointer = llvm::dyn_cast<clang::PointerType>(written)) {
            type = pointer->getPointeeType();
        } else if(auto* reference = llvm::dyn_cast<clang::ReferenceType>(written)) {
            type = reference->getPointeeType();
        } else if(auto* array = llvm::dyn_cast<clang::ArrayType>(written)) {
            type = array->getElementType();
        } else if(auto* deduced = llvm::dyn_cast<clang::DeducedType>(written);
                  deduced && deduced->getDeducedKind() == clang::DeducedKind::Deduced) {
            type = deduced->getDeducedType();
        } else if(auto* decltype_type = llvm::dyn_cast<clang::DecltypeType>(written);
                  decltype_type && decltype_type->isSugared()) {
            type = decltype_type->getUnderlyingType();
        } else {
            break;
        }
    }
    return type;
}

auto destructor_of(clang::QualType type) -> const clang::CXXDestructorDecl* {
    auto* RD = type->getAsCXXRecordDecl();
    if(!RD || !RD->hasDefinition() || RD->hasTrivialDestructor()) {
        return nullptr;
    }
    return RD->getDestructor();
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

/// The deduced return type of a function whose return type holds the
/// placeholder:
/// - auto foo() {}
/// - auto& foo() {}
/// - auto foo() -> int {}
/// - auto foo() -> decltype(1+1) {}
/// - operator auto() const { return 10; }
auto deduced_return_type(const clang::FunctionDecl* function) -> clang::QualType {
    auto returned = function->getReturnType();
    if(const auto* AT = returned->getContainedAutoType();
       AT && AT->getDeducedKind() == clang::DeducedKind::Deduced) {
        return AT->getDeducedType();
    }
    /// auto in a trailing return type just points to a DecltypeType and
    /// getContainedAutoType does not unwrap it.
    if(const auto* DT = llvm::dyn_cast<clang::DecltypeType>(returned)) {
        return DT->getUnderlyingType();
    }
    return returned;
}

/// The type an abbreviated template's `auto` parameter took, when the
/// template was instantiated exactly once.
auto deduced_param_type(const clang::ParmVarDecl* param, clang::TemplateTypeParmTypeLoc written)
    -> clang::QualType {
    /// We expect the TTP to be attached to this function template.
    const auto* templated = llvm::dyn_cast<clang::FunctionDecl>(param->getDeclContext());
    auto* template_decl = templated ? templated->getDescribedFunctionTemplate() : nullptr;
    if(!template_decl) {
        return {};
    }

    auto* params = template_decl->getTemplateParameters();
    auto* found = llvm::find(params->asArray(), written.getDecl());
    assert(found != params->end() && "auto TTP is not from enclosing function?");

    // The helper keeps its clangd signature, which speaks mutable pointers;
    // it only reads through them.
    auto* instantiation = llvm::dyn_cast_or_null<clang::FunctionDecl>(
        decls::only_instantiation(const_cast<clang::FunctionDecl*>(templated)));
    if(!instantiation) {
        return {};
    }

    const auto* args = instantiation->getTemplateSpecializationArgs();
    if(args->size() != params->size()) {
        /// No weird variadic stuff.
        return {};
    }
    return args->get(found - params->begin()).getAsType();
}

}  // namespace

auto deduced_type(clang::DynTypedNode written, const clang::Decl* owner) -> clang::QualType {
    /// A DecltypeType's underlying type can be another DecltypeType! E.g.
    ///   int I = 0;
    ///   decltype(I) J = I;
    ///   decltype(J) K = J;
    if(const auto* loc = written.get<clang::TypeLoc>()) {
        if(auto decltype_loc = loc->getAs<clang::DecltypeTypeLoc>()) {
            clang::QualType deduced;
            const auto* type = decltype_loc.getTypePtr();
            while(type && !type->getUnderlyingType().isNull()) {
                deduced = type->getUnderlyingType();
                type = llvm::dyn_cast<clang::DecltypeType>(deduced.getTypePtr());
            }
            return deduced;
        }
    }

    const auto* declarator = llvm::dyn_cast_if_present<clang::DeclaratorDecl>(owner);
    if(!declarator || !declarator->getTypeSourceInfo()) {
        return {};
    }

    /// The leading `auto` of a trailing return type belongs to the function
    /// itself.
    if(written.get<clang::Decl>() == owner) {
        const auto* function = llvm::dyn_cast<clang::FunctionDecl>(owner);
        const auto* proto = function ? function->getType()->getAs<clang::FunctionProtoType>()
                                     : nullptr;
        return proto && proto->hasTrailingReturn() ? deduced_return_type(function)
                                                   : clang::QualType();
    }

    const auto* loc = written.get<clang::TypeLoc>();
    if(!loc) {
        return {};
    }
    auto declared = declarator->getTypeSourceInfo()->getTypeLoc();

    /// 'auto' of a parameter does not name an AutoType, but an implicit
    /// template parameter.
    if(auto param_loc = loc->getAs<clang::TemplateTypeParmTypeLoc>()) {
        const auto* param = llvm::dyn_cast<clang::ParmVarDecl>(declarator);
        if(!param || contained_auto_param_type(declared) != param_loc) {
            return {};
        }
        return deduced_param_type(param, param_loc);
    }

    auto auto_loc = loc->getAs<clang::AutoTypeLoc>();
    if(!auto_loc) {
        return {};
    }

    if(const auto* function = llvm::dyn_cast<clang::FunctionDecl>(declarator)) {
        /// `operator auto()` spells its placeholder in the name.
        auto returned = function->getFunctionTypeLoc().getReturnLoc();
        if(llvm::isa<clang::CXXConversionDecl>(function)) {
            returned = function->getNameInfo().getNamedTypeInfo()->getTypeLoc();
        }
        if(returned.getContainedAutoTypeLoc().getNameLoc() != auto_loc.getNameLoc()) {
            return {};
        }
        return deduced_return_type(function);
    }

    if(declared.getContainedAutoTypeLoc().getNameLoc() != auto_loc.getNameLoc()) {
        return {};
    }
    if(const auto* type = declarator->getType()->getContainedAutoType()) {
        return type->desugar();
    }
    return {};
}

}  // namespace clice::types
