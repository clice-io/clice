module clice;

import :semantic.decls;
import :semantic.template_resolver;
import :semantic.template_resolver.deduction;
import :semantic.template_resolver.instantiator;

namespace clice::types {

namespace {

/// A parameter list naming the parameters of `owner`, a class template the
/// unit instantiates once, with that instantiation.
struct OnlyInstantiation {
    clang::ClassTemplateDecl* owner;
    clang::TemplateParameterList* params;
    const clang::ClassTemplateSpecializationDecl* instantiation;
};

/// Whether `instantiation` explicitly specializes `member`, a member of
/// its template's pattern (`template <> void A<int>::f() {}`): it never
/// instantiates that member's body.
bool explicitly_specializes(const clang::ClassTemplateSpecializationDecl& instantiation,
                            const clang::Decl& member) {
    auto* function = llvm::dyn_cast<clang::FunctionDecl>(&member);
    if(!function) {
        return false;
    }
    return ranges::any_of(instantiation.decls(), [&](const clang::Decl* decl) {
        auto* specialized = llvm::dyn_cast<clang::FunctionDecl>(decl);
        auto* pattern = specialized ? specialized->getInstantiatedFromMemberFunction() : nullptr;
        return pattern && pattern->getCanonicalDecl() == function->getCanonicalDecl() &&
               specialized->getTemplateSpecializationKind() == clang::TSK_ExplicitSpecialization;
    });
}

/// The parameter lists through which `var` can name the parameters of the
/// class templates around it. Besides each template's own list, a
/// declaration defined outside its class (`void Holder<T>::f() {}`) names
/// them through lists of its own, one per enclosing template, outermost
/// first.
llvm::SmallVector<OnlyInstantiation> only_instantiations(const clang::VarDecl* var) {
    /// Innermost first, with a null instantiation for a template that has
    /// none or several.
    llvm::SmallVector<OnlyInstantiation> enclosing;
    /// The lists of a declaration outside its class, and how many enclosing
    /// templates were met up to it.
    llvm::SmallVector<std::pair<llvm::ArrayRef<clang::TemplateParameterList*>, unsigned>> outside;

    /// A friend defined inside a class belongs to the enclosing namespace
    /// but sees the class's parameters.
    auto scope = [](const clang::Decl* decl) {
        return decl->getFriendObjectKind() != clang::Decl::FOK_None ? decl->getLexicalDeclContext()
                                                                    : decl->getDeclContext();
    };
    /// The member of the class the walk reached that holds `var`.
    const clang::Decl* member = nullptr;
    for(const clang::Decl* decl = var; decl && !llvm::isa<clang::TranslationUnitDecl>(decl);
        member = decl, decl = llvm::dyn_cast<clang::Decl>(scope(decl))) {
        auto* record = llvm::dyn_cast<clang::CXXRecordDecl>(decl);
        if(auto* owner = record ? record->getDescribedClassTemplate() : nullptr) {
            // The helper keeps its clangd signature, which speaks mutable
            // pointers; it only reads through them.
            auto* instantiation = llvm::dyn_cast_or_null<clang::ClassTemplateSpecializationDecl>(
                decls::only_instantiation(const_cast<clang::CXXRecordDecl*>(record)));
            if(instantiation && member && explicitly_specializes(*instantiation, *member)) {
                instantiation = nullptr;
            }
            enclosing.push_back({owner, owner->getTemplateParameters(), instantiation});
        }
        /// A member class template's own list is not among its outer ones.
        auto lists = decls::outer_template_parameters(decl);
        if(!lists.empty()) {
            outside.emplace_back(lists, enclosing.size());
        }
    }

    llvm::SmallVector<OnlyInstantiation> bound;
    for(auto& entry: enclosing) {
        if(entry.instantiation) {
            bound.push_back(entry);
        }
    }
    for(auto& [lists, below]: outside) {
        auto above = llvm::ArrayRef(enclosing).drop_front(below);
        if(lists.size() != above.size()) {
            continue;
        }
        for(unsigned i = 0; i < lists.size(); i += 1) {
            auto& entry = above[above.size() - 1 - i];
            if(entry.instantiation) {
                bound.push_back({entry.owner, lists[i], entry.instantiation});
            }
        }
    }
    return bound;
}

template <typename Specialization>
const clang::NamedDecl* pattern_of(const Specialization& specialization) {
    auto* primary = specialization.getSpecializedTemplate();
    /// The choice must not depend on what the unit instantiates: the
    /// pattern names the specialization's entity in every unit naming it.
    llvm::DenseMap<const void*, clang::QualType> resolved;
    ResolverOptions options{.only_instantiation = false};
    PseudoInstantiator instantiator(primary->getASTContext(), resolved, options);
    auto* pattern =
        instantiator.select_pattern(primary, specialization.getTemplateArgs().asArray());
    return pattern ? pattern : primary->getTemplatedDecl();
}

}  // namespace

clang::QualType TemplateResolver::resolve(clang::QualType type) {
    PseudoInstantiator instantiator(context, resolved, options);
    return instantiator.resolve(type);
}

TemplateResolver::lookup_result TemplateResolver::lookup(clang::NestedNameSpecifier NNS,
                                                         clang::DeclarationName name) {
    PseudoInstantiator instantiator(context, resolved, options);
    return instantiator.lookup(NNS, name);
}

clang::TagDecl* TemplateResolver::resolve_tag(clang::QualType type) {
    PseudoInstantiator instantiator(context, resolved, options);
    type = instantiator.resolve(type);
    if(auto* tag = type->getAsTagDecl()) {
        return tag;
    }

    auto* TST = type->getAs<clang::TemplateSpecializationType>();
    if(!TST) {
        return nullptr;
    }
    auto* CTD = llvm::dyn_cast_or_null<clang::ClassTemplateDecl>(
        TST->getTemplateName().getAsTemplateDecl());
    if(!CTD) {
        return nullptr;
    }
    return instantiator.select_pattern(CTD, TST->template_arguments());
}

bool TemplateResolver::specializable(clang::QualType type) {
    auto* chosen = resolve_tag(type);
    PseudoInstantiator instantiator(context, resolved, options);
    type = instantiator.resolve(type);
    auto* TST = type.isNull() ? nullptr : type->getAs<clang::TemplateSpecializationType>();
    auto* CTD = TST ? llvm::dyn_cast_or_null<clang::ClassTemplateDecl>(
                          TST->getTemplateName().getAsTemplateDecl())
                    : nullptr;
    if(!CTD || !TST->isDependentType()) {
        return false;
    }
    auto matches = [&](const clang::ClassTemplateSpecializationDecl* specialization) {
        return specialization != chosen && may_match(context,
                                                     TST->template_arguments(),
                                                     specialization->getTemplateArgs().asArray());
    };
    llvm::SmallVector<clang::ClassTemplatePartialSpecializationDecl*> partials;
    CTD->getPartialSpecializations(partials);
    return ranges::any_of(partials, matches) ||
           ranges::any_of(CTD->specializations(),
                          [&](const clang::ClassTemplateSpecializationDecl* specialization) {
                              return decls::explicitly_specialized(specialization) &&
                                     matches(specialization);
                          });
}

clang::QualType TemplateResolver::deduce(const clang::VarDecl* var) {
    /// A variable inside class templates the unit instantiates once deduces
    /// as in those instantiations. Their parameters are bound for this query
    /// alone, so what it resolves under them stays out of the unit's cache.
    llvm::SmallVector<OnlyInstantiation> bindings;
    if(options.only_instantiation) {
        bindings = only_instantiations(var);
    }
    llvm::DenseMap<const void*, clang::QualType> scoped;
    PseudoInstantiator instantiator(context, bindings.empty() ? resolved : scoped, options);
    for(auto& binding: bindings) {
        instantiator.assume(binding.owner,
                            binding.params,
                            binding.instantiation->getTemplateArgs().asArray());
    }
    auto type = instantiator.deduce_auto(var);
    return type.isNull() ? type : instantiator.resolve(type);
}

clang::QualType TemplateResolver::member_object(const clang::Expr* base, bool arrow) {
    PseudoInstantiator instantiator(context, resolved, options);
    return instantiator.resolve_object(
        instantiator.member_object(instantiator.expr_type(base), arrow));
}

clang::QualType TemplateResolver::arrow_pointee(clang::QualType type) {
    PseudoInstantiator instantiator(context, resolved, options);
    return instantiator.resolve(instantiator.arrow_pointee(type));
}

TemplateResolver::lookup_result
    TemplateResolver::lookup(const clang::CXXDependentScopeMemberExpr* expr) {
    PseudoInstantiator instantiator(context, resolved, options);
    return instantiator.lookup_member(instantiator.member_object(expr), expr->getMember());
}

TemplateResolver::lookup_result
    TemplateResolver::lookup(const clang::TemplateSpecializationType* type) {
    auto dependent = type->getTemplateName().getAsDependentTemplateName();
    if(!dependent) {
        return {};
    }

    auto name = dependent->getName();
    if(auto identifier = name.getIdentifier()) {
        return lookup(dependent->getQualifier(), identifier);
    }
    return lookup(dependent->getQualifier(),
                  context.DeclarationNames.getCXXOperatorName(name.getOperator()));
}

llvm::SmallVector<const clang::NamedDecl*, 4>
    TemplateResolver::callees(const clang::CallExpr* expr) {
    PseudoInstantiator instantiator(context, resolved, options);
    return std::move(instantiator.call_candidates(expr).candidates);
}

llvm::SmallVector<const clang::NamedDecl*, 4>
    TemplateResolver::callees(const clang::Expr* callee,
                              llvm::ArrayRef<const clang::Expr*> written) {
    PseudoInstantiator instantiator(context, resolved, options);
    return instantiator.written_call_candidates(callee, written);
}

auto instantiation_pattern(const clang::ClassTemplateSpecializationDecl& specialization)
    -> const clang::NamedDecl* {
    return pattern_of(specialization);
}

auto instantiation_pattern(const clang::VarTemplateSpecializationDecl& specialization)
    -> const clang::NamedDecl* {
    return pattern_of(specialization);
}

}  // namespace clice::types
