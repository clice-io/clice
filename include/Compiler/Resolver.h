#pragma once

#include "Clang.h"

namespace clice {

/// This class is used to resolve dependent names in the AST.
/// For dependent names, we cannot know the any information about the name until
/// the template is instantiated. This can be frustrating, you cannot get
/// completion, you cannot get go-to-definition, etc. To avoid this, we just use
/// some heuristics to simplify the dependent names as normal type/expression.
/// For example, `std::vector<T>::value_type` can be simplified as `T`.
class TemplateResolver {
public:
    TemplateResolver(clang::Sema& sema) : sema(sema) {}

    clang::QualType resolve(clang::QualType type);

    clang::ExprResult resolve(clang::CXXUnresolvedConstructExpr* expr);

    clang::ExprResult resolve(clang::UnresolvedLookupExpr* expr);

    // TODO: use a relative clear way to resolve `UnresolvedLookupExpr`.

    void resolve(clang::UnresolvedUsingType* type);

    /// Resugar the canonical `TemplateTypeParmType` with given template context.
    /// `decl` should be the declaration that the type is in.
    clang::QualType resugar(clang::QualType type, clang::Decl* decl);

    /// Look up the name in the given nested name specifier.
    clang::lookup_result lookup(const clang::NestedNameSpecifier* NNS, clang::DeclarationName name);

    clang::lookup_result lookup(const clang::DependentNameType* type) {
        return lookup(type->getQualifier(), type->getIdentifier());
    }

    clang::lookup_result lookup(const clang::DependentTemplateSpecializationType* type) {
        return lookup(type->getQualifier(), type->getIdentifier());
    }

    clang::lookup_result lookup(const clang::DependentScopeDeclRefExpr* expr) {
        return lookup(expr->getQualifier(), expr->getNameInfo().getName());
    }

    clang::lookup_result lookup(const clang::UnresolvedLookupExpr* expr) {
        /// FIXME: 
        for(auto decl: expr->decls()) {
            if(auto TD = llvm::dyn_cast<clang::TemplateDecl>(decl)) {
                return clang::lookup_result(TD);
            }
        }

        return {};
    }

    clang::lookup_result lookup(const clang::UnresolvedMemberExpr* expr) {
        return {};
    }

    /// TODO:
    clang::lookup_result lookup(clang::CXXDependentScopeMemberExpr* expr) {
        return {};
    }

    clang::lookup_result lookup(const clang::UnresolvedUsingValueDecl* decl) {
        return lookup(decl->getQualifier(), decl->getDeclName());
    }

    clang::lookup_result resolve(const clang::UnresolvedUsingTypenameDecl* decl) {
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
