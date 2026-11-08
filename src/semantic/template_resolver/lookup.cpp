module clice;

import :semantic.decls;
import :semantic.template_resolver.deduction;
import :semantic.template_resolver.instantiator;
import :support.logging;

namespace clice::types {

namespace {

bool same_declarations(clang::DeclContext::lookup_result lhs,
                       clang::DeclContext::lookup_result rhs) {
    auto canonical = [](clang::DeclContext::lookup_result members) {
        llvm::SmallPtrSet<const clang::Decl*, 4> decls;
        for(auto* member: members) {
            decls.insert(member->getCanonicalDecl());
        }
        return decls;
    };
    return canonical(lhs) == canonical(rhs);
}

}  // namespace

clang::Decl* PseudoInstantiator::preferred(lookup_result members) {
    clang::Decl* decl = nullptr;
    ranges::for_each(members, [&](auto member) { decl = member; });
    return decl;
}

PseudoInstantiator::lookup_result PseudoInstantiator::lookup(clang::QualType type,
                                                             clang::DeclarationName name) {
    auto written = type;
    type = resolve_object(type);
    if(auto* DNT = type->getAs<clang::DependentNameType>()) {
        if(auto alternatives = std_alternatives(DNT); !alternatives.empty()) {
            return lookup_through_alternatives(alternatives, name);
        }
    }

    if(auto* record = type->getAsCXXRecordDecl()) {
        /// A member class reached through a dependent name
        /// (`Host<T>::Nested`), or rewritten with its enclosing class as
        /// its qualifier, is the bare member of the pattern; looking it
        /// up again through its qualifier leaves the enclosing
        /// specialization's bindings on the stack, so what its members
        /// are written in (`T`) stays bound.
        clang::NestedNameSpecifier qualifier = std::nullopt;
        if(auto* DNT = written->getAs<clang::DependentNameType>()) {
            qualifier = DNT->getQualifier();
        } else if(auto* RT = type->getAs<clang::RecordType>()) {
            qualifier = RT->getQualifier();
        }
        if(qualifier && record->isDependentContext()) {
            auto frames = stack.size();
            auto* member = preferred(lookup(qualifier, record->getDeclName()));
            if(member && member->getCanonicalDecl() == record->getCanonicalDecl()) {
                auto members = lookup_in_record(record, name);
                if(members.empty()) {
                    stack.truncate(frames);
                }
                return members;
            }
            stack.truncate(frames);
        }

        /// A specialization the TU never instantiated has no members
        /// yet; look them up in the pattern it would instantiate from. One
        /// explicitly specialized and never defined has none at all.
        if(auto* spec = llvm::dyn_cast<clang::ClassTemplateSpecializationDecl>(record);
           spec && !spec->hasDefinition()) {
            if(decls::explicitly_specialized(spec)) {
                return lookup_result();
            }
            return lookup(spec->getSpecializedTemplate(), name, spec->getTemplateArgs().asArray());
        }
        return lookup_in_record(record, name);
    }

    auto TST = type->getAs<clang::TemplateSpecializationType>();
    if(!TST) {
        return lookup_result();
    }

    clang::Decl* TD = TST->getTemplateName().getAsTemplateDecl();
    if(auto dependent = !TD ? TST->getTemplateName().getAsDependentTemplateName() : nullptr) {
        // If this dependent specialization was already resolved (possibly to
        // itself when unresolvable), skip the redundant lookup.
        if(resolution_kept(TST) && kept_resolution(cache.resolved, TST)) {
            return lookup_result();
        }

        auto name = dependent->getName().getIdentifier();
        if(!name) {
            return {};
        }

        TD = preferred(lookup(dependent->getQualifier(), name));
    }

    if(auto CTD = llvm::dyn_cast_or_null<clang::ClassTemplateDecl>(TD)) {
        return lookup(CTD, name, TST->template_arguments());
    } else if(auto TATD = llvm::dyn_cast_or_null<clang::TypeAliasTemplateDecl>(TD)) {
        /// An alias may name itself again (`using alias = typename
        /// X<T>::template alias<U>`): each expansion nests, against the
        /// limit rewrites nest against.
        if(depth > depth_limit) {
            give_up(Trouble::Depth);
            return lookup_result();
        }
        if(deduce_template_arguments(TATD, TST->template_arguments()) != Deduction::Failed) {
            auto type = substitute(TATD->getTemplatedDecl()->getUnderlyingType());
            stack.pop();
            depth += 1;
            auto members = lookup(type, name);
            depth -= 1;
            return members;
        }
    }

    return lookup_result();
}

PseudoInstantiator::lookup_result PseudoInstantiator::lookup(clang::NestedNameSpecifier NNS,
                                                             clang::DeclarationName name) {
    // Handle each NestedNameSpecifier kind:
    // - Type: concrete or dependent type used as qualifier (e.g. `vector<T>::`);
    //   dependent chains (`base::type::inner`) are DependentNameTypes resolved
    //   recursively through the type lookup
    // - Global/Namespace/MicrosoftSuper: not dependent, cannot resolve further
    switch(NNS.getKind()) {
        case clang::NestedNameSpecifier::Kind::Type: {
            return lookup(clang::QualType(NNS.getAsType(), 0), name);
        }

        /// Namespaces and the global scope are ordinary declaration
        /// contexts; a plain lookup returns the full overload set.
        case clang::NestedNameSpecifier::Kind::Namespace: {
            return NNS.getAsNamespaceAndPrefix().Namespace->getNamespace()->lookup(name);
        }

        case clang::NestedNameSpecifier::Kind::Global: {
            return context.getTranslationUnitDecl()->lookup(name);
        }

        case clang::NestedNameSpecifier::Kind::Null:
        case clang::NestedNameSpecifier::Kind::MicrosoftSuper: {
            return {};
        }
    }

    return lookup_result();
}

PseudoInstantiator::lookup_result
    PseudoInstantiator::lookup_in_record(clang::CXXRecordDecl* record,
                                         clang::DeclarationName name) {
    auto members = record->lookup(name);
    if(members.empty()) {
        return lookup_in_bases(record, name);
    }
    /// A using-declaration naming a member of a dependent base stands
    /// for what it names there.
    if(members.isSingleResult()) {
        if(auto* named = llvm::dyn_cast<clang::UnresolvedUsingValueDecl>(members.front())) {
            return lookup(named->getQualifier(), name);
        }
        if(auto* named = llvm::dyn_cast<clang::UnresolvedUsingTypenameDecl>(members.front())) {
            return lookup(named->getQualifier(), name);
        }
    }
    return members;
}

PseudoInstantiator::lookup_result PseudoInstantiator::lookup_in_bases(clang::CXXRecordDecl* CRD,
                                                                      clang::DeclarationName name) {
    if(!CRD->hasDefinition()) {
        return lookup_result();
    }

    auto frames = stack.size();
    std::optional<Found> found;
    for(auto& base: CRD->bases()) {
        auto base_frames = stack.size();
        auto members = lookup_in_base(base, name);
        if(members.empty()) {
            stack.truncate(base_frames);
            continue;
        }
        auto here = found_by(members, base_frames);
        if(!found) {
            LOG_DEBUG(
                "{}"
                "found '{}' via base '{}'",
                pad(),
                name.getAsString(),
                base.getType().getAsString());
            found = std::move(here);
            continue;
        }
        stack.truncate(base_frames);
        /// FIXME: no dominance through virtual bases: with B<T> and C<T> both
        /// virtually deriving A<T>, B's `type` hides A's in `D : C<T>, B<T>`, yet the
        /// two paths read as ambiguous here and the lookup answers nothing.
        if(!same_found(*found, here)) {
            LOG_DEBUG(
                "{}"
                "'{}' is ambiguous among the bases",
                pad(),
                name.getAsString());
            note(Trouble::AmbiguousBase);
            stack.truncate(frames);
            return lookup_result();
        }
    }
    return found ? found->members : lookup_result();
}

Found PseudoInstantiator::found_by(lookup_result members, unsigned since) {
    Found found{.members = members};
    auto* owner = llvm::dyn_cast<clang::CXXRecordDecl>(members.front()->getDeclContext());
    const clang::Decl* pattern = nullptr;
    if(owner && llvm::isa<clang::ClassTemplatePartialSpecializationDecl>(owner)) {
        pattern = owner;
    } else if(owner) {
        pattern = owner->getDescribedClassTemplate();
    }
    for(unsigned f = stack.size(); pattern && f > since; f -= 1) {
        auto& frame = stack.frames()[f - 1];
        if(frame.decl->getCanonicalDecl() == pattern->getCanonicalDecl()) {
            found.owner = frame.arguments;
            break;
        }
    }
    if(auto* alias = llvm::dyn_cast<clang::TypedefNameDecl>(members.front());
       alias && members.isSingleResult()) {
        found.aliased = substitute(alias->getUnderlyingType());
    }
    return found;
}

bool PseudoInstantiator::same_found(const Found& lhs, const Found& rhs) {
    if(!same_declarations(lhs.members, rhs.members)) {
        return false;
    }
    if(ranges::equal(
           lhs.owner,
           rhs.owner,
           [&](const clang::TemplateArgument& left, const clang::TemplateArgument& right) {
               return context.isSameTemplateArgument(left, right);
           })) {
        return true;
    }
    return !lhs.aliased.isNull() && !rhs.aliased.isNull() &&
           context.hasSameType(lhs.aliased, rhs.aliased);
}

PseudoInstantiator::lookup_result
    PseudoInstantiator::lookup_in_base(const clang::CXXBaseSpecifier& base,
                                       clang::DeclarationName name) {
    auto type = base.getType();
    if(type->isDependentType()) {
        return lookup(substitute(type), name);
    }
    /// A dependent derived class may still inherit a fixed base; plain
    /// lookup suffices there.
    if(auto* record = type->getAsCXXRecordDecl()) {
        return lookup_in_record(record, name);
    }
    return lookup_result();
}

PseudoInstantiator::lookup_result PseudoInstantiator::lookup(clang::ClassTemplateDecl* CTD,
                                                             clang::DeclarationName name,
                                                             TemplateArguments written) {
    /// Read through the member template it was instantiated from, with the
    /// enclosing specialization's arguments bound.
    if(auto* from = member_pattern(CTD)) {
        auto frames = push_specialization(
            llvm::cast<clang::ClassTemplateSpecializationDecl>(CTD->getDeclContext()));
        auto members = lookup(from, name, written);
        if(members.empty()) {
            stack.truncate(frames);
        }
        return members;
    }

    // Detect recursive lookup of the same CTD + name.
    // e.g. callback_traits<F> : callback_traits<decltype(&F::operator())>
    // would infinitely recurse through lookup_in_bases.
    auto key = std::make_pair(static_cast<const void*>(CTD), name.getAsOpaquePtr());
    if(!looking_up.insert(key).second) {
        /// The empty result here means "already in progress", not "absent";
        /// the pseudo-SFINAE probe must not read it as a proof of absence.
        give_up(Trouble::Cycle);
        return lookup_result();
    }
    auto done = llvm::make_scope_exit([&] { looking_up.erase(key); });

    llvm::SmallVector<clang::TemplateArgument, 4> arguments;
    if(!fill_defaults(CTD, written, arguments)) {
        return lookup_result();
    }

    LOG_DEBUG(
        "{}"
        "lookup '{}' in '{}'",
        pad(),
        name.getAsString(),
        CTD->getNameAsString());
    indent += 1;

    /// An ambiguous or unverified winner degrades to unresolved:
    /// neither a partial nor the primary may be chosen (see
    /// select_partial).
    auto choice = match_partial(CTD, arguments);
    if(choice.result == PartialResult::Ambiguous || choice.result == PartialResult::Unverified) {
        LOG_DEBUG(
            "{}"
            "no certain partial; looking through each",
            pad());
        auto members = lookup_through_any(CTD, arguments, name);
        if(members.empty()) {
            note(Trouble::Undecided);
        }
        indent -= 1;
        return members;
    }
    /// Instantiation reads the pattern it chose alone: a name the chosen
    /// partial specialization lacks is no member, whatever the primary
    /// template declares.
    clang::CXXRecordDecl* pattern = choice.winner;
    Deduction deduction = Deduction::Failed;
    if(pattern) {
        LOG_DEBUG(
            "{}"
            "matched partial '{}'",
            pad(),
            pattern->getNameAsString());
        deduction = deduce_template_arguments(choice.winner, arguments);
    } else {
        LOG_DEBUG("{}using primary template", pad());
        pattern = CTD->getTemplatedDecl();
        deduction = deduce_template_arguments(CTD, arguments);
    }
    if(deduction != Deduction::Failed) {
        if(auto members = lookup_in_record(pattern, name); !members.empty()) {
            indent -= 1;
            return members;
        }
        stack.pop();
    }

    indent -= 1;
    return lookup_result();
}

PseudoInstantiator::lookup_result
    PseudoInstantiator::lookup_through_any(clang::ClassTemplateDecl* CTD,
                                           TemplateArguments arguments,
                                           clang::DeclarationName name) {
    auto frames = stack.size();
    std::optional<Found> common;
    /// `name` in `pattern`, its parameters bound: none when it leaves no
    /// one answer — it finds what the others do not, or may have the name
    /// without the lookup telling —, else what it finds, empty if certainly
    /// nothing.
    auto agrees = [&](clang::CXXRecordDecl* pattern) -> std::optional<lookup_result> {
        auto judged = own_conclusion();
        auto members = lookup_in_record(pattern, name);
        if(members.empty()) {
            return inconclusive || has_dependent_base(pattern) ? std::nullopt
                                                               : std::optional(members);
        }
        auto here = found_by(members, frames);
        if(common && !same_found(*common, here)) {
            return std::nullopt;
        }
        common = std::move(here);
        return members;
    };

    clang::CXXRecordDecl* finding = nullptr;
    auto consider = [&](clang::CXXRecordDecl* pattern) {
        auto members = agrees(pattern);
        stack.truncate(frames);
        if(members && !members->empty() && !finding) {
            finding = pattern;
        }
        return members.has_value();
    };
    for(auto& match: viable_partials(CTD, arguments)) {
        if(deduce_template_arguments(match.partial, arguments) == Deduction::Failed ||
           !consider(match.partial)) {
            return lookup_result();
        }
    }
    for(auto* specialization: undecided_specializations(CTD, arguments)) {
        /// One declared and never defined cannot instantiate at all.
        if(auto* definition = specialization->getDefinition();
           definition && !consider(definition)) {
            return lookup_result();
        }
    }
    if(deduce_template_arguments(CTD, arguments) == Deduction::Failed) {
        return lookup_result();
    }
    auto members = agrees(CTD->getTemplatedDecl());
    if(members && !members->empty()) {
        return *members;
    }
    stack.truncate(frames);
    if(!members || !finding) {
        return lookup_result();
    }
    /// The frames of the lookup in a pattern that finds the name.
    if(auto* partial = llvm::dyn_cast<clang::ClassTemplatePartialSpecializationDecl>(finding)) {
        deduce_template_arguments(partial, arguments);
    }
    return lookup_in_record(finding, name);
}

PseudoInstantiator::lookup_result PseudoInstantiator::lookup_member(clang::QualType object,
                                                                    clang::DeclarationName name) {
    return object.isNull() ? lookup_result() : lookup(object, name);
}

}  // namespace clice::types
