module;

#include "modules/prelude.h"

module clice:semantic.unifier;

namespace clice::types {

/// Sema-free structural unification of template argument lists, replacing
/// `Sema::DeduceTemplateArguments` and `Sema::getMoreSpecializedPartialSpecialization`
/// for pseudo-instantiation.
///
/// Differences from clang's deduction, by design:
///   - Works on sugared types: a parameter binds the argument *as written*
///     (e.g. `T = std::string`, not `T = std::basic_string<char, ...>`), so no
///     separate resugar pass is needed downstream.
///   - Binds template parameters to dependent arguments (`T = U`), which real
///     deduction never faces but pseudo-instantiation relies on.
///   - Skips conformance corners irrelevant to lookup (reference collapsing
///     adjustments, array bound promotion). Failure means "this pattern
///     doesn't match", which degrades to an unresolved name.
class Unifier {
public:
    using TemplateArguments = llvm::ArrayRef<clang::TemplateArgument>;

    explicit Unifier(clang::ASTContext& context, unsigned depth, unsigned size) :
        context(context), depth(depth), bindings(size) {}

    /// Unify `patterns` (a partial specialization's argument pattern or a
    /// primary template's injected arguments) against `arguments`. On success,
    /// every deduced parameter at `depth` is recorded in `bindings`.
    bool unify(TemplateArguments patterns, TemplateArguments arguments);

    /// The deduced arguments, indexed by parameter index. Unbound parameters
    /// hold a null TemplateArgument.
    TemplateArguments results() const {
        return bindings;
    }

    /// A compound expression pattern (`N + 1`) and the argument it met.
    /// Unification cannot deduce from it; it constrains the match only once
    /// the bindings are substituted in, which deduce_arguments checks.
    struct Deferred {
        const clang::Expr* pattern;
        clang::TemplateArgument argument;
    };

    llvm::ArrayRef<Deferred> deferred() const {
        return deferred_checks;
    }

private:
    bool unify(const clang::TemplateArgument& pattern, const clang::TemplateArgument& argument);

    bool unify(clang::QualType pattern, clang::QualType argument);

    /// Extract a template-id (template + arguments) view of `type`, looking
    /// through TST, ClassTemplateSpecializationDecl records and injected class
    /// names. Returns false if `type` is not a template-id.
    bool template_id(clang::QualType type,
                     clang::TemplateName& name,
                     TemplateArguments& arguments) const;

    /// Argument equality for repeated bindings: canonical structural
    /// equality, with bare references to the same NTTP compared by decl
    /// (canonical dependent expressions keep distinct node identities).
    bool equivalent(const clang::TemplateArgument& lhs, const clang::TemplateArgument& rhs) const;

    bool bind(unsigned index, const clang::TemplateArgument& argument);

    /// Record one element of a pack parameter's binding while matching a
    /// structured pack expansion pattern (`box<Us>...`) element-wise.
    bool collect(unsigned index, const clang::TemplateArgument& argument);

    clang::ASTContext& context;
    unsigned depth;
    llvm::SmallVector<clang::TemplateArgument, 4> bindings;
    llvm::SmallVector<Deferred, 1> deferred_checks;

    /// Element-wise matching state for a structured pack expansion pattern:
    /// while `expanding`, pack parameters accumulate one element per matched
    /// argument instead of binding directly. `element_ordinal` is the index
    /// of the argument currently being matched, so repeated pack references
    /// within one element are checked for consistency instead of appended.
    bool expanding = false;
    unsigned element_ordinal = 0;
    llvm::SmallVector<llvm::SmallVector<clang::TemplateArgument, 2>, 2> elements;
};

/// The outcome of deduce_arguments.
enum class Deduction : std::uint8_t {
    /// The pattern cannot match the arguments.
    Failed,
    /// Matched, and every check on the match came out true.
    Matched,
    /// Matched structurally, but a non-deduced argument (`P<N, N + 1>`) or
    /// an associated constraint could not be decided under the bindings.
    Unverified,
};

/// Deduce the arguments of `params` at its own depth by matching `patterns`
/// against `arguments`. An unbound pack deduces as empty; any other unbound
/// parameter fails the deduction. Default arguments are not consulted here —
/// the caller fills them (with its own instantiation stack) before deducing.
///
/// After deduction, the non-deduced expression arguments and the list's
/// associated constraints are checked with the bindings substituted: one
/// that is provably false fails the deduction, one that cannot be decided
/// leaves it Unverified.
///
/// `patterns` and `params` come in the same pairings the resolver already
/// uses: injected arguments for primary templates and alias templates,
/// `getTemplateArgs()` for partial specializations.
Deduction deduce_arguments(clang::ASTContext& context,
                           clang::TemplateParameterList* params,
                           llvm::ArrayRef<clang::TemplateArgument> patterns,
                           llvm::ArrayRef<clang::TemplateArgument> arguments,
                           llvm::SmallVectorImpl<clang::TemplateArgument>& deduced);

/// The verdict of select_partial: a winner, or why there is none. Real
/// instantiation diagnoses an ambiguity (no partial dominates every other
/// match), and a winner whose match is Unverified may not apply at all.
/// Callers degrade on both rather than pick arbitrarily or trust the
/// unverified.
enum class PartialVerdict : std::uint8_t {
    None,
    Selected,
    Ambiguous,
    Unverified,
};

template <typename Partial>
struct PartialChoice {
    PartialVerdict verdict = PartialVerdict::None;
    /// Set for Selected only.
    Partial* winner = nullptr;
};

/// A partial specialization whose pattern deduced against the arguments.
template <typename Partial>
struct PartialMatch {
    Partial* partial;
    /// Whether the deduction came out Matched rather than Unverified.
    bool verified;
};

/// The partial specialization real instantiation would pick among
/// `viable`. Ordering is structural, plus the one constraint rule that needs
/// no subsumption: of two equivalent patterns, a constrained partial is more
/// specialized than an unconstrained one.
PartialChoice<clang::ClassTemplatePartialSpecializationDecl> select_partial(
    clang::ASTContext& context,
    llvm::ArrayRef<PartialMatch<clang::ClassTemplatePartialSpecializationDecl>> viable);

PartialChoice<clang::VarTemplatePartialSpecializationDecl> select_partial(
    clang::ASTContext& context,
    llvm::ArrayRef<PartialMatch<clang::VarTemplatePartialSpecializationDecl>> viable);

/// `value` converted to the integral or enumeration type `type` the way an
/// integral conversion would; nullopt when `type` is not one, or is still
/// dependent.
std::optional<llvm::APSInt> convert_integral(clang::ASTContext& context,
                                             const llvm::APSInt& value,
                                             clang::QualType type);

/// The value of an integral constant expression that may still reference
/// template parameters. `value_of` supplies the value of each name clang's
/// constant evaluator cannot see through — a template parameter, a
/// dependent member — or nullopt. Arithmetic, comparison, logical,
/// conditional operators and integral casts are folded here; any other
/// value-dependent form is unknown. The value has the width of `expr`'s type
/// when that type is known.
std::optional<llvm::APSInt>
    evaluate_integral(clang::ASTContext& context,
                      const clang::Expr* expr,
                      llvm::function_ref<std::optional<llvm::APSInt>(const clang::Expr*)> value_of);

/// If `expr` is a (possibly parenthesized/casted) reference to a non-type
/// template parameter, return its declaration.
const clang::NonTypeTemplateParmDecl* referenced_nttp(const clang::Expr* expr);

}  // namespace clice::types
