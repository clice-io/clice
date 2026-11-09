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
import :semantic.template_resolver.visibility;
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

    /// The innermost frame holding the template of a parameter, bound or
    /// not.
    ///
    /// When the parameter's declaration is known, only a frame whose
    /// parameter list holds that declaration matches — unrelated templates
    /// that merely share the same depth (e.g. `test` and `__alloc_traits`,
    /// both at depth 0) never capture each other's parameters.
    ///
    /// Canonical parameters carry no declaration; those fall back to matching
    /// the frame's parameter list depth.
    std::optional<unsigned> holder(const clang::NamedDecl* decl,
                                   unsigned depth,
                                   unsigned index) const {
        for(unsigned f = data.size(); f > 0; f -= 1) {
            const auto* params = data[f - 1].params;
            if(params && (decl ? index < params->size() && params->getParam(index) == decl
                               : params->getDepth() == depth)) {
                return f - 1;
            }
        }
        return std::nullopt;
    }

    /// Where a parameter's binding is, in the innermost frame holding its
    /// template; none when no frame does, or that one leaves it unbound.
    std::optional<Slot> find_slot(const clang::NamedDecl* decl,
                                  unsigned depth,
                                  unsigned index) const {
        auto frame = holder(decl, depth, index);
        if(!frame || index >= data[*frame].arguments.size()) {
            return std::nullopt;
        }
        return Slot{*frame, index};
    }

    clang::TemplateArgument& slot(Slot handle) {
        return data[handle.first].arguments[handle.second];
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

/// The member template a member template of a class template's
/// specialization was instantiated from, while its own definition waits for
/// an instantiation that needs one: `Outer<int>::Inner` is a declaration
/// until some `Outer<int>::Inner<char>` is used, its members are
/// `Outer<T>::Inner`'s. Null for any other template.
clang::ClassTemplateDecl* member_pattern(const clang::ClassTemplateDecl* CTD);

/// `arguments` canonical, as clang registers a specialization of `params`:
/// those a parameter pack takes grouped into one. Without `params`, each as
/// it is.
llvm::SmallVector<clang::TemplateArgument, 4>
    canonical_arguments(clang::ASTContext& context,
                        const clang::TemplateParameterList* params,
                        llvm::ArrayRef<clang::TemplateArgument> arguments);

/// Whether `record`'s definition inherits from a still-dependent base, which
/// may bring a member only once it instantiates; an undefined record tells
/// nothing either.
bool has_dependent_base(const clang::CXXRecordDecl* record);

/// An argument of a call as deduction sees it: its type, resolved, and
/// whether it is an lvalue.
struct CallArgument {
    clang::QualType type;

    bool lvalue;

    /// The argument as written, when the call spells it.
    const clang::Expr* expr = nullptr;
};

/// An operand evaluated: its type (see PseudoInstantiator::expr_type) and
/// whether it is an rvalue (see PseudoInstantiator::rvalue).
struct Operand {
    clang::QualType type;

    bool rvalue;
};

/// What a lookup found, with what tells two finds of one declaration apart:
/// the arguments the lookup bound the template it is a member of to (empty
/// for a class that is no template's pattern), and the type an alias among
/// them names under those.
struct Found {
    clang::DeclContext::lookup_result members;

    llvm::SmallVector<clang::TemplateArgument, 4> owner;

    clang::QualType aliased;
};

/// The functions a dependent call may call.
struct Callees {
    llvm::SmallVector<const clang::NamedDecl*, 4> candidates;

    /// Whether the candidates tell what the call calls: false for a call
    /// needing argument-dependent lookup whose arguments' types do not all
    /// resolve — its instantiation may find more —, and for one reaching a
    /// declaration through two specializations of its class, whose bindings
    /// are not kept apart.
    bool complete = true;

    /// The call's arguments, resolved in the caller's terms; none when one
    /// does not resolve or expands a pack.
    std::optional<llvm::SmallVector<CallArgument, 4>> arguments;
};

/// A declaration the query judged the visibility of, and the judgement
/// (Visibility::sees).
struct VisibilityCheck {
    const clang::NamedDecl* decl;

    bool seen;
};

/// A template parameter: its declaration, null for a canonical one, which
/// goes by its depth and index alone.
struct ParameterRef {
    const clang::NamedDecl* decl;

    unsigned depth;

    unsigned index;

    bool operator==(const ParameterRef&) const = default;
};

/// What a step read of its query beside its input while it runs, and left
/// in it for the steps after it: the declarations whose visibility it judged,
/// the parameters it found nothing binding, the value defaults it found not
/// to fold (deduce_arguments), and the lowest frame whose binding it read.
struct StepRecording {
    /// The frame count when the step started: a binding read below it is
    /// the caller's.
    unsigned base;

    llvm::SmallVector<VisibilityCheck, 0> checks;

    llvm::SmallVector<ParameterRef, 0> free;

    llvm::SmallVector<const clang::Expr*, 0> open_defaults;

    /// The defaults it asked whether they fold, beside those it found not
    /// to fold itself, and what it found (default_open).
    llvm::SmallVector<std::pair<const clang::Expr*, bool>, 0> defaults_read;

    unsigned lowest_read = std::numeric_limits<unsigned>::max();

    /// The declarations judged and the parameters found free already: a
    /// query finds one alike each time.
    llvm::SmallPtrSet<const clang::NamedDecl*, 8> judged;

    llvm::SmallPtrSet<const clang::NamedDecl*, 4> found;

    /// Whether the step read only what it bound itself: what it found then
    /// holds for any caller that leaves its free parameters free.
    bool read_no_binding() const {
        return lowest_read >= base;
    }

    void judge(llvm::ArrayRef<VisibilityCheck> checked) {
        for(auto& check: checked) {
            if(judged.insert(check.decl).second) {
                checks.push_back(check);
            }
        }
    }

    void read_defaults(llvm::ArrayRef<std::pair<const clang::Expr*, bool>> read) {
        for(auto& entry: read) {
            if(!ranges::contains(open_defaults, entry.first) &&
               !ranges::contains(defaults_read, entry)) {
                defaults_read.push_back(entry);
            }
        }
    }

    void found_free(llvm::ArrayRef<ParameterRef> parameters) {
        for(auto& parameter: parameters) {
            if(parameter.decl ? found.insert(parameter.decl).second
                              : !ranges::contains(free, parameter)) {
                free.push_back(parameter);
            }
        }
    }
};

/// A step kept for the queries of a unit: what it gave, the frames of the
/// lookups it ended in, and what it read and left (StepRecording). A later
/// query takes it where it judges every declaration alike and finds every
/// parameter it found free free too (PseudoInstantiator::kept_step).
template <typename Result>
struct KeptStep {
    using Value = Result;

    Result result;

    llvm::SmallVector<InstantiationStack::Frame, 0> frames;

    llvm::SmallVector<const clang::Expr*, 0> open_defaults;

    llvm::SmallVector<std::pair<const clang::Expr*, bool>, 0> defaults_read;

    llvm::SmallVector<VisibilityCheck, 0> checks;

    llvm::SmallVector<ParameterRef, 0> free;

    /// The view (Visibility::view) a query last judged its checks alike in.
    std::uint32_t view;
};

/// What `name` finds as a member of an object, and its type in the terms the
/// object is written in (PseudoInstantiator::member_of).
struct Member {
    const clang::Decl* decl = nullptr;

    clang::QualType type;
};

/// What the queries of one unit keep for each other: the steps that read
/// none of their caller's bindings (PseudoInstantiator::kept_step), by what
/// they were given.
struct UnitCache {
    /// Rewrites of the types that look a name up or fold an expression (rewrite), by
    /// the type and the policy.
    llvm::DenseMap<std::pair<const void*, unsigned>, KeptStep<clang::QualType>> rewrites;

    /// Dependent names resolved to a type naming no template parameter, or
    /// to nothing (a null type), by their canonical node, where they name no
    /// parameter anything binds: the sugar of one name differs by the path
    /// that wrote it, and such an answer carries none of it.
    llvm::DenseMap<const void*, KeptStep<clang::QualType>> canonical_names;

    /// Types of expressions by the expression and whether it is read
    /// through its object (type_of), values of expressions, candidates of
    /// calls.
    llvm::DenseMap<std::pair<const clang::Expr*, unsigned>, KeptStep<clang::QualType>> types;

    llvm::DenseMap<const clang::Expr*, KeptStep<std::optional<llvm::APSInt>>> values;

    llvm::DenseMap<const clang::CallExpr*, KeptStep<Callees>> calls;

    /// Members by the resolved object and the name (member_of).
    llvm::DenseMap<std::pair<const void*, const void*>, KeptStep<Member>> members;

    /// Values of dependent members by their class, resolved and canonical,
    /// and their name (PseudoInstantiator::member_value): every member type
    /// of `allocator_traits<A>` reads `_Is_default_allocator<A>::value`, and
    /// every overload set of a container the same iterator traits.
    llvm::DenseMap<std::pair<const void*, void*>, KeptStep<std::optional<llvm::APSInt>>>
        member_values;

    /// Whether a type, the bindings substituted, cannot form, by the type
    /// (PseudoInstantiator::unformable): the overloads of a container take
    /// the same iterator types at every call.
    llvm::DenseMap<const void*, KeptStep<bool>> unformable;

    /// Members found alike in every alternative of a condition not known,
    /// by the condition's `type` and the name (lookup_through_alternatives):
    /// each of MSVC's special member layers reaches the layer below from
    /// all three of its choices, twelve layers deep under `std::variant`.
    llvm::DenseMap<std::pair<const void*, const void*>, KeptStep<clang::DeclContext::lookup_result>>
        alternatives;

    /// Values of variable templates by the template and its arguments, when
    /// they are all types or templates (PseudoInstantiator::value_of):
    /// `is_empty_v<_Alloc>` decides the layout of every MSVC container.
    std::map<std::vector<const void*>, KeptStep<std::optional<llvm::APSInt>>> variable_values;

    /// The template parameters each type names (names_only_free_parameters).
    llvm::DenseMap<const clang::Type*, llvm::SmallVector<ParameterRef, 2>> named_parameters;

    /// The explicit specializations of each class and variable template
    /// (PseudoInstantiator::explicit_specializations).
    llvm::DenseMap<const clang::Decl*, llvm::SmallVector<clang::NamedDecl*, 0>>
        explicit_specializations;
};

/// Where a query's folding of a dependent member's value was cut short by
/// the nesting limits (PseudoInstantiator::cut_values): how deep the
/// evaluations and the rewrites nested there.
struct CutFolding {
    unsigned evaluating;

    unsigned depth;

    /// Whether folding it where the nesting is as given is cut too.
    bool holds_at(unsigned evaluating, unsigned depth) const {
        return evaluating >= this->evaluating && depth >= this->depth;
    }
};

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

    /// Without `visibility`, the query sees the whole unit.
    /// What the query cost goes to `audits` when it ends; nowhere when null.
    PseudoInstantiator(clang::ASTContext& context,
                       UnitCache& cache,
                       const ResolverOptions& options,
                       Visibility* visibility,
                       std::vector<QueryAudit>* audits = nullptr) :
        context(context), cache(cache), options(options), visibility(visibility), audits(audits) {}

    PseudoInstantiator(const PseudoInstantiator&) = delete;

    ~PseudoInstantiator() {
        if(audits) {
            audits->push_back({.steps = steps, .troubles = troubles});
        }
    }

    /// The declarations the overload set `expr` names that the query sees
    /// (TemplateResolver::overloads).
    llvm::SmallVector<const clang::NamedDecl*, 4> overloads(const clang::OverloadExpr* expr);

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

    /// What resolve_dependent_name finds for `DNT`, by its rule or its
    /// lookup; `DNT` itself when nothing.
    clang::QualType resolve_name(const clang::DependentNameType* DNT);

    /// Whether `NNS` reaches a template type parameter nothing binds through
    /// members of members (`typename _Traits::allocator_type::template
    /// rebind<U>::other` with `_Traits` free): no lookup into it finds
    /// anything, whatever the rest names.
    bool unbound_root(clang::NestedNameSpecifier NNS);

    /// Whether every template parameter `type` names is free, neither a
    /// frame nor its function's only instantiation binding it; then noted
    /// free for the step being kept. Only then does a step on `type` answer
    /// for its canonical form, which any template's parameters at the same
    /// places share.
    bool names_only_free_parameters(const clang::Type* type);

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

    /// What binds a template parameter (binding).
    struct Binding {
        /// The argument; null when unbound.
        const clang::TemplateArgument* argument = nullptr;

        /// Whether a frame holds the parameter's template, bound or not.
        bool held = false;
    };

    /// What binds the template parameter `param`, `index` in its list at
    /// `depth`: the frame holding its template when a frame does — the
    /// query names a specialization of it or calls it —, else, for a
    /// parameter of the template the query is inside of, the template's
    /// only instantiation. Every read of a binding goes through here: the
    /// step being kept notes what it read (StepRecording), a parameter bound
    /// to itself as free.
    Binding binding(const clang::NamedDecl* param, unsigned depth, unsigned index);

    /// The argument binding() finds; null when unbound.
    const clang::TemplateArgument* bound_argument(const clang::NamedDecl* param,
                                                  unsigned depth,
                                                  unsigned index) {
        return binding(param, depth, index).argument;
    }

    /// Whether nothing binds `parameter` but itself: no frame holds its
    /// template, or one holding it binds it to itself, as inside the
    /// template; nor does its function's only instantiation.
    bool free(const ParameterRef& parameter) const;

    /// Note that the step being kept read the frame `frame`.
    void read_frame(unsigned frame) {
        if(recording) {
            recording->lowest_read = std::min(recording->lowest_read, frame);
        }
    }

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

    /// Push the parameters of the pattern `spec` instantiates from — its
    /// template's or a partial specialization's — bound to its arguments.
    /// Returns the frame count to truncate back to.
    unsigned push_specialization(const clang::ClassTemplateSpecializationDecl* spec);

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

    Deduction deduce_template_arguments(clang::VarTemplateDecl* VTD, TemplateArguments arguments);

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

    /// `members`, found by a lookup whose frames are those past `since`.
    Found found_by(lookup_result members, unsigned since);

    /// Whether two finds name one thing: the same declarations of the same
    /// specialization, or an alias reading as the same type in both, as
    /// clang takes it.
    bool same_found(const Found& lhs, const Found& rhs);

    lookup_result lookup(clang::ClassTemplateDecl* CTD,
                         clang::DeclarationName name,
                         TemplateArguments written);

    /// `name` in `CTD<arguments>` where it is not certain which pattern
    /// applies — a value is not known, a constraint is not decided: what
    /// every pattern that may apply finds alike, a member of a base they
    /// share; else nothing. A pattern certainly lacking the name drops out:
    /// the code names it, so instantiation reads another. The frames of the
    /// lookup in a pattern finding it stay, as lookup's do.
    lookup_result lookup_through_any(clang::ClassTemplateDecl* CTD,
                                     TemplateArguments arguments,
                                     clang::DeclarationName name);

    /// `DNT`, the `type` of a standard library class template's
    /// specialization, answered by the standard's rule for it (std.cpp)
    /// rather than by reading the library's definition: the type; `DNT`
    /// itself when the rule tells the answer waits for what is not known
    /// (`remove_reference<T>` of a parameter); none where no rule applies.
    std::optional<clang::QualType> std_type(const clang::DependentNameType* DNT);

    /// What `DNT`, a `conditional<B, T, F>::type` whose condition is not
    /// known, may be: `T` and `F`; none for any other type.
    llvm::SmallVector<clang::QualType, 2> std_alternatives(const clang::DependentNameType* DNT);

    /// The condition a standard library template takes as its argument.
    std::optional<bool> std_condition(const clang::TemplateArgument& argument);

    /// `name` in each alternative of `DNT` (std_alternatives), when every
    /// one finds it alike, kept for the unit; nothing for a `DNT` with no
    /// alternatives. The frames of the last lookup stay.
    lookup_result lookup_through_alternatives(const clang::DependentNameType* DNT,
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
    /// would. An explicit specialization the arguments may name
    /// (undecided_specializations) leaves the choice Unverified.
    PartialChoice<clang::ClassTemplatePartialSpecializationDecl>
        match_partial(clang::ClassTemplateDecl* CTD, TemplateArguments arguments);

    PartialChoice<clang::VarTemplatePartialSpecializationDecl>
        match_partial(clang::VarTemplateDecl* VTD, TemplateArguments arguments);

    /// The explicit specializations of `CTD` that `arguments` may name
    /// without certainly naming one (may_name): `__conditional<B>` with `B`
    /// not known may be `__conditional<false>`.
    llvm::SmallVector<clang::ClassTemplateSpecializationDecl*, 2>
        undecided_specializations(clang::ClassTemplateDecl* CTD, TemplateArguments arguments);

    llvm::SmallVector<clang::VarTemplateSpecializationDecl*, 2>
        undecided_specializations(clang::VarTemplateDecl* VTD, TemplateArguments arguments);

    /// The explicit specializations of `TD`, kept for the unit: what it
    /// instantiated beside them, which a match would walk through each
    /// time, runs to hundreds.
    template <typename Template>
    llvm::SmallVector<clang::NamedDecl*, 2> explicit_specializations(Template* TD) {
        auto [found, added] = cache.explicit_specializations.try_emplace(TD);
        if(added) {
            for(auto* specialization: TD->specializations()) {
                if(specialization->isExplicitSpecialization()) {
                    found->second.push_back(specialization);
                }
            }
        }
        return {found->second.begin(), found->second.end()};
    }

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

    /// The value of `VTD<written>`: its pattern's initializer under the
    /// bindings of the arguments. Where it is not certain which pattern
    /// applies, the value every pattern that may apply has alike.
    std::optional<llvm::APSInt> value_of(clang::VarTemplateDecl* VTD,
                                         llvm::ArrayRef<clang::TemplateArgumentLoc> written);

    /// The value of `VTD<arguments>`, the arguments resolved.
    std::optional<llvm::APSInt> value_of(clang::VarTemplateDecl* VTD,
                                         llvm::ArrayRef<clang::TemplateArgument> arguments);

    /// The value of the member `name` of `scope`, a class resolved; kept for
    /// the unit by the class's canonical form when it names only free
    /// parameters.
    std::optional<llvm::APSInt> member_value(clang::QualType scope, clang::DeclarationName name);

    /// The value of `decl`, a variable or an enumerator, under the current
    /// bindings.
    std::optional<llvm::APSInt> declared_value(const clang::Decl* decl);

    /// The value `var`'s initializer folds to, converted to its type.
    std::optional<llvm::APSInt> initial_value(const clang::VarDecl* var);

    /// The type a dependent expression evaluates to, resolved: a name, a
    /// member access or a call takes the found declaration's type — the
    /// callee's return type for a call — with the bindings of the lookup
    /// that found it substituted in, a subscript what it yields, and an
    /// `auto` variable what its initializer deduces it to. A data member
    /// has its declared type, the one `decltype` names, unless
    /// `through_object`: the expression's type carries the cv-qualifiers of
    /// the object it is accessed through. Null for forms that name nothing.
    clang::QualType type_of(const clang::Expr* expr, bool through_object = false);

    /// What `name` finds as a member of `object`, with its type substituted
    /// under the bindings of the lookup. A resolved object names one
    /// specialization whatever bound the parameters it was written in, so
    /// the answer is kept for the unit by it (kept_step): MSVC reaches its
    /// containers' state through the same `_Mypair._Myval2` everywhere.
    Member member_of(clang::QualType object, clang::DeclarationName name);

    /// The type an expression naming `value` has, under the current
    /// bindings: a variable's deduced, a pack's pattern.
    clang::QualType value_type(const clang::ValueDecl* value);

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

    /// `expr` evaluated once for both its type and its value category: a
    /// dependent call's category is what its type tells, and asking rvalue
    /// would evaluate it again, and each call it nests.
    Operand operand(const clang::Expr* expr);

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
    /// taking each of the resolved `arguments` at the rank of an exact match
    /// — as is, decayed, or to a more qualified pointer —, alone when it is
    /// the only such, or the only such that is no template specialization:
    /// overload resolution prefers it to every candidate that needs a
    /// conversion, or the ellipsis. A parameter that stays dependent leaves
    /// the rest of the set as it is; one deduction makes concrete drops its
    /// candidate when the argument cannot initialize it.
    void prefer_exact(llvm::SmallVectorImpl<const clang::NamedDecl*>& candidates,
                      llvm::ArrayRef<CallArgument> arguments,
                      bool member_call);

    /// Whether `type`, the bindings substituted, names a member certainly
    /// absent, as written or through its aliases (`void_t<...>` holds its
    /// probe as written, `enable_if_t<...>` in what it aliases).
    bool member_absent(clang::QualType type);

    /// member_absent for a type naming no parameter of the template being
    /// called, kept for the unit by the type (kept_step).
    bool unformable(clang::QualType type);

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

    /// Cut the work short for `trouble`: what it answers is not conclusive.
    void give_up(Trouble trouble) {
        inconclusive = true;
        note(trouble);
    }

    /// Count `trouble` for the query's record.
    void note(Trouble trouble) {
        troubles[static_cast<std::size_t>(trouble)] += 1;
    }

    /// Clear `inconclusive` for the work up to the end of the scope, and
    /// merge it back with what came before then.
    auto own_conclusion() {
        return llvm::make_scope_exit(
            [this, outer = std::exchange(inconclusive, false)] { inconclusive |= outer; });
    }

    /// Record what the work up to the end of the scope reads and leaves in
    /// `step` too, for a step being kept; merged into the step around it.
    auto record_effects(StepRecording& step) {
        return llvm::make_scope_exit([this, &step, outer = std::exchange(recording, &step)] {
            recording = outer;
            if(outer) {
                outer->read_defaults(step.defaults_read);
                outer->open_defaults.append(step.open_defaults);
                outer->judge(step.checks);
                outer->found_free(step.free);
                outer->lowest_read = std::min(outer->lowest_read, step.lowest_read);
            }
        });
    }

    /// What `compute` gives for `key`, kept for the unit. A step that read
    /// no binding of its caller's frames gives the same for any query that
    /// judges every declaration it judged alike and finds every parameter it
    /// found free free too: an earlier query's is taken then, the frames and
    /// the effects it left restored. The frames it leaves are those above
    /// where it started.
    /// Of what it gives, `keep` tells what may be kept; all of it without.
    template <typename Results, typename Compute>
    auto kept_step(Results& results,
                   const typename Results::key_type& key,
                   Compute&& compute,
                   llvm::function_ref<bool(const typename Results::mapped_type::Value&)> keep = {})
        -> typename Results::mapped_type::Value {
        using Result = typename Results::mapped_type::Value;
        auto base = stack.size();
        if(auto found = results.find(key); found != results.end() && holds(found->second)) {
            auto& known = found->second;
            stack.frames().append(known.frames.begin(), known.frames.end());
            for(auto* expr: known.open_defaults) {
                open_default(expr);
            }
            judged_visibility(known.checks);
            if(recording) {
                recording->read_defaults(known.defaults_read);
                recording->found_free(known.free);
            }
            return known.result;
        }
        auto judged = own_conclusion();
        StepRecording step{.base = base};
        Result result = [&] {
            auto recorder = record_effects(step);
            return compute();
        }();
        if(!inconclusive && step.read_no_binding() && (!keep || keep(result))) {
            results.insert_or_assign(
                key,
                KeptStep<Result>{
                    .result = result,
                    .frames = {stack.frames().begin() + base, stack.frames().end()},
                    .open_defaults = std::move(step.open_defaults),
                    .defaults_read = std::move(step.defaults_read),
                    .checks = std::move(step.checks),
                    .free = std::move(step.free),
                    .view = view(),
            });
        }
        return result;
    }

    /// Whether a kept step holds for the query: it finds every parameter the
    /// step found free free and every default the step asked about folding
    /// as the step did, and judges every declaration the step judged alike,
    /// as one from the view kept beside them did; that view updated.
    template <typename Result>
    bool holds(KeptStep<Result>& known) {
        if(!ranges::all_of(known.free,
                           [&](const ParameterRef& parameter) { return free(parameter); }) ||
           !ranges::all_of(known.defaults_read, [&](const auto& entry) {
               return open_defaults.contains(entry.first) == entry.second;
           })) {
            return false;
        }
        auto current = view();
        if(known.view == current) {
            return true;
        }
        if(!ranges::all_of(known.checks, [&](const VisibilityCheck& check) {
               return (!visibility || visibility->sees(check.decl)) == check.seen;
           })) {
            return false;
        }
        known.view = current;
        return true;
    }

    /// Where the query is answered from (Visibility::view); 0 when no
    /// Visibility limits it to a place.
    std::uint32_t view() const {
        return visibility ? visibility->view() : 0;
    }

    /// Whether the default `expr` did not fold, noted for the step being
    /// kept (kept_step).
    bool default_open(const clang::Expr* expr) {
        bool open = open_defaults.contains(expr);
        if(recording) {
            recording->read_defaults({
                {expr, open}
            });
        }
        return open;
    }

    /// default_open, for deduction to ask (DeductionOptions::open).
    auto open_default_reader() {
        return [this](const clang::Expr* expr) {
            return default_open(expr);
        };
    }

    /// Note that the default `expr` did not fold, for the step being kept
    /// too (kept_step).
    void open_default(const clang::Expr* expr) {
        open_defaults.insert(expr);
        if(recording) {
            recording->open_defaults.push_back(expr);
        }
    }

    /// Note `checks`, judgements of visibility, for the step being kept
    /// (kept_step).
    void judged_visibility(llvm::ArrayRef<VisibilityCheck> checks) {
        if(recording) {
            recording->judge(checks);
        }
    }

    /// Whether the query sees `decl`, the judgement noted.
    bool sees(const clang::NamedDecl* decl) {
        if(!visibility) {
            return true;
        }
        bool seen = visibility->sees(decl);
        judged_visibility({
            {decl, seen}
        });
        return seen;
    }

    /// `found`, what an unqualified or namespace-qualified name finds or the
    /// specializations of a template, without what the query does not see.
    template <typename Decl>
    void drop_unseen(llvm::SmallVectorImpl<Decl*>& found) {
        llvm::erase_if(found, [&](const clang::NamedDecl* decl) { return !sees(decl); });
    }

    /// The partial specializations of a template the query sees.
    llvm::SmallVector<clang::ClassTemplatePartialSpecializationDecl*, 4>
        seen_partials(clang::ClassTemplateDecl* CTD);

    llvm::SmallVector<clang::VarTemplatePartialSpecializationDecl*, 4>
        seen_partials(clang::VarTemplateDecl* VTD);

    /// The functions named `name` that argument-dependent lookup finds for a
    /// call with `arguments`, that may take them and that the query sees:
    /// those of the namespaces the arguments' types are associated with,
    /// and the friends their associated classes declare. None when one of
    /// the types is still dependent: its associated namespaces wait for
    /// instantiation.
    std::optional<llvm::SmallVector<const clang::NamedDecl*, 4>>
        argument_dependent_lookup(clang::DeclarationName name,
                                  llvm::ArrayRef<CallArgument> arguments);

private:
    clang::ASTContext& context;

    InstantiationStack stack;

    UnitCache& cache;

    const ResolverOptions& options;

    /// The dependent names and specializations being resolved.
    llvm::SmallPtrSet<const void*, 8> resolving;

    /// The (class template, name) lookups in progress.
    llvm::DenseSet<std::pair<const void*, void*>> looking_up;

    llvm::SmallPtrSet<const clang::VarDecl*, 4> deducing;

    /// The dependent members' values this query found cut short, by their
    /// class, resolved, and their name (member_value): met again no
    /// shallower, they are cut alike, and folding them again costs each
    /// path through `<ratio>`'s values with their operands unknown the
    /// whole tree below it.
    llvm::DenseMap<std::pair<const void*, void*>, CutFolding> cut_values;

    /// The defaults of value parameters that did not fold, which may hold
    /// any value (deduce_arguments).
    llvm::SmallSetVector<const clang::Expr*, 4> open_defaults;

    /// Where the step kept_step is keeping records what it reads and leaves.
    StepRecording* recording = nullptr;

    /// Where the query is answered from; the whole unit when null.
    Visibility* visibility;

    unsigned depth = 0;

    std::uint32_t steps = 0;

    /// How often each Trouble struck the query (give_up, note).
    std::array<std::uint32_t, 6> troubles = {};

    /// Where the query's cost goes when it ends.
    std::vector<QueryAudit>* audits;

    unsigned probing = 0;

    unsigned evaluating = 0;

    /// Some answer came back empty or partial because a lookup or the
    /// deduction of a variable was already in progress (a recursive chain
    /// such as `callback_traits<F> : callback_traits<decltype(&F::operator())>`)
    /// or a rewrite or an evaluation hit its nesting or step limit, not
    /// because what it looked for is absent. Such an answer is not conclusive: it never enters the
    /// unit's cache — a later query with a fresh budget could still resolve
    /// it — and never proves a member absent. A resolution judges its own work
    /// (own_conclusion), so one that came before it in the query does not
    /// keep it out of the cache.
    bool inconclusive = false;

    /// Hard ceiling on rewrite steps per query; bounds the total work
    /// including pseudo-SFINAE probes over rejected branches.
    constexpr static unsigned step_budget = 4096;

    /// Hard ceiling on nested rewrites. What an expression in a type
    /// evaluates to nests inside it: the iterator MSVC's `std::map::begin()`
    /// returns, through its `conditional_t`s, runs past 16 under a pointer.
    constexpr static unsigned depth_limit = 32;

    unsigned indent = 0;
};

}  // namespace clice::types
