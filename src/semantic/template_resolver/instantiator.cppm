module;

/// Pseudo-instantiation of dependent C++ types: heuristic lookup in primary
/// templates and partial specializations, driven by a hand-written
/// QualType → QualType rewriter with two policies:
///   ├─ Policy::Substitute — expand typedefs/aliases and substitute template
///   │                       parameters from the stack; dependent names pass
///   │                       through untouched (no lookup)
///   └─ Policy::Resolve    — Substitute plus heuristic resolution of
///                           dependent names and dependent template
///                           specializations via member lookup and
///                           argument deduction
///
/// Key invariant: typedef/alias expansion always runs under Policy::Substitute,
/// so it can never re-enter heuristic lookup. Violating this causes
/// typedef ↔ lookup infinite cycles.
///
/// Everything is pure AST computation (deduction + ASTContext node
/// construction); Sema and TreeTransform are deliberately not used, so
/// resolution cannot emit diagnostics or mutate the unit's semantic state.

module clice:semantic.template_resolver.instantiator;

import :semantic.template_resolver;
import :semantic.template_resolver.deduction;
import :support.prelude;

namespace clice::types {

/// A helper class to record the instantiation stack.
struct InstantiationStack {
    using Arguments = llvm::SmallVector<clang::TemplateArgument, 4>;
    using TemplateArguments = llvm::ArrayRef<clang::TemplateArgument>;

    struct Frame {
        clang::Decl* decl;
        clang::TemplateParameterList* params;
        Arguments arguments;
    };

    llvm::SmallVector<Frame> data;

    bool empty() const {
        return data.empty();
    }

    void push(clang::Decl* decl,
              clang::TemplateParameterList* params,
              TemplateArguments arguments) {
        data.emplace_back(decl, params, Arguments(arguments.begin(), arguments.end()));
    }

    void pop() {
        data.pop_back();
    }

    unsigned size() const {
        return data.size();
    }

    /// Drop every frame pushed since the stack had `size` frames.
    void truncate(unsigned size) {
        data.truncate(size);
    }

    auto& frames() {
        return data;
    }

    /// Stable handle to a binding: (frame index, argument index). Pointers
    /// into the frame vector go stale whenever a nested rewrite pushes a
    /// frame and the vector reallocates; indices survive because pushes only
    /// append and the walk never outlives the frames below it.
    using Slot = std::pair<unsigned, unsigned>;

    /// Look up a template parameter's binding, innermost frame first.
    ///
    /// When the parameter's declaration is known, only the frame whose
    /// parameter list actually contains that declaration matches — unrelated
    /// templates that merely share the same depth (e.g. `test` and
    /// `__alloc_traits`, both at depth 0) never capture each other's
    /// parameters. If no frame owns the declaration, the parameter is left
    /// unsubstituted rather than guessed.
    ///
    /// Canonical parameters carry no declaration; those fall back to matching
    /// the frame's parameter list depth.
    std::optional<Slot> find_slot(const clang::NamedDecl* decl,
                                  unsigned depth,
                                  unsigned index) const {
        if(decl) {
            for(unsigned f = data.size(); f > 0; f -= 1) {
                const auto& frame = data[f - 1];
                if(frame.params && index < frame.params->size() &&
                   frame.params->getParam(index) == decl) {
                    if(index < frame.arguments.size()) {
                        return Slot{f - 1, index};
                    }
                    return std::nullopt;
                }
            }
            return std::nullopt;
        }

        for(unsigned f = data.size(); f > 0; f -= 1) {
            const auto& frame = data[f - 1];
            if(frame.params && frame.params->getDepth() == depth) {
                if(index < frame.arguments.size()) {
                    return Slot{f - 1, index};
                }
                return std::nullopt;
            }
        }
        return std::nullopt;
    }

    /// Whether a frame holds the parameter's template, bound or not.
    bool owns(const clang::NamedDecl* decl, unsigned depth) const {
        return ranges::any_of(data, [&](const Frame& frame) {
            if(!frame.params) {
                return false;
            }
            return decl ? ranges::contains(frame.params->asArray(), decl)
                        : frame.params->getDepth() == depth;
        });
    }

    clang::TemplateArgument& slot(Slot handle) {
        return data[handle.first].arguments[handle.second];
    }

    const clang::TemplateArgument* find_argument(const clang::NamedDecl* decl,
                                                 unsigned depth,
                                                 unsigned index) const {
        auto handle = find_slot(decl, depth, index);
        return handle ? &data[handle->first].arguments[handle->second] : nullptr;
    }
};

/// The function `decl` names, through a using-declaration; null for any
/// other declaration.
const inline clang::FunctionDecl* as_function(const clang::NamedDecl* decl) {
    if(auto* shadow = llvm::dyn_cast<clang::UsingShadowDecl>(decl)) {
        decl = shadow->getTargetDecl();
    }
    return decl->getAsFunction();
}

/// Drop the overloads that cannot accept `count` arguments.
void drop_by_arity(llvm::SmallVectorImpl<const clang::NamedDecl*>& candidates,
                   unsigned count,
                   bool member_call);

/// Whether `type` still refers to a parameter of `params`.
bool mentions_parameters(clang::QualType type, const clang::TemplateParameterList* params);

/// An argument of a call as deduction sees it: its type, resolved, and
/// whether it is an lvalue.
struct CallArgument {
    clang::QualType type;

    bool lvalue;

    /// The argument as written, when the call spells it.
    const clang::Expr* expr = nullptr;
};

/// The functions a dependent call may call.
struct Callees {
    llvm::SmallVector<const clang::NamedDecl*, 4> candidates;

    /// Whether the call may call no other function: false for a call
    /// needing argument-dependent lookup whose arguments' types do not all
    /// resolve — its instantiation may find more.
    bool complete = true;

    /// The call's arguments, resolved in the caller's terms; none when one
    /// does not resolve or expands a pack.
    std::optional<llvm::SmallVector<CallArgument, 4>> arguments;
};

/// The functions named `name` that argument-dependent lookup finds for a
/// call with `arguments` and that may take them: those of the namespaces
/// the arguments' types are associated with, and the friends their
/// associated classes declare. None when one of the types is still
/// dependent: its associated namespaces wait for instantiation.
std::optional<llvm::SmallVector<const clang::NamedDecl*, 4>>
    argument_dependent_lookup(clang::DeclarationName name, llvm::ArrayRef<CallArgument> arguments);

/// The core pseudo-instantiation engine, one query's state. Resolves
/// dependent names by looking up members in primary templates and partial
/// specializations — a capability clang's own instantiation machinery does
/// not have (it only ever sees concrete arguments, so e.g. alias templates
/// never appear on its paths).
///
/// Resolution flow for `typename A<T>::type`:
///   1. rewrite() dispatches the DependentNameType to resolve_dependent_name
///   2. lookup(A<T>, "type") → deduce_template_arguments → find member decl
///   3. substitute(underlying_type) expands typedefs + substitutes params
///   4. Pop lookup frames, then rewrite the result for further resolution
///
/// Uses `resolving` / `looking_up` for cycle detection. Its operations are
/// split by what they answer: rewriting a type (rewrite.cpp), its
/// specializations (specializations.cpp), binding parameters
/// (bindings.cpp), looking names up (lookup.cpp), choosing the pattern
/// (patterns.cpp), evaluating expressions (expressions.cpp) and a call's
/// candidates (overloads.cpp).
class PseudoInstantiator {
public:
    using TemplateArguments = llvm::ArrayRef<clang::TemplateArgument>;

    using lookup_result = clang::DeclContext::lookup_result;

    PseudoInstantiator(clang::ASTContext& context,
                       llvm::DenseMap<const void*, clang::QualType>& resolved,
                       const ResolverOptions& options) :
        context(context), resolved(resolved), options(options) {}

    /// Rewrite policy. The two-phase split is the resolver's core invariant:
    /// typedef/alias expansion must never re-enter heuristic lookup, or
    /// mutually recursive typedefs cycle forever.
    enum class Policy : std::uint8_t {
        /// Expand sugar and substitute stack parameters only.
        Substitute,
        /// Substitute plus dependent name resolution through lookup.
        Resolve,
    };

    clang::QualType resolve(clang::QualType type);

    clang::QualType substitute(clang::QualType type);

    /// Entry point for all type rewriting. Guards against:
    /// - Null types (return as-is)
    /// - Non-dependent types (no transformation needed)
    /// - Excessive recursion depth (bail out to prevent runaway recursion)
    /// - Exhausted step budget (bounds the total work of one query, including
    ///   pseudo-SFINAE probes that explore rejected branches)
    /// - Null results (return original type instead)
    clang::QualType rewrite(clang::QualType type, Policy policy);

    /// Look up `name` in the given type. First rewrites the type (to substitute
    /// any template parameters in it), then looks in the class it names: a
    /// template specialization dispatches on its template, a record is
    /// searched directly.
    ///
    /// On success, the frames the lookup pushed stay on the stack so the
    /// caller can substitute what the found declaration is written in; the
    /// caller pops them.
    lookup_result lookup(clang::QualType type, clang::DeclarationName name);

    lookup_result lookup(clang::NestedNameSpecifier NNS, clang::DeclarationName name);

    /// `name` as a member of `object`, written or resolved.
    lookup_result lookup_member(clang::QualType object, clang::DeclarationName name);

    /// The pattern real instantiation of `CTD<written>` would use: the most
    /// specialized viable partial specialization, else the primary template.
    /// Null when the arguments do not fit the parameter list or the choice
    /// degrades (see select_partial).
    clang::CXXRecordDecl* select_pattern(clang::ClassTemplateDecl* CTD, TemplateArguments written);

    /// The same for a variable template.
    clang::VarDecl* select_pattern(clang::VarTemplateDecl* VTD, TemplateArguments written);

    /// Bind `params`, which name the parameters of `owner`, to `arguments`
    /// for the rest of this query.
    void assume(clang::ClassTemplateDecl* owner,
                clang::TemplateParameterList* params,
                TemplateArguments arguments);

    /// What the `auto` variable `var` deduces to from its initializer, by
    /// the rules of a function template parameter of its declared type: a
    /// value drops the initializer's cv-qualifiers and decays an array, a
    /// reference keeps them, and `auto&&` binds an lvalue as an lvalue
    /// reference. `decltype(auto)` takes the initializer's type as is.
    /// Null when the initializer does not resolve or the declarator wraps
    /// `auto` in anything but pointers and references.
    clang::QualType deduce_auto(const clang::VarDecl* var);

    /// What `expr` evaluates to. Clang leaves the type unknown for a member
    /// access, call or subscript on a dependent operand, and undeduced for
    /// an `auto` variable; those take what type_of evaluates them to, a
    /// call's reference return type included. Any other type is the
    /// written one, for lookup to resolve.
    clang::QualType expr_type(const clang::Expr* expr);

    /// The object `type` denotes once resolved: an alias may resolve to a
    /// reference (`vector<T>::reference`), an expression never has one.
    clang::QualType resolve_object(clang::QualType type);

    /// What a member access on a base of type `base` names a member of:
    /// the base itself, or what `->` reaches from it. Null when unresolved.
    clang::QualType member_object(clang::QualType base, bool arrow);

    /// What the dependent member access `expr` names a member of: its base
    /// evaluated (see expr_type), or `this` for an implicit access.
    clang::QualType member_object(const clang::CXXDependentScopeMemberExpr* expr);

    /// What `->` reaches from `type`: the pointee, once overloaded
    /// `operator->` chains (smart pointers) unwrap to a raw pointer;
    /// bounded. Null when the chain never gets there (no operator->, or a
    /// cycle): the arrow is ill-formed, and treating it like a dot access
    /// would fabricate candidates.
    clang::QualType arrow_pointee(clang::QualType type);

    /// A call's candidate set: the callee clang resolved itself (a call
    /// that is only value-dependent, a member of the current
    /// instantiation), else the resolved overload set filtered down to
    /// overloads whose parameter list can accept the call's argument count
    /// and whose parameters its arguments can initialize, and for a member
    /// call those its object can bind best. Only what certainly fails is
    /// dropped: ranking the rest is full overload resolution (Sema
    /// territory). The frames of the lookup that found the set stay on the
    /// stack.
    Callees call_candidates(const clang::CallExpr* expr);

    /// The overloads a dependent `callee` may call, its call written as far
    /// as the `written` arguments: at the first argument every one, past it
    /// those that take one more; for a member call those its object binds
    /// best.
    llvm::SmallVector<const clang::NamedDecl*, 4>
        written_call_candidates(const clang::Expr* callee,
                                llvm::ArrayRef<const clang::Expr*> written);

private:
    /// Per-kind dispatch. Whitelist of type classes the resolver understands;
    /// anything else passes through unchanged, which downstream treats as
    /// unresolved. Local qualifiers are stripped here and reapplied on the
    /// rewritten result.
    clang::QualType rewrite_type(clang::QualType type, Policy policy);

    clang::QualType rewrite_parameter(const clang::TemplateTypeParmType* TTPT, Policy policy);

    /// What the transform `kind` (`__add_pointer`, `__remove_cvref`) makes
    /// of `base`, as Sema computes it, where the shape of `base` decides it:
    /// not for a bare template parameter, which may become a reference, nor
    /// for the signedness transforms. Null where it does not.
    clang::QualType transformed(clang::UnaryTransformType::UTTKind kind, clang::QualType base);

    clang::QualType resolve_dependent_name(const clang::DependentNameType* DNT);

    /// Resolve a specialization whose template is a dependent name
    /// (`T::template rebind<U>`) to a concrete TST when lookup finds the
    /// template.
    clang::QualType resolve_dependent_template(const clang::TemplateSpecializationType* TST);

    /// Build a template specialization type from as-written (flat) arguments.
    ///
    /// The canonical argument list must mirror Sema's argument conversion or
    /// the produced type would never compare equal to a parsed `X<...>`: the
    /// trailing arguments of a parameter pack are grouped into a single Pack
    /// argument, while the specified list stays flat as written.
    clang::QualType make_specialization(clang::TemplateName name, TemplateArguments arguments);

    clang::QualType rewrite_template(const clang::TemplateSpecializationType* TST, Policy policy);

    /// Rewrite a template argument list. Returns true if anything changed.
    /// Pack expansions whose pattern is a bound pack parameter are spliced
    /// inline, so `type_list<Us...>` with `Us = {int, float}` becomes
    /// `type_list<int, float>`.
    bool rewrite_arguments(TemplateArguments arguments,
                           llvm::SmallVectorImpl<clang::TemplateArgument>& out,
                           Policy policy);

    /// Element-wise expansion of a structured pack pattern: if every pack
    /// parameter referenced in `pattern` is bound to a Pack of one common
    /// length, rewrite the pattern once per element and return the list.
    /// Returns nullopt when the pattern has no bound packs, lengths disagree,
    /// or an element fails to rewrite cleanly — callers then fall back to
    /// rewriting the pattern as a whole.
    std::optional<llvm::SmallVector<clang::TemplateArgument, 4>>
        expand_pack_pattern(clang::QualType pattern, Policy policy);

    /// Rewrites the qualifier of a dependent template name. Substitution can
    /// make the scope concrete (`traits<A>::template rebind` once `A` is
    /// bound), and a dependent template name cannot carry a non-dependent
    /// qualifier; the name is then looked up in that scope, while the
    /// bindings that made it concrete are still in place. Nullopt when the
    /// scope became concrete but names no template there.
    std::optional<clang::TemplateName> rewrite_template_name(clang::TemplateName name,
                                                             Policy policy);

    clang::NestedNameSpecifier rewrite_specifier(clang::NestedNameSpecifier NNS, Policy policy);

    /// The argument the template parameter `param`, `index` in its list at
    /// `depth`, is bound to: by the frame holding its template when a frame
    /// does — the query names a specialization of it or calls it —, else,
    /// for a parameter of the template the query is inside of, by the
    /// template's only instantiation. Null when unbound.
    const clang::TemplateArgument* bound_argument(const clang::NamedDecl* param,
                                                  unsigned depth,
                                                  unsigned index) const;

    /// The argument a function template's parameter took in the template's
    /// only instantiation: a generic lambda's `auto` parameter stands for
    /// the type the lambda is called with. Several instantiations give no
    /// one answer. A class template's parameters stay as they are: the
    /// resolver reads its patterns with them unbound, save where a query
    /// binds them first (TemplateResolver::deduce).
    const clang::TemplateArgument* only_instantiation_argument(const clang::NamedDecl* param,
                                                               unsigned index) const;

    /// Push `TD`'s parameters bound to `arguments`, once per redeclaration:
    /// a default inherited from an earlier redeclaration still references
    /// that declaration's parameter decls, and decl-pointer matching must
    /// find them wherever the default was written. Returns the frame count
    /// to truncate back to.
    unsigned push_parameters(clang::TemplateDecl* TD, TemplateArguments arguments);

    /// `arguments` with the default template arguments of `TD`'s parameters
    /// after them filled in, into `out`; false when they do not fit the
    /// parameter list. Defaults are computed under the arguments supplied so
    /// far, so earlier parameters can appear in them (e.g. `allocator<_Tp>`
    /// for vector's `_Alloc`, `Config<Acc>::kStages` for a value). Template
    /// template defaults are filled when representable without building
    /// expressions.
    bool fill_defaults(clang::TemplateDecl* TD,
                       TemplateArguments arguments,
                       llvm::SmallVectorImpl<clang::TemplateArgument>& out);

    /// Deduce the parameters of a template, or of a partial specialization,
    /// from `arguments`; unless that fails, the deduced frame is pushed for
    /// the caller to pop.
    Deduction deduce_template_arguments(clang::ClassTemplateDecl* CTD, TemplateArguments arguments);

    Deduction deduce_template_arguments(clang::ClassTemplatePartialSpecializationDecl* partial,
                                        TemplateArguments arguments);

    Deduction deduce_template_arguments(clang::VarTemplatePartialSpecializationDecl* partial,
                                        TemplateArguments arguments);

    Deduction deduce_template_arguments(clang::TypeAliasTemplateDecl* alias,
                                        TemplateArguments arguments);

    /// `patterns`, which may reference parameters of templates enclosing
    /// `decl`, with the current bindings of those substituted in, into
    /// `storage` when that changes them: they are concrete from the
    /// deduction of `list`, the parameters of `decl`, which compares them
    /// structurally (a member partial specialization pinning an outer
    /// parameter, e.g. `Inner<pair<O, U>>`) instead of guessing.
    TemplateArguments substitute_outer(clang::Decl* decl,
                                       clang::TemplateParameterList* list,
                                       TemplateArguments patterns,
                                       llvm::SmallVectorImpl<clang::TemplateArgument>& storage);

    /// Deduce `list`, the parameters of `decl`, by matching `patterns`
    /// against `arguments`, and push the deduced frame unless that fails.
    /// `kind` names `decl` in the log.
    Deduction push_deduction(clang::Decl* decl,
                             clang::TemplateParameterList* list,
                             TemplateArguments patterns,
                             TemplateArguments arguments,
                             llvm::StringRef kind);

    /// The parameters of `FTD`, the template of `function`, that a call with
    /// `arguments` deduces, null where it leaves one unbound; empty when
    /// the call cannot match. Simplified [temp.deduct.call]: a reference
    /// parameter matches its referred type (a forwarding reference an lvalue
    /// as an lvalue reference), any other the argument decayed and
    /// unqualified.
    llvm::SmallVector<clang::TemplateArgument, 4>
        deduce_call(clang::FunctionTemplateDecl* FTD,
                    const clang::FunctionDecl* function,
                    llvm::ArrayRef<CallArgument> arguments);

    /// When DeclContext::lookup returns multiple declarations (e.g. a member in
    /// both a base class and derived class), take the last one. This heuristic
    /// favors the most-derived declaration, though the ordering depends on clang's
    /// internal DeclContext storage.
    clang::Decl* preferred(lookup_result members);

    /// `name` in `record` itself, else in its bases.
    lookup_result lookup_in_record(clang::CXXRecordDecl* record, clang::DeclarationName name);

    /// Search for `name` in the base classes of `CRD`, each of them: a name
    /// found in two bases is ambiguous, as real lookup would diagnose, and
    /// degrades to empty; the same declarations of one specialization
    /// reached along two paths (`Left<T>::type` through a diamond) are not,
    /// those of two specializations (`X<T>` and `X<T*>`) are.
    ///
    /// IMPORTANT: when a member is found, stack frames pushed during the lookup
    /// are intentionally left intact. The caller (resolve_dependent_name)
    /// needs them to substitute the found decl's underlying type. The caller
    /// is responsible for popping frames after substitution.
    lookup_result lookup_in_bases(clang::CXXRecordDecl* CRD, clang::DeclarationName name);

    lookup_result lookup_in_base(const clang::CXXBaseSpecifier& base, clang::DeclarationName name);

    /// The arguments the lookup that found `members`, its frames those past
    /// `since`, bound the template they are members of to; empty for the
    /// members of a class that is no template's pattern.
    llvm::SmallVector<clang::TemplateArgument, 4> owner_arguments(lookup_result members,
                                                                  unsigned since);

    lookup_result lookup(clang::ClassTemplateDecl* CTD,
                         clang::DeclarationName name,
                         TemplateArguments written);

    /// `name` in `CTD<arguments>` where it is not certain which pattern
    /// applies — a defaulted argument does not fold, a constraint is not
    /// decided: what every pattern that may apply finds alike, a member of a
    /// base they share; else nothing. The frames of the primary's lookup
    /// stay, as lookup's do.
    lookup_result lookup_through_any(clang::ClassTemplateDecl* CTD,
                                     TemplateArguments arguments,
                                     clang::DeclarationName name);

    /// The partial specializations whose pattern deduces against
    /// `arguments` and survives the pseudo-SFINAE probe (see member_absent).
    llvm::SmallVector<PartialMatch<clang::ClassTemplatePartialSpecializationDecl>, 4>
        viable_partials(clang::ClassTemplateDecl* CTD, TemplateArguments arguments);

    llvm::SmallVector<PartialMatch<clang::VarTemplatePartialSpecializationDecl>, 4>
        viable_partials(clang::VarTemplateDecl* VTD, TemplateArguments arguments);

    /// `partial` as a match of `arguments`, unless its pattern does not
    /// deduce against them or fails the pseudo-SFINAE probe.
    template <typename Partial>
    std::optional<PartialMatch<Partial>> viable(Partial* partial, TemplateArguments arguments);

    /// Deduction alone may match several overlapping partials; pick the
    /// most specialized one among the viable ones, as real instantiation
    /// would.
    PartialChoice<clang::ClassTemplatePartialSpecializationDecl>
        match_partial(clang::ClassTemplateDecl* CTD, TemplateArguments arguments);

    /// Pseudo-SFINAE: decide whether a partial specialization's dependent
    /// pattern constraints (e.g. the `void_t<typename A::rebind<U>::other>`
    /// idiom), its arguments as `written`, are satisfiable under the current
    /// bindings.
    ///
    /// Real SFINAE substitutes and rejects on ill-formedness. We approximate
    /// with a three-way probe on each dependent member access in the pattern:
    ///   - member resolves → constraint holds, keep the partial
    ///   - qualifier resolves to a known template/record but the member does
    ///     not exist there → constraint provably fails, prune the partial
    ///   - qualifier unknown (bare parameter etc.) → benefit of the doubt,
    ///     keep the partial; never guess a concrete answer from uncertainty
    bool satisfies_pattern(const clang::ASTTemplateArgumentListInfo* written);

    /// Walk the written form of `type` (through alias sugar arguments, which
    /// is where `void_t` hides its probe expression) and report whether any
    /// dependent member access provably names a non-existent member.
    bool member_absent(clang::QualType type, unsigned guard);

    /// Does any link of the specifier chain provably name a missing member?
    bool specifier_absent(clang::NestedNameSpecifier NNS, unsigned guard);

    /// Does any specialization of `CTD` — partial or explicit — declare
    /// `name`? Used to gate the Absent result: if some specialization has
    /// the member, an empty lookup on dependent arguments proves nothing.
    bool any_specialization_declares(clang::ClassTemplateDecl* CTD, clang::DeclarationName name);

    /// Resolve `scope` and ask whether it is a known template or record that
    /// definitely has no member called `name`. Unknown scopes return false.
    ///
    /// "Definitely" requires a clean result: an empty lookup caused by the
    /// recursion guard or by step-budget exhaustion proves nothing, and
    /// treating it as absence would prune a partial that real SFINAE keeps —
    /// a wrong answer, not a degradation. Those cases stay Unknown.
    bool scope_lacks(clang::NestedNameSpecifier scope,
                     clang::DeclarationName name,
                     bool wants_template = false);

    /// The value of `expr` under the current bindings: template parameters
    /// take their bound values, dependent static members and enumerators
    /// (`Config<T>::kStages`) are looked up and their initializers folded in
    /// turn.
    std::optional<llvm::APSInt> evaluate(const clang::Expr* expr);

    /// The value of a name the folding cannot see through.
    std::optional<llvm::APSInt> value_of(const clang::Expr* expr);

    /// The type a dependent expression evaluates to, resolved: a name, a
    /// member access or a call takes the found declaration's type — the
    /// callee's return type for a call — with the bindings of the lookup
    /// that found it substituted in, a subscript what it yields, and an
    /// `auto` variable what its initializer deduces it to. A data member
    /// has its declared type, the one `decltype` names, unless
    /// `through_object`: the expression's type carries the cv-qualifiers of
    /// the object it is accessed through. Null for forms that name nothing.
    clang::QualType type_of(const clang::Expr* expr, bool through_object = false);

    /// `declared` with its `auto` deduced from `init`.
    clang::QualType deduce(clang::QualType declared, const clang::Expr* init);

    /// `declared`, a non-`decltype(auto)` type holding `auto`, with it
    /// deduced from a value of type `source`, an lvalue when `lvalue`.
    clang::QualType bind_auto(clang::QualType declared, clang::QualType source, bool lvalue);

    /// What the range-based `for` declaring `var` binds it to: what
    /// dereferencing the `begin()` of its range yields, an array's element.
    /// Null while the statement is no part of its function's body yet, as
    /// when a completion parses the loop. The frames of the lookups stay
    /// for the caller to truncate.
    clang::QualType loop_element(const clang::VarDecl* var);

    /// `declared` with its `auto` replaced by `argument`, each pointer of
    /// the declarator taking one off the argument.
    clang::QualType replace_auto(clang::QualType declared, clang::QualType argument);

    /// What a subscript on a dependent operand yields: the element of a
    /// pointer or array, else what the class's `operator[]` returns. That
    /// operator can only be a member, so unlike other dependent operators
    /// the class's own overloads are its whole candidate set. The frames
    /// of the lookup that found it stay for the caller to truncate.
    clang::QualType subscript_type(const clang::ArraySubscriptExpr* expr);

    /// What dereferencing an `object` yields: a pointer's pointee, else what
    /// the class's own `operator*` returns, or one argument-dependent lookup
    /// finds for the object's resolved type. A non-member operator only the
    /// definition sees is no candidate, as for any dependent operator but
    /// `[]` and `->`, which can only be members: instantiation adds the
    /// operands' associated operators, so what the definition happened to
    /// see says nothing. The frames of the lookup stay for the caller to
    /// truncate.
    clang::QualType deref_type(clang::QualType object, bool rvalue);

    /// Whether the object `base` denotes is an rvalue: a call returning no
    /// lvalue reference, or a temporary. A name denotes an lvalue.
    bool rvalue(const clang::Expr* base);

    /// Whether `expr` is an xvalue, also where clang cannot tell: a dependent
    /// access to a data member of an rvalue, which clang marks an lvalue.
    bool xvalue(const clang::Expr* expr);

    /// The type a call returns, before resolution; the frames it pushes
    /// stay for the caller to truncate.
    clang::QualType call_type(const clang::CallExpr* call);

    /// What calling one of `candidates` with `arguments` returns, before
    /// resolution: the type they all return (`map::operator[]` takes a
    /// `const key_type&` or a `key_type&&`, and returns `mapped_type&`
    /// either way). Null when they disagree, or there are none. The
    /// `operands` of an operator are its arguments, of which a member takes
    /// the first as its object.
    ///
    /// A template returning the `decltype` of a call on the parameters only
    /// deduction would bind tells nothing, and the others decide: beside a
    /// map's plain `find`, the heterogeneous `find(const K&) ->
    /// decltype(...)` a defaulted template argument disables unless the
    /// comparator opts in. One returning a parameter itself may be the one
    /// chosen, and leaves the call untyped.
    clang::QualType
        common_return_type(llvm::ArrayRef<const clang::NamedDecl*> candidates,
                           llvm::ArrayRef<clang::TemplateArgumentLoc> template_arguments,
                           llvm::ArrayRef<CallArgument> arguments = {},
                           bool operands = false);

    /// What calling `candidate` with `arguments` returns, before resolution;
    /// the frames it pushes stay for the caller to truncate. A function
    /// template's own parameters are bound from the call's explicit template
    /// arguments, else deduced from `arguments`; those left take their
    /// defaults. One still unbound leaves the call untyped.
    clang::QualType return_type(const clang::NamedDecl* candidate,
                                llvm::ArrayRef<clang::TemplateArgumentLoc> template_arguments,
                                llvm::ArrayRef<CallArgument> arguments);

    /// The declaration a name or member expression refers to; the frames
    /// of the lookup that found it stay on the stack (see lookup).
    const clang::Decl* referenced_decl(const clang::Expr* expr);

    /// The arguments of a call as deduction sees them; none when one of
    /// them expands a pack, which may stand for any number of arguments, or
    /// has a type that does not resolve.
    std::optional<llvm::SmallVector<CallArgument, 4>>
        call_arguments(llvm::ArrayRef<const clang::Expr*> arguments);

    /// Whether every one of `arguments` may initialize its parameter of
    /// `FD`, none certain to fail (never_initializes): both sides
    /// non-dependent, matched by position up to an argument or a parameter
    /// pack.
    bool accepts(const clang::FunctionDecl* FD,
                 llvm::ArrayRef<const clang::Expr*> arguments,
                 bool member_call);

    /// Of `candidates` every argument of a call may initialize, the one
    /// taking each argument as is, of its type exactly — binding a
    /// reference or dropping top-level qualifiers aside —, alone when it is
    /// the only such: every other one needs a conversion of some argument,
    /// and overload resolution prefers it. Dependent arguments or
    /// parameters leave the set as it is.
    void prefer_exact(llvm::SmallVectorImpl<const clang::NamedDecl*>& candidates,
                      llvm::ArrayRef<const clang::Expr*> arguments);

    /// Overloads that differ only in their object parameter — its cv- and
    /// ref-qualifiers, `front()` and `front() const`, `value() &` and
    /// `value() &&` — are told apart by the object, an lvalue unless
    /// `rvalue`, as overload resolution would: a method that cannot bind the
    /// object drops out (one less cv-qualified than it, `&` on an rvalue,
    /// `&&` on an lvalue), and of the rest the one adding the fewest
    /// qualifiers binds best, an rvalue taking `const&` last. An overload
    /// with no such sibling is left alone, ranking it needs conversions,
    /// unless it cannot bind the object while another can.
    void select_by_object(llvm::SmallVectorImpl<const clang::NamedDecl*>& candidates,
                          clang::QualType object,
                          bool rvalue = false);

    /// The resolved overload set of a dependent `callee` called with
    /// `arguments` — for a name needing argument-dependent lookup, with what
    /// that lookup finds —, the functions `viable` rejects left out, and for
    /// a member call those its object binds best. A using-declaration among
    /// them stands for what it names in its base.
    Callees callee_candidates(const clang::Expr* callee,
                              llvm::ArrayRef<const clang::Expr*> arguments,
                              llvm::function_ref<bool(const clang::FunctionDecl*, bool)> viable);

    std::string pad() const {
        return std::string(indent * 2, ' ');
    }

private:
    clang::ASTContext& context;

    InstantiationStack stack;

    llvm::DenseMap<const void*, clang::QualType>& resolved;

    const ResolverOptions& options;

    /// The dependent names and specializations being resolved.
    llvm::SmallPtrSet<const void*, 8> resolving;

    /// The (class template, name) lookups in progress.
    llvm::DenseSet<std::pair<const void*, void*>> looking_up;

    llvm::SmallPtrSet<const clang::VarDecl*, 4> deducing;

    /// The defaults of value parameters that did not fold, which may hold
    /// any value (deduce_arguments).
    llvm::SmallSetVector<const clang::Expr*, 4> open_defaults;

    unsigned depth = 0;

    unsigned steps = 0;

    unsigned probing = 0;

    unsigned evaluating = 0;

    /// Non-zero while a pack element rewrite has a Pack binding narrowed to
    /// one element; node-pointer-keyed caches are bypassed for the duration.
    unsigned pack_narrowing = 0;

    /// Sticky: some answer in this query came back empty or partial because
    /// a lookup was already in progress (a recursive chain such as
    /// `callback_traits<F> : callback_traits<decltype(&F::operator())>`) or
    /// a rewrite hit the depth or step limit, not because what it looked for
    /// is absent. Such an answer is not conclusive: it never enters the
    /// unit-wide cache — a later query with a fresh budget could still
    /// resolve it — and never proves a member absent.
    bool inconclusive = false;

    /// Hard ceiling on rewrite steps per query; bounds the total work
    /// including pseudo-SFINAE probes over rejected branches.
    constexpr static unsigned step_budget = 4096;

    unsigned indent = 0;
};

}  // namespace clice::types
