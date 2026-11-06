module clice;

import :semantic.template_resolver.deduction;
import :semantic.template_resolver.instantiator;
import :support.logging;

namespace clice::types {

bool has_dependent_base(const clang::CXXRecordDecl* record) {
    auto definition = record->getDefinition();
    if(!definition) {
        return true;
    }
    return ranges::any_of(definition->bases(), [](const clang::CXXBaseSpecifier& base) {
        return base.getType()->isDependentType();
    });
}

clang::ClassTemplateDecl* member_pattern(const clang::ClassTemplateDecl* CTD) {
    if(CTD->getTemplatedDecl()->hasDefinition() ||
       !llvm::isa<clang::ClassTemplateSpecializationDecl>(CTD->getDeclContext())) {
        return nullptr;
    }
    return CTD->getInstantiatedFromMemberTemplate();
}

namespace {

/// The explicit specializations among `specializations` that `arguments`
/// may name without certainly naming one. Only a value not known leaves
/// matching their concrete arguments undecided.
template <typename Specialization>
llvm::SmallVector<Specialization*, 2>
    undecided_among(clang::ASTContext& context,
                    auto specializations,
                    llvm::ArrayRef<clang::TemplateArgument> arguments,
                    const DeductionOptions& options) {
    auto unknown = [](const clang::TemplateArgument& argument) {
        return argument.getKind() == clang::TemplateArgument::Expression &&
               argument.getAsExpr()->isValueDependent();
    };
    llvm::SmallVector<Specialization*, 2> undecided;
    if(ranges::none_of(arguments, [&](const clang::TemplateArgument& argument) {
           return argument.getKind() == clang::TemplateArgument::Pack
                      ? ranges::any_of(argument.pack_elements(), unknown)
                      : unknown(argument);
       })) {
        return undecided;
    }
    for(auto* specialization: specializations) {
        if(specialization->isExplicitSpecialization() &&
           may_name(context, specialization->getTemplateArgs().asArray(), arguments, options)) {
            undecided.push_back(specialization);
        }
    }
    return undecided;
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
    auto choice = select_partial(context, viable_partials(CTD, arguments));
    if(choice.result != PartialResult::Ambiguous &&
       !undecided_specializations(CTD, arguments).empty()) {
        return {.result = PartialResult::Unverified};
    }
    return choice;
}

PartialChoice<clang::VarTemplatePartialSpecializationDecl>
    PseudoInstantiator::match_partial(clang::VarTemplateDecl* VTD, TemplateArguments arguments) {
    auto choice = select_partial(context, viable_partials(VTD, arguments));
    if(choice.result != PartialResult::Ambiguous &&
       !undecided_specializations(VTD, arguments).empty()) {
        return {.result = PartialResult::Unverified};
    }
    return choice;
}

llvm::SmallVector<clang::ClassTemplateSpecializationDecl*, 2>
    PseudoInstantiator::undecided_specializations(clang::ClassTemplateDecl* CTD,
                                                  TemplateArguments arguments) {
    return undecided_among<clang::ClassTemplateSpecializationDecl>(
        context,
        CTD->specializations(),
        arguments,
        {.open = open_defaults.getArrayRef()});
}

llvm::SmallVector<clang::VarTemplateSpecializationDecl*, 2>
    PseudoInstantiator::undecided_specializations(clang::VarTemplateDecl* VTD,
                                                  TemplateArguments arguments) {
    return undecided_among<clang::VarTemplateSpecializationDecl>(
        context,
        VTD->specializations(),
        arguments,
        {.open = open_defaults.getArrayRef()});
}

clang::CXXRecordDecl* PseudoInstantiator::select_pattern(clang::ClassTemplateDecl* CTD,
                                                         TemplateArguments written) {
    if(auto* from = member_pattern(CTD)) {
        CTD = from;
    }
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

    auto choice = match_partial(VTD, arguments);
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

    /// The probe judges its own work: `scope_lacks` reenters itself through
    /// lookup's partial probing, and what an enclosing probe observed stays
    /// with it. Resolving the scope prefix is part of that work.
    auto judged = own_conclusion();

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
                    /// at all. A value the bindings fold (`enable_if<false,
                    /// int>` from `__is_same(U, float)`) has decided.
                    auto open = [&](const clang::TemplateArgument& argument) {
                        return argument.isDependent() &&
                               !(argument.getKind() == clang::TemplateArgument::Expression &&
                                 evaluate(argument.getAsExpr()));
                    };
                    if(lacks && ranges::any_of(TST->template_arguments(), open) &&
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
                /// A member of the class hides those of its bases.
                if(auto own = RD->lookup(name); !own.empty()) {
                    lacks = conclusive(own);
                } else {
                    lacks = conclusive(lookup_in_bases(RD, name)) && !has_dependent_base(RD);
                }
            } else if(!type->isDependentType()) {
                /// Only a class has members.
                lacks = true;
            }
        }
    }

    stack.truncate(stack_size);
    return lacks && !inconclusive;
}

}  // namespace clice::types
