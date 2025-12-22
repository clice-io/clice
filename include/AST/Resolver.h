#pragma once

#include "clang/AST/ExprCXX.h"
#include "clang/AST/Type.h"

namespace clang {

class Sema;

}

namespace clice {

/// This class is used to resolve dependent names in the unit.
/// For dependent names, we cannot know the any information about the name until
/// the template is instantiated. This can be frustrating, you cannot get
/// completion, you cannot get go-to-definition, etc. To avoid this, we just use
/// some heuristics to simplify the dependent names as normal type/expression.
/// For example, `std::vector<T>::value_type` can be simplified as `T`.
class TemplateResolver {
public:
    TemplateResolver(clang::Sema& sema) : sema(sema) {}

    clang::QualType resolve(clang::QualType type);

    void resolve(clang::CXXUnresolvedConstructExpr* expr);

    void resolve(clang::UnresolvedLookupExpr* expr);

    // TODO: use a relative clear way to resolve `UnresolvedLookupExpr`.

    void resolve(clang::UnresolvedUsingType* type);

    /// Resugar the canonical `TemplateTypeParmType` with given template context.
    /// `decl` should be the declaration that the type is in.
    clang::QualType resugar(clang::QualType type, clang::Decl* decl);

    using lookup_result = clang::DeclContext::lookup_result;

    /// Look up the name in the given nested name specifier.
    lookup_result lookup(const clang::NestedNameSpecifier* NNS, clang::DeclarationName name);

    lookup_result lookup(const clang::DependentNameType* type) {
        return lookup(type->getQualifier(), type->getIdentifier());
    }

    lookup_result lookup(const clang::DependentTemplateSpecializationType* type) {
        auto& template_name = type->getDependentTemplateName();
        auto identifier = template_name.getName().getIdentifier();
        if(identifier) {
            return lookup(template_name.getQualifier(), identifier);
        } else {
            /// FIXME: Operators does't have a name.
            return {};
        }
    }

    lookup_result lookup(const clang::DependentScopeDeclRefExpr* expr) {
        return lookup(expr->getQualifier(), expr->getNameInfo().getName());
    }

    lookup_result lookup(const clang::UnresolvedLookupExpr* expr) {
        /// FIXME:
        for(auto decl: expr->decls()) {
            if(auto TD = llvm::dyn_cast<clang::TemplateDecl>(decl)) {
                return lookup_result(TD);
            }
        }

        return {};
    }

    lookup_result lookup(const clang::UnresolvedMemberExpr* expr) {
        return {};
    }

    /// TODO:
    lookup_result lookup(clang::CXXDependentScopeMemberExpr* expr) {
        return {};
    }

    lookup_result lookup(const clang::UnresolvedUsingValueDecl* decl) {
        return lookup(decl->getQualifier(), decl->getDeclName());
    }

    lookup_result resolve(const clang::UnresolvedUsingTypenameDecl* decl) {
        return lookup(decl->getQualifier(), decl->getDeclName());
    }

#ifndef NDEBUG
    static inline bool debug = false;
#endif

private:
    clang::Sema& sema;
    llvm::DenseMap<const void*, clang::QualType> resolved;
};

}  // namespace clice
