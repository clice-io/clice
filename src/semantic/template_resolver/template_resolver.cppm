module clice:semantic.template_resolver;

import :compile.compilation;
import :support.prelude;

namespace clice::types {

/// What a parameter of the template a query is inside of stands for when
/// no instantiation in progress binds it.
struct ResolverOptions {
    /// The argument of the template's only instantiation in the unit. An
    /// index stored for other units to share leaves it off: the index of a
    /// header would differ with what each unit including it instantiates.
    bool only_instantiation = true;

    /// With only_instantiation, the instantiation of a function template
    /// whose arguments its parameters stand for, in place of its only one:
    /// the template may have several.
    const clang::FunctionDecl* instantiation = nullptr;

    /// Keep what each query cost and where its work was cut short
    /// (TemplateResolver::audits), for finding the code the resolver
    /// struggles with (`clice inspect template-resolver --audit`).
    bool audit = false;
};

/// Where a query's work was cut short, or a choice was left open.
enum class Trouble : std::uint8_t {
    /// Rewrites nested past the depth limit.
    Depth,
    /// The query ran out of steps.
    Steps,
    /// Evaluations nested past their limit.
    Evaluation,
    /// A lookup, a dependent name or a variable's deduction met itself.
    Cycle,
    /// The partial specializations that may apply find a member
    /// differently.
    Undecided,
    /// Bases find a member differently.
    AmbiguousBase,
};

/// What one query cost (TemplateResolver::audits).
struct QueryAudit {
    /// The rewrites it took.
    std::uint32_t steps = 0;

    /// How often each Trouble struck it.
    std::array<std::uint32_t, 6> troubles = {};
};

struct UnitCache;

class Visibility;

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
/// Its cache persists across the queries of the unit.
class TemplateResolver {
public:
    explicit TemplateResolver(clang::ASTContext& context, ResolverOptions options = {});

    explicit TemplateResolver(CompilationUnit& unit, ResolverOptions options = {});

    ~TemplateResolver();

    /// Answer the queries from here on as the place `location` is in sees
    /// the unit on its own (Visibility): a header the declarations of what
    /// it includes or imports before it, the focus file the whole unit. What
    /// a header's index records must not differ with what the units
    /// including it declared before it. Before any call, a query sees the
    /// whole unit.
    void see_from(clang::SourceLocation location);

    /// What each query cost, in the order they were asked, with
    /// ResolverOptions::audit; empty without.
    llvm::ArrayRef<QueryAudit> audits() const {
        return audited;
    }

    clang::QualType resolve(clang::QualType type);

    /// The class or enumeration `type` denotes once resolved. A dependent
    /// specialization yields the pattern real instantiation would use: the
    /// matching partial specialization, else the primary template. A
    /// concrete specialization is the specialization itself, which the TU
    /// may never have instantiated — only Sema can give it a definition.
    /// Null when the type does not resolve to a class or enumeration.
    clang::TagDecl* resolve_tag(clang::QualType type);

    /// Whether a specialization of the class template the dependent `type`
    /// names — explicit or partial, but the pattern resolve_tag chose — may
    /// apply once the template parameters in its arguments are known: the
    /// class resolve_tag gives then holds for some instantiations only.
    bool specializable(clang::QualType type);

    /// What a member access on `base` (`base.`, or `base->` when `arrow`)
    /// names its members in, resolved: what the base evaluates to, through
    /// `operator->` chains for an arrow. Null when it does not resolve.
    clang::QualType member_object(const clang::Expr* base, bool arrow);

    /// What `->` reaches from an object of class type `type` through its
    /// `operator->` chain — a smart pointer's pointee — resolved. Null when
    /// the class has no `operator->`.
    clang::QualType arrow_pointee(clang::QualType type);

    /// What an `auto` variable a dependent initializer left undeduced
    /// deduces to, resolved; with `only_instantiation`, inside a class
    /// template the unit instantiates once, as in that instantiation. Null
    /// when the initializer does not resolve.
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

    /// A dependent call's candidate set: the callee clang resolved itself
    /// when it did, else the resolved overload set filtered down to
    /// overloads whose parameter list can accept the call's argument count,
    /// and for a member call those its object binds best (`front()` or
    /// `front() const`). Full overload resolution needs conversion rules
    /// (Sema territory); arity and the object's qualifiers are the safe,
    /// conversion-free subset of it. A dependent operator (`a == b`) gets
    /// no candidates: instantiation adds the operands' associated operators
    /// and the built-in ones, so the operators its definition happened to
    /// see say nothing, and differ between the units including it. A call
    /// needing argument-dependent lookup takes in what that lookup finds
    /// for its arguments' resolved types; while one of them stays dependent,
    /// the visible overloads are all it has.
    llvm::SmallVector<const clang::NamedDecl*, 4> callees(const clang::CallExpr* expr);

    /// The overloads a dependent call being written, its arguments so far
    /// `written`, may call — clang's signature help ignores such a call: the
    /// callee's resolved overload set, at the first argument whole, past it
    /// the overloads that take another argument; for a member call those its
    /// object binds best.
    llvm::SmallVector<const clang::NamedDecl*, 4>
        callees(const clang::Expr* callee, llvm::ArrayRef<const clang::Expr*> written);

    /// The declarations the overload set `expr` names outside a call: of an
    /// unqualified or namespace-qualified name those the query sees
    /// (see_from), of a member every one.
    llvm::SmallVector<const clang::NamedDecl*, 4> overloads(const clang::OverloadExpr* expr);

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
    /// Where a query's cost goes, with ResolverOptions::audit.
    std::vector<QueryAudit>* auditing();

private:
    clang::ASTContext& context;

    /// The unit, for what its files see on their own (see_from).
    CompilationUnit* unit = nullptr;

    ResolverOptions options;

    std::unique_ptr<UnitCache> cache;

    /// Where the queries are answered from, once see_from is called.
    std::unique_ptr<Visibility> visibility;

    /// What each query cost, with ResolverOptions::audit.
    std::vector<QueryAudit> audited;
};

/// The pattern a specialization the unit names but never instantiated would
/// be instantiated from: the partial specialization real instantiation would
/// choose, else the primary template's pattern — also when the choice is
/// ambiguous or cannot be verified.
auto instantiation_pattern(const clang::ClassTemplateSpecializationDecl& specialization)
    -> const clang::NamedDecl*;

auto instantiation_pattern(const clang::VarTemplateSpecializationDecl& specialization)
    -> const clang::NamedDecl*;

}  // namespace clice::types
