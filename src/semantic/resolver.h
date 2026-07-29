#pragma once

#include "clang/AST/ExprCXX.h"
#include "clang/AST/Type.h"

namespace clice {

/// This class is used to resolve dependent names in the unit.
/// For dependent names, we cannot know the any information about the name until
/// the template is instantiated. This can be frustrating, you cannot get
/// completion, you cannot get go-to-definition, etc. To avoid this, we just use
/// some heuristics to simplify the dependent names as normal type/expression.
/// For example, `std::vector<T>::value_type` can be simplified as `T`.
///
/// Resolution is pure AST computation: it never enters Sema, so speculative
/// lookups cannot emit diagnostics, register specializations, or otherwise
/// mutate the unit's semantic state.
///
/// Thread safety: NOT thread-safe. Each compilation unit should have its own resolver.
/// The `resolved` cache persists across multiple resolve() calls on the same unit.
class TemplateResolver {
public:
    explicit TemplateResolver(clang::ASTContext& context) : context(context) {}

    clang::QualType resolve(clang::QualType type);

    using lookup_result = clang::DeclContext::lookup_result;

    /// Look up the name in the given nested name specifier.
    lookup_result lookup(const clang::NestedNameSpecifier* NNS, clang::DeclarationName name);

    lookup_result lookup(const clang::DependentNameType* type) {
        return lookup(type->getQualifier(), type->getIdentifier());
    }

    lookup_result lookup(const clang::DependentTemplateSpecializationType* type);

    lookup_result lookup(const clang::DependentScopeDeclRefExpr* expr) {
        return lookup(expr->getQualifier(), expr->getNameInfo().getName());
    }

    lookup_result lookup(const clang::UnresolvedLookupExpr* expr);

    /// Resolve the base type through pseudo-instantiation, then look the
    /// member up in the resolved record. Complements the candidates clang
    /// already stores on the expression: pseudo-instantiation can reach
    /// members of matching partial specializations.
    lookup_result lookup(const clang::UnresolvedMemberExpr* expr);

    /// Resolve a dependent call's candidate set, filtered down to overloads
    /// whose parameter list can accept the call's argument count. Full
    /// overload resolution needs conversion rules (Sema territory); arity is
    /// the safe, conversion-free subset of it.
    llvm::SmallVector<clang::NamedDecl*, 4> lookup(const clang::CallExpr* expr);

    /// Resolve the base type through pseudo-instantiation, then look the
    /// member up in the resolved record (e.g. `this->foo()` inherited from
    /// `Base<T>`).
    lookup_result lookup(const clang::CXXDependentScopeMemberExpr* expr);

    lookup_result lookup(const clang::UnresolvedUsingValueDecl* decl) {
        return lookup(decl->getQualifier(), decl->getDeclName());
    }

    lookup_result lookup(const clang::UnresolvedUsingTypenameDecl* decl) {
        return lookup(decl->getQualifier(), decl->getDeclName());
    }

private:
    clang::ASTContext& context;

    /// Cache of resolved dependent types, keyed by AST node pointer.
    /// Shared across resolve() calls within the same TU for performance.
    /// This is safe because a given AST node (DependentNameType*, etc.) has a
    /// unique identity within the TU — the same pointer always refers to the same
    /// syntactic occurrence. Different syntactic occurrences of the "same" type
    /// have different AST node pointers.
    llvm::DenseMap<const void*, clang::QualType> resolved;
};

}  // namespace clice
