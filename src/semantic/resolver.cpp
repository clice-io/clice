#include "semantic/resolver.h"

#include <ranges>

#include "semantic/unifier.h"
#include "support/logging.h"

#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/DeclTemplate.h"
#include "clang/AST/RecursiveASTVisitor.h"

/// Template Resolver — pseudo-instantiation of dependent C++ types.
///
/// Architecture:
///   PseudoInstantiator — heuristic lookup in primary templates/partial specs,
///   driven by a hand-written QualType → QualType rewriter with two policies:
///     ├─ Policy::Substitute — expand typedefs/aliases and substitute template
///     │                       parameters from the stack; dependent names pass
///     │                       through untouched (no lookup)
///     └─ Policy::Resolve    — Substitute plus heuristic resolution of
///                             DependentNameType/DependentTemplateSpecializationType
///                             via member lookup and argument deduction
///
/// Key invariant: typedef/alias expansion always runs under Policy::Substitute,
/// so it can never re-enter heuristic lookup. Violating this causes
/// typedef ↔ lookup infinite cycles.
///
/// Everything is pure AST computation (TypeUnifier + ASTContext node
/// construction); Sema and TreeTransform are deliberately not used, so
/// resolution cannot emit diagnostics or mutate the unit's semantic state.

namespace clice {

namespace {

template <typename T>
constexpr inline bool dependent_false = false;

/// Walk from `decl` up to the TranslationUnit, collecting template parameter lists
/// at each enclosing template context. Used to build outer context frames for
/// deduce_template_arguments when the stack is empty.
template <typename Callback>
void visit_template_decl_contexts(clang::Decl* decl, const Callback& callback) {
    while(true) {
        if(llvm::isa<clang::TranslationUnitDecl>(decl)) {
            break;
        }

        clang::TemplateParameterList* params = nullptr;

        if(auto TD = decl->getDescribedTemplate()) {
            params = TD->getTemplateParameters();
        }

        if(auto CTPSD = llvm::dyn_cast<clang::ClassTemplatePartialSpecializationDecl>(decl)) {
            params = CTPSD->getTemplateParameters();
        }

        if(auto VTPSD = llvm::dyn_cast<clang::VarTemplatePartialSpecializationDecl>(decl)) {
            params = VTPSD->getTemplateParameters();
        }

        if(params) {
            callback(decl, params);
        }

        decl = llvm::dyn_cast<clang::Decl>(decl->getDeclContext());
        if(!decl)
            break;
    }
}

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

    auto& frames() {
        return data;
    }

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
    /// Stable handle to a binding: (frame index, argument index). Pointers
    /// into the frame vector go stale whenever a nested rewrite pushes a
    /// frame and the vector reallocates; indices survive because pushes only
    /// append and the walk never outlives the frames below it.
    using Slot = std::pair<unsigned, unsigned>;

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

    clang::TemplateArgument& slot(Slot handle) {
        return data[handle.first].arguments[handle.second];
    }

    const clang::TemplateArgument* find_argument(const clang::NamedDecl* decl,
                                                 unsigned depth,
                                                 unsigned index) const {
        auto handle = find_slot(decl, depth, index);
        return handle ? &data[handle->first].arguments[handle->second] : nullptr;
    }

    const clang::TemplateArgument* find_argument(const clang::TemplateTypeParmType* T) const {
        return find_argument(T->getDecl(), T->getDepth(), T->getIndex());
    }
};

/// Collect every template type parameter pack referenced anywhere inside a
/// type, for element-wise expansion of structured pack patterns.
struct PackParmCollector : clang::RecursiveASTVisitor<PackParmCollector> {
    llvm::SmallVector<const clang::TemplateTypeParmType*, 2> packs;
    llvm::SmallVector<const clang::TemplateTemplateParmDecl*, 2> template_packs;
    llvm::SmallVector<const clang::NonTypeTemplateParmDecl*, 2> value_packs;

    bool VisitTemplateTypeParmType(clang::TemplateTypeParmType* T) {
        if(T->isParameterPack() && !llvm::is_contained(packs, T)) {
            packs.push_back(T);
        }
        return true;
    }

    bool VisitDeclRefExpr(clang::DeclRefExpr* expr) {
        if(auto NTTP = llvm::dyn_cast<clang::NonTypeTemplateParmDecl>(expr->getDecl());
           NTTP && NTTP->isParameterPack() && !llvm::is_contained(value_packs, NTTP)) {
            value_packs.push_back(NTTP);
        }
        return true;
    }

    bool TraverseTemplateName(clang::TemplateName name) {
        if(auto TTP =
               llvm::dyn_cast_or_null<clang::TemplateTemplateParmDecl>(name.getAsTemplateDecl());
           TTP && TTP->isParameterPack() && !llvm::is_contained(template_packs, TTP)) {
            template_packs.push_back(TTP);
        }
        return RecursiveASTVisitor::TraverseTemplateName(name);
    }
};

/// Helper to extract underlying type from a Decl.
clang::QualType get_decl_type(clang::Decl* decl) {
    if(!decl)
        return clang::QualType();
    if(auto* TND = llvm::dyn_cast<clang::TypedefNameDecl>(decl))
        return TND->getUnderlyingType();
    if(auto* RD = llvm::dyn_cast<clang::RecordDecl>(decl))
        return clang::QualType(RD->getTypeForDecl(), 0);
    return clang::QualType();
}

/// The core pseudo-instantiation engine. Resolves dependent names by looking up
/// members in primary templates and partial specializations — a capability
/// clang's own instantiation machinery does not have (it only ever sees
/// concrete arguments, so e.g. alias templates never appear on its paths).
///
/// Resolution flow for `typename A<T>::type`:
///   1. rewrite() dispatches the DependentNameType to resolve_dependent_name
///   2. lookup(A<T>, "type") → deduce_template_arguments → find member decl
///   3. substitute(underlying_type) expands typedefs + substitutes params
///   4. Pop lookup frames, then rewrite the result for further resolution
///
/// Uses active_resolutions / active_ctd_lookups for cycle detection.
class PseudoInstantiator {
public:
    using TemplateArguments = llvm::ArrayRef<clang::TemplateArgument>;

    PseudoInstantiator(clang::ASTContext& context,
                       llvm::DenseMap<const void*, clang::QualType>& resolved,
                       unsigned parent_indent = 0) :
        context(context), resolved(resolved), indent(parent_indent) {}

    /// Rewrite policy. The two-phase split is the resolver's core invariant:
    /// typedef/alias expansion must never re-enter heuristic lookup, or
    /// mutually recursive typedefs cycle forever.
    enum class Policy {
        /// Expand sugar and substitute stack parameters only.
        Substitute,
        /// Substitute plus dependent name resolution through lookup.
        Resolve,
    };

    clang::QualType resolve(clang::QualType type) {
        return rewrite(type, Policy::Resolve);
    }

    clang::QualType substitute(clang::QualType type) {
        return rewrite(type, Policy::Substitute);
    }

    /// Entry point for all type rewriting. Guards against:
    /// - Null types (return as-is)
    /// - Non-dependent types (no transformation needed)
    /// - Excessive recursion depth (bail out to prevent runaway recursion)
    /// - Exhausted step budget (bounds the total work of one query, including
    ///   pseudo-SFINAE probes that explore rejected branches)
    /// - Null results (return original type instead)
    clang::QualType rewrite(clang::QualType type, Policy policy) {
        if(type.isNull() || !type->isDependentType()) {
            return type;
        }
        steps += 1;
        if(depth > 16 || steps > step_budget) {
            truncated = true;
            return type;
        }
        depth += 1;
        auto result = rewrite_type(type, policy);
        depth -= 1;
        return result.isNull() ? type : result;
    }

    using lookup_result = clang::DeclContext::lookup_result;

    /// When DeclContext::lookup returns multiple declarations (e.g. a member in
    /// both a base class and derived class), take the last one. This heuristic
    /// favors the most-derived declaration, though the ordering depends on clang's
    /// internal DeclContext storage.
    clang::Decl* preferred(lookup_result members) {
        clang::Decl* decl = nullptr;
        std::ranges::for_each(members, [&](auto member) { decl = member; });
        return decl;
    }

    /// Verify that `arguments` match `TD`'s parameter list, filling in default
    /// template arguments where needed. Type defaults are substituted using the
    /// current stack, so parameters already provided can appear in default
    /// expressions (e.g. `allocator<_Tp>` for vector's `_Alloc`). Non-type and
    /// template template defaults are filled when representable without
    /// building expressions.
    bool check_template_arguments(clang::TemplateDecl* TD,
                                  TemplateArguments& arguments,
                                  llvm::SmallVectorImpl<clang::TemplateArgument>& out) {
        auto list = TD->getTemplateParameters();
        out.reserve(list->size());
        for(auto arg: arguments) {
            out.emplace_back(arg);
        }

        for(auto i = out.size(); i < list->size(); i += 1) {
            auto param = list->getParam(i);

            if(auto TTPD = llvm::dyn_cast<clang::TemplateTypeParmDecl>(param);
               TTPD && TTPD->hasDefaultArgument()) {
                auto type = TTPD->getDefaultArgument().getArgument().getAsType();

                /// A default inherited from an earlier redeclaration still
                /// references that declaration's parameter decls; push every
                /// redecl's list (sharing the same arguments) so decl-pointer
                /// matching finds them wherever the default was written.
                auto frames = stack.data.size();
                if(auto RTD = llvm::dyn_cast<clang::RedeclarableTemplateDecl>(TD)) {
                    for(auto redecl: RTD->redecls()) {
                        auto params =
                            llvm::cast<clang::TemplateDecl>(redecl)->getTemplateParameters();
                        if(params != list) {
                            stack.push(TD, params, out);
                        }
                    }
                }
                stack.push(TD, list, out);

                auto result = substitute(type);

                while(stack.data.size() > frames) {
                    stack.pop();
                }

                if(result.isNull()) {
                    return false;
                }

                LOG_DEBUG(
                    "{}" "default arg: '{}' = '{}'",
                    pad(),
                    TTPD->getNameAsString(),
                    result.getAsString());
                out.emplace_back(result);
                continue;
            }

            if(auto NTTPD = llvm::dyn_cast<clang::NonTypeTemplateParmDecl>(param);
               NTTPD && NTTPD->hasDefaultArgument()) {
                auto& argument = NTTPD->getDefaultArgument().getArgument();
                if(argument.getKind() != clang::TemplateArgument::Expression) {
                    out.emplace_back(argument);
                    continue;
                }
                auto expr = argument.getAsExpr();
                if(!expr->isValueDependent()) {
                    if(auto value = expr->getIntegerConstantExpr(context)) {
                        out.emplace_back(
                            clang::TemplateArgument(context, *value, NTTPD->getType()));
                        continue;
                    }
                }
                /// A default naming an earlier parameter (`M = N`) takes that
                /// parameter's already-supplied argument, which may itself
                /// still be dependent. Compound expressions stay unfilled.
                if(auto NTTP = referenced_nttp(expr);
                   NTTP && NTTP->getIndex() < out.size() && NTTP->getDepth() == list->getDepth()) {
                    out.emplace_back(out[NTTP->getIndex()]);
                    continue;
                }
                break;
            }

            if(auto TTPD = llvm::dyn_cast<clang::TemplateTemplateParmDecl>(param);
               TTPD && TTPD->hasDefaultArgument()) {
                auto& fallback = TTPD->getDefaultArgument().getArgument();

                /// A default naming an earlier parameter (`U = TT`) takes
                /// that parameter's already-supplied argument.
                if(fallback.getKind() == clang::TemplateArgument::Template) {
                    if(auto prev = llvm::dyn_cast_or_null<clang::TemplateTemplateParmDecl>(
                           fallback.getAsTemplate().getAsTemplateDecl());
                       prev && prev->getDepth() == list->getDepth() &&
                       prev->getIndex() < out.size()) {
                        out.emplace_back(out[prev->getIndex()]);
                        continue;
                    }
                }

                out.emplace_back(fallback);
                continue;
            }

            break;
        }

        if(out.size() == list->size()) {
            return true;
        }

        /// A parameter pack absorbs any number of surplus arguments (and may
        /// stay empty), so exact arity only applies to pack-free lists.
        return list->hasParameterPack() && out.size() + 1 >= list->size();
    }

    template <typename Decl>
    bool deduce_template_arguments(Decl* decl, TemplateArguments arguments) {
        clang::TemplateParameterList* list = nullptr;
        TemplateArguments patterns = {};

        if constexpr(std::is_same_v<Decl, clang::ClassTemplateDecl>) {
            const clang::ClassTemplateDecl* CTD = decl;
            list = CTD->getTemplateParameters();
            patterns = list->getInjectedTemplateArgs(context);
        } else if constexpr(std::is_same_v<Decl, clang::ClassTemplatePartialSpecializationDecl>) {
            const clang::ClassTemplatePartialSpecializationDecl* CTPSD = decl;
            list = CTPSD->getTemplateParameters();
            patterns = CTPSD->getTemplateArgs().asArray();
        } else if constexpr(std::is_same_v<Decl, clang::TypeAliasTemplateDecl>) {
            const clang::TypeAliasTemplateDecl* TATD = decl;
            list = TATD->getTemplateParameters();
            patterns = list->getInjectedTemplateArgs(context);
        } else {
            static_assert(dependent_false<Decl>, "Unknown declaration type");
        }

        assert(list && "No template parameters found");

        /// Patterns may reference parameters of enclosing templates (a member
        /// partial specialization pinning an outer parameter, e.g.
        /// `Inner<pair<O, U>>`). Those are concrete from this deduction's
        /// point of view: substitute the current bindings in first so the
        /// unifier compares them structurally instead of guessing.
        ///
        /// The parameters being deduced must stay free, yet an enclosing
        /// in-flight deduction of this same template may have them bound on
        /// the stack (recursive chains like allocator_traits' rebind). An
        /// empty-argument frame masks them: decl matching stops at it and
        /// reports "unbound", while enclosing templates' frames still apply.
        llvm::SmallVector<clang::TemplateArgument, 4> substituted;
        if(!stack.empty()) {
            stack.push(decl, list, {});
            bool changed = rewrite_arguments(patterns, substituted, Policy::Substitute);
            stack.pop();
            if(changed) {
                patterns = substituted;
            }
        }

        llvm::SmallVector<clang::TemplateArgument, 4> deduced;
        if(!deduce_arguments(context, list, patterns, arguments, deduced)) {
            return false;
        }

        /// If the stack is empty, we need to fabricate outer template contexts so that
        /// parameter depth/index in the deduced result can be correctly mapped. Walk
        /// up through enclosing template declarations and push their injected args.
        /// This handles cases like resolving members of a class template that is
        /// itself nested inside other templates.
        if(stack.empty()) {
            visit_template_decl_contexts(
                llvm::dyn_cast<clang::Decl>(decl->getDeclContext()),
                [&](clang::Decl* decl, clang::TemplateParameterList* params) {
                    stack.push(decl, params, params->getInjectedTemplateArgs(context));
                });
            std::ranges::reverse(stack.frames());
        }

        stack.push(decl, list, deduced);

        LOG_DEBUG(
            "{}deduce {}: {{{}}}",
            pad(),
            [&] {
                const char* kind = "primary";
                if constexpr(std::is_same_v<Decl, clang::ClassTemplatePartialSpecializationDecl>)
                    kind = "partial";
                else if constexpr(std::is_same_v<Decl, clang::TypeAliasTemplateDecl>)
                    kind = "alias";
                return kind;
            }(),
            [&] {
                std::string mapping;
                for(unsigned j = 0; j < deduced.size(); j += 1) {
                    if(j > 0)
                        mapping += ", ";
                    if(j < list->size()) {
                        mapping += list->getParam(j)->getNameAsString();
                        mapping += "=";
                    }
                    if(deduced[j].getKind() == clang::TemplateArgument::Type) {
                        mapping += "'";
                        mapping += deduced[j].getAsType().getAsString();
                        mapping += "'";
                    } else if(deduced[j].getKind() == clang::TemplateArgument::Pack)
                        mapping += "<pack>";
                    else
                        mapping += "<non-type>";
                }
                return mapping;
            }());

        return true;
    }

    /// Look up `name` in the given type. First rewrites the type (to substitute
    /// any template parameters in it), then extracts the ClassTemplateDecl or
    /// TypeAliasTemplateDecl from the resulting TST/DTST and dispatches to the
    /// appropriate lookup overload.
    lookup_result lookup(clang::QualType type, clang::DeclarationName name) {
        clang::Decl* TD = nullptr;
        llvm::ArrayRef<clang::TemplateArgument> args;
        type = resolve(type);

        if(type.isNull()) {
            return lookup_result();
        }

        if(auto TST = type->getAs<clang::TemplateSpecializationType>()) {
            TD = TST->getTemplateName().getAsTemplateDecl();
            args = TST->template_arguments();
        } else if(auto DTST = type->getAs<clang::DependentTemplateSpecializationType>()) {
            // If this DTST was already resolved (possibly to itself when unresolvable),
            // skip the redundant lookup.
            if(pack_narrowing == 0 && resolved.count(DTST)) {
                return lookup_result();
            }

            auto& template_name = DTST->getDependentTemplateName();
            auto name = template_name.getName().getIdentifier();
            if(!name) {
                return {};
            }

            if(auto decl = preferred(lookup(template_name.getQualifier(), name))) {
                TD = decl;
                args = DTST->template_arguments();
            }
        }

        if(!TD) {
            return lookup_result();
        }

        if(auto CTD = llvm::dyn_cast<clang::ClassTemplateDecl>(TD)) {
            return lookup(CTD, name, args);
        } else if(auto TATD = llvm::dyn_cast<clang::TypeAliasTemplateDecl>(TD)) {
            if(deduce_template_arguments(TATD, args)) {
                auto type = substitute(TATD->getTemplatedDecl()->getUnderlyingType());
                stack.pop();
                if(!type.isNull()) {
                    return lookup(type, name);
                }
            }
        }

        return lookup_result();
    }

    lookup_result lookup(const clang::NestedNameSpecifier* NNS, clang::DeclarationName name) {
        if(!NNS) {
            return lookup_result();
        }

        if(pack_narrowing == 0) {
            if(auto iter = resolved.find(NNS); iter != resolved.end()) {
                return lookup(iter->second, name);
            }
        }

        // Handle each NestedNameSpecifier kind:
        // - Identifier: dependent name in NNS chain (e.g. `base::type::inner`), resolve recursively
        // - TypeSpec: concrete or dependent type used as qualifier (e.g. `vector<T>::`)
        // - Global/Namespace/NamespaceAlias/Super: not dependent, cannot resolve further
        switch(NNS->getKind()) {
            case clang::NestedNameSpecifier::Identifier: {
                auto stack_size = stack.data.size();
                auto* decl = preferred(lookup(NNS->getPrefix(), NNS->getAsIdentifier()));
                auto type = get_decl_type(decl);
                if(!type.isNull()) {
                    type = substitute(type);
                }
                while(stack.data.size() > stack_size) {
                    stack.pop();
                }
                if(!type.isNull()) {
                    if(pack_narrowing == 0 && !truncated && !ctd_guard_tripped) {
                        resolved.try_emplace(NNS, type);
                    }
                    return lookup(type, name);
                }
                return {};
            }

            case clang::NestedNameSpecifier::TypeSpec: {
                return lookup(clang::QualType(NNS->getAsType(), 0), name);
            }

            /// Namespaces and the global scope are ordinary declaration
            /// contexts; a plain lookup returns the full overload set.
            case clang::NestedNameSpecifier::Namespace: {
                return NNS->getAsNamespace()->lookup(name);
            }

            case clang::NestedNameSpecifier::NamespaceAlias: {
                return NNS->getAsNamespaceAlias()->getNamespace()->lookup(name);
            }

            case clang::NestedNameSpecifier::Global: {
                return context.getTranslationUnitDecl()->lookup(name);
            }

            case clang::NestedNameSpecifier::Super: {
                return {};
            }
        }

        return lookup_result();
    }

    /// Search for `name` in the dependent base classes of `CRD`. Each base type
    /// is substituted (to resolve template params in it) then looked up.
    ///
    /// IMPORTANT: when a member is found, stack frames pushed during the lookup
    /// are intentionally left intact. The caller (resolve_dependent_name)
    /// needs them to substitute the found decl's underlying type. The caller
    /// is responsible for popping frames after substitution.
    lookup_result lookup_in_bases(clang::CXXRecordDecl* CRD, clang::DeclarationName name) {
        if(!CRD->hasDefinition()) {
            return lookup_result();
        }

        for(auto base: CRD->bases()) {
            auto type = base.getType();
            if(type->isDependentType()) {
                auto stack_size = stack.data.size();
                auto resolved_type = substitute(type);
                if(!resolved_type.isNull()) {
                    if(auto members = lookup(resolved_type, name); !members.empty()) {
                        LOG_DEBUG(
                            "{}" "found '{}' via base '{}'",
                            pad(),
                            name.getAsString(),
                            resolved_type.getAsString());
                        return members;
                    }
                }
                while(stack.data.size() > stack_size) {
                    stack.pop();
                }
            } else if(auto* record = type->getAsCXXRecordDecl()) {
                /// A dependent derived class may still inherit a fixed base;
                /// plain lookup suffices there.
                if(auto members = record->lookup(name); !members.empty()) {
                    return members;
                }
                if(auto members = lookup_in_bases(record, name); !members.empty()) {
                    return members;
                }
            }
        }

        return lookup_result();
    }

    lookup_result lookup(clang::ClassTemplateDecl* CTD,
                         clang::DeclarationName name,
                         TemplateArguments visibleArguments) {
        // Detect recursive lookup of the same CTD + name.
        // e.g. callback_traits<F> : callback_traits<decltype(&F::operator())>
        // would infinitely recurse through lookup_in_bases.
        auto ctd_key = std::make_pair(static_cast<const void*>(CTD), name.getAsOpaquePtr());
        if(!active_ctd_lookups.insert(ctd_key).second) {
            /// The empty result here means "already in flight", not "absent";
            /// the pseudo-SFINAE probe must not read it as a proof of absence.
            ctd_guard_tripped = true;
            return lookup_result();
        }

        // RAII: erase key on all exit paths.
        struct CtdGuard {
            llvm::DenseSet<std::pair<const void*, void*>>& set;
            std::pair<const void*, void*> key;

            ~CtdGuard() {
                set.erase(key);
            }
        } ctd_guard{active_ctd_lookups, ctd_key};

        llvm::SmallVector<clang::TemplateArgument, 4> arguments;
        if(!check_template_arguments(CTD, visibleArguments, arguments)) {
            return lookup_result();
        }

        llvm::SmallVector<clang::ClassTemplatePartialSpecializationDecl*> partials;
        CTD->getPartialSpecializations(partials);

        LOG_DEBUG(
            "{}" "lookup '{}' in '{}' (partials={})",
            pad(),
            name.getAsString(),
            CTD->getNameAsString(),
            partials.size());
        indent += 1;
        /// Deduction alone may match several overlapping partials; pick the
        /// most specialized one, as real instantiation would — but only among
        /// partials whose dependent pattern constraints survive the
        /// pseudo-SFINAE probe (see member_absent).
        clang::ClassTemplatePartialSpecializationDecl* best = nullptr;
        llvm::SmallVector<clang::ClassTemplatePartialSpecializationDecl*, 4> matched;
        for(auto partial: partials) {
            if(deduce_template_arguments(partial, arguments)) {
                bool viable = satisfies_pattern(partial);
                stack.pop();
                if(!viable) {
                    LOG_DEBUG(
                        "{}" "pruned partial '{}' (member absent)",
                        pad(),
                        partial->getNameAsString());
                    continue;
                }
                matched.push_back(partial);
                if(!best || more_specialized(context, partial, best)) {
                    best = partial;
                }
            }
        }

        /// Real instantiation diagnoses ambiguity: if the winner does not
        /// dominate every other match, neither a partial nor the primary may
        /// be chosen — degrade to unresolved rather than picking arbitrarily.
        if(best && matched.size() > 1) {
            for(auto partial: matched) {
                if(partial != best && !more_specialized(context, best, partial)) {
                    LOG_DEBUG("{}" "ambiguous partials; degrading", pad());
                    indent -= 1;
                    return lookup_result();
                }
            }
        }

        /// Constraint satisfaction is not evaluated (`requires false` would
        /// need subsumption machinery); a structurally matching constrained
        /// partial is therefore unverifiable — degrade rather than trust it.
        if(best && best->getTemplateParameters()->hasAssociatedConstraints()) {
            LOG_DEBUG("{}" "constrained partial; degrading", pad());
            indent -= 1;
            return lookup_result();
        }

        if(best && deduce_template_arguments(best, arguments)) {
            LOG_DEBUG("{}" "matched partial '{}'", pad(), best->getNameAsString());
            if(auto members = best->lookup(name); !members.empty()) {
                LOG_DEBUG("{}" "found in 'partial'", pad());
                indent -= 1;
                return members;
            }

            if(auto members = lookup_in_bases(best, name); !members.empty()) {
                LOG_DEBUG("{}" "found in 'base'", pad());
                indent -= 1;
                return members;
            }

            stack.pop();
        }

        if(deduce_template_arguments(CTD, arguments)) {
            LOG_DEBUG("{}using primary template", pad());
            auto CRD = CTD->getTemplatedDecl();
            if(auto members = CRD->lookup(name); !members.empty()) {
                LOG_DEBUG("{}" "found in 'primary'", pad());
                indent -= 1;
                return members;
            }

            if(auto members = lookup_in_bases(CRD, name); !members.empty()) {
                LOG_DEBUG("{}" "found in 'base'", pad());
                indent -= 1;
                return members;
            }

            stack.pop();
        }

        indent -= 1;
        return lookup_result();
    }

private:
    /// Per-kind dispatch. Whitelist of type classes the resolver understands;
    /// anything else passes through unchanged, which downstream treats as
    /// unresolved. Local qualifiers are stripped here and reapplied on the
    /// rewritten result.
    clang::QualType rewrite_type(clang::QualType type, Policy policy) {
        auto quals = type.getLocalQualifiers();
        const clang::Type* T = type.getLocalUnqualifiedType().getTypePtr();

        clang::QualType result;
        switch(T->getTypeClass()) {
            case clang::Type::TemplateTypeParm: {
                result = rewrite_parameter(llvm::cast<clang::TemplateTypeParmType>(T), policy);
                break;
            }

            /// Sugar nodes: rewrite what they point at; the wrapper is dropped,
            /// which is fine because consumers compare canonically or look
            /// through sugar.
            case clang::Type::Elaborated: {
                result = rewrite(llvm::cast<clang::ElaboratedType>(T)->getNamedType(), policy);
                break;
            }
            case clang::Type::Paren: {
                result = rewrite(llvm::cast<clang::ParenType>(T)->getInnerType(), policy);
                break;
            }
            case clang::Type::Using: {
                result = rewrite(llvm::cast<clang::UsingType>(T)->getUnderlyingType(), policy);
                break;
            }
            case clang::Type::MacroQualified: {
                result =
                    rewrite(llvm::cast<clang::MacroQualifiedType>(T)->getUnderlyingType(), policy);
                break;
            }
            case clang::Type::SubstTemplateTypeParm: {
                result =
                    rewrite(llvm::cast<clang::SubstTemplateTypeParmType>(T)->getReplacementType(),
                            policy);
                break;
            }

            /// Dependent typedefs expand under Policy::Substitute regardless of
            /// the current policy — the invariant that breaks typedef ↔ lookup
            /// cycles lives on this single line.
            case clang::Type::Typedef: {
                auto TND = llvm::cast<clang::TypedefType>(T)->getDecl();
                auto underlying = TND->getUnderlyingType();
                if(underlying->isDependentType()) {
                    result = substitute(underlying);
                }
                break;
            }

            case clang::Type::InjectedClassName: {
                auto ICT = llvm::cast<clang::InjectedClassNameType>(T);
                result = rewrite(ICT->getInjectedSpecializationType(), policy);
                break;
            }

            case clang::Type::TemplateSpecialization: {
                result = rewrite_template(llvm::cast<clang::TemplateSpecializationType>(T), policy);
                break;
            }

            case clang::Type::DependentName: {
                auto DNT = llvm::cast<clang::DependentNameType>(T);
                if(policy == Policy::Resolve) {
                    result = resolve_dependent_name(DNT);
                } else {
                    auto NNS = rewrite_specifier(DNT->getQualifier(), policy);
                    if(NNS != DNT->getQualifier()) {
                        result = context.getDependentNameType(
                            DNT->getKeyword(),
                            const_cast<clang::NestedNameSpecifier*>(NNS),
                            DNT->getIdentifier());
                    }
                }
                break;
            }

            case clang::Type::DependentTemplateSpecialization: {
                auto DTST = llvm::cast<clang::DependentTemplateSpecializationType>(T);
                if(policy == Policy::Resolve) {
                    result = resolve_dependent_template(DTST);
                } else {
                    auto& template_name = DTST->getDependentTemplateName();
                    auto NNS = rewrite_specifier(template_name.getQualifier(), policy);
                    llvm::SmallVector<clang::TemplateArgument, 4> arguments;
                    bool changed = rewrite_arguments(DTST->template_arguments(), arguments, policy);
                    if(NNS != template_name.getQualifier() || changed) {
                        result = context.getDependentTemplateSpecializationType(
                            DTST->getKeyword(),
                            clang::DependentTemplateStorage(
                                const_cast<clang::NestedNameSpecifier*>(NNS),
                                template_name.getName(),
                                template_name.hasTemplateKeyword()),
                            arguments);
                    }
                }
                break;
            }

            case clang::Type::Pointer: {
                auto pointee = rewrite(llvm::cast<clang::PointerType>(T)->getPointeeType(), policy);
                /// A substituted reference pointee makes the pointer
                /// ill-formed (`P<X&>::type` with `type = T*`); degrade
                /// rather than fabricate a malformed node.
                if(pointee->isReferenceType()) {
                    break;
                }
                result = context.getPointerType(pointee);
                break;
            }

            /// Substituted reference pointees collapse per the language
            /// rules: `&` wins over anything, `&& &&` stays `&&`. Pointees a
            /// reference cannot bind to (`void`, qualified function types)
            /// degrade instead of fabricating an invalid node.
            case clang::Type::LValueReference: {
                auto pointee =
                    rewrite(llvm::cast<clang::ReferenceType>(T)->getPointeeType(), policy);
                if(!pointee.getNonReferenceType().isReferenceable()) {
                    break;
                }
                result = context.getLValueReferenceType(pointee.getNonReferenceType());
                break;
            }

            case clang::Type::RValueReference: {
                auto pointee =
                    rewrite(llvm::cast<clang::ReferenceType>(T)->getPointeeType(), policy);
                if(!pointee.getNonReferenceType().isReferenceable()) {
                    break;
                }
                if(pointee->isLValueReferenceType()) {
                    result = context.getLValueReferenceType(pointee.getNonReferenceType());
                } else {
                    result = context.getRValueReferenceType(pointee.getNonReferenceType());
                }
                break;
            }

            case clang::Type::Attributed: {
                auto AT = llvm::cast<clang::AttributedType>(T);
                auto modified = rewrite(AT->getModifiedType(), policy);
                auto equivalent = rewrite(AT->getEquivalentType(), policy);
                if(modified == AT->getModifiedType() && equivalent == AT->getEquivalentType()) {
                    break;
                }
                result = context.getAttributedType(AT->getAttrKind(), modified, equivalent);
                break;
            }

            case clang::Type::Atomic: {
                auto AT = llvm::cast<clang::AtomicType>(T);
                auto value = rewrite(AT->getValueType(), policy);
                if(value == AT->getValueType()) {
                    break;
                }
                /// Atomic operands must be unqualified, non-reference object
                /// types; anything else degrades.
                if(value->isReferenceType() || value->isArrayType() || value->isFunctionType() ||
                   value->isAtomicType() || value.hasQualifiers()) {
                    break;
                }
                result = context.getAtomicType(value);
                break;
            }

            case clang::Type::Decayed: {
                /// A parameter written as a dependent array or function type
                /// stores its adjusted form in a DecayedType; rebuild from
                /// the substituted original.
                auto DT = llvm::cast<clang::DecayedType>(T);
                auto original = rewrite(DT->getOriginalType(), policy);
                if(original == DT->getOriginalType()) {
                    break;
                }
                result = context.getAdjustedParameterType(original);
                break;
            }

            case clang::Type::UnaryTransform: {
                auto UTT = llvm::cast<clang::UnaryTransformType>(T);
                auto base = rewrite(UTT->getBaseType(), policy);
                if(base == UTT->getBaseType()) {
                    break;
                }
                /// A resolved enum operand computes the transform directly.
                if(UTT->getUTTKind() == clang::UnaryTransformType::EnumUnderlyingType) {
                    if(auto ET = base->getAs<clang::EnumType>()) {
                        if(auto definition = ET->getDecl()->getDefinition()) {
                            result = definition->getIntegerType();
                            break;
                        }
                    }
                }
                /// Rebuilding with a non-dependent operand would fabricate a
                /// node whose canonical type is the operand itself — a wrong
                /// answer for every transform (Sema computes the result type
                /// and we do not, except for the enum case above). Degrade to
                /// the unrewritten node instead.
                if(!base->isDependentType()) {
                    break;
                }
                result = context.getUnaryTransformType(base, base, UTT->getUTTKind());
                break;
            }

            case clang::Type::MemberPointer: {
                auto MPT = llvm::cast<clang::MemberPointerType>(T);
                auto cls = MPT->getQualifier() ? MPT->getQualifier()->getAsType() : nullptr;
                if(!cls) {
                    break;
                }
                auto pointee = rewrite(MPT->getPointeeType(), policy);
                auto rewritten_cls = rewrite(clang::QualType(cls, 0), policy);
                if(pointee == MPT->getPointeeType() && rewritten_cls.getTypePtr() == cls) {
                    break;
                }
                /// Pointers to reference or void members do not exist, and
                /// the owner must stay a class type (or still-dependent);
                /// degrade.
                if(pointee->isReferenceType() || pointee->isVoidType()) {
                    break;
                }
                if(!rewritten_cls->isDependentType() && !rewritten_cls->isRecordType()) {
                    break;
                }
                auto qualifier = clang::NestedNameSpecifier::Create(context,
                                                                    nullptr,
                                                                    rewritten_cls.getTypePtr());
                result = context.getMemberPointerType(pointee,
                                                      qualifier,
                                                      rewritten_cls->getAsCXXRecordDecl());
                break;
            }

            case clang::Type::PackExpansion: {
                auto PET = llvm::cast<clang::PackExpansionType>(T);
                auto pattern = rewrite(PET->getPattern(), policy);
                if(pattern == PET->getPattern()) {
                    break;
                }
                if(pattern->containsUnexpandedParameterPack()) {
                    result = context.getPackExpansionType(pattern, PET->getNumExpansions());
                } else {
                    /// The pack was substituted with a concrete (single)
                    /// argument; the expansion collapses to it.
                    result = pattern;
                }
                break;
            }

            /// Attempt to resolve decltype expressions that reference variables.
            /// Only handles the simple case of `decltype(var)` where `var` is a VarDecl.
            /// TODO: Handle more complex decltype expressions (member access, function calls).
            case clang::Type::Decltype: {
                auto expr = llvm::cast<clang::DecltypeType>(T)->getUnderlyingExpr();
                if(auto DRE = llvm::dyn_cast<clang::DeclRefExpr>(expr)) {
                    if(auto decl = DRE->getDecl(); llvm::isa<clang::VarDecl>(decl)) {
                        result = rewrite(decl->getType(), policy);
                    }
                }
                break;
            }

            case clang::Type::DependentSizedArray: {
                auto DSAT = llvm::cast<clang::DependentSizedArrayType>(T);
                auto element = rewrite(DSAT->getElementType(), policy);
                /// Array elements must be object types (no references,
                /// void, or function types); degrade otherwise.
                if(element->isReferenceType() || element->isVoidType() ||
                   element->isFunctionType()) {
                    break;
                }

                /// `T[N]` with a known N collapses to a constant array; a
                /// bound that is itself a dependent expression (`N = M` from
                /// deduction) is threaded through instead of left stale.
                if(auto NTTP = referenced_nttp(DSAT->getSizeExpr())) {
                    auto* argument = stack.find_argument(NTTP, NTTP->getDepth(), NTTP->getIndex());
                    if(argument && argument->getKind() == clang::TemplateArgument::Integral) {
                        /// A negative bound is ill-formed; fabricating an
                        /// array from its two's-complement bits would be a
                        /// wrong answer. Degrade.
                        if(argument->getAsIntegral().isNegative()) {
                            break;
                        }
                        result = context.getConstantArrayType(element,
                                                              argument->getAsIntegral(),
                                                              nullptr,
                                                              DSAT->getSizeModifier(),
                                                              DSAT->getIndexTypeCVRQualifiers());
                        break;
                    }
                    if(argument && argument->getKind() == clang::TemplateArgument::Expression) {
                        result =
                            context.getDependentSizedArrayType(element,
                                                               argument->getAsExpr(),
                                                               DSAT->getSizeModifier(),
                                                               DSAT->getIndexTypeCVRQualifiers());
                        break;
                    }
                }

                if(element != DSAT->getElementType()) {
                    result = context.getDependentSizedArrayType(element,
                                                                DSAT->getSizeExpr(),
                                                                DSAT->getSizeModifier(),
                                                                DSAT->getIndexTypeCVRQualifiers());
                }
                break;
            }

            case clang::Type::ConstantArray: {
                auto CAT = llvm::cast<clang::ConstantArrayType>(T);
                auto element = rewrite(CAT->getElementType(), policy);
                if(element->isReferenceType() || element->isVoidType() ||
                   element->isFunctionType()) {
                    break;
                }
                if(element != CAT->getElementType()) {
                    result = context.getConstantArrayType(element,
                                                          CAT->getSize(),
                                                          CAT->getSizeExpr(),
                                                          CAT->getSizeModifier(),
                                                          CAT->getIndexTypeCVRQualifiers());
                }
                break;
            }

            case clang::Type::IncompleteArray: {
                auto IAT = llvm::cast<clang::IncompleteArrayType>(T);
                auto element = rewrite(IAT->getElementType(), policy);
                if(element->isReferenceType() || element->isVoidType() ||
                   element->isFunctionType()) {
                    break;
                }
                if(element != IAT->getElementType()) {
                    result = context.getIncompleteArrayType(element,
                                                            IAT->getSizeModifier(),
                                                            IAT->getIndexTypeCVRQualifiers());
                }
                break;
            }

            case clang::Type::FunctionProto: {
                auto FPT = llvm::cast<clang::FunctionProtoType>(T);
                auto ret = rewrite(FPT->getReturnType(), policy);
                llvm::SmallVector<clang::QualType, 4> params;
                bool changed = ret != FPT->getReturnType();
                bool representable = true;
                for(auto param: FPT->getParamTypes()) {
                    /// A parameter pack (`void(Ts...)`) splices its bound
                    /// elements into the parameter list.
                    if(auto PET = param->getAs<clang::PackExpansionType>()) {
                        auto splice = [&](llvm::ArrayRef<clang::TemplateArgument> elements) {
                            for(auto& element: elements) {
                                if(element.getKind() != clang::TemplateArgument::Type ||
                                   element.getAsType()->isVoidType()) {
                                    representable = false;
                                    return;
                                }
                                params.push_back(
                                    context.getAdjustedParameterType(element.getAsType()));
                            }
                            changed = true;
                        };

                        auto pattern = PET->getPattern();
                        const clang::TemplateArgument* bound = nullptr;
                        if(auto TTPT = pattern->getAs<clang::TemplateTypeParmType>()) {
                            bound = stack.find_argument(TTPT);
                        }
                        if(bound && bound->getKind() == clang::TemplateArgument::Pack) {
                            splice(bound->getPackAsArray());
                            continue;
                        }
                        if(auto expanded = expand_pack_pattern(pattern, policy)) {
                            splice(*expanded);
                            continue;
                        }
                    }

                    auto rewritten = rewrite(param, policy);
                    changed |= rewritten != param;
                    /// Parameters decay (`void(T)` with `T = int[2]` is
                    /// `void(int*)`), so a substituted type must be adjusted
                    /// before it enters the prototype; a substituted `void`
                    /// parameter cannot exist and degrades.
                    if(rewritten->isVoidType()) {
                        representable = false;
                        break;
                    }
                    params.push_back(context.getAdjustedParameterType(rewritten));
                }
                if(!representable) {
                    break;
                }
                /// Functions cannot return arrays or functions; degrade.
                if(ret->isArrayType() || ret->isFunctionType()) {
                    break;
                }

                auto EPI = FPT->getExtProtoInfo();

                /// `noexcept(B)` with a bound bare parameter substitutes at
                /// the specification level: an expression binding replaces
                /// the operand whole, a value binding selects the concrete
                /// specification — no expression is ever rebuilt.
                if(FPT->getExceptionSpecType() == clang::EST_DependentNoexcept) {
                    if(auto NTTP = referenced_nttp(FPT->getNoexceptExpr())) {
                        auto* bound = stack.find_argument(NTTP, NTTP->getDepth(), NTTP->getIndex());
                        if(bound && bound->getKind() == clang::TemplateArgument::Integral) {
                            EPI.ExceptionSpec = {};
                            EPI.ExceptionSpec.Type = bound->getAsIntegral().getBoolValue()
                                                         ? clang::EST_BasicNoexcept
                                                         : clang::EST_None;
                            changed = true;
                        } else if(bound &&
                                  bound->getKind() == clang::TemplateArgument::Expression) {
                            EPI.ExceptionSpec.NoexceptExpr = bound->getAsExpr();
                            changed = true;
                        }
                    }
                }

                if(changed) {
                    /// Splicing changes the arity; per-parameter ABI info
                    /// cannot be carried over.
                    if(params.size() != FPT->getNumParams()) {
                        EPI.ExtParameterInfos = nullptr;
                    }
                    result = context.getFunctionType(ret, params, EPI);
                }
                break;
            }

            default: {
                break;
            }
        }

        if(result.isNull()) {
            return clang::QualType();
        }
        /// cv-qualifiers applied through substitution are ignored when the
        /// substituted type is a reference or function type (`const T` with
        /// `T = X&` is just `X&`).
        if(quals.hasQualifiers() && !result->isReferenceType() && !result->isFunctionType()) {
            result = context.getQualifiedType(result, quals);
        }
        return result;
    }

    clang::QualType rewrite_parameter(const clang::TemplateTypeParmType* TTPT, Policy policy) {
        // First, try to find a substitution in the instantiation stack.
        if(auto* argument = stack.find_argument(TTPT)) {
            clang::QualType type;

            if(argument->getKind() == clang::TemplateArgument::Type) {
                type = argument->getAsType();
            } else if(argument->getKind() == clang::TemplateArgument::Pack) {
                auto pack = argument->getPackAsArray();
                if(pack.size() == 1 && pack[0].getKind() == clang::TemplateArgument::Type) {
                    type = pack[0].getAsType();
                }
                /// Multi-element packs are spliced at the template argument
                /// list level (rewrite_arguments); a bare parameter cannot
                /// stand for several types at once.
            }

            return type;
        }

        // No stack substitution available. Fall back to using the parameter's
        // default argument if one exists. This enables resolution chains like:
        //   template<typename T, typename Alloc = allocator<T>> struct vector;
        // where Alloc's default depends on T.
        if(policy == Policy::Resolve) {
            if(clang::TemplateTypeParmDecl* TTPD = TTPT->getDecl();
               TTPD && TTPD->hasDefaultArgument()) {
                const auto& argument = TTPD->getDefaultArgument().getArgument();
                if(argument.getKind() == clang::TemplateArgument::Type) {
                    return rewrite(argument.getAsType(), policy);
                }
            }
        }

        return clang::QualType();
    }

    /// Build a template specialization type from as-written (flat) arguments.
    ///
    /// The canonical argument list must mirror Sema's argument conversion or
    /// the produced type would never compare equal to a parsed `X<...>`: the
    /// trailing arguments of a parameter pack are grouped into a single Pack
    /// argument, while the specified list stays flat as written.
    clang::QualType make_specialization(clang::TemplateName name, TemplateArguments arguments) {
        /// A template template parameter can be bound to a dependent template
        /// name (libc++ binds `_Alloc::template rebind` this way). A
        /// TemplateSpecializationType cannot carry those (clang asserts);
        /// rebuild the dependent form with the substituted arguments instead.
        if(auto dependent = name.getAsDependentTemplateName()) {
            return context.getDependentTemplateSpecializationType(
                clang::ElaboratedTypeKeyword::None,
                *dependent,
                arguments);
        }

        llvm::SmallVector<clang::TemplateArgument, 4> canonical;

        /// A head bound through a template template parameter may carry an
        /// argument list the parameters cannot accept — too few (defaulted
        /// trailing parameters must be filled or the canonical list never
        /// compares equal to a parsed `target<X>`) or too many (rebuilding
        /// would fabricate an invalid specialization; degrade instead).
        llvm::SmallVector<clang::TemplateArgument, 4> full;
        clang::TemplateParameterList* params = nullptr;
        if(auto TD = name.getAsTemplateDecl()) {
            params = TD->getTemplateParameters();
            if(!check_template_arguments(TD, arguments, full)) {
                return clang::QualType();
            }
            arguments = full;
        }

        unsigned i = 0;
        if(params) {
            for(auto param: *params) {
                if(param->isTemplateParameterPack()) {
                    if(arguments.size() - i == 1 &&
                       arguments[i].getKind() == clang::TemplateArgument::Pack) {
                        /// Already grouped by deduction.
                        canonical.emplace_back(context.getCanonicalTemplateArgument(arguments[i]));
                        i += 1;
                    } else {
                        llvm::SmallVector<clang::TemplateArgument, 4> pack;
                        for(; i < arguments.size(); i += 1) {
                            pack.emplace_back(context.getCanonicalTemplateArgument(arguments[i]));
                        }
                        canonical.emplace_back(
                            clang::TemplateArgument::CreatePackCopy(context, pack));
                    }
                    break;
                }
                if(i >= arguments.size()) {
                    break;
                }
                canonical.emplace_back(context.getCanonicalTemplateArgument(arguments[i]));
                i += 1;
            }
        }
        for(; i < arguments.size(); i += 1) {
            canonical.emplace_back(context.getCanonicalTemplateArgument(arguments[i]));
        }

        /// Fully concrete results should compare equal to the same type
        /// written in source, whose canonical form is the specialization
        /// decl's record type. findSpecialization is a read-only registry
        /// query — if the TU never named this specialization, we keep the
        /// bare canonical TST rather than fabricating a declaration.
        clang::QualType underlying;
        bool concrete = std::ranges::none_of(canonical, [](const clang::TemplateArgument& arg) {
            return arg.isDependent();
        });
        if(concrete) {
            if(auto CTD =
                   llvm::dyn_cast_or_null<clang::ClassTemplateDecl>(name.getAsTemplateDecl())) {
                void* pos = nullptr;
                if(auto CTSD = CTD->findSpecialization(canonical, pos)) {
                    underlying = context.getTypeDeclType(CTSD);
                }
            }
        }

        /// An alias specialization must carry its aliased type unless the
        /// arguments still hold unexpanded packs (clang asserts on this).
        /// This arises when a template template parameter got substituted
        /// with an alias template: the head is ours, so the aliasing is ours
        /// to compute. Failure degrades to an unrewritten (null) result.
        if(auto TATD =
               llvm::dyn_cast_or_null<clang::TypeAliasTemplateDecl>(name.getAsTemplateDecl())) {
            bool expansions =
                std::ranges::any_of(canonical, [](const clang::TemplateArgument& arg) {
                    return arg.isPackExpansion();
                });
            if(!expansions) {
                clang::QualType aliased;
                if(deduce_template_arguments(TATD, arguments)) {
                    aliased = substitute(TATD->getTemplatedDecl()->getUnderlyingType());
                    stack.pop();
                }
                if(aliased.isNull()) {
                    return clang::QualType();
                }
                return context.getTemplateSpecializationType(name, arguments, canonical, aliased);
            }
        }

        return context.getTemplateSpecializationType(name, arguments, canonical, underlying);
    }

    clang::QualType rewrite_template(const clang::TemplateSpecializationType* TST, Policy policy) {
        /// Alias specializations carry the substituted underlying type as
        /// sugar; expanding it is substitution, not lookup, so it is safe
        /// under both policies.
        if(TST->isTypeAlias()) {
            return rewrite(TST->desugar(), policy);
        }

        /// A bound template template parameter in the head is substituted
        /// with its deduced template, e.g. `TT<U, Ts...>` after matching
        /// `replace_first<TT<T, Ts...>, U>` against `box<X>`.
        auto name = TST->getTemplateName();
        bool head_changed = false;
        if(auto TTP =
               llvm::dyn_cast_or_null<clang::TemplateTemplateParmDecl>(name.getAsTemplateDecl())) {
            if(auto* bound = stack.find_argument(TTP, TTP->getDepth(), TTP->getIndex());
               bound && bound->getKind() == clang::TemplateArgument::Template) {
                name = bound->getAsTemplate();
                head_changed = true;
            }
        }

        llvm::SmallVector<clang::TemplateArgument, 4> arguments;
        bool args_changed = rewrite_arguments(TST->template_arguments(), arguments, policy);
        if(!head_changed && !args_changed) {
            return clang::QualType();
        }

        return make_specialization(name, arguments);
    }

    /// Rewrite a template argument list. Returns true if anything changed.
    /// Pack expansions whose pattern is a bound pack parameter are spliced
    /// inline, so `type_list<Us...>` with `Us = {int, float}` becomes
    /// `type_list<int, float>`.
    bool rewrite_arguments(TemplateArguments arguments,
                           llvm::SmallVectorImpl<clang::TemplateArgument>& out,
                           Policy policy) {
        bool changed = false;

        for(auto& argument: arguments) {
            switch(argument.getKind()) {
                case clang::TemplateArgument::Type: {
                    auto type = argument.getAsType();

                    if(auto PET = type->getAs<clang::PackExpansionType>()) {
                        auto pattern = PET->getPattern();
                        if(auto TTPT = pattern->getAs<clang::TemplateTypeParmType>()) {
                            auto* bound = stack.find_argument(TTPT);
                            if(bound && bound->getKind() == clang::TemplateArgument::Pack) {
                                out.append(bound->pack_begin(), bound->pack_end());
                                changed = true;
                                continue;
                            }
                        }

                        /// A pattern that merely contains bound packs
                        /// (`box<Us>...`) expands element-wise: rewrite it
                        /// once per element with every referenced pack
                        /// temporarily narrowed to its k-th element. Packs
                        /// expand in lockstep, so their lengths must agree.
                        if(auto expanded = expand_pack_pattern(pattern, policy)) {
                            out.append(expanded->begin(), expanded->end());
                            changed = true;
                            continue;
                        }

                        auto rewritten = rewrite(pattern, policy);
                        if(rewritten != pattern) {
                            changed = true;
                            if(rewritten->containsUnexpandedParameterPack()) {
                                rewritten = context.getPackExpansionType(rewritten,
                                                                         PET->getNumExpansions());
                            }
                            out.emplace_back(rewritten);
                        } else {
                            out.push_back(argument);
                        }
                        continue;
                    }

                    auto rewritten = rewrite(type, policy);
                    changed |= rewritten != type;
                    out.emplace_back(rewritten);
                    break;
                }

                case clang::TemplateArgument::Expression: {
                    /// An expression pack expansion (`Ns...`) splices its
                    /// bound pack, mirroring the type-pack path above.
                    if(auto PEE = llvm::dyn_cast<clang::PackExpansionExpr>(argument.getAsExpr())) {
                        if(auto NTTP = referenced_nttp(PEE->getPattern())) {
                            auto* bound =
                                stack.find_argument(NTTP, NTTP->getDepth(), NTTP->getIndex());
                            if(bound && bound->getKind() == clang::TemplateArgument::Pack) {
                                out.append(bound->pack_begin(), bound->pack_end());
                                changed = true;
                                continue;
                            }
                        }
                    }

                    /// Substitute a bound non-type parameter at the argument
                    /// level; expressions themselves are never rebuilt.
                    /// TODO(nttp-expr): compound expressions (`value<N + 1>`)
                    /// therefore keep referencing the original parameter and
                    /// degrade to unresolved downstream — never a wrong
                    /// answer, never a crash. Rebuilding them needs an
                    /// expression substitution engine; deliberately deferred.
                    /// A Pack binding is only usable inside a narrowed element
                    /// rewrite (where the slot holds one element) — splicing
                    /// it here as a single argument would be malformed.
                    if(auto NTTP = referenced_nttp(argument.getAsExpr())) {
                        auto* bound = stack.find_argument(NTTP, NTTP->getDepth(), NTTP->getIndex());
                        if(bound && !bound->isNull() &&
                           bound->getKind() != clang::TemplateArgument::Pack) {
                            out.push_back(*bound);
                            changed = true;
                            continue;
                        }
                    }
                    out.push_back(argument);
                    break;
                }

                case clang::TemplateArgument::Template: {
                    /// A template template parameter forwarded as an argument
                    /// (`apply<TT, Us...>`) is substituted with its binding.
                    if(auto TTP = llvm::dyn_cast_or_null<clang::TemplateTemplateParmDecl>(
                           argument.getAsTemplate().getAsTemplateDecl())) {
                        auto* bound = stack.find_argument(TTP, TTP->getDepth(), TTP->getIndex());
                        if(bound && bound->getKind() == clang::TemplateArgument::Template) {
                            out.push_back(*bound);
                            changed = true;
                            continue;
                        }
                    }

                    /// A dependent name (`apply<T::template tmpl>`) carries
                    /// its qualifier inside the TemplateName; rewrite it so
                    /// the frame's bindings do not go stale.
                    if(auto dependent = argument.getAsTemplate().getAsDependentTemplateName()) {
                        auto qualifier = rewrite_specifier(dependent->getQualifier(), policy);
                        if(qualifier != dependent->getQualifier()) {
                            auto name =
                                context.getDependentTemplateName(clang::DependentTemplateStorage(
                                    const_cast<clang::NestedNameSpecifier*>(qualifier),
                                    dependent->getName(),
                                    dependent->hasTemplateKeyword()));
                            out.emplace_back(name);
                            changed = true;
                            continue;
                        }
                    }
                    out.push_back(argument);
                    break;
                }

                case clang::TemplateArgument::TemplateExpansion: {
                    /// `Fs...` splices its bound template pack, symmetric
                    /// with the type and expression pack paths.
                    if(auto TTP = llvm::dyn_cast_or_null<clang::TemplateTemplateParmDecl>(
                           argument.getAsTemplateOrTemplatePattern().getAsTemplateDecl())) {
                        auto* bound = stack.find_argument(TTP, TTP->getDepth(), TTP->getIndex());
                        if(bound && bound->getKind() == clang::TemplateArgument::Pack) {
                            out.append(bound->pack_begin(), bound->pack_end());
                            changed = true;
                            continue;
                        }
                    }
                    out.push_back(argument);
                    break;
                }

                default: {
                    out.push_back(argument);
                    break;
                }
            }
        }

        return changed;
    }

    /// Element-wise expansion of a structured pack pattern: if every pack
    /// parameter referenced in `pattern` is bound to a Pack of one common
    /// length, rewrite the pattern once per element and return the list.
    /// Returns nullopt when the pattern has no bound packs, lengths disagree,
    /// or an element fails to rewrite cleanly — callers then fall back to
    /// rewriting the pattern as a whole.
    std::optional<llvm::SmallVector<clang::TemplateArgument, 4>>
        expand_pack_pattern(clang::QualType pattern, Policy policy) {
        PackParmCollector collector;
        collector.TraverseType(pattern);
        if(collector.packs.empty() && collector.template_packs.empty() &&
           collector.value_packs.empty()) {
            return std::nullopt;
        }

        llvm::SmallVector<InstantiationStack::Slot, 2> slots;
        unsigned length = 0;
        auto admit = [&](std::optional<InstantiationStack::Slot> handle) {
            if(!handle) {
                return false;
            }
            auto& bound = stack.slot(*handle);
            if(bound.getKind() != clang::TemplateArgument::Pack) {
                return false;
            }
            if(!slots.empty() && bound.pack_size() != length) {
                return false;
            }
            length = bound.pack_size();
            if(!llvm::is_contained(slots, *handle)) {
                slots.push_back(*handle);
            }
            return true;
        };

        for(auto TTPT: collector.packs) {
            if(!admit(stack.find_slot(TTPT->getDecl(), TTPT->getDepth(), TTPT->getIndex()))) {
                return std::nullopt;
            }
        }
        for(auto TTP: collector.template_packs) {
            if(!admit(stack.find_slot(TTP, TTP->getDepth(), TTP->getIndex()))) {
                return std::nullopt;
            }
        }
        for(auto NTTP: collector.value_packs) {
            if(!admit(stack.find_slot(NTTP, NTTP->getDepth(), NTTP->getIndex()))) {
                return std::nullopt;
            }
        }

        llvm::SmallVector<clang::TemplateArgument, 2> saved(
            llvm::map_range(slots, [&](auto handle) { return stack.slot(handle); }));

        /// While narrowed, node-pointer-keyed caches are bypassed: the same
        /// pattern node resolves differently per element, and a cached first
        /// element would silently repeat for every later one.
        pack_narrowing += 1;
        bool ok = true;
        llvm::SmallVector<clang::TemplateArgument, 4> expanded;
        for(unsigned k = 0; k < length && ok; k += 1) {
            for(auto [handle, pack]: llvm::zip(slots, saved)) {
                stack.slot(handle) = pack.pack_begin()[k];
            }
            auto element = rewrite(pattern, policy);
            ok = element != pattern && !element->containsUnexpandedParameterPack();
            if(ok) {
                expanded.emplace_back(element);
            }
        }
        pack_narrowing -= 1;

        for(auto [handle, pack]: llvm::zip(slots, saved)) {
            stack.slot(handle) = pack;
        }

        if(!ok) {
            return std::nullopt;
        }
        return expanded;
    }

    const clang::NestedNameSpecifier* rewrite_specifier(const clang::NestedNameSpecifier* NNS,
                                                        Policy policy) {
        if(!NNS) {
            return nullptr;
        }

        switch(NNS->getKind()) {
            case clang::NestedNameSpecifier::TypeSpec: {
                auto prefix = rewrite_specifier(NNS->getPrefix(), policy);

                /// A dependent component written as `prefix::template B<Y>` keeps
                /// its qualifier in the specifier chain, not in the type node
                /// itself; resolve it in the scope of the rewritten prefix.
                clang::QualType type;
                auto component = clang::QualType(NNS->getAsType(), 0);
                auto DTST =
                    llvm::dyn_cast<clang::DependentTemplateSpecializationType>(NNS->getAsType());
                if(DTST && !DTST->getDependentTemplateName().getQualifier() &&
                   policy == Policy::Resolve) {
                    type = resolve_dependent_template(DTST, prefix);
                } else {
                    type = rewrite(component, policy);
                }

                if(prefix == NNS->getPrefix() && type.getTypePtr() == NNS->getAsType()) {
                    return NNS;
                }
                return clang::NestedNameSpecifier::Create(
                    context,
                    const_cast<clang::NestedNameSpecifier*>(prefix),
                    type.getTypePtr());
            }

            /// Identifier components are resolved by lookup itself; the prefix
            /// may still contain substitutable types.
            case clang::NestedNameSpecifier::Identifier: {
                auto prefix = rewrite_specifier(NNS->getPrefix(), policy);
                if(prefix == NNS->getPrefix()) {
                    return NNS;
                }
                return clang::NestedNameSpecifier::Create(
                    context,
                    const_cast<clang::NestedNameSpecifier*>(prefix),
                    NNS->getAsIdentifier());
            }

            default: {
                return NNS;
            }
        }
    }

    /// Pseudo-SFINAE: decide whether a partial specialization's dependent
    /// pattern constraints (e.g. the `void_t<typename A::rebind<U>::other>`
    /// idiom) are satisfiable under the current bindings.
    ///
    /// Real SFINAE substitutes and rejects on ill-formedness. We approximate
    /// with a three-way probe on each dependent member access in the pattern:
    ///   - member resolves → constraint holds, keep the partial
    ///   - qualifier resolves to a known template/record but the member does
    ///     not exist there → constraint provably fails, prune the partial
    ///   - qualifier unknown (bare parameter etc.) → benefit of the doubt,
    ///     keep the partial; never guess a concrete answer from uncertainty
    bool satisfies_pattern(clang::ClassTemplatePartialSpecializationDecl* partial) {
        if(probing > 4) {
            return true;
        }

        /// The probe expression only survives in the as-written arguments:
        /// the converted list has already desugared `void_t<...>` to `void`.
        auto written = partial->getTemplateArgsAsWritten();
        if(!written) {
            return true;
        }

        probing += 1;
        bool viable = true;
        for(const clang::TemplateArgumentLoc& loc: written->arguments()) {
            auto& argument = loc.getArgument();
            if(argument.getKind() == clang::TemplateArgument::Type &&
               member_absent(argument.getAsType(), 0)) {
                viable = false;
                break;
            }
        }
        probing -= 1;
        return viable;
    }

    /// Walk the written form of `type` (through alias sugar arguments, which
    /// is where `void_t` hides its probe expression) and report whether any
    /// dependent member access provably names a non-existent member.
    bool member_absent(clang::QualType type, unsigned guard) {
        /// Note: `void_t<DNT>` canonically IS `void`, so this must test
        /// instantiation dependence, not type dependence.
        if(type.isNull() || guard > 16 || !type->isInstantiationDependentType()) {
            return false;
        }

        const clang::Type* T = type.getLocalUnqualifiedType().getTypePtr();
        switch(T->getTypeClass()) {
            case clang::Type::DependentName: {
                auto DNT = llvm::cast<clang::DependentNameType>(T);
                return specifier_absent(DNT->getQualifier(), guard) ||
                       scope_lacks(DNT->getQualifier(),
                                   DNT->getIdentifier(),
                                   /*wants_template=*/false);
            }

            case clang::Type::DependentTemplateSpecialization: {
                auto DTST = llvm::cast<clang::DependentTemplateSpecializationType>(T);
                auto& template_name = DTST->getDependentTemplateName();
                auto identifier = template_name.getName().getIdentifier();
                auto qualifier = template_name.getQualifier();
                if(specifier_absent(qualifier, guard)) {
                    return true;
                }
                return identifier && scope_lacks(qualifier, identifier, /*wants_template=*/true);
            }

            case clang::Type::TemplateSpecialization: {
                auto TST = llvm::cast<clang::TemplateSpecializationType>(T);
                /// Check the arguments as written: alias sugar (`void_t<...>`)
                /// desugars to a type that no longer contains the probe.
                for(auto& argument: TST->template_arguments()) {
                    if(argument.getKind() == clang::TemplateArgument::Type &&
                       member_absent(argument.getAsType(), guard + 1)) {
                        return true;
                    }
                }
                return false;
            }

            case clang::Type::Elaborated: {
                return member_absent(llvm::cast<clang::ElaboratedType>(T)->getNamedType(),
                                     guard + 1);
            }
            case clang::Type::Paren: {
                return member_absent(llvm::cast<clang::ParenType>(T)->getInnerType(), guard + 1);
            }
            case clang::Type::Pointer: {
                return member_absent(llvm::cast<clang::PointerType>(T)->getPointeeType(),
                                     guard + 1);
            }
            case clang::Type::LValueReference:
            case clang::Type::RValueReference: {
                return member_absent(llvm::cast<clang::ReferenceType>(T)->getPointeeType(),
                                     guard + 1);
            }
            case clang::Type::PackExpansion: {
                return member_absent(llvm::cast<clang::PackExpansionType>(T)->getPattern(),
                                     guard + 1);
            }

            default: {
                return false;
            }
        }
    }

    /// Does any link of the specifier chain provably name a missing member?
    bool specifier_absent(const clang::NestedNameSpecifier* NNS, unsigned guard) {
        if(!NNS || guard > 16) {
            return false;
        }
        if(specifier_absent(NNS->getPrefix(), guard + 1)) {
            return true;
        }

        switch(NNS->getKind()) {
            case clang::NestedNameSpecifier::Identifier: {
                return scope_lacks(NNS->getPrefix(), NNS->getAsIdentifier());
            }
            case clang::NestedNameSpecifier::TypeSpec: {
                const clang::Type* T = NNS->getAsType();
                if(auto DTST = llvm::dyn_cast<clang::DependentTemplateSpecializationType>(T)) {
                    auto& template_name = DTST->getDependentTemplateName();
                    auto scope = template_name.getQualifier() ? template_name.getQualifier()
                                                              : NNS->getPrefix();
                    auto identifier = template_name.getName().getIdentifier();
                    return identifier && scope_lacks(scope, identifier, /*wants_template=*/true);
                }
                if(auto DNT = llvm::dyn_cast<clang::DependentNameType>(T)) {
                    return scope_lacks(DNT->getQualifier(), DNT->getIdentifier());
                }
                return false;
            }
            default: {
                return false;
            }
        }
    }

    /// Resolve `scope` and ask whether it is a known template or record that
    /// definitely has no member called `name`. Unknown scopes return false.
    ///
    /// "Definitely" requires a clean verdict: an empty lookup caused by the
    /// CTD recursion guard or by step-budget exhaustion proves nothing, and
    /// treating it as absence would prune a partial that real SFINAE keeps —
    /// a wrong answer, not a degradation. Those cases stay Unknown.
    /// Does `record`'s definition inherit from a still-dependent base? Such
    /// a base may contribute the probed member only after instantiation, so
    /// absence is never conclusive through it. An undefined record proves
    /// nothing either.
    static bool has_dependent_base(const clang::CXXRecordDecl* record) {
        auto definition = record->getDefinition();
        if(!definition) {
            return true;
        }
        return std::ranges::any_of(definition->bases(), [](const clang::CXXBaseSpecifier& base) {
            return base.getType()->isDependentType();
        });
    }

    /// Does any specialization of `CTD` — partial or explicit — declare
    /// `name`? Used to gate the Absent verdict: if some specialization has
    /// the member, an empty lookup on dependent arguments proves nothing.
    bool any_specialization_declares(clang::ClassTemplateDecl* CTD, clang::DeclarationName name) {
        auto declares = [&](clang::CXXRecordDecl* record) {
            auto* definition = record->getDefinition();
            return definition && (!definition->lookup(name).empty() ||
                                  !lookup_in_bases(definition, name).empty());
        };

        llvm::SmallVector<clang::ClassTemplatePartialSpecializationDecl*, 4> partials;
        CTD->getPartialSpecializations(partials);
        return std::ranges::any_of(partials, declares) ||
               std::ranges::any_of(CTD->specializations(), declares);
    }

    /// Is `decl` provably unusable for the probe's usage form? `typename
    /// X::m` cannot name a value; `X::template m<...>` cannot name a value
    /// or a plain type. Unknown declaration kinds stay presumed viable.
    static bool unusable_member(clang::Decl* decl, bool wants_template) {
        if(auto shadow = llvm::dyn_cast<clang::UsingShadowDecl>(decl)) {
            decl = shadow->getTargetDecl();
        }
        if(llvm::isa<clang::ValueDecl, clang::FunctionTemplateDecl, clang::VarTemplateDecl>(decl)) {
            return true;
        }
        /// Access participates in SFINAE: the probe models a use from
        /// outside the class, where a non-public member never resolves.
        /// Friendship of the probing specialization is not modeled — an
        /// accepted approximation; detectors befriended by their subjects
        /// degrade to the primary.
        if(decl->getAccess() == clang::AS_private || decl->getAccess() == clang::AS_protected) {
            return true;
        }
        /// `typename X::m` cannot name an unspecialized template either.
        if(!wants_template && llvm::isa<clang::TemplateDecl>(decl)) {
            return true;
        }
        return wants_template && llvm::isa<clang::TypeDecl>(decl);
    }

    bool scope_lacks(const clang::NestedNameSpecifier* scope,
                     clang::DeclarationName name,
                     bool wants_template = false) {
        if(!scope) {
            return false;
        }

        /// Empty lookups and lookups whose every candidate is provably
        /// unusable both fail the probe conclusively.
        auto conclusive = [&](lookup_result members) {
            return std::ranges::all_of(members, [&](clang::Decl* decl) {
                return unusable_member(decl, wants_template);
            });
        };

        auto stack_size = stack.data.size();

        /// The flag is save/reset/restored rather than just read: `scope_lacks`
        /// reenters itself through lookup's partial probing, and a nested clean
        /// probe must not wash out a trip observed by an in-flight outer one.
        /// Restoring with `saved || tripped` propagates any trip outwards, so
        /// every enclosing probe also stays Unknown. The reset sits before
        /// `rewrite_specifier` because resolving the scope prefix can trip the
        /// guard too.
        bool saved = ctd_guard_tripped;
        ctd_guard_tripped = false;

        auto resolved_scope = rewrite_specifier(scope, Policy::Resolve);

        bool lacks = false;
        if(resolved_scope && resolved_scope->getKind() == clang::NestedNameSpecifier::TypeSpec) {
            auto type = resolve(clang::QualType(resolved_scope->getAsType(), 0));
            if(!type.isNull()) {
                if(auto TST = type->getAs<clang::TemplateSpecializationType>()) {
                    if(auto CTD = llvm::dyn_cast_or_null<clang::ClassTemplateDecl>(
                           TST->getTemplateName().getAsTemplateDecl())) {
                        lacks = conclusive(lookup(type, name));

                        /// With dependent arguments the selection above is
                        /// provisional — a specialization rejected against a
                        /// bare parameter may apply once it instantiates. An
                        /// empty result is then conclusive only if no
                        /// specialization of the template declares the member
                        /// at all.
                        if(lacks && TST->isDependentType() &&
                           any_specialization_declares(CTD, name)) {
                            lacks = false;
                        }

                        /// A dependent base (`struct D : T`) may contribute
                        /// the member after instantiation.
                        if(lacks && has_dependent_base(CTD->getTemplatedDecl())) {
                            lacks = false;
                        }
                    }
                } else if(auto RD = type->getAsCXXRecordDecl()) {
                    lacks = conclusive(RD->lookup(name)) && conclusive(lookup_in_bases(RD, name)) &&
                            !has_dependent_base(RD);
                }
            }
        }

        if(ctd_guard_tripped || truncated) {
            lacks = false;
        }
        ctd_guard_tripped = saved || ctd_guard_tripped;

        while(stack.data.size() > stack_size) {
            stack.pop();
        }
        return lacks;
    }

    clang::QualType resolve_dependent_name(const clang::DependentNameType* DNT) {
        LOG_DEBUG("{}" "resolve '{}'", pad(), clang::QualType(DNT, 0).getAsString());
        indent += 1;

        // Check cache.
        if(pack_narrowing == 0) {
            if(auto iter = resolved.find(DNT); iter != resolved.end()) {
                LOG_DEBUG("{}" "→ '{}' (cached)", pad(), iter->second.getAsString());
                indent -= 1;
                return iter->second;
            }
        }

        // Cycle detection: if we're already resolving this DNT, bail out.
        if(!active_resolutions.insert(DNT).second) {
            LOG_DEBUG("{}→ <cycle detected, returning original>", pad());
            indent -= 1;
            return clang::QualType(DNT, 0);
        }

        auto* NNS = rewrite_specifier(DNT->getQualifier(), Policy::Resolve);
        auto stack_size = stack.data.size();
        auto* decl = preferred(lookup(NNS, DNT->getIdentifier()));
        auto type = get_decl_type(decl);

        clang::QualType result;
        if(!type.isNull()) {
            const char* decl_kind = "decl";
            if(llvm::isa<clang::TypedefNameDecl>(decl))
                decl_kind = "typedef";
            else if(llvm::isa<clang::RecordDecl>(decl))
                decl_kind = "record";
            auto decl_name = llvm::dyn_cast<clang::NamedDecl>(decl)
                                 ? llvm::dyn_cast<clang::NamedDecl>(decl)->getNameAsString()
                                 : "?";
            LOG_DEBUG("{}" "found {} '{}' = '{}'", pad(), decl_kind, decl_name, type.getAsString());

            // Step 1: substitute params (expand typedefs, no lookup).
            result = substitute(type);
            LOG_DEBUG("{}" "substitute → '{}'", pad(), result.getAsString());

            // Pop lookup frames BEFORE further resolution. The substitute step already
            // used the full stack for parameter substitution. Resolution should only
            // see the outer context to avoid polluting free variables (e.g. T) with
            // mappings from intermediate lookup frames.
            while(stack.data.size() > stack_size) {
                stack.pop();
            }

            // Step 2: if still dependent, do full resolution (may trigger more lookups).
            if(!result.isNull() && result->isDependentType()) {
                result = rewrite(result, Policy::Resolve);
            }
        } else {
            while(stack.data.size() > stack_size) {
                stack.pop();
            }
        }

        active_resolutions.erase(DNT);

        if(!result.isNull()) {
            LOG_DEBUG("{}" "→ '{}'", pad(), result.getAsString());
            indent -= 1;
            if(pack_narrowing == 0 && !truncated && !ctd_guard_tripped) {
                resolved.try_emplace(DNT, result);
            }
            return result;
        }

        LOG_DEBUG("{}→ <unresolved>", pad());
        indent -= 1;
        return clang::QualType(DNT, 0);
    }

    /// `scope` carries the enclosing specifier prefix for components whose own
    /// qualifier is null (see rewrite_specifier). Such resolutions are not
    /// cached: the node's identity does not include the scope it was found in.
    clang::QualType
        resolve_dependent_template(const clang::DependentTemplateSpecializationType* DTST,
                                   const clang::NestedNameSpecifier* scope = nullptr) {
        LOG_DEBUG("{}" "resolve DTST '{}'", pad(), clang::QualType(DTST, 0).getAsString());
        indent += 1;

        auto& template_name = DTST->getDependentTemplateName();
        /// Scope-threaded and pack-narrowed resolutions both depend on
        /// context the node pointer does not capture; neither may be cached.
        bool cacheable = (template_name.getQualifier() != nullptr || !scope) && pack_narrowing == 0;

        if(cacheable) {
            if(auto iter = resolved.find(DTST); iter != resolved.end()) {
                indent -= 1;
                return iter->second;
            }
        }

        const clang::NestedNameSpecifier* NNS =
            template_name.getQualifier()
                ? rewrite_specifier(template_name.getQualifier(), Policy::Resolve)
                : scope;

        llvm::SmallVector<clang::TemplateArgument, 4> arguments;
        rewrite_arguments(DTST->template_arguments(), arguments, Policy::Resolve);

        auto* name = template_name.getName().getIdentifier();
        if(!name) {
            LOG_DEBUG("{}→ <unresolved DTST>", pad());
            indent -= 1;
            return clang::QualType(DTST, 0);
        }

        auto stack_size = stack.data.size();
        if(auto* decl = preferred(lookup(NNS, name))) {
            if(auto* TATD = llvm::dyn_cast<clang::TypeAliasTemplateDecl>(decl)) {
                if(deduce_template_arguments(TATD, arguments)) {
                    auto type = substitute(TATD->getTemplatedDecl()->getUnderlyingType());
                    // Pop lookup frames before further resolution.
                    while(stack.data.size() > stack_size) {
                        stack.pop();
                    }
                    if(!type.isNull() && type->isDependentType()) {
                        type = rewrite(type, Policy::Resolve);
                    }
                    if(!type.isNull()) {
                        LOG_DEBUG("{}" "→ '{}' (alias)", pad(), type.getAsString());
                        indent -= 1;
                        if(cacheable && !truncated && !ctd_guard_tripped) {
                            resolved.try_emplace(DTST, type);
                        }
                        return type;
                    }
                }
            } else if(auto* CTD = llvm::dyn_cast<clang::ClassTemplateDecl>(decl)) {
                // Resolve DTST to a concrete TemplateSpecializationType.
                // e.g. __alloc_traits<allocator<T>>::rebind<T> → rebind<T> (a TST)
                // This allows subsequent lookup of members (like "other") to work.
                // Keep lookup frames on stack — the caller (e.g. rewrite_specifier
                // processing A<X>::B<Y>::C<Z>) needs them for parameter substitution.
                auto result = make_specialization(clang::TemplateName(CTD), arguments);
                LOG_DEBUG("{}" "→ TST '{}' (class)", pad(), result.getAsString());
                indent -= 1;
                if(cacheable && !truncated && !ctd_guard_tripped) {
                    resolved.try_emplace(DTST, result);
                }
                return result;
            }
        }
        while(stack.data.size() > stack_size) {
            stack.pop();
        }

        LOG_DEBUG("{}→ <unresolved DTST>", pad());
        indent -= 1;
        auto fallback = clang::QualType(DTST, 0);
        /// Only a conclusive failure may be cached: an exhausted step budget
        /// or a tripped recursion guard proves nothing, and the cache is
        /// TU-wide — a truncated query must not poison this node for later
        /// queries that could still resolve it.
        if(cacheable && !truncated && !ctd_guard_tripped) {
            resolved.try_emplace(DTST, fallback);
        }
        return fallback;
    }

private:
    clang::ASTContext& context;
    InstantiationStack stack;
    llvm::DenseMap<const void*, clang::QualType>& resolved;
    llvm::SmallPtrSet<const void*, 8> active_resolutions;
    llvm::DenseSet<std::pair<const void*, void*>> active_ctd_lookups;
    unsigned depth = 0;
    unsigned steps = 0;
    unsigned probing = 0;
    /// Non-zero while a pack element rewrite has a Pack binding narrowed to
    /// one element; node-pointer-keyed caches are bypassed for the duration.
    unsigned pack_narrowing = 0;
    bool ctd_guard_tripped = false;
    /// Sticky: some rewrite in this query hit the depth or step limit. A
    /// truncated result is not conclusive and must never enter the TU-wide
    /// cache — a later query with a fresh budget could still resolve it.
    bool truncated = false;

    /// Hard ceiling on rewrite steps per query; bounds the total work
    /// including pseudo-SFINAE probes over rejected branches.
    constexpr static unsigned step_budget = 4096;
    unsigned indent = 0;

    std::string pad() const {
        return std::string(indent * 2, ' ');
    }
};

}  // namespace

clang::QualType TemplateResolver::resolve(clang::QualType type) {
    PseudoInstantiator instantiator(context, resolved);
    return instantiator.resolve(type);
}

TemplateResolver::lookup_result TemplateResolver::lookup(const clang::NestedNameSpecifier* NNS,
                                                         clang::DeclarationName name) {
    PseudoInstantiator instantiator(context, resolved);
    return instantiator.lookup(NNS, name);
}

/// Shared base-type member resolution for dependent member expressions.
static TemplateResolver::lookup_result
    lookup_member(clang::ASTContext& context,
                  llvm::DenseMap<const void*, clang::QualType>& resolved,
                  clang::QualType type,
                  bool arrow,
                  clang::DeclarationName name) {
    if(type.isNull()) {
        return {};
    }

    if(arrow) {
        /// Follow overloaded operator-> chains (smart pointers) until a raw
        /// pointer appears; bounded. A chain that never dereferences to a
        /// pointer (no operator->, or a cycle) makes the arrow ill-formed —
        /// treating it like a dot access would fabricate candidates.
        auto arrow_name = context.DeclarationNames.getCXXOperatorName(clang::OO_Arrow);
        bool dereferenced = false;
        for(unsigned hop = 0; hop < 8; hop += 1) {
            if(auto* PT = type->getAs<clang::PointerType>()) {
                type = PT->getPointeeType();
                dereferenced = true;
                break;
            }
            PseudoInstantiator instantiator(context, resolved);
            const clang::CXXMethodDecl* method = nullptr;
            for(auto* candidate: instantiator.lookup(type, arrow_name)) {
                if((method = llvm::dyn_cast<clang::CXXMethodDecl>(candidate))) {
                    break;
                }
            }
            if(!method) {
                break;
            }
            type = method->getReturnType();
            if(type.isNull()) {
                return {};
            }
        }
        if(!dereferenced) {
            return {};
        }
    }

    /// Inside the class's own definition `this` is the injected class name;
    /// unwrap it to the equivalent template specialization the lookup
    /// understands.
    if(auto* ICNT = type->getAs<clang::InjectedClassNameType>()) {
        type = ICNT->getInjectedSpecializationType();
    }

    PseudoInstantiator instantiator(context, resolved);
    return instantiator.lookup(type, name);
}

TemplateResolver::lookup_result
    TemplateResolver::lookup(const clang::CXXDependentScopeMemberExpr* expr) {
    return lookup_member(context,
                         resolved,
                         expr->getBaseType(),
                         expr->isArrow(),
                         expr->getMemberNameInfo().getName());
}

TemplateResolver::lookup_result TemplateResolver::lookup(const clang::UnresolvedMemberExpr* expr) {
    return lookup_member(context,
                         resolved,
                         expr->getBaseType(),
                         expr->isArrow(),
                         expr->getMemberName());
}

TemplateResolver::lookup_result
    TemplateResolver::lookup(const clang::DependentTemplateSpecializationType* type) {
    auto& template_name = type->getDependentTemplateName();
    auto name = template_name.getName();
    if(auto identifier = name.getIdentifier()) {
        return lookup(template_name.getQualifier(), identifier);
    }
    return lookup(template_name.getQualifier(),
                  context.DeclarationNames.getCXXOperatorName(name.getOperator()));
}

TemplateResolver::lookup_result TemplateResolver::lookup(const clang::UnresolvedLookupExpr* expr) {
    /// A qualified name resolves through its scope, which yields the full
    /// overload set from the named context.
    if(auto NNS = expr->getQualifier()) {
        if(auto members = lookup(NNS, expr->getName()); !members.empty()) {
            return members;
        }
    }

    /// TODO: Unqualified overload sets cannot be returned as a lookup_result
    /// (the candidates have no contiguous storage); fall back to the first
    /// template declaration.
    for(auto decl: expr->decls()) {
        if(auto TD = llvm::dyn_cast<clang::TemplateDecl>(decl)) {
            return lookup_result(TD);
        }
    }

    return {};
}

/// Can `FD` accept a call with `count` arguments? Default arguments lower the
/// minimum; C-style variadics and parameter packs lift the maximum.
static bool arity_viable(const clang::FunctionDecl* FD, unsigned count, bool member_call) {
    /// A member-syntax call does not spell the explicit object argument
    /// (`s.foo(1)` with `foo(this S&, int)`), but the declaration counts it.
    if(member_call) {
        if(auto method = llvm::dyn_cast<clang::CXXMethodDecl>(FD);
           method && method->isExplicitObjectMemberFunction()) {
            count += 1;
        }
    }
    if(count < FD->getMinRequiredArguments()) {
        return false;
    }
    if(count <= FD->getNumParams() || FD->isVariadic()) {
        return true;
    }
    return std::ranges::any_of(FD->parameters(), [](const clang::ParmVarDecl* param) {
        return param->isParameterPack();
    });
}

llvm::SmallVector<clang::NamedDecl*, 4> TemplateResolver::lookup(const clang::CallExpr* expr) {
    llvm::SmallVector<clang::NamedDecl*, 4> candidates;

    auto callee = expr->getCallee()->IgnoreParenImpCasts();
    bool member_call =
        llvm::isa<clang::UnresolvedMemberExpr, clang::CXXDependentScopeMemberExpr>(callee);
    if(auto OE = llvm::dyn_cast<clang::OverloadExpr>(callee)) {
        for(auto decl: OE->decls()) {
            candidates.push_back(decl);
        }
    } else if(auto DSME = llvm::dyn_cast<clang::CXXDependentScopeMemberExpr>(callee)) {
        for(auto decl: lookup(DSME)) {
            candidates.push_back(decl);
        }
    } else if(auto DSDRE = llvm::dyn_cast<clang::DependentScopeDeclRefExpr>(callee)) {
        for(auto decl: lookup(DSDRE)) {
            candidates.push_back(decl);
        }
    }

    /// An argument pack (`f(xs...)`) may instantiate to any number of
    /// arguments; fixed-arity filtering would remove viable overloads.
    bool has_pack_argument =
        std::ranges::any_of(expr->arguments(), [](const clang::Expr* argument) {
            return llvm::isa<clang::PackExpansionExpr>(argument);
        });
    if(has_pack_argument) {
        return candidates;
    }

    auto removed = std::ranges::remove_if(candidates, [&](clang::NamedDecl* decl) {
        auto target = decl;
        if(auto shadow = llvm::dyn_cast<clang::UsingShadowDecl>(target)) {
            target = shadow->getTargetDecl();
        }
        if(auto FTD = llvm::dyn_cast<clang::FunctionTemplateDecl>(target)) {
            target = FTD->getTemplatedDecl();
        }
        /// Non-function candidates (e.g. a callable object's variable) stay:
        /// arity says nothing about them.
        auto FD = llvm::dyn_cast<clang::FunctionDecl>(target);
        return FD && !arity_viable(FD, expr->getNumArgs(), member_call);
    });
    candidates.erase(removed.begin(), removed.end());
    return candidates;
}

}  // namespace clice
