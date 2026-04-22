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
///
/// Thread safety: NOT thread-safe. Each compilation unit should have its own resolver.
/// The `resolved` cache persists across multiple resolve() calls on the same unit.
class TemplateResolver {
public:
    explicit TemplateResolver(clang::Sema& sema) : sema(sema) {}

    clang::QualType resolve(clang::QualType type);

    void resolve(clang::CXXUnresolvedConstructExpr* expr);

    void resolve(clang::UnresolvedLookupExpr* expr);

    // TODO: Use a clearer approach for resolving UnresolvedLookupExpr.

    void resolve(clang::UnresolvedUsingType* type);

    /// Resugar the canonical `TemplateTypeParmType` with given template context.
    /// `decl` should be the declaration that the type is in.
    clang::QualType resugar(clang::QualType type, clang::Decl* decl);

    using lookup_result = clang::DeclContext::lookup_result;

    /// Look up the name in the given nested name specifier.
    lookup_result lookup(clang::NestedNameSpecifier NNS, clang::DeclarationName name);

    lookup_result lookup(const clang::DependentNameType* type) {
        return lookup(type->getQualifier(), type->getIdentifier());
    }

    lookup_result lookup(const clang::TemplateSpecializationType* type,
                         const clang::DependentTemplateName* DTN) {
        auto identifier = DTN->getName().getIdentifier();
        if(identifier) {
            return lookup(DTN->getQualifier(), identifier);
        } else {
            /// TODO: Operators don't have an IdentifierInfo; need DeclarationName-based lookup.
            return {};
        }
    }

    lookup_result lookup(const clang::DependentScopeDeclRefExpr* expr) {
        return lookup(expr->getQualifier(), expr->getNameInfo().getName());
    }

    lookup_result lookup(const clang::UnresolvedLookupExpr* expr) {
        /// TODO: Only returns the first TemplateDecl; should handle overloaded lookups.
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

    /// TODO: Implement dependent member expression lookup (e.g. `x.template foo<T>()`).
    lookup_result lookup(const clang::CXXDependentScopeMemberExpr* expr) {
        return {};
    }

    lookup_result lookup(const clang::UnresolvedUsingValueDecl* decl) {
        return lookup(decl->getQualifier(), decl->getDeclName());
    }

    lookup_result lookup(const clang::UnresolvedUsingTypenameDecl* decl) {
        return lookup(decl->getQualifier(), decl->getDeclName());
    }

private:
    clang::Sema& sema;

    /// Cache of resolved dependent types, keyed by AST node pointer.
    /// Shared across resolve() calls within the same TU for performance.
    /// This is safe because a given AST node (DependentNameType*, etc.) has a
    /// unique identity within the TU — the same pointer always refers to the same
    /// syntactic occurrence. Different syntactic occurrences of the "same" type
    /// have different AST node pointers.
    llvm::DenseMap<const void*, clang::QualType> resolved;
};

}  // namespace clice
