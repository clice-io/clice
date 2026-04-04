#include "semantic/resolver.h"

#include <format>
#include <ranges>

#include "support/logging.h"

#include "clang/Sema/Template.h"
#include "clang/Sema/TemplateDeduction.h"
#include "clang/Sema/TreeTransform.h"

namespace clice {

namespace {

template <typename T>
constexpr inline bool dependent_false = false;

template <typename Callback>
void visitTemplateDeclContexts(clang::Decl* decl, const Callback& callback) {
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
    }
}

/// Resugar canonical TemplateTypeParmType with original parameter declarations.
class ResugarOnly : public clang::TreeTransform<ResugarOnly> {
public:
    ResugarOnly(clang::Sema& sema, clang::Decl* decl) :
        TreeTransform(sema), context(sema.getASTContext()) {
        visitTemplateDeclContexts(decl,
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

    /// Look up a TemplateTypeParmType by matching depth against stack frames.
    const clang::TemplateArgument* findArgument(const clang::TemplateTypeParmType* T) const {
        auto depth = T->getDepth();
        auto index = T->getIndex();
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
                if(index < it->second.size()) {
                    return &it->second[index];
                }
                return nullptr;
            }
        }
        return nullptr;
    }
};

/// Helper to extract underlying type from a Decl.
static clang::QualType getDeclType(clang::Decl* decl) {
    if(!decl)
        return clang::QualType();
    if(auto* TND = llvm::dyn_cast<clang::TypedefNameDecl>(decl))
        return TND->getUnderlyingType();
    if(auto* RD = llvm::dyn_cast<clang::RecordDecl>(decl))
        return clang::QualType(RD->getTypeForDecl(), 0);
    return clang::QualType();
}

/// Substitution-only transform: expands typedefs and substitutes template parameters
/// from the InstantiationStack, but does NOT do lookup on DependentNameTypes.
/// This breaks the typedef <-> lookup cycle.
class SubstituteOnly : public clang::TreeTransform<SubstituteOnly> {
    using Base = clang::TreeTransform<SubstituteOnly>;

public:
    SubstituteOnly(clang::Sema& sema, InstantiationStack& stack) :
        Base(sema), context(sema.getASTContext()), stack(stack) {}

    /// Desugar dependent typedefs to expose template parameters for substitution.
    clang::QualType TransformTypedefType(clang::TypeLocBuilder& TLB, clang::TypedefTypeLoc TL) {
        if(auto* TND = TL.getTypedefNameDecl()) {
            auto underlying = TND->getUnderlyingType();
            if(underlying->isDependentType()) {
                auto type = TransformType(underlying);
                if(!type.isNull()) {
                    if(auto ET = llvm::dyn_cast<clang::ElaboratedType>(type)) {
                        type = ET->getNamedType();
                    }
                    TLB.pushTrivial(context, type, {});
                    return type;
                }
            }
        }
        return Base::TransformTypedefType(TLB, TL);
    }

    clang::QualType TransformElaboratedType(clang::TypeLocBuilder& TLB,
                                            clang::ElaboratedTypeLoc TL) {
        clang::QualType type = TransformType(TL.getNamedTypeLoc().getType());
        if(type.isNull()) {
            return Base::TransformElaboratedType(TLB, TL);
        }
        TLB.pushTrivial(context, type, {});
        return type;
    }

    clang::QualType TransformInjectedClassNameType(clang::TypeLocBuilder& TLB,
                                                   clang::InjectedClassNameTypeLoc TL) {
        auto ICT = TL.getTypePtr();
        clang::QualType type = TransformType(ICT->getInjectedSpecializationType());
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

        if(auto* arg = stack.findArgument(T)) {
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
};

/// The core class that performs pseudo template instantiation.
/// Uses SubstituteOnly for typedef expansion/parameter substitution,
/// and does lookup only in TransformDependentNameType/TransformDependentTemplateSpecializationType.
class PseudoInstantiator : public clang::TreeTransform<PseudoInstantiator> {
public:
    using Base = clang::TreeTransform<PseudoInstantiator>;

    using TemplateArguments = llvm::ArrayRef<clang::TemplateArgument>;

    using TemplateDeductionInfo = clang::sema::TemplateDeductionInfo;

    PseudoInstantiator(clang::Sema& sema,
                       llvm::DenseMap<const void*, clang::QualType>& resolved,
                       unsigned parentIndent = 0) :
        Base(sema), sema(sema), context(sema.getASTContext()), resolved(resolved),
        indent(parentIndent) {}

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

    bool checkTemplateArguments(clang::TemplateDecl* TD,
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
    bool deduceTemplateArguments(Decl* decl, TemplateArguments arguments) {
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

        /// made up class template context.
        if(stack.empty()) {
            visitTemplateDeclContexts(llvm::dyn_cast<clang::Decl>(decl->getDeclContext()),
                                      [&](clang::Decl* decl, clang::TemplateParameterList* params) {
                                          stack.push(decl,
                                                     params->getInjectedTemplateArgs(context));
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

    clang::Decl* preferred(lookup_result members) {
        clang::Decl* decl = nullptr;
        std::ranges::for_each(members, [&](auto member) { decl = member; });
        return decl;
    }

    lookup_result lookup(clang::QualType type, clang::DeclarationName name) {
        clang::Decl* TD = nullptr;
        llvm::ArrayRef<clang::TemplateArgument> args;
        type = TransformType(type);

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
            if(deduceTemplateArguments(TATD, args)) {
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

        switch(NNS->getKind()) {
            case clang::NestedNameSpecifier::Identifier: {
                auto stackSize = stack.data.size();
                auto* decl = preferred(lookup(NNS->getPrefix(), NNS->getAsIdentifier()));
                auto type = getDeclType(decl);
                if(!type.isNull()) {
                    type = substitute(type);
                }
                while(stack.data.size() > stackSize) {
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

            case clang::NestedNameSpecifier::Global:
            case clang::NestedNameSpecifier::Namespace:
            case clang::NestedNameSpecifier::NamespaceAlias:
            case clang::NestedNameSpecifier::Super: {
                return {};
            }
        }

        return lookup_result();
    }

    lookup_result lookupInBases(clang::CXXRecordDecl* CRD, clang::DeclarationName name) {
        if(!CRD->hasDefinition()) {
            return lookup_result();
        }

        for(auto base: CRD->bases()) {
            if(auto type = base.getType(); type->isDependentType()) {
                auto stackSize = stack.data.size();
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
                while(stack.data.size() > stackSize) {
                    stack.pop();
                }
            }
        }

        return lookup_result();
    }

    lookup_result lookup(clang::ClassTemplateDecl* CTD,
                         clang::DeclarationName name,
                         TemplateArguments visibleArguments) {
        llvm::SmallVector<clang::TemplateArgument, 4> arguments;
        if(!checkTemplateArguments(CTD, visibleArguments, arguments)) {
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
            if(deduceTemplateArguments(partial, arguments)) {
                LOG_DEBUG("{}" "matched partial '{}'", pad(), partial->getNameAsString());
                if(auto members = partial->lookup(name); !members.empty()) {
                    LOG_DEBUG("{}" "found in 'partial'", pad());
                    --indent;
                    return members;
                }

                if(auto members = lookupInBases(partial, name); !members.empty()) {
                    LOG_DEBUG("{}" "found in 'base'", pad());
                    --indent;
                    return members;
                }

                stack.pop();
            }
        }

        if(deduceTemplateArguments(CTD, arguments)) {
            LOG_DEBUG("{}using primary template", pad());
            auto CRD = CTD->getTemplatedDecl();
            if(auto members = CRD->lookup(name); !members.empty()) {
                LOG_DEBUG("{}" "found in 'primary'", pad());
                --indent;
                return members;
            }

            if(auto members = lookupInBases(CRD, name); !members.empty()) {
                LOG_DEBUG("{}" "found in 'base'", pad());
                --indent;
                return members;
            }

            stack.pop();
        }

        --indent;
        return lookup_result();
    }

    /// FIXME: Use a general method to dig holes.
    clang::QualType hole(clang::NestedNameSpecifier* NNS,
                         const clang::IdentifierInfo* member,
                         TemplateArguments arguments) {
        if(NNS->getKind() != clang::NestedNameSpecifier::TypeSpec) {
            return clang::QualType();
        }

        auto TST = NNS->getAsType()->getAs<clang::TemplateSpecializationType>();
        if(!TST) {
            return clang::QualType();
        }

        auto TD = TST->getTemplateName().getAsTemplateDecl();
        if(!TD->getDeclContext()->isStdNamespace()) {
            return clang::QualType();
        }

        if(TD->getName() == "allocator_traits") {
            if(TST->template_arguments().size() != 1) {
                return clang::QualType();
            }
            auto Alloc = TST->template_arguments()[0].getAsType();

            if(member->getName() == "rebind_alloc") {
                auto T = arguments[0].getAsType();

                auto prefix =
                    clang::NestedNameSpecifier::Create(context, nullptr, Alloc.getTypePtr());

                auto rebind = sema.getPreprocessor().getIdentifierInfo("rebind");

                auto DTST = context.getDependentTemplateSpecializationType(
                    clang::ElaboratedTypeKeyword::None,
                    clang::DependentTemplateStorage(prefix, rebind, false),
                    arguments);

                prefix = clang::NestedNameSpecifier::Create(context, prefix, DTST.getTypePtr());

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
                    auto result = context.getTemplateSpecializationType(TST->getTemplateName(),
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

    clang::QualType TransformType(clang::QualType type) {
        if(type.isNull() || !type->isDependentType()) {
            return type;
        }
        if(depth > 64) {
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

        if(auto* arg = stack.findArgument(T)) {
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

    clang::QualType TransformDependentNameType(clang::TypeLocBuilder& TLB,
                                               clang::DependentNameTypeLoc TL,
                                               bool DeducedTSTContext = false) {
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

        auto NNSLoc = TransformNestedNameSpecifierLoc(TL.getQualifierLoc());
        if(!NNSLoc) {
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

        auto* NNS = NNSLoc.getNestedNameSpecifier();
        auto stackSize = stack.data.size();
        auto* decl = preferred(lookup(NNS, DNT->getIdentifier()));
        auto type = getDeclType(decl);

        clang::QualType result;
        if(!type.isNull()) {
            if(decl) {
                const char* declKind = "decl";
                if(llvm::isa<clang::TypedefNameDecl>(decl))
                    declKind = "typedef";
                else if(llvm::isa<clang::RecordDecl>(decl))
                    declKind = "record";
                auto declName = llvm::dyn_cast<clang::NamedDecl>(decl)
                                    ? llvm::dyn_cast<clang::NamedDecl>(decl)->getNameAsString()
                                    : "?";
                LOG_DEBUG(
                    "{}" "found {} '{}' = '{}'",
                    pad(),
                    declKind,
                    declName,
                    type.getAsString());
            }

            // Step 1: substitute params (expand typedefs, no lookup).
            result = substitute(type);
            LOG_DEBUG("{}" "substitute → '{}'", pad(), result.getAsString());

            // Pop lookup frames BEFORE further resolution. The substitute step already
            // used the full stack for parameter substitution. TransformType should only
            // see the outer context to avoid polluting free variables (e.g. T) with
            // mappings from intermediate lookup frames.
            while(stack.data.size() > stackSize) {
                stack.pop();
            }

            // Step 2: if still dependent, do full transform (may trigger more lookups).
            if(!result.isNull() && result->isDependentType()) {
                result = TransformType(result);
            }
        } else {
            while(stack.data.size() > stackSize) {
                stack.pop();
            }
        }

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

    using Base::TransformDependentTemplateSpecializationType;

    clang::QualType rebuildDTST(clang::TypeLocBuilder& TLB,
                                clang::DependentTemplateSpecializationTypeLoc TL) {
        auto* DTST = TL.getTypePtr();
        return TLB.push<clang::DependentTemplateSpecializationTypeLoc>(clang::QualType(DTST, 0))
            .getType();
    }

    clang::QualType TransformDependentTemplateSpecializationType(
        clang::TypeLocBuilder& TLB,
        clang::DependentTemplateSpecializationTypeLoc TL) {
        auto* DTST = TL.getTypePtr();
        LOG_DEBUG("{}" "resolve DTST '{}'", pad(), clang::QualType(DTST, 0).getAsString());
        ++indent;

        if(auto iter = resolved.find(DTST); iter != resolved.end()) {
            --indent;
            TLB.pushTrivial(context, iter->second, {});
            return iter->second;
        }

        auto NNSLoc = TransformNestedNameSpecifierLoc(TL.getQualifierLoc());
        if(!NNSLoc) {
            LOG_DEBUG("{}→ <unresolved DTST>", pad());
            --indent;
            return rebuildDTST(TLB, TL);
        }
        auto* NNS = NNSLoc.getNestedNameSpecifier();

        clang::TemplateArgumentListInfo info;
        using iterator = clang::TemplateArgumentLocContainerIterator<
            clang::DependentTemplateSpecializationTypeLoc>;
        if(TransformTemplateArguments(iterator(TL, 0), iterator(TL, TL.getNumArgs()), info)) {
            LOG_DEBUG("{}→ <unresolved DTST>", pad());
            --indent;
            return rebuildDTST(TLB, TL);
        }

        llvm::SmallVector<clang::TemplateArgument, 4> arguments;
        for(auto& arg: info.arguments()) {
            arguments.push_back(arg.getArgument());
        }

        auto* name = DTST->getDependentTemplateName().getName().getIdentifier();
        if(!name) {
            LOG_DEBUG("{}→ <unresolved DTST>", pad());
            --indent;
            return rebuildDTST(TLB, TL);
        }

        if(auto result = hole(NNS, name, arguments); !result.isNull()) {
            LOG_DEBUG("{}" "hole: '{}' → '{}'", pad(), name->getName().str(), result.getAsString());
            --indent;
            resolved.try_emplace(DTST, result);
            TLB.pushTrivial(context, result, {});
            return result;
        }

        auto stackSize = stack.data.size();
        if(auto* decl = preferred(lookup(NNS, name))) {
            if(auto* TATD = llvm::dyn_cast<clang::TypeAliasTemplateDecl>(decl)) {
                if(deduceTemplateArguments(TATD, arguments)) {
                    auto type = substitute(TATD->getTemplatedDecl()->getUnderlyingType());
                    // Pop lookup frames before further resolution.
                    while(stack.data.size() > stackSize) {
                        stack.pop();
                    }
                    if(!type.isNull() && type->isDependentType()) {
                        type = TransformType(type);
                    }
                    if(!type.isNull()) {
                        LOG_DEBUG("{}" "→ '{}' (alias)", pad(), type.getAsString());
                        --indent;
                        resolved.try_emplace(DTST, type);
                        TLB.pushTrivial(context, type, {});
                        return type;
                    }
                }
            } else if(auto* CTD = llvm::dyn_cast<clang::ClassTemplateDecl>(decl)) {
                // Resolve DTST to a concrete TemplateSpecializationType.
                // e.g. __alloc_traits<allocator<T>>::rebind<T> → rebind<T> (a TST)
                // This allows subsequent lookup of members (like "other") to work.
                // Keep lookup frames on stack — the caller (e.g. TransformNestedNameSpecifierLoc
                // processing A<X>::B<Y>::C<Z>) needs them for parameter substitution.
                clang::TemplateName TN(CTD);
                llvm::SmallVector<clang::TemplateArgument> canonArgs;
                for(auto& arg: arguments) {
                    canonArgs.push_back(context.getCanonicalTemplateArgument(arg));
                }
                auto result = context.getTemplateSpecializationType(TN, arguments, canonArgs);
                LOG_DEBUG("{}" "→ TST '{}' (class)", pad(), result.getAsString());
                --indent;
                resolved.try_emplace(DTST, result);
                TLB.pushTrivial(context, result, {});
                return result;
            }
        }
        while(stack.data.size() > stackSize) {
            stack.pop();
        }

        LOG_DEBUG("{}→ <unresolved DTST>", pad());
        --indent;
        auto fallback = rebuildDTST(TLB, TL);
        resolved.try_emplace(DTST, fallback);
        return fallback;
    }

    /// Desugar dependent typedefs via SubstituteOnly (no lookup, no cycle risk).
    clang::QualType TransformTypedefType(clang::TypeLocBuilder& TLB, clang::TypedefTypeLoc TL) {
        if(auto* TND = TL.getTypedefNameDecl()) {
            auto underlying = TND->getUnderlyingType();
            if(underlying->isDependentType()) {
                auto type = substitute(underlying);
                if(!type.isNull()) {
                    if(auto ET = llvm::dyn_cast<clang::ElaboratedType>(type)) {
                        type = ET->getNamedType();
                    }
                    TLB.pushTrivial(context, type, {});
                    return type;
                }
            }
        }
        return Base::TransformTypedefType(TLB, TL);
    }

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

private:
    clang::Sema& sema;
    clang::ASTContext& context;
    InstantiationStack stack;
    llvm::DenseMap<const void*, clang::QualType>& resolved;
    llvm::SmallPtrSet<const void*, 8> active_resolutions;
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

TemplateResolver::lookup_result TemplateResolver::lookup(const clang::NestedNameSpecifier* NNS,
                                                         clang::DeclarationName name) {
    PseudoInstantiator instantiator(sema, resolved);
    return instantiator.lookup(NNS, name);
}

}  // namespace clice
