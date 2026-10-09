module;

#include "modules/prelude.h"

module clice:semantic.resolver;

namespace clice::types {

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

    /// The class or enumeration `type` denotes once resolved. A dependent
    /// specialization yields the pattern real instantiation would use: the
    /// matching partial specialization, else the primary template. A
    /// concrete specialization is the specialization itself, which the TU
    /// may never have instantiated — only Sema can give it a definition.
    /// Null when the type does not resolve to a class or enumeration.
    clang::TagDecl* resolve_tag(clang::QualType type);

    /// What a member access on `base` (`base.`, or `base->` when `arrow`)
    /// names its members in, resolved: what the base evaluates to, through
    /// `operator->` chains for an arrow. Null when it does not resolve.
    clang::QualType member_object(const clang::Expr* base, bool arrow);

    /// What an `auto` variable a dependent initializer left undeduced
    /// deduces to, resolved. Null when the initializer does not resolve.
    clang::QualType deduce(const clang::VarDecl* var);

    using lookup_result = clang::DeclContext::lookup_result;

    /// Look up the name in the given nested name specifier.
    lookup_result lookup(clang::NestedNameSpecifier NNS, clang::DeclarationName name);

    lookup_result lookup(const clang::DependentNameType* type) {
        return lookup(type->getQualifier(), type->getIdentifier());
    }

    /// Look up a dependent template name (`T::template rebind`); returns an
    /// empty result for specializations whose template is already known.
    lookup_result lookup(const clang::TemplateSpecializationType* type);

    lookup_result lookup(const clang::DependentScopeDeclRefExpr* expr) {
        return lookup(expr->getQualifier(), expr->getNameInfo().getName());
    }

    lookup_result lookup(const clang::UnresolvedLookupExpr* expr);

    /// Resolve the base type through pseudo-instantiation, then look the
    /// member up in the resolved record. Complements the candidates clang
    /// already stores on the expression: pseudo-instantiation can reach
    /// members of matching partial specializations.
    lookup_result lookup(const clang::UnresolvedMemberExpr* expr);

    /// A dependent call's candidate set: the callee clang resolved itself
    /// when it did, else the resolved overload set filtered down to
    /// overloads whose parameter list can accept the call's argument count,
    /// and for a member call those its object binds best (`front()` or
    /// `front() const`). Full overload resolution needs conversion rules
    /// (Sema territory); arity and the object's qualifiers are the safe,
    /// conversion-free subset of it. A dependent operator (`a == b`) gets
    /// no candidates: instantiation adds the operands' associated operators
    /// and the built-in ones, so the operators its definition happened to
    /// see say nothing, and differ between the units including it.
    llvm::SmallVector<const clang::NamedDecl*, 4> lookup(const clang::CallExpr* expr);

    /// Resolve the base type through pseudo-instantiation, then look the
    /// member up in the resolved record (e.g. `this->foo()` inherited from
    /// `Base<T>`). A base that is itself a dependent member access, call,
    /// subscript or `auto` variable (`box.inner.leaf`, `rows[0].size`) is
    /// resolved to the type it evaluates to first.
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

}  // namespace clice::types
