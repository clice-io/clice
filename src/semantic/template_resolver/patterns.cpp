module clice;

import :semantic.template_resolver.deduction;
import :semantic.template_resolver.instantiator;
import :support.logging;

namespace clice::types {

namespace {

/// Does `record`'s definition inherit from a still-dependent base? Such
/// a base may contribute the probed member only after instantiation, so
/// absence is never conclusive through it. An undefined record proves
/// nothing either.
bool has_dependent_base(const clang::CXXRecordDecl* record) {
    auto definition = record->getDefinition();
    if(!definition) {
        return true;
    }
    return ranges::any_of(definition->bases(), [](const clang::CXXBaseSpecifier& base) {
        return base.getType()->isDependentType();
    });
}

/// Is `decl` provably unusable for the probe's usage form? `typename
/// X::m` cannot name a value; `X::template m<...>` cannot name a value
/// or a plain type. Unknown declaration kinds stay presumed viable.
bool unusable_member(clang::Decl* decl, bool wants_template) {
    /// Access participates in SFINAE: the probe models a use from
    /// outside the class, where a non-public member never resolves.
    /// Friendship of the probing specialization is not modeled — an
    /// accepted approximation; detectors befriended by their subjects
    /// degrade to the primary. A using-declaration grants the access it
    /// is declared with.
    if(decl->getAccess() == clang::AS_private || decl->getAccess() == clang::AS_protected) {
        return true;
    }
    if(auto shadow = llvm::dyn_cast<clang::UsingShadowDecl>(decl)) {
        decl = shadow->getTargetDecl();
    }
    if(llvm::isa<clang::ValueDecl, clang::FunctionTemplateDecl, clang::VarTemplateDecl>(decl)) {
        return true;
    }
    /// `typename X::m` cannot name an unspecialized template either.
    if(!wants_template && llvm::isa<clang::TemplateDecl>(decl)) {
        return true;
    }
    return wants_template && llvm::isa<clang::TypeDecl>(decl);
}

}  // namespace

template <typename Partial>
std::optional<PartialMatch<Partial>> PseudoInstantiator::viable(Partial* partial,
                                                                TemplateArguments arguments) {
    auto deduction = deduce_template_arguments(partial, arguments);
    if(deduction == Deduction::Failed) {
        return std::nullopt;
    }
    bool viable = satisfies_pattern(partial->getTemplateArgsAsWritten());
    stack.pop();
    if(!viable) {
        LOG_DEBUG(
            "{}"
            "pruned partial '{}' (member absent)",
            pad(),
            partial->getNameAsString());
        return std::nullopt;
    }
    return PartialMatch<Partial>{.partial = partial, .verified = deduction == Deduction::Matched};
}

llvm::SmallVector<PartialMatch<clang::ClassTemplatePartialSpecializationDecl>, 4>
    PseudoInstantiator::viable_partials(clang::ClassTemplateDecl* CTD,
                                        TemplateArguments arguments) {
    llvm::SmallVector<clang::ClassTemplatePartialSpecializationDecl*> partials;
    CTD->getPartialSpecializations(partials);
    llvm::SmallVector<PartialMatch<clang::ClassTemplatePartialSpecializationDecl>, 4> matched;
    for(auto* partial: partials) {
        if(auto match = viable(partial, arguments)) {
            matched.push_back(*match);
        }
    }
    return matched;
}

llvm::SmallVector<PartialMatch<clang::VarTemplatePartialSpecializationDecl>, 4>
    PseudoInstantiator::viable_partials(clang::VarTemplateDecl* VTD, TemplateArguments arguments) {
    llvm::SmallVector<clang::VarTemplatePartialSpecializationDecl*> partials;
    VTD->getPartialSpecializations(partials);
    llvm::SmallVector<PartialMatch<clang::VarTemplatePartialSpecializationDecl>, 4> matched;
    for(auto* partial: partials) {
        if(auto match = viable(partial, arguments)) {
            matched.push_back(*match);
        }
    }
    return matched;
}

PartialChoice<clang::ClassTemplatePartialSpecializationDecl>
    PseudoInstantiator::match_partial(clang::ClassTemplateDecl* CTD, TemplateArguments arguments) {
    return select_partial(context, viable_partials(CTD, arguments));
}

clang::CXXRecordDecl* PseudoInstantiator::select_pattern(clang::ClassTemplateDecl* CTD,
                                                         TemplateArguments written) {
    llvm::SmallVector<clang::TemplateArgument, 4> arguments;
    if(!fill_defaults(CTD, written, arguments)) {
        return nullptr;
    }

    auto choice = match_partial(CTD, arguments);
    switch(choice.result) {
        case PartialResult::Selected: return choice.winner;
        case PartialResult::None: return CTD->getTemplatedDecl();
        case PartialResult::Ambiguous:
        case PartialResult::Unverified: return nullptr;
    }
    std::unreachable();
}

clang::VarDecl* PseudoInstantiator::select_pattern(clang::VarTemplateDecl* VTD,
                                                   TemplateArguments written) {
    llvm::SmallVector<clang::TemplateArgument, 4> arguments;
    if(!fill_defaults(VTD, written, arguments)) {
        return nullptr;
    }

    auto choice = select_partial(context, viable_partials(VTD, arguments));
    switch(choice.result) {
        case PartialResult::Selected: return choice.winner;
        case PartialResult::None: return VTD->getTemplatedDecl();
        case PartialResult::Ambiguous:
        case PartialResult::Unverified: return nullptr;
    }
    std::unreachable();
}

bool PseudoInstantiator::satisfies_pattern(const clang::ASTTemplateArgumentListInfo* written) {
    if(probing > 4) {
        return true;
    }

    /// The probe expression only survives in the as-written arguments:
    /// the converted list has already desugared `void_t<...>` to `void`.
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

bool PseudoInstantiator::member_absent(clang::QualType type, unsigned guard) {
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

        case clang::Type::TemplateSpecialization: {
            auto TST = llvm::cast<clang::TemplateSpecializationType>(T);
            if(auto dependent = TST->getTemplateName().getAsDependentTemplateName()) {
                auto identifier = dependent->getName().getIdentifier();
                auto qualifier = dependent->getQualifier();
                if(specifier_absent(qualifier, guard)) {
                    return true;
                }
                return identifier && scope_lacks(qualifier, identifier, /*wants_template=*/true);
            }

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

        case clang::Type::Paren: {
            return member_absent(llvm::cast<clang::ParenType>(T)->getInnerType(), guard + 1);
        }
        case clang::Type::Pointer: {
            return member_absent(llvm::cast<clang::PointerType>(T)->getPointeeType(), guard + 1);
        }
        case clang::Type::LValueReference:
        case clang::Type::RValueReference: {
            return member_absent(llvm::cast<clang::ReferenceType>(T)->getPointeeType(), guard + 1);
        }
        case clang::Type::PackExpansion: {
            return member_absent(llvm::cast<clang::PackExpansionType>(T)->getPattern(), guard + 1);
        }

        default: {
            return false;
        }
    }
}

bool PseudoInstantiator::specifier_absent(clang::NestedNameSpecifier NNS, unsigned guard) {
    if(!NNS || guard > 16 || NNS.getKind() != clang::NestedNameSpecifier::Kind::Type) {
        return false;
    }

    const clang::Type* T = NNS.getAsType();
    if(auto TST = llvm::dyn_cast<clang::TemplateSpecializationType>(T)) {
        auto dependent = TST->getTemplateName().getAsDependentTemplateName();
        if(!dependent) {
            return false;
        }
        auto scope = dependent->getQualifier();
        if(specifier_absent(scope, guard + 1)) {
            return true;
        }
        auto identifier = dependent->getName().getIdentifier();
        return identifier && scope_lacks(scope, identifier, /*wants_template=*/true);
    }
    if(auto DNT = llvm::dyn_cast<clang::DependentNameType>(T)) {
        if(specifier_absent(DNT->getQualifier(), guard + 1)) {
            return true;
        }
        return scope_lacks(DNT->getQualifier(), DNT->getIdentifier());
    }
    return false;
}

bool PseudoInstantiator::any_specialization_declares(clang::ClassTemplateDecl* CTD,
                                                     clang::DeclarationName name) {
    auto declares = [&](clang::CXXRecordDecl* record) {
        auto* definition = record->getDefinition();
        return definition &&
               (!definition->lookup(name).empty() || !lookup_in_bases(definition, name).empty());
    };

    llvm::SmallVector<clang::ClassTemplatePartialSpecializationDecl*, 4> partials;
    CTD->getPartialSpecializations(partials);
    return ranges::any_of(partials, declares) || ranges::any_of(CTD->specializations(), declares);
}

bool PseudoInstantiator::scope_lacks(clang::NestedNameSpecifier scope,
                                     clang::DeclarationName name,
                                     bool wants_template) {
    if(!scope) {
        return false;
    }

    /// Empty lookups and lookups whose every candidate is provably
    /// unusable both fail the probe conclusively.
    auto conclusive = [&](lookup_result members) {
        return ranges::all_of(members, [&](clang::Decl* decl) {
            return unusable_member(decl, wants_template);
        });
    };

    auto stack_size = stack.size();

    /// The flag is save/reset/restored rather than just read: `scope_lacks`
    /// reenters itself through lookup's partial probing, and a nested clean
    /// probe must not wash out what an in-flight outer one observed.
    /// Restoring with `saved || inconclusive` propagates it outwards, so
    /// every enclosing probe also stays Unknown. The reset sits before
    /// `rewrite_specifier` because resolving the scope prefix can make the
    /// probe inconclusive too.
    bool saved = inconclusive;
    inconclusive = false;

    auto resolved_scope = rewrite_specifier(scope, Policy::Resolve);

    bool lacks = false;
    if(resolved_scope.getKind() == clang::NestedNameSpecifier::Kind::Type) {
        auto type = resolve(clang::QualType(resolved_scope.getAsType(), 0));
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
                    if(lacks && TST->isDependentType() && any_specialization_declares(CTD, name)) {
                        lacks = false;
                    }

                    /// A dependent base (`struct D : T`) may contribute
                    /// the member after instantiation.
                    if(lacks && has_dependent_base(CTD->getTemplatedDecl())) {
                        lacks = false;
                    }
                }
            } else if(auto RD = type->getAsCXXRecordDecl()) {
                /// A member of the class hides those of its bases.
                if(auto own = RD->lookup(name); !own.empty()) {
                    lacks = conclusive(own);
                } else {
                    lacks = conclusive(lookup_in_bases(RD, name)) && !has_dependent_base(RD);
                }
            }
        }
    }

    if(inconclusive) {
        lacks = false;
    }
    inconclusive = saved || inconclusive;

    stack.truncate(stack_size);
    return lacks;
}

}  // namespace clice::types
