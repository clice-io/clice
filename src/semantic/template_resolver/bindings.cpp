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

unsigned
    PseudoInstantiator::push_specialization(const clang::ClassTemplateSpecializationDecl* spec) {
    auto from = spec->getSpecializedTemplateOrPartial();
    if(auto* partial = from.dyn_cast<clang::ClassTemplatePartialSpecializationDecl*>()) {
        auto frames = stack.size();
        stack.push(partial,
                   partial->getTemplateParameters(),
                   spec->getTemplateInstantiationArgs().asArray());
        return frames;
    }
    return push_parameters(llvm::cast<clang::ClassTemplateDecl*>(from),
                           spec->getTemplateArgs().asArray());
}

bool PseudoInstantiator::fill_defaults(clang::TemplateDecl* TD,
                                       TemplateArguments arguments,
                                       llvm::SmallVectorImpl<clang::TemplateArgument>& out) {
    auto list = TD->getTemplateParameters();
    out.reserve(list->size());
    for(auto [index, argument]: views::enumerate(arguments)) {
        /// A value written as is (`Vec<T, 3>`), or folded from an expression:
        /// a constant one, or under the bindings one whose type waits for
        /// conversion to the parameter's (`is_same_v<T, U>`), which rewriting
        /// the arguments leaves to here; the specialization holds it
        /// converted to the parameter's type.
        auto* NTTP = static_cast<unsigned>(index) < list->size()
                         ? llvm::dyn_cast<clang::NonTypeTemplateParmDecl>(
                               list->getParam(static_cast<unsigned>(index)))
                         : nullptr;
        std::optional<llvm::APSInt> value;
        if(NTTP && !NTTP->isParameterPack()) {
            if(argument.getKind() == clang::TemplateArgument::Integral) {
                value = argument.getAsIntegral();
            } else if(auto* expr = argument.getKind() == clang::TemplateArgument::Expression
                                       ? argument.getAsExpr()
                                       : nullptr;
                      expr && !expr->isValueDependent()) {
                value = expr->getIntegerConstantExpr(context);
            } else if(expr && expr->getType()->isDependentType()) {
                value = evaluate(expr);
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
                open_default(expr);
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

Deduction PseudoInstantiator::deduce_template_arguments(clang::VarTemplateDecl* VTD,
                                                        TemplateArguments arguments) {
    auto* list = VTD->getTemplateParameters();
    return push_deduction(VTD, list, list->getInjectedTemplateArgs(context), arguments, "primary");
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

    /// The templates enclosing `decl` that no frame holds stand for
    /// themselves: their parameters bound to themselves, a canonical one in
    /// the deduced result maps to its own declaration rather than to another
    /// template's at the same depth.
    llvm::SmallVector<std::pair<clang::Decl*, clang::TemplateParameterList*>, 2> enclosing;
    visit_template_decl_contexts(llvm::dyn_cast<clang::Decl>(decl->getDeclContext()),
                                 [&](clang::Decl* decl, clang::TemplateParameterList* params) {
                                     if(!params->empty() &&
                                        !binding(params->getParam(0), params->getDepth(), 0).held) {
                                         enclosing.emplace_back(decl, params);
                                     }
                                 });
    for(auto [outer, params]: llvm::reverse(enclosing)) {
        stack.push(outer, params, params->getInjectedTemplateArgs(context));
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
    stack.push(decl, list, {});
    bool changed = rewrite_arguments(patterns, storage, Policy::Substitute);
    stack.pop();
    return changed ? TemplateArguments(storage) : patterns;
}

namespace {

struct ParameterFinder : clang::RecursiveASTVisitor<ParameterFinder> {
    llvm::function_ref<bool(const clang::NamedDecl*, unsigned, unsigned)> wanted;

    bool found = false;

    bool VisitTemplateTypeParmType(clang::TemplateTypeParmType* T) {
        found |= wanted(T->getDecl(), T->getDepth(), T->getIndex());
        return !found;
    }

    /// A function parameter named in a trailing return type
    /// (`-> decltype(f(key))`) stands for its type.
    bool VisitDeclRefExpr(clang::DeclRefExpr* expr) {
        auto* decl = expr->getDecl();
        if(auto* NTTP = llvm::dyn_cast<clang::NonTypeTemplateParmDecl>(decl)) {
            found |= wanted(NTTP, NTTP->getDepth(), NTTP->getIndex());
        } else if(llvm::isa<clang::ParmVarDecl>(decl)) {
            TraverseType(decl->getType());
        }
        return !found;
    }

    bool TraverseTemplateName(clang::TemplateName name) {
        if(auto* TTP =
               llvm::dyn_cast_or_null<clang::TemplateTemplateParmDecl>(name.getAsTemplateDecl())) {
            found |= wanted(TTP, TTP->getDepth(), TTP->getIndex());
        }
        return !found && RecursiveASTVisitor::TraverseTemplateName(name);
    }
};

/// Whether `type` names a template parameter `wanted` holds of; a canonical
/// type parameter comes with no declaration.
bool names_parameter(
    clang::QualType type,
    llvm::function_ref<bool(const clang::NamedDecl* param, unsigned depth, unsigned index)>
        wanted) {
    ParameterFinder finder;
    finder.wanted = wanted;
    finder.TraverseType(type);
    return finder.found;
}

}  // namespace

bool mentions_parameters(clang::QualType type, const clang::TemplateParameterList* params) {
    return names_parameter(type, [&](const clang::NamedDecl* param, unsigned depth, unsigned) {
        return param ? ranges::contains(*params, param) : depth == params->getDepth();
    });
}

bool PseudoInstantiator::names_only_free_parameters(const clang::Type* type) {
    auto [found, added] = cache.named_parameters.try_emplace(type);
    if(added) {
        llvm::SmallVector<ParameterRef, 2> named;
        names_parameter(clang::QualType(type, 0),
                        [&](const clang::NamedDecl* param, unsigned depth, unsigned index) {
                            if(!ranges::contains(named, ParameterRef{param, depth, index})) {
                                named.push_back({param, depth, index});
                            }
                            return false;
                        });
        found = cache.named_parameters.insert_or_assign(type, std::move(named)).first;
    }
    auto& named = found->second;
    if(!ranges::all_of(named, [&](const ParameterRef& parameter) { return free(parameter); })) {
        return false;
    }
    if(recording) {
        recording->found_free(named);
    }
    return true;
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
        auto decayed = context.getAdjustedParameterType(type).getUnqualifiedType();
        /// A pointer converts to one to a more qualified type.
        if(auto* pointer = parameter.getCanonicalType()->getAs<clang::PointerType>();
           pointer && decayed->isPointerType()) {
            return context.getPointerType(
                context.getQualifiedType(decayed->getPointeeType(),
                                         pointer->getPointeeType().getQualifiers()));
        }
        return decayed;
    };

    llvm::SmallVector<clang::TemplateArgument, 4> patterns;
    llvm::SmallVector<clang::TemplateArgument, 4> types;
    unsigned index = 0;
    for(auto* parameter: function->parameters()) {
        auto type = parameter->getType();
        /// A function parameter pack takes every argument left; one before
        /// other parameters is not deduced, and takes none.
        if(auto* expansion = type->getAs<clang::PackExpansionType>()) {
            if(parameter != function->parameters().back()) {
                continue;
            }
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
        /// One naming no parameter of the template deduces nothing either:
        /// its argument converts.
        if(mentions_parameters(type, params)) {
            patterns.emplace_back(pattern_of(type));
            types.emplace_back(argument_for(type, arguments[index]));
        }
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

namespace {

/// Whether `argument` is `param` itself, as a template's own parameters bind
/// it inside it (TemplateParameterList::getInjectedTemplateArgs).
bool names_itself(const clang::TemplateArgument& argument, const clang::NamedDecl* param) {
    switch(argument.getKind()) {
        case clang::TemplateArgument::Type: {
            auto type = argument.getAsType();
            if(auto* expansion = type->getAs<clang::PackExpansionType>()) {
                type = expansion->getPattern();
            }
            auto* TTPT = type->getAs<clang::TemplateTypeParmType>();
            return TTPT && TTPT->getDecl() == param;
        }
        case clang::TemplateArgument::Expression: {
            auto* expr = argument.getAsExpr();
            if(auto* expansion = llvm::dyn_cast<clang::PackExpansionExpr>(expr)) {
                expr = expansion->getPattern();
            }
            auto* reference = llvm::dyn_cast<clang::DeclRefExpr>(expr->IgnoreImpCasts());
            return reference && reference->getDecl() == param;
        }
        case clang::TemplateArgument::Template:
        case clang::TemplateArgument::TemplateExpansion:
            return argument.getAsTemplateOrTemplatePattern().getAsTemplateDecl() == param;
        /// A pack deduced from its own expansion holds that expansion alone.
        case clang::TemplateArgument::Pack:
            return argument.pack_size() == 1 && names_itself(argument.pack_begin()[0], param);
        default: return false;
    }
}

}  // namespace

PseudoInstantiator::Binding PseudoInstantiator::binding(const clang::NamedDecl* param,
                                                        unsigned depth,
                                                        unsigned index) {
    if(auto frame = stack.holder(param, depth, index)) {
        auto& arguments = stack.frames()[*frame].arguments;
        auto* argument = index < arguments.size() ? &arguments[index] : nullptr;
        if(argument && param && names_itself(*argument, param)) {
            if(recording) {
                recording->found_free({
                    {param, depth, index}
                });
            }
        } else {
            read_frame(*frame);
        }
        return {.argument = argument, .held = true};
    }
    if(auto* argument = only_instantiation_argument(param, index)) {
        return {.argument = argument};
    }
    if(recording) {
        recording->found_free({
            {param, depth, index}
        });
    }
    return {};
}

bool PseudoInstantiator::free(const ParameterRef& parameter) const {
    auto frame = stack.holder(parameter.decl, parameter.depth, parameter.index);
    if(!frame) {
        return !only_instantiation_argument(parameter.decl, parameter.index);
    }
    auto& arguments = stack.data[*frame].arguments;
    return parameter.decl && parameter.index < arguments.size() &&
           names_itself(arguments[parameter.index], parameter.decl);
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
