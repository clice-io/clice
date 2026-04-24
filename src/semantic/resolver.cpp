#include "semantic/resolver.h"

#include <format>
#include <ranges>

#include "support/logging.h"

#include "clang/Sema/Template.h"
#include "clang/Sema/TemplateDeduction.h"
#include "clang/Sema/TreeTransform.h"

/// Template Resolver — pseudo-instantiation of dependent C++ types.
///
/// Architecture:
///   PseudoInstantiator (TreeTransform) — heuristic lookup in primary templates/partial specs
///     ├─ TransformDependentNameType       — lookup member in template, substitute, recurse
///     ├─ TransformDependentTemplateSPTType — resolve DTST via hole()/lookup, CTD→TST
///     ├─ TransformTemplateTypeParmType     — substitute from stack (+ default arg fallback)
///     ├─ TransformTypedefType              — delegate to SubstituteOnly (no lookup)
///     └─ TransformType                     — depth guard + null safety
///
///   SubstituteOnly (TreeTransform) — Phase 2: typedef expansion + param substitution only
///     Does NOT override TransformDependentNameType → no heuristic lookup → breaks cycles.
///
/// Key invariant: Phase 2 (SubstituteOnly) must NEVER trigger Phase 1 (heuristic lookup).
/// Violating this causes typedef ↔ lookup infinite cycles.
///
/// See docs: temp/template-resolver-analysis.md, temp/resolver-vector-pipeline.md

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

/// Resugar canonical TemplateTypeParmType with original parameter declarations.
class ResugarOnly : public clang::TreeTransform<ResugarOnly> {
public:
    ResugarOnly(clang::Sema& sema, clang::Decl* decl) :
        TreeTransform(sema), context(sema.getASTContext()) {
        visit_template_decl_contexts(decl,
                                     [&](clang::Decl* decl, clang::TemplateParameterList* params) {
                                         lists.push_back(params);
                                     });
        std::ranges::reverse(lists);
    }

    clang::QualType TransformTemplateTypeParmType(clang::TypeLocBuilder& TLB,
                                                  clang::TemplateTypeParmTypeLoc TL,
                                                  bool = false) {
        clang::QualType type = TL.getType();
        auto TTPT = TL.getTypePtr();
        if(!TTPT->getDecl()) {
            auto depth = TTPT->getDepth();
            if(depth >= lists.size()) {
                return TLB.push<clang::TemplateTypeParmTypeLoc>(type).getType();
            }
            auto index = TTPT->getIndex();
            auto isPack = TTPT->isParameterPack();
            auto param = llvm::cast<clang::TemplateTypeParmDecl>(lists[depth]->getParam(index));
            type = context.getTemplateTypeParmType(depth, index, isPack, param);
        }
        return TLB.push<clang::TemplateTypeParmTypeLoc>(type).getType();
    }

private:
    clang::ASTContext& context;
    llvm::SmallVector<clang::TemplateParameterList*> lists;
};

/// A helper class to record the instantiation stack.
struct InstantiationStack {
    using Arguments = llvm::SmallVector<clang::TemplateArgument, 4>;
    using TemplateArguments = llvm::ArrayRef<clang::TemplateArgument>;

    llvm::SmallVector<std::pair<clang::Decl*, Arguments>> data;

    bool empty() const {
        return data.empty();
    }

    void push(clang::Decl* decl, TemplateArguments arguments) {
        data.emplace_back(decl, arguments);
    }

    void pop() {
        data.pop_back();
    }

    auto& frames() {
        return data;
    }

    /// Look up a template type parameter in the stack by matching its depth and
    /// ownership against each frame. Searches from innermost (top) to outermost
    /// (bottom). Returns nullptr if no matching frame or index out of range.
    ///
    /// When the TTP has a declaration, ownership is verified: the frame's template
    /// parameter at the same index must be the exact same TemplateTypeParmDecl.
    /// This prevents depth collisions between unrelated templates at the same
    /// nesting level (e.g. __alloc_traits and vector both at depth 0).
    const clang::TemplateArgument* find_argument(const clang::TemplateTypeParmType* T) const {
        auto depth = T->getDepth();
        auto index = T->getIndex();
        auto* decl = T->getDecl();
        for(auto it = data.rbegin(); it != data.rend(); ++it) {
            clang::TemplateParameterList* params = nullptr;
            if(auto* CTD = llvm::dyn_cast<clang::ClassTemplateDecl>(it->first)) {
                params = CTD->getTemplateParameters();
            } else if(auto* CTPSD = llvm::dyn_cast<clang::ClassTemplatePartialSpecializationDecl>(
                          it->first)) {
                params = CTPSD->getTemplateParameters();
            } else if(auto* TATD = llvm::dyn_cast<clang::TypeAliasTemplateDecl>(it->first)) {
                params = TATD->getTemplateParameters();
            } else if(auto* FTD = llvm::dyn_cast<clang::FunctionTemplateDecl>(it->first)) {
                params = FTD->getTemplateParameters();
            }
            if(params && params->getDepth() == depth) {
                if(index >= it->second.size()) {
                    return nullptr;
                }
                // If the TTP has a declaration, verify it actually belongs to this
                // frame's template by checking the parameter list. Skip frames from
                // unrelated templates that happen to share the same depth.
                if(decl && index < params->size() && params->getParam(index) != decl) {
                    continue;
                }
                return &it->second[index];
            }
        }
        return nullptr;
    }
};

/// Helper to extract underlying type from a Decl.
static clang::QualType get_decl_type(clang::Decl* decl) {
    if(!decl)
        return clang::QualType();
    if(auto* TND = llvm::dyn_cast<clang::TypedefNameDecl>(decl))
        return TND->getUnderlyingType();
    if(auto* RD = llvm::dyn_cast<clang::RecordDecl>(decl))
        return decl->getASTContext().getTagType(clang::ElaboratedTypeKeyword::None,
                                                std::nullopt,
                                                RD,
                                                false);
    return clang::QualType();
}

/// Phase 2 substitution transform. Expands typedefs and substitutes template parameters
/// from the InstantiationStack, but does NOT override TransformDependentNameType.
///
/// This is critical: the base class TransformDependentNameType just substitutes params
/// in the qualifier and rebuilds the DependentNameType — it does NOT do our heuristic
/// lookup. This breaks the typedef ↔ lookup cycle that would occur if typedef expansion
/// triggered PseudoInstantiator's TransformDependentNameType.
///
/// Handles: TypedefType, ElaboratedType, InjectedClassNameType, alias TST, TTPT.
/// Does NOT handle: multi-element pack expansion, NTTP, template template params.
class SubstituteOnly : public clang::TreeTransform<SubstituteOnly> {
    using Base = clang::TreeTransform<SubstituteOnly>;

public:
    SubstituteOnly(clang::Sema& sema, InstantiationStack& stack) :
        Base(sema), context(sema.getASTContext()), stack(stack) {}

    using Base::TransformType;

    clang::QualType TransformType(clang::QualType type) {
        if(type.isNull() || !type->isDependentType()) {
            return type;
        }
        if(depth > 16) {
            return type;
        }
        ++depth;
        auto result = Base::TransformType(type);
        --depth;
        return result.isNull() ? type : result;
    }

    /// Desugar dependent typedefs to expose template parameters for substitution.
    clang::QualType TransformTypedefType(clang::TypeLocBuilder& TLB, clang::TypedefTypeLoc TL) {
        if(auto* TND = TL.getDecl()) {
            auto underlying = TND->getUnderlyingType();
            if(underlying->isDependentType()) {
                auto type = TransformType(underlying);
                if(!type.isNull()) {
                    TLB.pushTrivial(context, type, {});
                    return type;
                }
            }
        }
        return Base::TransformTypedefType(TLB, TL);
    }

    clang::QualType TransformInjectedClassNameType(clang::TypeLocBuilder& TLB,
                                                   clang::InjectedClassNameTypeLoc TL) {
        auto ICT = TL.getTypePtr();
        clang::QualType type = TransformType(ICT->desugar());
        if(type.isNull()) {
            return Base::TransformInjectedClassNameType(TLB, TL);
        }
        TLB.pushTrivial(context, type, {});
        return type;
    }

    using Base::TransformTemplateSpecializationType;

    clang::QualType TransformTemplateSpecializationType(clang::TypeLocBuilder& TLB,
                                                        clang::TemplateSpecializationTypeLoc TL) {
        if(TL.getTypePtr()->isTypeAlias()) {
            clang::QualType type = TransformType(TL.getTypePtr()->desugar());
            if(!type.isNull()) {
                TLB.pushTrivial(context, type, {});
                return type;
            }
        }
        return Base::TransformTemplateSpecializationType(TLB, TL);
    }

    /// Substitute template parameters from the stack.
    clang::QualType TransformTemplateTypeParmType(clang::TypeLocBuilder& TLB,
                                                  clang::TemplateTypeParmTypeLoc TL,
                                                  bool = false) {
        auto* T = TL.getTypePtr();

        if(auto* arg = stack.find_argument(T)) {
            clang::QualType type;

            if(arg->getKind() == clang::TemplateArgument::Type) {
                type = arg->getAsType();
            } else if(arg->getKind() == clang::TemplateArgument::Pack) {
                auto pack = arg->getPackAsArray();
                if(pack.size() == 1 && pack[0].getKind() == clang::TemplateArgument::Type) {
                    type = pack[0].getAsType();
                }
            }

            // TODO(pack): Only handles single-element packs (common pack forwarding case).
            // Multi-element packs (e.g. Us... = {int, float}) are not expanded here and
            // will fall through to return the original type.
            if(!type.isNull()) {
                TLB.pushTrivial(context, type, TL.getNameLoc());
                return type;
            }
        }

        // No substitution: return original type unchanged.
        TLB.push<clang::TemplateTypeParmTypeLoc>(TL.getType());
        return TL.getType();
    }

    // TransformDependentNameType is NOT overridden.
    // Base class behavior: transforms the qualifier (substitutes params there),
    // then rebuilds the DependentNameType. No lookup.

private:
    clang::ASTContext& context;
    InstantiationStack& stack;
    unsigned depth = 0;
};

/// The core pseudo-instantiation engine. Extends TreeTransform to resolve dependent
/// names by looking up members in primary templates and partial specializations —
/// a capability clang's own TemplateInstantiator does not have.
///
/// Resolution flow for `typename A<T>::type`:
///   1. TransformDependentNameType intercepts the DependentNameType
///   2. lookup(A<T>, "type") → deduce_template_arguments → find member decl
///   3. substitute(underlying_type) → SubstituteOnly expands typedefs + substitutes params
///   4. Pop lookup frames, then TransformType on result for further resolution
///
/// Uses SubstituteOnly for Phase 2 to avoid typedef ↔ lookup cycles.
/// Uses active_resolutions / active_ctd_lookups for cycle detection.
class PseudoInstantiator : public clang::TreeTransform<PseudoInstantiator> {
public:
    using Base = clang::TreeTransform<PseudoInstantiator>;

    using TemplateArguments = llvm::ArrayRef<clang::TemplateArgument>;

    using TemplateDeductionInfo = clang::sema::TemplateDeductionInfo;

    PseudoInstantiator(clang::Sema& sema,
                       llvm::DenseMap<const void*, clang::QualType>& resolved,
                       unsigned parent_indent = 0) :
        Base(sema), sema(sema), context(sema.getASTContext()), resolved(resolved),
        indent(parent_indent) {}

public:
    /// Use SubstituteOnly to expand typedefs and substitute parameters without doing lookup.
    clang::QualType substitute(clang::QualType type) {
        if(type.isNull() || !type->isDependentType()) {
            return type;
        }
        SubstituteOnly subst(sema, stack);
        auto result = subst.TransformType(type);
        return result.isNull() ? type : result;
    }

    /// Verify that `arguments` match `TD`'s parameter list, filling in default
    /// template arguments where needed. Default args are substituted using the
    /// current stack (via SubstituteOnly), so parameters already provided can
    /// appear in default expressions (e.g. `allocator<_Tp>` for vector's `_Alloc`).
    bool check_template_arguments(clang::TemplateDecl* TD,
                                  TemplateArguments& arguments,
                                  llvm::SmallVectorImpl<clang::TemplateArgument>& out) {
        auto list = TD->getTemplateParameters();
        out.reserve(list->size());
        for(auto arg: arguments) {
            out.emplace_back(arg);
        }

        if(out.size() != list->size()) {
            for(auto i = out.size(); i < list->size(); ++i) {
                auto param = list->getParam(i);
                // TODO(nttp): Only TemplateTypeParmDecl default arguments are handled.
                // NonTypeTemplateParmDecl and TemplateTemplateParmDecl defaults are skipped,
                // causing check_template_arguments to return false for templates like:
                //   template<typename T, int N = 0> struct S;
                auto TTPD = llvm::dyn_cast<clang::TemplateTypeParmDecl>(param);
                if(TTPD && TTPD->hasDefaultArgument()) {
                    auto type = TTPD->getDefaultArgument().getArgument().getAsType();

                    stack.push(TD, out);
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
                }
            }
        }

        if(out.size() != list->size()) {
            return false;
        }

        return true;
    }

    template <typename Decl>
    bool deduce_template_arguments(Decl* decl, TemplateArguments arguments) {
        clang::TemplateParameterList* list = nullptr;
        TemplateArguments params = {};

        if constexpr(std::is_same_v<Decl, clang::ClassTemplateDecl>) {
            const clang::ClassTemplateDecl* CTD = decl;
            list = CTD->getTemplateParameters();
            params = list->getInjectedTemplateArgs(context);
        } else if constexpr(std::is_same_v<Decl, clang::ClassTemplatePartialSpecializationDecl>) {
            const clang::ClassTemplatePartialSpecializationDecl* CTPSD = decl;
            list = CTPSD->getTemplateParameters();
            params = CTPSD->getTemplateArgs().asArray();
        } else if constexpr(std::is_same_v<Decl, clang::TypeAliasTemplateDecl>) {
            const clang::TypeAliasTemplateDecl* TATD = decl;
            list = TATD->getTemplateParameters();
            params = list->getInjectedTemplateArgs(context);
        } else {
            static_assert(dependent_false<Decl>, "Unknown declaration type");
        }

        assert(list && "No template parameters found");

        TemplateDeductionInfo info = {clang::SourceLocation(), list->getDepth()};
        llvm::SmallVector<clang::DeducedTemplateArgument, 4> deduced(list->size());

        auto result = sema.DeduceTemplateArguments(list, params, arguments, info, deduced, true);
        bool success =
            result == clang::TemplateDeductionResult::Success && !info.hasSFINAEDiagnostic();

        if(!success) {
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
                    auto args = params->getInjectedTemplateArgs(context);
                    stack.push(decl, args);
                });
            std::ranges::reverse(stack.frames());
        }

        llvm::SmallVector<clang::TemplateArgument, 4> output(deduced.begin(), deduced.end());
        stack.push(decl, output);

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
                for(unsigned j = 0; j < output.size(); ++j) {
                    if(j > 0)
                        mapping += ", ";
                    if(j < list->size()) {
                        mapping += list->getParam(j)->getNameAsString();
                        mapping += "=";
                    }
                    if(output[j].getKind() == clang::TemplateArgument::Type) {
                        mapping += "'";
                        mapping += output[j].getAsType().getAsString();
                        mapping += "'";
                    } else if(output[j].getKind() == clang::TemplateArgument::Pack)
                        mapping += "<pack>";
                    else
                        mapping += "<non-type>";
                }
                return mapping;
            }());

        return true;
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

    /// Look up `name` in the given type. First transforms the type (to substitute
    /// any template parameters in it), then extracts the ClassTemplateDecl or
    /// TypeAliasTemplateDecl from the resulting TST/DTST and dispatches to the
    /// appropriate lookup overload.
    lookup_result lookup(clang::QualType type, clang::DeclarationName name) {
        clang::Decl* TD = nullptr;
        llvm::ArrayRef<clang::TemplateArgument> args;

        // For concrete TSTs (non-dependent template name), extract template info directly
        // without calling TransformType. This avoids corrupting template arguments when
        // unrelated stack frames match by depth (e.g. __alloc_traits at depth 0 would
        // incorrectly substitute test's T which is also at depth 0).
        if(auto TST = type->getAs<clang::TemplateSpecializationType>()) {
            if(!TST->getTemplateName().getAsDependentTemplateName()) {
                TD = TST->getTemplateName().getAsTemplateDecl();
                args = TST->template_arguments();
            }
        }

        if(!TD) {
            type = TransformType(type);

            if(type.isNull()) {
                return lookup_result();
            }

            if(auto TST = type->getAs<clang::TemplateSpecializationType>()) {
                if(auto* DTN = TST->getTemplateName().getAsDependentTemplateName()) {
                    // If this dependent TST was already resolved (possibly to itself when
                    // unresolvable), skip the redundant lookup.
                    if(resolved.count(TST)) {
                        return lookup_result();
                    }

                    auto name = DTN->getName().getIdentifier();
                    if(!name) {
                        return {};
                    }

                    if(auto decl = preferred(lookup(DTN->getQualifier(), name))) {
                        TD = decl;
                        args = TST->template_arguments();
                    }
                } else {
                    TD = TST->getTemplateName().getAsTemplateDecl();
                    args = TST->template_arguments();
                }
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

    lookup_result lookup(clang::NestedNameSpecifier NNS, clang::DeclarationName name) {
        if(!NNS) {
            return lookup_result();
        }

        // In LLVM 22+, the old Identifier NNS kind is represented as a Type
        // (DependentNameType), so the Type case handles both old TypeSpec and
        // Identifier cases via lookup(QualType, name) → TransformDependentNameType.
        switch(NNS.getKind()) {
            case clang::NestedNameSpecifier::Kind::Type: {
                return lookup(clang::QualType(NNS.getAsType(), 0), name);
            }

            case clang::NestedNameSpecifier::Kind::Global:
            case clang::NestedNameSpecifier::Kind::Namespace:
            case clang::NestedNameSpecifier::Kind::MicrosoftSuper:
            case clang::NestedNameSpecifier::Kind::Null: {
                return {};
            }
        }

        return lookup_result();
    }

    /// Search for `name` in the dependent base classes of `CRD`. Each base type
    /// is substituted (to resolve template params in it) then looked up.
    ///
    /// IMPORTANT: when a member is found, stack frames pushed during the lookup
    /// are intentionally left intact. The caller (TransformDependentNameType)
    /// needs them to substitute the found decl's underlying type. The caller
    /// is responsible for popping frames after substitution.
    lookup_result lookup_in_bases(clang::CXXRecordDecl* CRD, clang::DeclarationName name) {
        if(!CRD->hasDefinition()) {
            return lookup_result();
        }

        for(auto base: CRD->bases()) {
            if(auto type = base.getType(); type->isDependentType()) {
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
        ++indent;
        for(auto partial: partials) {
            if(deduce_template_arguments(partial, arguments)) {
                LOG_DEBUG("{}" "matched partial '{}'", pad(), partial->getNameAsString());
                if(auto members = partial->lookup(name); !members.empty()) {
                    LOG_DEBUG("{}" "found in 'partial'", pad());
                    --indent;
                    return members;
                }

                if(auto members = lookup_in_bases(partial, name); !members.empty()) {
                    LOG_DEBUG("{}" "found in 'base'", pad());
                    --indent;
                    return members;
                }

                stack.pop();
            }
        }

        if(deduce_template_arguments(CTD, arguments)) {
            LOG_DEBUG("{}using primary template", pad());
            auto CRD = CTD->getTemplatedDecl();
            if(auto members = CRD->lookup(name); !members.empty()) {
                LOG_DEBUG("{}" "found in 'primary'", pad());
                --indent;
                return members;
            }

            if(auto members = lookup_in_bases(CRD, name); !members.empty()) {
                LOG_DEBUG("{}" "found in 'base'", pad());
                --indent;
                return members;
            }

            stack.pop();
        }

        --indent;
        return lookup_result();
    }

    /// Short-circuit resolution for `std::allocator_traits::rebind_alloc`.
    ///
    /// libstdc++'s allocator rebind chain (vector → __alloc_traits → allocator_traits →
    /// allocator::rebind) creates deeply nested dependent types that are hard to resolve
    /// generically. This function intercepts `allocator_traits<Alloc>::rebind_alloc<T>`
    /// and attempts direct resolution.
    ///
    /// Strategy:
    ///   1. Try Alloc::rebind<T>::other (the standard allocator rebind protocol)
    ///   2. If that fails (e.g. C++20 removed allocator::rebind), fall back to
    ///      replacing the first template argument: allocator<U> → allocator<T>
    ///
    /// TODO: Replace with a general mechanism for resolving well-known standard
    /// library patterns, or improve the resolver to handle these chains naturally.
    clang::QualType hole(clang::NestedNameSpecifier NNS,
                         const clang::IdentifierInfo* member,
                         TemplateArguments arguments) {
        if(NNS.getKind() != clang::NestedNameSpecifier::Kind::Type) {
            return clang::QualType();
        }

        auto TST = NNS.getAsType()->getAs<clang::TemplateSpecializationType>();
        if(!TST) {
            return clang::QualType();
        }

        auto TD = TST->getTemplateName().getAsTemplateDecl();
        if(!TD)
            return clang::QualType();
        if(!TD->getDeclContext()->isStdNamespace()) {
            return clang::QualType();
        }

        if(TD->getName() == "allocator_traits") {
            if(TST->template_arguments().size() != 1) {
                return clang::QualType();
            }
            auto Alloc = TST->template_arguments()[0].getAsType();

            if(member->getName() == "rebind_alloc") {
                if(arguments.empty())
                    return clang::QualType();
                auto T = arguments[0].getAsType();

                clang::NestedNameSpecifier prefix(Alloc.getTypePtr());

                auto rebind = sema.getPreprocessor().getIdentifierInfo("rebind");

                auto DTN = context.getDependentTemplateName(
                    clang::DependentTemplateStorage(prefix, rebind, false));
                auto TST = context.getTemplateSpecializationType(clang::ElaboratedTypeKeyword::None,
                                                                 DTN,
                                                                 arguments,
                                                                 {});

                prefix = clang::NestedNameSpecifier(TST.getTypePtr());

                auto other = sema.getPreprocessor().getIdentifierInfo("other");
                auto DNT = context.getDependentNameType(clang::ElaboratedTypeKeyword::Typename,
                                                        prefix,
                                                        other);

                auto result = PseudoInstantiator(sema, resolved, indent).TransformType(DNT);
                if(!result.isNull() && !result->isDependentType()) {
                    LOG_DEBUG(
                        "{}" "hole: 'allocator_traits::rebind_alloc' → '{}'",
                        pad(),
                        result.getAsString());
                    return result;
                }

                if(auto TST = Alloc->getAs<clang::TemplateSpecializationType>()) {
                    llvm::SmallVector<clang::TemplateArgument, 1> replaceArguments = {T};
                    llvm::SmallVector<clang::TemplateArgument, 1> canonicalArguments;
                    for(auto& arg: replaceArguments) {
                        canonicalArguments.emplace_back(context.getCanonicalTemplateArgument(arg));
                    }
                    auto result =
                        context.getTemplateSpecializationType(clang::ElaboratedTypeKeyword::None,
                                                              TST->getTemplateName(),
                                                              replaceArguments,
                                                              canonicalArguments);
                    LOG_DEBUG(
                        "{}" "hole: 'allocator_traits::rebind_alloc' → '{}'",
                        pad(),
                        result.getAsString());
                    return result;
                }
            }
        }

        return clang::QualType();
    }

public:
    using Base::TransformType;

    /// Entry point for all type transformations. Guards against:
    /// - Null types (return as-is)
    /// - Non-dependent types (no transformation needed)
    /// - Excessive recursion depth (bail out to prevent stack overflow)
    /// - Null results from base transform (return original type instead)
    clang::QualType TransformType(clang::QualType type) {
        if(type.isNull() || !type->isDependentType()) {
            return type;
        }
        if(depth > 16) {
            return type;
        }
        ++depth;
        auto result = Base::TransformType(type);
        --depth;
        if(result.isNull()) {
            return type;
        }
        return result;
    }

    clang::QualType TransformTemplateTypeParmType(clang::TypeLocBuilder& TLB,
                                                  clang::TemplateTypeParmTypeLoc TL,
                                                  bool = false) {
        auto* T = TL.getTypePtr();

        // First, try to find a substitution in the instantiation stack.
        if(auto* arg = stack.find_argument(T)) {
            clang::QualType type;

            if(arg->getKind() == clang::TemplateArgument::Type) {
                type = arg->getAsType();
            } else if(arg->getKind() == clang::TemplateArgument::Pack) {
                auto pack = arg->getPackAsArray();
                if(pack.size() == 1 && pack[0].getKind() == clang::TemplateArgument::Type) {
                    type = pack[0].getAsType();
                }
            }

            if(!type.isNull()) {
                TLB.pushTrivial(context, type, TL.getNameLoc());
                return type;
            }

            TLB.push<clang::TemplateTypeParmTypeLoc>(TL.getType());
            return TL.getType();
        }

        // No stack substitution available. Fall back to using the parameter's
        // default argument if one exists. This enables resolution chains like:
        //   template<typename T, typename Alloc = allocator<T>> struct vector;
        // where Alloc's default depends on T.
        if(clang::TemplateTypeParmDecl* TTPD = TL.getDecl()) {
            if(TTPD->hasDefaultArgument()) {
                const clang::TemplateArgument& argument = TTPD->getDefaultArgument().getArgument();
                if(argument.getKind() == clang::TemplateArgument::Type) {
                    clang::QualType type = TransformType(argument.getAsType());
                    if(!type.isNull()) {
                        TLB.pushTrivial(context, type, clang::SourceLocation());
                        return type;
                    }
                }
            }
        }

        TLB.push<clang::TemplateTypeParmTypeLoc>(TL.getType());
        return TL.getType();
    }

    /// Push frames for outer templates in a DependentTemplateName qualifier chain.
    /// For `A<X>::template B<Y>::template C<Z>::type`, walking the chain from the
    /// DependentNameType's NNS pushes frames for A (T1→X) and B (T2→Y) so that
    /// when "type" is found in C<Z>, the substitute step correctly maps outer
    /// template parameters to the actual arguments from the NNS chain.
    void push_nns_qualifier_frames(clang::NestedNameSpecifier NNS) {
        if(!NNS || NNS.getKind() != clang::NestedNameSpecifier::Kind::Type)
            return;

        auto* TST = NNS.getAsType()->getAs<clang::TemplateSpecializationType>();
        if(!TST)
            return;

        if(auto* DTN = TST->getTemplateName().getAsDependentTemplateName()) {
            push_nns_qualifier_frames(DTN->getQualifier());

            if(auto it = resolved.find(TST); it != resolved.end()) {
                if(auto* rTST = it->second->getAs<clang::TemplateSpecializationType>()) {
                    if(auto* TD = rTST->getTemplateName().getAsTemplateDecl()) {
                        stack.push(TD, TST->template_arguments());
                    }
                }
            }
        } else if(auto* TD = TST->getTemplateName().getAsTemplateDecl()) {
            stack.push(TD, TST->template_arguments());
        }
    }

    clang::QualType TransformDependentNameType(clang::TypeLocBuilder& TLB,
                                               clang::DependentNameTypeLoc TL,
                                               bool DeducedTSTContext = false,
                                               clang::QualType ObjectType = {},
                                               clang::NamedDecl* UnqualLookup = nullptr) {
        auto* DNT = TL.getTypePtr();
        LOG_DEBUG("{}" "resolve '{}'", pad(), clang::QualType(DNT, 0).getAsString());
        ++indent;

        // Check cache.
        if(auto iter = resolved.find(DNT); iter != resolved.end()) {
            LOG_DEBUG("{}" "→ '{}' (cached)", pad(), iter->second.getAsString());
            --indent;
            TLB.pushTrivial(context, iter->second, {});
            return iter->second;
        }

        // Cycle detection: if we're already resolving this DNT, bail out.
        if(!active_resolutions.insert(DNT).second) {
            LOG_DEBUG("{}→ <cycle detected, returning original>", pad());
            --indent;
            auto original = clang::QualType(DNT, 0);
            auto NewTL = TLB.push<clang::DependentNameTypeLoc>(original);
            NewTL.setElaboratedKeywordLoc(TL.getElaboratedKeywordLoc());
            NewTL.setQualifierLoc(TL.getQualifierLoc());
            NewTL.setNameLoc(TL.getNameLoc());
            return original;
        }

        // Save entry stack size to clean up any frames leaked by nested operations
        // (NNS transform, push_nns_qualifier_frames, lookup, TransformType).
        auto entry_stack_size = stack.data.size();

        auto NNSLoc = TransformNestedNameSpecifierLoc(TL.getQualifierLoc());
        if(!NNSLoc) {
            while(stack.data.size() > entry_stack_size)
                stack.pop();
            active_resolutions.erase(DNT);
            LOG_DEBUG("{}→ <unresolved>", pad());
            --indent;
            auto original = clang::QualType(DNT, 0);
            auto NewTL = TLB.push<clang::DependentNameTypeLoc>(original);
            NewTL.setElaboratedKeywordLoc(TL.getElaboratedKeywordLoc());
            NewTL.setQualifierLoc(TL.getQualifierLoc());
            NewTL.setNameLoc(TL.getNameLoc());
            return original;
        }

        auto NNS = NNSLoc.getNestedNameSpecifier();

        // For nested dependent template specializations like A<X>::B<Y>::C<Z>::type,
        // push frames for outer templates (A, B) using args from the original NNS
        // chain. Without this, the lookup fabricates self-referential injected args
        // for outer templates, leaving their parameters unresolved.
        auto origNNS = DNT->getQualifier();
        if(origNNS.getKind() == clang::NestedNameSpecifier::Kind::Type) {
            if(auto* TST = origNNS.getAsType()->getAs<clang::TemplateSpecializationType>()) {
                if(auto* DTN = TST->getTemplateName().getAsDependentTemplateName()) {
                    push_nns_qualifier_frames(DTN->getQualifier());
                }
            }
        }

        auto stack_size = stack.data.size();
        auto* decl = preferred(lookup(NNS, DNT->getIdentifier()));
        auto type = get_decl_type(decl);

        clang::QualType result;
        if(!type.isNull()) {
            if(decl) {
                const char* decl_kind = "decl";
                if(llvm::isa<clang::TypedefNameDecl>(decl))
                    decl_kind = "typedef";
                else if(llvm::isa<clang::RecordDecl>(decl))
                    decl_kind = "record";
                auto decl_name = llvm::dyn_cast<clang::NamedDecl>(decl)
                                     ? llvm::dyn_cast<clang::NamedDecl>(decl)->getNameAsString()
                                     : "?";
                LOG_DEBUG(
                    "{}" "found {} '{}' = '{}'",
                    pad(),
                    decl_kind,
                    decl_name,
                    type.getAsString());
            }

            // Step 1: substitute params (expand typedefs, no lookup).
            result = substitute(type);
            LOG_DEBUG("{}" "substitute → '{}'", pad(), result.getAsString());

            // Pop lookup frames BEFORE further resolution. The substitute step already
            // used the full stack for parameter substitution. TransformType should only
            // see the outer context to avoid polluting free variables (e.g. T) with
            // mappings from intermediate lookup frames.
            while(stack.data.size() > stack_size) {
                stack.pop();
            }

            // Step 2: if still dependent, do full transform (may trigger more lookups).
            if(!result.isNull() && result->isDependentType()) {
                result = TransformType(result);
            }
        } else {
            while(stack.data.size() > stack_size) {
                stack.pop();
            }
        }

        // Clean up all frames pushed during this resolution (push_nns_qualifier_frames,
        // nested lookups via TransformType, etc.) to prevent leaking into sibling calls.
        while(stack.data.size() > entry_stack_size)
            stack.pop();

        active_resolutions.erase(DNT);

        if(!result.isNull()) {
            LOG_DEBUG("{}" "→ '{}'", pad(), result.getAsString());
            --indent;
            resolved.try_emplace(DNT, result);
            TLB.pushTrivial(context, result, {});
            return result;
        }

        LOG_DEBUG("{}→ <unresolved>", pad());
        --indent;
        auto original = clang::QualType(DNT, 0);
        auto NewTL = TLB.push<clang::DependentNameTypeLoc>(original);
        NewTL.setElaboratedKeywordLoc(TL.getElaboratedKeywordLoc());
        NewTL.setQualifierLoc(TL.getQualifierLoc());
        NewTL.setNameLoc(TL.getNameLoc());
        return original;
    }

    clang::QualType rebuild_tst(clang::TypeLocBuilder& TLB,
                                clang::TemplateSpecializationTypeLoc TL) {
        auto* TST = TL.getTypePtr();
        return TLB.push<clang::TemplateSpecializationTypeLoc>(clang::QualType(TST, 0)).getType();
    }

    clang::QualType
        TransformTemplateSpecializationType(clang::TypeLocBuilder& TLB,
                                            clang::TemplateSpecializationTypeLoc TL,
                                            clang::QualType ObjectType = {},
                                            clang::NamedDecl* FirstQualifierInScope = nullptr,
                                            bool AllowInjectedClassName = false) {
        auto* TST = TL.getTypePtr();
        auto* DTN = TST->getTemplateName().getAsDependentTemplateName();
        if(!DTN) {
            return Base::TransformTemplateSpecializationType(TLB,
                                                             TL,
                                                             ObjectType,
                                                             FirstQualifierInScope,
                                                             AllowInjectedClassName);
        }

        LOG_DEBUG("{}" "resolve dependent TST '{}'", pad(), clang::QualType(TST, 0).getAsString());
        ++indent;

        if(auto iter = resolved.find(TST); iter != resolved.end()) {
            --indent;
            TLB.pushTrivial(context, iter->second, {});
            return iter->second;
        }

        // Save stack state to clean up frames pushed by NNS/lookup side effects.
        auto stack_size = stack.data.size();

        auto NNSLoc = TransformNestedNameSpecifierLoc(TL.getQualifierLoc());
        if(!NNSLoc) {
            while(stack.data.size() > stack_size)
                stack.pop();
            LOG_DEBUG("{}→ <unresolved dependent TST>", pad());
            --indent;
            return rebuild_tst(TLB, TL);
        }
        auto NNS = NNSLoc.getNestedNameSpecifier();

        clang::TemplateArgumentListInfo info;
        using iterator =
            clang::TemplateArgumentLocContainerIterator<clang::TemplateSpecializationTypeLoc>;
        if(TransformTemplateArguments(iterator(TL, 0), iterator(TL, TL.getNumArgs()), info)) {
            LOG_DEBUG("{}→ <unresolved dependent TST>", pad());
            --indent;
            return rebuild_tst(TLB, TL);
        }

        llvm::SmallVector<clang::TemplateArgument, 4> arguments;
        for(auto& arg: info.arguments()) {
            arguments.push_back(arg.getArgument());
        }

        auto* name = DTN->getName().getIdentifier();
        if(!name) {
            LOG_DEBUG("{}→ <unresolved dependent TST>", pad());
            --indent;
            return rebuild_tst(TLB, TL);
        }

        if(auto result = hole(NNS, name, arguments); !result.isNull()) {
            LOG_DEBUG("{}" "hole: '{}' → '{}'", pad(), name->getName().str(), result.getAsString());
            --indent;
            resolved.try_emplace(TST, result);
            TLB.pushTrivial(context, result, {});
            return result;
        }

        if(auto* decl = preferred(lookup(NNS, name))) {
            if(auto* TATD = llvm::dyn_cast<clang::TypeAliasTemplateDecl>(decl)) {
                if(deduce_template_arguments(TATD, arguments)) {
                    auto type = substitute(TATD->getTemplatedDecl()->getUnderlyingType());
                    // Pop lookup frames before further resolution.
                    while(stack.data.size() > stack_size) {
                        stack.pop();
                    }
                    if(!type.isNull() && type->isDependentType()) {
                        type = TransformType(type);
                    }
                    if(!type.isNull()) {
                        LOG_DEBUG("{}" "→ '{}' (alias)", pad(), type.getAsString());
                        --indent;
                        resolved.try_emplace(TST, type);
                        TLB.pushTrivial(context, type, {});
                        return type;
                    }
                }
            } else if(auto* CTD = llvm::dyn_cast<clang::ClassTemplateDecl>(decl)) {
                // Pop lookup frames — we only needed them to find the CTD.
                while(stack.data.size() > stack_size) {
                    stack.pop();
                }
                clang::TemplateName TN(CTD);
                llvm::SmallVector<clang::TemplateArgument> canonArgs;
                for(auto& arg: arguments) {
                    canonArgs.push_back(context.getCanonicalTemplateArgument(arg));
                }
                auto result =
                    context.getTemplateSpecializationType(clang::ElaboratedTypeKeyword::None,
                                                          TN,
                                                          arguments,
                                                          canonArgs);
                LOG_DEBUG("{}" "→ TST '{}' (class)", pad(), result.getAsString());
                --indent;
                resolved.try_emplace(TST, result);
                TLB.pushTrivial(context, result, {});
                return result;
            }
        }
        while(stack.data.size() > stack_size) {
            stack.pop();
        }

        LOG_DEBUG("{}→ <unresolved dependent TST>", pad());
        --indent;
        auto fallback = rebuild_tst(TLB, TL);
        resolved.try_emplace(TST, fallback);
        return fallback;
    }

    /// Desugar dependent typedefs by delegating to SubstituteOnly.
    /// This is called by PseudoInstantiator (not by SubstituteOnly itself, which has
    /// its own TransformTypedefType). Using substitute() here ensures that typedef
    /// expansion does NOT trigger heuristic lookup, preventing the typedef ↔ lookup cycle.
    clang::QualType TransformTypedefType(clang::TypeLocBuilder& TLB, clang::TypedefTypeLoc TL) {
        if(auto* TND = TL.getDecl()) {
            auto underlying = TND->getUnderlyingType();
            if(underlying->isDependentType()) {
                auto type = substitute(underlying);
                if(!type.isNull()) {
                    TLB.pushTrivial(context, type, {});
                    return type;
                }
            }
        }
        return Base::TransformTypedefType(TLB, TL);
    }

    /// Attempt to resolve decltype expressions that reference variables.
    /// Only handles the simple case of `decltype(var)` where `var` is a VarDecl.
    /// TODO: Handle more complex decltype expressions (member access, function calls, etc.)
    clang::QualType TransformDecltypeType(clang::TypeLocBuilder& TLB, clang::DecltypeTypeLoc TL) {
        auto expr = TL.getTypePtr()->getUnderlyingExpr();
        if(auto DRE = llvm::dyn_cast<clang::DeclRefExpr>(expr)) {
            if(auto decl = DRE->getDecl(); llvm::isa<clang::VarDecl>(decl)) {
                auto type = TransformType(decl->getType());
                if(!type.isNull()) {
                    TLB.pushTrivial(context, type, {});
                    return type;
                }
            }
        }

        return Base::TransformDecltypeType(TLB, TL);
    }

    // --- State ---

private:
    clang::Sema& sema;
    clang::ASTContext& context;
    InstantiationStack stack;
    llvm::DenseMap<const void*, clang::QualType>& resolved;
    llvm::SmallPtrSet<const void*, 8> active_resolutions;
    llvm::DenseSet<std::pair<const void*, void*>> active_ctd_lookups;
    unsigned depth = 0;
    unsigned indent = 0;

    std::string pad() const {
        return std::string(indent * 2, ' ');
    }
};

}  // namespace

clang::QualType TemplateResolver::resolve(clang::QualType type) {
    PseudoInstantiator instantiator(sema, resolved);
    return instantiator.TransformType(type);
}

clang::QualType TemplateResolver::resugar(clang::QualType type, clang::Decl* decl) {
    ResugarOnly resugar(sema, decl);
    return resugar.TransformType(type);
}

TemplateResolver::lookup_result TemplateResolver::lookup(clang::NestedNameSpecifier NNS,
                                                         clang::DeclarationName name) {
    PseudoInstantiator instantiator(sema, resolved);
    return instantiator.lookup(NNS, name);
}

}  // namespace clice
