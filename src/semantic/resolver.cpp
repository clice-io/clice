#include "semantic/resolver.h"

#include <ranges>

#include "semantic/unifier.h"
#include "support/logging.h"

#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/DeclTemplate.h"

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
    const clang::TemplateArgument* find_argument(const clang::NamedDecl* decl,
                                                 unsigned depth,
                                                 unsigned index) const {
        if(decl) {
            for(const auto& frame: std::ranges::reverse_view(data)) {
                if(frame.params && index < frame.params->size() &&
                   frame.params->getParam(index) == decl) {
                    if(index < frame.arguments.size()) {
                        return &frame.arguments[index];
                    }
                    return nullptr;
                }
            }
            return nullptr;
        }

        for(const auto& frame: std::ranges::reverse_view(data)) {
            if(frame.params && frame.params->getDepth() == depth) {
                if(index < frame.arguments.size()) {
                    return &frame.arguments[index];
                }
                return nullptr;
            }
        }
        return nullptr;
    }

    const clang::TemplateArgument* find_argument(const clang::TemplateTypeParmType* T) const {
        return find_argument(T->getDecl(), T->getDepth(), T->getIndex());
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

                stack.push(TD, list, out);
                auto result = substitute(type);
                stack.pop();

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
                break;
            }

            if(auto TTPD = llvm::dyn_cast<clang::TemplateTemplateParmDecl>(param);
               TTPD && TTPD->hasDefaultArgument()) {
                out.emplace_back(TTPD->getDefaultArgument().getArgument());
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
            if(resolved.count(DTST)) {
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

        if(auto iter = resolved.find(NNS); iter != resolved.end()) {
            return lookup(iter->second, name);
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
                    resolved.try_emplace(NNS, type);
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
                if(!best || more_specialized(context, partial, best)) {
                    best = partial;
                }
            }
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
                result = context.getPointerType(pointee);
                break;
            }

            case clang::Type::LValueReference: {
                auto pointee =
                    rewrite(llvm::cast<clang::ReferenceType>(T)->getPointeeType(), policy);
                result = context.getLValueReferenceType(pointee);
                break;
            }

            case clang::Type::RValueReference: {
                auto pointee =
                    rewrite(llvm::cast<clang::ReferenceType>(T)->getPointeeType(), policy);
                result = context.getRValueReferenceType(pointee);
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

                /// `T[N]` with a known N collapses to a constant array.
                if(auto NTTP = referenced_nttp(DSAT->getSizeExpr())) {
                    if(auto* argument =
                           stack.find_argument(NTTP, NTTP->getDepth(), NTTP->getIndex());
                       argument && argument->getKind() == clang::TemplateArgument::Integral) {
                        result = context.getConstantArrayType(element,
                                                              argument->getAsIntegral(),
                                                              nullptr,
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
                for(auto param: FPT->getParamTypes()) {
                    auto rewritten = rewrite(param, policy);
                    changed |= rewritten != param;
                    params.push_back(rewritten);
                }
                if(changed) {
                    result = context.getFunctionType(ret, params, FPT->getExtProtoInfo());
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
        if(quals.hasQualifiers()) {
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
        llvm::SmallVector<clang::TemplateArgument, 4> canonical;

        clang::TemplateParameterList* params = nullptr;
        if(auto TD = name.getAsTemplateDecl()) {
            params = TD->getTemplateParameters();
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
                    /// Substitute a bound non-type parameter at the argument
                    /// level; expressions themselves are never rebuilt.
                    if(auto NTTP = referenced_nttp(argument.getAsExpr())) {
                        auto* bound = stack.find_argument(NTTP, NTTP->getDepth(), NTTP->getIndex());
                        if(bound && !bound->isNull()) {
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
                       scope_lacks(DNT->getQualifier(), DNT->getIdentifier());
            }

            case clang::Type::DependentTemplateSpecialization: {
                auto DTST = llvm::cast<clang::DependentTemplateSpecializationType>(T);
                auto& template_name = DTST->getDependentTemplateName();
                auto identifier = template_name.getName().getIdentifier();
                auto qualifier = template_name.getQualifier();
                if(specifier_absent(qualifier, guard)) {
                    return true;
                }
                return identifier && scope_lacks(qualifier, identifier);
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
                    return identifier && scope_lacks(scope, identifier);
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
    bool scope_lacks(const clang::NestedNameSpecifier* scope, clang::DeclarationName name) {
        if(!scope) {
            return false;
        }

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
                    if(llvm::isa_and_nonnull<clang::ClassTemplateDecl>(
                           TST->getTemplateName().getAsTemplateDecl())) {
                        lacks = lookup(type, name).empty();
                    }
                } else if(auto RD = type->getAsCXXRecordDecl()) {
                    lacks = RD->lookup(name).empty() && lookup_in_bases(RD, name).empty();
                }
            }
        }

        if(ctd_guard_tripped || steps > step_budget) {
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
        if(auto iter = resolved.find(DNT); iter != resolved.end()) {
            LOG_DEBUG("{}" "→ '{}' (cached)", pad(), iter->second.getAsString());
            indent -= 1;
            return iter->second;
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
            resolved.try_emplace(DNT, result);
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
        bool cacheable = template_name.getQualifier() != nullptr || !scope;

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
                        if(cacheable) {
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
                if(cacheable) {
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
        if(cacheable) {
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
    bool ctd_guard_tripped = false;

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
        /// pointer appears; bounded, cycles just stop resolving.
        auto arrow_name = context.DeclarationNames.getCXXOperatorName(clang::OO_Arrow);
        for(unsigned hop = 0; hop < 8; hop += 1) {
            if(auto* PT = type->getAs<clang::PointerType>()) {
                type = PT->getPointeeType();
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
static bool arity_viable(const clang::FunctionDecl* FD, unsigned count) {
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
        return FD && !arity_viable(FD, expr->getNumArgs());
    });
    candidates.erase(removed.begin(), removed.end());
    return candidates;
}

}  // namespace clice
