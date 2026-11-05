module clice;

import :semantic.template_resolver.deduction;
import :semantic.template_resolver.instantiator;
import :support.logging;

namespace clice::types {

namespace {

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

}  // namespace

unsigned PseudoInstantiator::push_parameters(clang::TemplateDecl* TD, TemplateArguments arguments) {
    auto frames = stack.size();
    auto list = TD->getTemplateParameters();
    if(auto RTD = llvm::dyn_cast<clang::RedeclarableTemplateDecl>(TD)) {
        for(auto redecl: RTD->redecls()) {
            auto params = llvm::cast<clang::TemplateDecl>(redecl)->getTemplateParameters();
            if(params != list) {
                stack.push(TD, params, arguments);
            }
        }
    }
    stack.push(TD, list, arguments);
    return frames;
}

bool PseudoInstantiator::fill_defaults(clang::TemplateDecl* TD,
                                       TemplateArguments arguments,
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
            auto frames = push_parameters(TD, out);
            auto result = substitute(type);
            stack.truncate(frames);

            if(result.isNull()) {
                return false;
            }

            LOG_DEBUG(
                "{}"
                "default arg: '{}' = '{}'",
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
            /// A default naming an earlier parameter (`M = N`) takes that
            /// parameter's already-supplied argument, which may itself
            /// still be dependent.
            if(auto NTTP = referenced_nttp(expr);
               NTTP && NTTP->getIndex() < out.size() && NTTP->getDepth() == list->getDepth()) {
                out.emplace_back(out[NTTP->getIndex()]);
                continue;
            }

            auto frames = push_parameters(TD, out);
            auto value = evaluate(expr);
            stack.truncate(frames);
            if(value) {
                value = convert_integral(context, *value, NTTPD->getType());
            }
            /// A value that does not fold stays the expression: the
            /// patterns it would choose between are then all possible
            /// (see lookup_through_any).
            if(!value) {
                open_defaults.insert(expr);
            }
            out.emplace_back(value ? clang::TemplateArgument(context, *value, NTTPD->getType())
                                   : argument);
            continue;
        }

        if(auto TTPD = llvm::dyn_cast<clang::TemplateTemplateParmDecl>(param);
           TTPD && TTPD->hasDefaultArgument()) {
            auto& fallback = TTPD->getDefaultArgument().getArgument();

            /// A default naming an earlier parameter (`U = TT`) takes
            /// that parameter's already-supplied argument.
            if(fallback.getKind() == clang::TemplateArgument::Template) {
                if(auto prev = llvm::dyn_cast_or_null<clang::TemplateTemplateParmDecl>(
                       fallback.getAsTemplate().getAsTemplateDecl());
                   prev && prev->getDepth() == list->getDepth() && prev->getIndex() < out.size()) {
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

Deduction PseudoInstantiator::deduce_template_arguments(clang::ClassTemplateDecl* CTD,
                                                        TemplateArguments arguments) {
    /// Members are written in the parameters of the definition; a
    /// redeclaration (libstdc++ redeclares `map` after defining it) has
    /// parameters of its own.
    const clang::ClassTemplateDecl* defined = CTD;
    if(auto* definition = CTD->getTemplatedDecl()->getDefinition()) {
        defined = definition->getDescribedClassTemplate();
    }
    auto* list = defined->getTemplateParameters();
    return push_deduction(CTD, list, list->getInjectedTemplateArgs(context), arguments, "primary");
}

Deduction PseudoInstantiator::deduce_template_arguments(
    clang::ClassTemplatePartialSpecializationDecl* partial,
    TemplateArguments arguments) {
    return push_deduction(partial,
                          partial->getTemplateParameters(),
                          partial->getTemplateArgs().asArray(),
                          arguments,
                          "partial");
}

Deduction PseudoInstantiator::deduce_template_arguments(
    clang::VarTemplatePartialSpecializationDecl* partial,
    TemplateArguments arguments) {
    return push_deduction(partial,
                          partial->getTemplateParameters(),
                          partial->getTemplateArgs().asArray(),
                          arguments,
                          "partial");
}

Deduction PseudoInstantiator::deduce_template_arguments(clang::TypeAliasTemplateDecl* alias,
                                                        TemplateArguments arguments) {
    auto* list = alias->getTemplateParameters();
    return push_deduction(alias, list, list->getInjectedTemplateArgs(context), arguments, "alias");
}

Deduction PseudoInstantiator::push_deduction(clang::Decl* decl,
                                             clang::TemplateParameterList* list,
                                             TemplateArguments patterns,
                                             TemplateArguments arguments,
                                             llvm::StringRef kind) {
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
    auto deduction =
        deduce_arguments(context, list, patterns, arguments, deduced, open_defaults.getArrayRef());
    if(deduction == Deduction::Failed) {
        return deduction;
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
        ranges::reverse(stack.frames());
    }

    stack.push(decl, list, deduced);

    LOG_DEBUG("{}deduce {}: {{{}}}", pad(), kind, [&] {
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

    return deduction;
}

void PseudoInstantiator::assume(clang::ClassTemplateDecl* owner,
                                clang::TemplateParameterList* params,
                                TemplateArguments arguments) {
    stack.push(owner, params, arguments);
}

}  // namespace clice::types
