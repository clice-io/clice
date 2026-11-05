module clice;

import :semantic.template_resolver.deduction;
import :semantic.template_resolver.instantiator;

namespace clice::types {

clang::QualType PseudoInstantiator::make_specialization(clang::TemplateName name,
                                                        TemplateArguments arguments) {
    /// A template template parameter can be bound to a dependent template
    /// name (libc++ binds `_Alloc::template rebind` this way). A
    /// TemplateSpecializationType cannot carry those (clang asserts);
    /// rebuild the dependent form with the substituted arguments instead.
    if(name.getAsDependentTemplateName()) {
        return context.getTemplateSpecializationType(clang::ElaboratedTypeKeyword::None,
                                                     name,
                                                     arguments,
                                                     /*CanonicalArgs=*/{});
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
        if(!fill_defaults(TD, arguments, full)) {
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
                    canonical.emplace_back(clang::TemplateArgument::CreatePackCopy(context, pack));
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
    bool concrete = ranges::none_of(canonical, [](const clang::TemplateArgument& arg) {
        return arg.isDependent();
    });
    if(concrete) {
        if(auto CTD = llvm::dyn_cast_or_null<clang::ClassTemplateDecl>(name.getAsTemplateDecl())) {
            void* pos = nullptr;
            if(auto CTSD = CTD->findSpecialization(canonical, pos)) {
                underlying = context.getCanonicalTagType(CTSD);
            }
        }
    }

    /// An alias specialization must carry its aliased type unless the
    /// arguments still hold unexpanded packs (clang asserts on this).
    /// This arises when a template template parameter got substituted
    /// with an alias template: the head is ours, so the aliasing is ours
    /// to compute. Failure degrades to an unrewritten (null) result.
    if(auto TATD = llvm::dyn_cast_or_null<clang::TypeAliasTemplateDecl>(name.getAsTemplateDecl())) {
        bool expansions = ranges::any_of(canonical, [](const clang::TemplateArgument& arg) {
            return arg.isPackExpansion();
        });
        if(!expansions) {
            clang::QualType aliased;
            if(deduce_template_arguments(TATD, arguments) != Deduction::Failed) {
                aliased = substitute(TATD->getTemplatedDecl()->getUnderlyingType());
                stack.pop();
            }
            if(aliased.isNull()) {
                return clang::QualType();
            }
            return context.getTemplateSpecializationType(clang::ElaboratedTypeKeyword::None,
                                                         name,
                                                         arguments,
                                                         canonical,
                                                         aliased);
        }
    }

    return context.getTemplateSpecializationType(clang::ElaboratedTypeKeyword::None,
                                                 name,
                                                 arguments,
                                                 canonical,
                                                 underlying);
}

clang::QualType PseudoInstantiator::rewrite_template(const clang::TemplateSpecializationType* TST,
                                                     Policy policy) {
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

bool PseudoInstantiator::rewrite_arguments(TemplateArguments arguments,
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
                            rewritten =
                                context.getPackExpansionType(rewritten, PET->getNumExpansions());
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
                        auto* bound = stack.find_argument(NTTP, NTTP->getDepth(), NTTP->getIndex());
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
                if(argument.getAsTemplate().getAsDependentTemplateName()) {
                    auto name = rewrite_template_name(argument.getAsTemplate(), policy);
                    if(name && *name != argument.getAsTemplate()) {
                        out.emplace_back(*name);
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

std::optional<llvm::SmallVector<clang::TemplateArgument, 4>>
    PseudoInstantiator::expand_pack_pattern(clang::QualType pattern, Policy policy) {
    auto packs = packs_in(clang::TemplateArgument(pattern));
    if(packs.empty()) {
        return std::nullopt;
    }

    llvm::SmallVector<InstantiationStack::Slot, 2> slots;
    unsigned length = 0;
    for(auto& pack: packs) {
        auto handle = stack.find_slot(pack.decl, pack.depth, pack.index);
        if(!handle) {
            return std::nullopt;
        }
        auto& bound = stack.slot(*handle);
        if(bound.getKind() != clang::TemplateArgument::Pack ||
           (!slots.empty() && bound.pack_size() != length)) {
            return std::nullopt;
        }
        length = bound.pack_size();
        if(!ranges::contains(slots, *handle)) {
            slots.push_back(*handle);
        }
    }

    auto saved = ranges::to<llvm::SmallVector<clang::TemplateArgument, 2>>(
        views::transform(slots, [&](auto handle) { return stack.slot(handle); }));

    /// While narrowed, node-pointer-keyed caches are bypassed: the same
    /// pattern node resolves differently per element, and a cached first
    /// element would silently repeat for every later one.
    pack_narrowing += 1;
    bool ok = true;
    llvm::SmallVector<clang::TemplateArgument, 4> expanded;
    for(unsigned k = 0; k < length && ok; k += 1) {
        for(auto [handle, pack]: views::zip(slots, saved)) {
            stack.slot(handle) = pack.pack_begin()[k];
        }
        auto element = rewrite(pattern, policy);
        ok = element != pattern && !element->containsUnexpandedParameterPack();
        if(ok) {
            expanded.emplace_back(element);
        }
    }
    pack_narrowing -= 1;

    for(auto [handle, pack]: views::zip(slots, saved)) {
        stack.slot(handle) = pack;
    }

    if(!ok) {
        return std::nullopt;
    }
    return expanded;
}

std::optional<clang::TemplateName>
    PseudoInstantiator::rewrite_template_name(clang::TemplateName name, Policy policy) {
    auto* dependent = name.getAsDependentTemplateName();
    auto NNS = rewrite_specifier(dependent->getQualifier(), policy);
    if(NNS == dependent->getQualifier()) {
        return name;
    }
    if(NNS.isDependent()) {
        return context.getDependentTemplateName(
            clang::DependentTemplateStorage(NNS,
                                            dependent->getName(),
                                            dependent->hasTemplateKeyword()));
    }
    auto* identifier = dependent->getName().getIdentifier();
    auto* decl =
        identifier ? llvm::dyn_cast_or_null<clang::TemplateDecl>(preferred(lookup(NNS, identifier)))
                   : nullptr;
    if(!decl) {
        return std::nullopt;
    }
    return clang::TemplateName(decl);
}

clang::NestedNameSpecifier PseudoInstantiator::rewrite_specifier(clang::NestedNameSpecifier NNS,
                                                                 Policy policy) {
    /// Only type components can contain substitutable parameters; a
    /// dependent chain (`base::type::inner`) is a DependentNameType whose
    /// own qualifier the rewrite recurses into.
    if(!NNS || NNS.getKind() != clang::NestedNameSpecifier::Kind::Type) {
        return NNS;
    }

    auto type = rewrite(clang::QualType(NNS.getAsType(), 0), policy);
    if(type.getTypePtr() == NNS.getAsType()) {
        return NNS;
    }
    return clang::NestedNameSpecifier(type.getTypePtr());
}

}  // namespace clice::types
