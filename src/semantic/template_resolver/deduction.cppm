module;

/// Sema-free structural unification of template argument lists, replacing
/// `Sema::DeduceTemplateArguments` and `Sema::getMoreSpecializedPartialSpecialization`
/// for pseudo-instantiation, and the folding of the constant expressions it
/// checks a match with.
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

module clice:semantic.template_resolver.deduction;

import :support.prelude;

namespace clice::types {

/// The result of deduce_arguments.
enum class Deduction : std::uint8_t {
    /// The pattern cannot match the arguments.
    Failed,
    /// Matched, and every check on the match came out true.
    Matched,
    /// Matched structurally, but a non-deduced argument (`P<N, N + 1>`) or
    /// an associated constraint could not be decided under the bindings.
    Unverified,
};

/// How deduce_arguments treats what the match leaves open.
///
/// The arguments' own template parameters may stand for anything, and a
/// match that holds only for some of what they stand for is undecided
/// (Deduction::Unverified): two arguments for one parameter that may turn
/// out the same type (`is_same<T, T>` against `<K, V>`), and a `bool` the
/// pattern fixes against a value not known (`conditional<false, ...>`), half
/// of what it may hold. A shape the pattern gives a type (`vector<bool>`
/// against `vector<T>`), and one value of a wider type (`array<T, 0>`
/// against `array<T, N>`), are special cases instead: they do not match,
/// and the primary template stands for the rest.
struct DeductionOptions {
    /// Value arguments that may hold any value — defaults that do not fold:
    /// a value pattern meeting one is undecided whatever its type.
    llvm::ArrayRef<const clang::Expr*> open;

    /// Leave a parameter the match binds nothing to null rather than fail
    /// the deduction: a function template's defaults fill it after the
    /// deduction from the call.
    bool partial = false;
};

/// Deduce the arguments of `params` at its own depth by matching `patterns`
/// against `arguments`. An unbound pack deduces as empty; any other unbound
/// parameter fails the deduction, unless `options.partial`. Default
/// arguments are not consulted here — the caller fills them (with its own
/// instantiation stack) before or after deducing.
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
                           llvm::SmallVectorImpl<clang::TemplateArgument>& deduced,
                           const DeductionOptions& options = {});

/// Whether `arguments` may name the explicit specialization whose arguments
/// are `specialized` without certainly naming it: they match them but for
/// what the match leaves undecided (see DeductionOptions), `__conditional<B>`
/// with `B` not known and `__conditional<false>`.
bool may_name(clang::ASTContext& context,
              llvm::ArrayRef<clang::TemplateArgument> specialized,
              llvm::ArrayRef<clang::TemplateArgument> arguments,
              const DeductionOptions& options = {});

/// What the name of `record`, a class template's pattern or a partial
/// specialization, stands for inside it: the specialization whose arguments
/// are its own parameters as written. The canonical form names a parameter
/// by its depth and index alone, which another template's parameter at the
/// same place would be taken for (`variant<_Types...>`'s `_Types` for
/// `conjunction<_First, _Rest...>`'s `_First`).
clang::QualType injected_specialization(clang::ASTContext& context,
                                        const clang::CXXRecordDecl* record);

/// Whether `lhs` and `rhs` are one type, by their shapes: none while the
/// template parameters in them leave it open. A type never holds itself, so
/// a parameter is never what a pointer, a function or a class template's
/// specialization built on it is (`K` and `pair<const K, V>`); a dependent
/// name may be anything, the parameter included.
std::optional<bool> same_type(clang::ASTContext& context, clang::QualType lhs, clang::QualType rhs);

/// The result of select_partial: a winner, or why there is none. Real
/// instantiation diagnoses an ambiguity (no partial dominates every other
/// match), and a winner whose match is Unverified may not apply at all.
/// Callers degrade on both rather than pick arbitrarily or trust the
/// unverified.
enum class PartialResult : std::uint8_t {
    None,
    Selected,
    Ambiguous,
    Unverified,
};

template <typename Partial>
struct PartialChoice {
    PartialResult result = PartialResult::None;
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

/// What a standard type-category trait `expr` spells — `std::is_object_v<T>`,
/// `std::is_object<T>::value` or `__is_object(T)` — says of its operand, or
/// `__is_same(T, U)` of its two, `bound` giving an operand under the
/// caller's bindings; none when it is no such trait, or the operands'
/// shapes leave it open (a template parameter may become any type).
std::optional<bool> type_trait(clang::ASTContext& context,
                               const clang::Expr* expr,
                               llvm::function_ref<clang::QualType(clang::QualType)> bound);

/// If `expr` is a (possibly parenthesized/casted) reference to a non-type
/// template parameter, return its declaration.
const clang::NonTypeTemplateParmDecl* referenced_nttp(const clang::Expr* expr);

/// Whether some arguments for the template parameters `lhs` and `rhs` name
/// may make the two lists the same: the gate of a deeper look, not a match
/// — a parameter matches anything, a value or a form not compared
/// structurally may hold anything, and a parameter named twice is not held
/// to one argument.
bool may_match(clang::ASTContext& context,
               llvm::ArrayRef<clang::TemplateArgument> lhs,
               llvm::ArrayRef<clang::TemplateArgument> rhs);

/// A parameter pack an expansion pattern names.
struct PackParameter {
    /// Null for a canonical type parameter, which carries only its depth
    /// and index.
    const clang::NamedDecl* decl;

    unsigned depth;

    unsigned index;
};

/// The parameter packs `pattern` names — type, value and template packs —
/// each once.
llvm::SmallVector<PackParameter, 2> packs_in(const clang::TemplateArgument& pattern);

}  // namespace clice::types
