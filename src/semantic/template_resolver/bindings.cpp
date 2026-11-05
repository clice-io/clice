module clice;

import :semantic.decls;
import :semantic.template_resolver;
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
    for(auto [index, argument]: views::enumerate(arguments)) {
        /// A value written as is (`Vec<T, 3>`), or folded from an expression
        /// with the expression's type; the specialization holds it converted
        /// to the parameter's.
        auto* NTTP = static_cast<unsigned>(index) < list->size()
                         ? llvm::dyn_cast<clang::NonTypeTemplateParmDecl>(
                               list->getParam(static_cast<unsigned>(index)))
                         : nullptr;
        std::optional<llvm::APSInt> value;
        if(NTTP && !NTTP->isParameterPack()) {
            if(argument.getKind() == clang::TemplateArgument::Integral) {
                value = argument.getAsIntegral();
            } else if(argument.getKind() == clang::TemplateArgument::Expression &&
                      !argument.getAsExpr()->isValueDependent()) {
                value = argument.getAsExpr()->getIntegerConstantExpr(context);
            }
        }
        if(auto converted =
               value ? convert_integral(context, *value, NTTP->getType()) : std::nullopt) {
            out.emplace_back(context, *converted, NTTP->getType());
            continue;
        }
        out.emplace_back(argument);
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
    llvm::SmallVector<clang::TemplateArgument, 4> substituted;
    patterns = substitute_outer(decl, list, patterns, substituted);

    llvm::SmallVector<clang::TemplateArgument, 4> deduced;
    auto deduction = deduce_arguments(context,
                                      list,
                                      patterns,
                                      arguments,
                                      deduced,
                                      {.open = open_defaults.getArrayRef()});
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

PseudoInstantiator::TemplateArguments
    PseudoInstantiator::substitute_outer(clang::Decl* decl,
                                         clang::TemplateParameterList* list,
                                         TemplateArguments patterns,
                                         llvm::SmallVectorImpl<clang::TemplateArgument>& storage) {
    /// The parameters being deduced must stay free, yet an enclosing
    /// in-flight deduction of this same template may have them bound on
    /// the stack (recursive chains like allocator_traits' rebind). An
    /// empty-argument frame masks them: decl matching stops at it and
    /// reports "unbound", while enclosing templates' frames still apply.
    if(stack.empty()) {
        return patterns;
    }
    stack.push(decl, list, {});
    bool changed = rewrite_arguments(patterns, storage, Policy::Substitute);
    stack.pop();
    return changed ? TemplateArguments(storage) : patterns;
}

llvm::SmallVector<clang::TemplateArgument, 4>
    PseudoInstantiator::deduce_call(clang::FunctionTemplateDecl* FTD,
                                    const clang::FunctionDecl* function,
                                    llvm::ArrayRef<CallArgument> arguments) {
    if(auto* method = llvm::dyn_cast<clang::CXXMethodDecl>(function);
       method && method->isExplicitObjectMemberFunction()) {
        return {};
    }
    auto* params = FTD->getTemplateParameters();
    /// `T&&` with `T` a parameter of the template, unqualified.
    auto forwarding = [&](clang::QualType parameter) {
        auto* reference = parameter->getAs<clang::RValueReferenceType>();
        if(!reference) {
            return false;
        }
        auto pointee = reference->getPointeeType();
        auto* TTPT = pointee->getAs<clang::TemplateTypeParmType>();
        return TTPT && TTPT->getDepth() == params->getDepth() &&
               !pointee.getCanonicalType().hasQualifiers();
    };
    /// The type deduction matches a parameter's arguments against.
    auto pattern_of = [&](clang::QualType parameter) {
        if(auto* reference = parameter->getAs<clang::ReferenceType>()) {
            return reference->getPointeeType();
        }
        return parameter.getUnqualifiedType();
    };
    /// What deduction matches against that pattern for `argument`.
    auto argument_for = [&](clang::QualType parameter, const CallArgument& argument) {
        auto type = argument.type.getNonReferenceType();
        if(forwarding(parameter)) {
            return argument.lvalue ? context.getLValueReferenceType(type) : type;
        }
        /// A reference binds an argument less qualified than what it
        /// refers to.
        if(parameter->isReferenceType()) {
            return context.getQualifiedType(
                type,
                pattern_of(parameter).getCanonicalType().getQualifiers());
        }
        return context.getAdjustedParameterType(type).getUnqualifiedType();
    };

    llvm::SmallVector<clang::TemplateArgument, 4> patterns;
    llvm::SmallVector<clang::TemplateArgument, 4> types;
    unsigned index = 0;
    for(auto* parameter: function->parameters()) {
        auto type = parameter->getType();
        /// A function parameter pack takes every argument left.
        if(auto* expansion = type->getAs<clang::PackExpansionType>()) {
            auto element = expansion->getPattern();
            for(; index < arguments.size(); index += 1) {
                types.emplace_back(argument_for(element, arguments[index]));
            }
            patterns.emplace_back(context.getPackExpansionType(pattern_of(element), std::nullopt));
            break;
        }
        /// A defaulted parameter the call leaves out deduces nothing.
        if(index == arguments.size()) {
            break;
        }
        patterns.emplace_back(pattern_of(type));
        types.emplace_back(argument_for(type, arguments[index]));
        index += 1;
    }

    llvm::SmallVector<clang::TemplateArgument, 4> substituted;
    auto written = substitute_outer(FTD, params, patterns, substituted);
    llvm::SmallVector<clang::TemplateArgument, 4> deduced;
    if(deduce_arguments(context,
                        params,
                        written,
                        types,
                        deduced,
                        {.open = open_defaults.getArrayRef(), .partial = true}) ==
       Deduction::Failed) {
        return {};
    }
    return deduced;
}

const clang::TemplateArgument* PseudoInstantiator::bound_argument(const clang::NamedDecl* param,
                                                                  unsigned depth,
                                                                  unsigned index) const {
    if(stack.owns(param, depth)) {
        return stack.find_argument(param, depth, index);
    }
    return only_instantiation_argument(param, index);
}

const clang::TemplateArgument*
    PseudoInstantiator::only_instantiation_argument(const clang::NamedDecl* param,
                                                    unsigned index) const {
    if(!options.only_instantiation || !param) {
        return nullptr;
    }
    /// A template parameter belongs to the function its template adopted
    /// it into; the parameters of any other template sit elsewhere.
    auto* function = llvm::dyn_cast<clang::FunctionDecl>(param->getDeclContext());
    /// A captureless generic lambda's conversion to a function pointer
    /// and its static invoker adopt the call operator's parameter list
    /// after it, but calls instantiate the call operator.
    if(auto* method = llvm::dyn_cast_or_null<clang::CXXMethodDecl>(function);
       method && method->getParent()->isLambda()) {
        function = method->getParent()->getLambdaCallOperator();
    }
    if(!function) {
        return nullptr;
    }
    auto* instantiation = llvm::dyn_cast_or_null<clang::FunctionDecl>(
        decls::only_instantiation(const_cast<clang::FunctionDecl*>(function)));
    if(!instantiation) {
        return nullptr;
    }
    return &instantiation->getTemplateSpecializationArgs()->get(index);
}

void PseudoInstantiator::assume(clang::ClassTemplateDecl* owner,
                                clang::TemplateParameterList* params,
                                TemplateArguments arguments) {
    stack.push(owner, params, arguments);
}

}  // namespace clice::types
