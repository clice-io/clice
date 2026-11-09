module clice;

import :semantic.decls;
import :semantic.template_resolver.deduction;
import :semantic.template_resolver.instantiator;

namespace clice::types {

namespace {

/// Whether `type` holds an `auto` no deduction filled in: a dependent
/// initializer leaves it so until instantiation.
bool undeduced(clang::QualType type) {
    auto* AT = type->getContainedAutoType();
    return AT && AT->getDeducedKind() != clang::DeducedKind::Deduced;
}

/// Whether clang left `type` for evaluation: unknown for a member access,
/// call or subscript on a dependent operand, undeduced for an `auto`
/// variable.
bool unevaluated(clang::QualType type) {
    return type->isSpecificBuiltinType(clang::BuiltinType::Dependent) || undeduced(type);
}

/// Whether `expr` is a call clang left for evaluation: what it returns
/// tells its value category.
bool evaluated_call(const clang::Expr* expr) {
    return llvm::isa<clang::CallExpr>(expr) && unevaluated(expr->getType());
}

/// The operand `expr` dereferences, a `*` a dependent operand left
/// unresolved; null for any other expression.
const clang::Expr* dereferenced(const clang::Expr* expr) {
    if(auto* unary = llvm::dyn_cast<clang::UnaryOperator>(expr);
       unary && unary->getOpcode() == clang::UO_Deref) {
        return unary->getSubExpr();
    }
    if(auto* call = llvm::dyn_cast<clang::CXXOperatorCallExpr>(expr);
       call && call->getOperator() == clang::OO_Star && call->getNumArgs() == 1) {
        return call->getArg(0);
    }
    return nullptr;
}

}  // namespace

std::optional<llvm::APSInt> PseudoInstantiator::evaluate(const clang::Expr* expr) {
    /// Initializers can refer back to each other (or a parameter be
    /// bound to itself); nesting is bounded instead of tracked.
    if(evaluating > 8) {
        give_up(Trouble::Evaluation);
        return std::nullopt;
    }
    evaluating += 1;
    auto value =
        evaluate_integral(context, expr, [&](const clang::Expr* leaf) { return value_of(leaf); });
    evaluating -= 1;
    return value;
}

std::optional<llvm::APSInt> PseudoInstantiator::value_of(const clang::Expr* expr) {
    return unbound_result(cache.values, expr, [&]() -> std::optional<llvm::APSInt> {
        /// A trait asks of the type its operand names: `typename
        /// allocator<T>::_From_primary` is an `allocator<T>`.
        if(auto trait = type_trait(context, expr, [&](clang::QualType operand) {
               return resolve(substitute(operand));
           })) {
            return llvm::APSInt(llvm::APInt(1, *trait ? 1 : 0), /*isUnsigned=*/true);
        }
        if(auto NTTP = referenced_nttp(expr)) {
            auto* bound = bound_argument(NTTP, NTTP->getDepth(), NTTP->getIndex());
            if(bound && bound->getKind() == clang::TemplateArgument::Integral) {
                return bound->getAsIntegral();
            }
            if(bound && bound->getKind() == clang::TemplateArgument::Expression) {
                return evaluate(bound->getAsExpr());
            }
            return std::nullopt;
        }

        if(auto* ULE = llvm::dyn_cast<clang::UnresolvedLookupExpr>(expr);
           ULE && ULE->hasExplicitTemplateArgs() && ULE->getNumDecls() == 1) {
            auto* VTD = llvm::dyn_cast<clang::VarTemplateDecl>(*ULE->decls_begin());
            return VTD ? value_of(VTD, ULE->template_arguments()) : std::nullopt;
        }

        auto judged = own_conclusion();
        auto* DSDRE = llvm::dyn_cast<clang::DependentScopeDeclRefExpr>(expr);
        if(DSDRE && DSDRE->getQualifier().getKind() == clang::NestedNameSpecifier::Kind::Type) {
            auto scope =
                resolve_object(substitute(clang::QualType(DSDRE->getQualifier().getAsType(), 0)));
            if(may_keep_resolution(scope.getTypePtr())) {
                return member_value(scope, DSDRE->getDeclName());
            }
        }

        auto frames = stack.size();
        const clang::Decl* decl = nullptr;
        if(DSDRE) {
            decl = preferred(lookup(DSDRE->getQualifier(), DSDRE->getDeclName()));
        } else if(auto* DRE = llvm::dyn_cast<clang::DeclRefExpr>(expr)) {
            decl = DRE->getDecl();
        }
        auto value = declared_value(decl);
        stack.truncate(frames);
        return value;
    });
}

std::optional<llvm::APSInt> PseudoInstantiator::member_value(clang::QualType scope,
                                                             clang::DeclarationName name) {
    std::pair key{scope.getCanonicalType().getAsOpaquePtr(), name.getAsOpaquePtr()};
    if(auto cut = cut_values.find(key);
       cut != cut_values.end() && cut->second.holds_at(evaluating, depth)) {
        inconclusive = true;
        return std::nullopt;
    }
    auto cycles = troubles[static_cast<std::size_t>(Trouble::Cycle)];
    auto value = kept_result(cache.member_values, key, [&] {
        auto outer = std::exchange(stack.frames(), {});
        auto value = declared_value(preferred(lookup(scope, name)));
        stack.frames() = std::move(outer);
        return value;
    });
    /// Where a cycle was cut, what is in progress decides, not the nesting.
    if(!value && inconclusive && troubles[static_cast<std::size_t>(Trouble::Cycle)] == cycles) {
        cut_values.insert_or_assign(key, CutFolding{.evaluating = evaluating, .depth = depth});
    }
    return value;
}

std::optional<llvm::APSInt> PseudoInstantiator::declared_value(const clang::Decl* decl) {
    if(auto* var = llvm::dyn_cast_or_null<clang::VarDecl>(decl)) {
        return initial_value(var);
    }
    auto* enumerator = llvm::dyn_cast_or_null<clang::EnumConstantDecl>(decl);
    if(!enumerator) {
        return std::nullopt;
    }
    /// Sema leaves the enumerators of an enumeration inside a template
    /// uncomputed.
    if(!enumerator->getType()->isDependentType()) {
        return enumerator->getInitVal();
    }
    auto* init = enumerator->getInitExpr();
    return init ? evaluate(init) : std::nullopt;
}

std::optional<llvm::APSInt> PseudoInstantiator::initial_value(const clang::VarDecl* var) {
    /// A member of a class template's specialization takes its initializer
    /// once a use needs it (`false_type::value`); until then it is the
    /// pattern's, under the specialization's arguments.
    auto* specialization =
        llvm::dyn_cast<clang::ClassTemplateSpecializationDecl>(var->getDeclContext());
    if(auto* pattern = var->getInstantiatedFromStaticDataMember();
       pattern && specialization && !var->getInit()) {
        auto frames = push_specialization(specialization);
        auto value = initial_value(pattern);
        stack.truncate(frames);
        return value;
    }
    if(!var->getInit() || !var->mightBeUsableInConstantExpressions(context)) {
        return std::nullopt;
    }
    auto value = evaluate(var->getInit());
    /// Sema converts the initializer only once the variable's type is
    /// known (`static constexpr I invalid = -1`); an `auto` variable
    /// takes the initializer's own type.
    if(value && !var->getType()->getContainedAutoType()) {
        auto type = substitute(var->getType());
        value = type->isDependentType() ? std::nullopt : convert_integral(context, *value, type);
    }
    return value;
}

std::optional<llvm::APSInt>
    PseudoInstantiator::value_of(clang::VarTemplateDecl* VTD,
                                 llvm::ArrayRef<clang::TemplateArgumentLoc> written) {
    if(auto* definition = VTD->getDefinition()) {
        VTD = definition;
    }
    llvm::SmallVector<clang::TemplateArgument, 4> spelled;
    for(auto& argument: written) {
        spelled.push_back(argument.getArgument());
    }
    llvm::SmallVector<clang::TemplateArgument, 4> substituted;
    rewrite_arguments(spelled, substituted, Policy::Substitute);
    llvm::SmallVector<clang::TemplateArgument, 4> rewritten;
    rewrite_arguments(substituted, rewritten, Policy::Resolve);
    /// A member typedef expands by substitution alone (`key_type` to
    /// `typename _Traits::key_type`); the patterns match what it names.
    for(auto& argument: rewritten) {
        if(argument.getKind() == clang::TemplateArgument::Type) {
            argument = clang::TemplateArgument(resolve(argument.getAsType()));
        }
    }
    /// Types and templates, resolved, hold all the caller's bindings say of
    /// them; a value written as an expression reads them still.
    std::vector<const void*> key{VTD};
    for(auto& argument: rewritten) {
        if(argument.getKind() == clang::TemplateArgument::Type) {
            key.push_back(argument.getAsType().getAsOpaquePtr());
        } else if(argument.getKind() == clang::TemplateArgument::Template) {
            key.push_back(argument.getAsTemplate().getAsVoidPointer());
        } else {
            return value_of(VTD, rewritten);
        }
    }
    return kept_result(cache.variable_values, key, [&] {
        auto outer = std::exchange(stack.frames(), {});
        auto value = value_of(VTD, rewritten);
        stack.frames() = std::move(outer);
        return value;
    });
}

std::optional<llvm::APSInt>
    PseudoInstantiator::value_of(clang::VarTemplateDecl* VTD,
                                 llvm::ArrayRef<clang::TemplateArgument> rewritten) {
    llvm::SmallVector<clang::TemplateArgument, 4> arguments;
    if(!fill_defaults(VTD, rewritten, arguments)) {
        return std::nullopt;
    }
    if(ranges::none_of(arguments, &clang::TemplateArgument::isDependent)) {
        auto canonical = canonical_arguments(context, VTD->getTemplateParameters(), arguments);
        void* position = nullptr;
        llvm::SmallVector<clang::VarTemplateSpecializationDecl*, 1> explicit_specialization;
        if(auto* specialization = VTD->findSpecialization(canonical, position);
           specialization && specialization->isExplicitSpecialization()) {
            explicit_specialization.push_back(specialization);
        }
        drop_unseen(explicit_specialization);
        if(!explicit_specialization.empty()) {
            return initial_value(explicit_specialization.front());
        }
    }

    auto frames = stack.size();
    auto value_in = [&](const clang::VarDecl* pattern,
                        Deduction deduction) -> std::optional<llvm::APSInt> {
        if(deduction == Deduction::Failed) {
            return std::nullopt;
        }
        auto value = initial_value(pattern);
        stack.truncate(frames);
        return value;
    };
    auto* primary = VTD->getTemplatedDecl();
    auto choice = match_partial(VTD, arguments);
    if(choice.result == PartialResult::Selected) {
        return value_in(choice.winner, deduce_template_arguments(choice.winner, arguments));
    }
    if(choice.result == PartialResult::None) {
        return value_in(primary, deduce_template_arguments(VTD, arguments));
    }

    /// Where it is not certain which pattern applies, the value every one
    /// that may apply folds to alike.
    std::optional<llvm::APSInt> common;
    auto agrees = [&](std::optional<llvm::APSInt> value) {
        if(!value || (common && !llvm::APSInt::isSameValue(*common, *value))) {
            return false;
        }
        common = value;
        return true;
    };
    for(auto& match: viable_partials(VTD, arguments)) {
        if(!agrees(value_in(match.partial, deduce_template_arguments(match.partial, arguments)))) {
            return std::nullopt;
        }
    }
    for(auto* specialization: undecided_specializations(VTD, arguments)) {
        if(!agrees(initial_value(specialization))) {
            return std::nullopt;
        }
    }
    return agrees(value_in(primary, deduce_template_arguments(VTD, arguments))) ? common
                                                                                : std::nullopt;
}

clang::QualType PseudoInstantiator::type_of(const clang::Expr* expr, bool through_object) {
    expr = expr->IgnoreParenImpCasts();
    return unbound_result(cache.types, std::pair{expr, static_cast<unsigned>(through_object)}, [&] {
        auto frames = stack.size();
        clang::QualType type;
        unsigned object_quals = 0;
        if(auto* pointer = dereferenced(expr)) {
            auto evaluated = operand(pointer);
            type = deref_type(resolve_object(evaluated.type), evaluated.rvalue);
        } else if(auto* call = llvm::dyn_cast<clang::CallExpr>(expr)) {
            type = call_type(call);
        } else if(auto* subscript = llvm::dyn_cast<clang::ArraySubscriptExpr>(expr)) {
            type = subscript_type(subscript);
        } else {
            const clang::Decl* decl = nullptr;
            if(auto* DSME = llvm::dyn_cast<clang::CXXDependentScopeMemberExpr>(expr)) {
                auto object = member_object(DSME);
                if(through_object && !object.isNull()) {
                    object_quals = resolve_object(object).getCVRQualifiers();
                }
                auto member = member_of(object, DSME->getMember());
                decl = member.decl;
                type = member.type;
            } else {
                decl = referenced_decl(expr);
                if(auto* value = llvm::dyn_cast_or_null<clang::ValueDecl>(decl)) {
                    type = value_type(value);
                }
            }
            if(auto* field = llvm::dyn_cast_or_null<clang::FieldDecl>(decl); !field) {
                object_quals = 0;
            } else if(field->isMutable()) {
                object_quals &= ~clang::Qualifiers::Const;
            }
        }
        stack.truncate(frames);
        if(type.isNull()) {
            return type;
        }
        /// FIXME: an `auto` member's deduced type is already in the caller's terms;
        /// under a frame of the class's own specialization it binds twice
        /// (`Holder<W<T>>::from_call` in `Holder<T>` reads `W<W<W<T>>>`). An empty
        /// stack fixes that but loses what a deferred `decltype` return type needs.
        type = resolve(type);
        return type->isReferenceType() ? type : type.withCVRQualifiers(object_quals);
    });
}

Member PseudoInstantiator::member_of(clang::QualType object, clang::DeclarationName name) {
    auto resolved = object.isNull() ? object : resolve_object(object);
    if(resolved.isNull()) {
        return {};
    }
    /// What the object still names unbound stays so: under no bindings of
    /// the caller's, the answer is the resolved object's alone.
    return kept_result(cache.members,
                       std::pair{resolved.getAsOpaquePtr(), name.getAsOpaquePtr()},
                       [&] {
                           auto outer = std::exchange(stack.frames(), {});
                           Member member{.decl = preferred(lookup_member(resolved, name))};
                           if(auto* value = llvm::dyn_cast_or_null<clang::ValueDecl>(member.decl)) {
                               member.type = value_type(value);
                           }
                           stack.frames() = std::move(outer);
                           return member;
                       });
}

clang::QualType PseudoInstantiator::value_type(const clang::ValueDecl* value) {
    auto type = value->getType();
    /// A function parameter pack (`Box<Ts>... boxes`) is declared with the
    /// expansion; a use of it has the pattern's type.
    if(auto* PET = type->getAs<clang::PackExpansionType>()) {
        type = PET->getPattern();
    }
    auto* var = llvm::dyn_cast<clang::VarDecl>(value);
    return var && (var->getInit() || var->isCXXForRangeDecl()) && undeduced(type)
               ? deduce_auto(var)
               : substitute(type);
}

clang::QualType PseudoInstantiator::expr_type(const clang::Expr* expr) {
    auto type = expr->getType();
    return unevaluated(type) ? type_of(expr, /*through_object=*/true) : type;
}

clang::QualType PseudoInstantiator::deduce_auto(const clang::VarDecl* var) {
    /// The initializer can name the variable again through another
    /// specialization (`Depth<N>::value` initialized from
    /// `Depth<N - 1>::value`).
    if(!deducing.insert(var).second) {
        give_up(Trouble::Cycle);
        return clang::QualType();
    }

    /// Clang gives the variable of a range-based `for` over a dependent
    /// range no initializer.
    const clang::Expr* init = var->getInit();
    if(!init) {
        auto element = var->isCXXForRangeDecl() ? loop_element(var) : clang::QualType();
        auto type = element.isNull()
                        ? element
                        : bind_auto(var->getType(), element, element->isLValueReferenceType());
        deducing.erase(var);
        return type;
    }

    /// Direct initialization deduces from its one expression; a braced
    /// list after `=` makes an `initializer_list`, which is not modeled.
    if(auto* list = llvm::dyn_cast<clang::ParenListExpr>(init)) {
        init = list->getNumExprs() == 1 ? list->getExpr(0) : nullptr;
    } else if(auto* list = llvm::dyn_cast<clang::InitListExpr>(init)) {
        bool direct = var->getInitStyle() == clang::VarDecl::ListInit;
        init = direct && list->getNumInits() == 1 ? list->getInit(0) : nullptr;
    }

    auto type = init ? deduce(var->getType(), init) : clang::QualType();
    deducing.erase(var);
    return type;
}

clang::QualType PseudoInstantiator::deduce(clang::QualType declared, const clang::Expr* init) {
    bool evaluated = unevaluated(init->getType());
    bool decltype_auto = declared->getContainedAutoType()->isDecltypeAuto();
    /// `decltype(auto)` takes what `decltype(init)` names: a data
    /// member's declared type, not the object's qualifiers on it, unless
    /// parenthesized, which names the lvalue the expression is.
    bool parenthesized = llvm::isa<clang::ParenExpr>(init);
    clang::QualType source;
    if(evaluated || (decltype_auto && !parenthesized)) {
        source = type_of(init, /*through_object=*/!decltype_auto || parenthesized);
    }
    /// An evaluated type is in the caller's terms already; the written
    /// one is in those of the variable's class, which the frames of the
    /// lookup that found the variable translate.
    if(source.isNull() && !evaluated) {
        source = substitute(init->getType());
    }
    if(source.isNull()) {
        return source;
    }
    if(decltype_auto) {
        if(!parenthesized || source->isReferenceType()) {
            return source;
        }
        if(xvalue(init)) {
            return context.getRValueReferenceType(source);
        }
        return init->isLValue() ? context.getLValueReferenceType(source) : source;
    }

    /// Clang cannot tell what a dependent call or subscript yields, nor
    /// what a cast to a dependent reference type collapses to
    /// (`static_cast<T&&>` with `T = int&`); the type says.
    bool opaque =
        evaluated && llvm::isa<clang::CallExpr, clang::ArraySubscriptExpr>(init->IgnoreParens());
    return bind_auto(declared,
                     source,
                     source->isLValueReferenceType() ||
                         (!opaque && init->isLValue() && !xvalue(init)));
}

bool PseudoInstantiator::xvalue(const clang::Expr* expr) {
    expr = expr->IgnoreParens();
    if(auto* access = llvm::dyn_cast<clang::CXXDependentScopeMemberExpr>(expr)) {
        if(access->isArrow() || access->isImplicitAccess()) {
            return false;
        }
        auto base = operand(access->getBase());
        if(!base.rvalue) {
            return false;
        }
        auto frames = stack.size();
        auto* field = llvm::dyn_cast_if_present<clang::FieldDecl>(
            preferred(lookup_member(base.type, access->getMember())));
        /// A reference member names what it refers to, an lvalue: one of a
        /// type a parameter binds to a reference too.
        auto type = field ? substitute(field->getType()) : clang::QualType();
        stack.truncate(frames);
        return !type.isNull() && !type->isReferenceType();
    }
    return expr->isXValue();
}

clang::QualType PseudoInstantiator::bind_auto(clang::QualType declared,
                                              clang::QualType source,
                                              bool lvalue) {
    auto argument = source.getNonReferenceType();
    if(auto* RRT = declared->getAs<clang::RValueReferenceType>()) {
        auto pointee = RRT->getPointeeType();
        if(lvalue && llvm::isa<clang::AutoType>(pointee) && !pointee.hasLocalQualifiers()) {
            return context.getLValueReferenceType(argument);
        }
    } else if(!declared->isReferenceType()) {
        /// Decayed first: an array's qualifiers are its elements'.
        argument = context.getAdjustedParameterType(argument).getUnqualifiedType();
    }
    return replace_auto(declared, argument);
}

clang::QualType PseudoInstantiator::loop_element(const clang::VarDecl* var) {
    auto* function = llvm::dyn_cast_or_null<clang::FunctionDecl>(var->getParentFunctionOrMethod());
    auto* body = function ? function->getBody() : nullptr;
    if(!body) {
        return clang::QualType();
    }

    struct Finder : clang::RecursiveASTVisitor<Finder> {
        const clang::VarDecl* var;
        const clang::CXXForRangeStmt* found = nullptr;

        bool VisitCXXForRangeStmt(clang::CXXForRangeStmt* loop) {
            if(loop->getLoopVariable() == var) {
                found = loop;
            }
            return !found;
        }
    } finder{.var = var};

    finder.TraverseStmt(body);
    if(!finder.found || !finder.found->getRangeInit()) {
        return clang::QualType();
    }

    auto* range_init = finder.found->getRangeInit();
    auto range = resolve_object(expr_type(range_init));
    if(range.isNull()) {
        return range;
    }
    if(auto* array = context.getAsArrayType(range)) {
        return context.getLValueReferenceType(array->getElementType());
    }
    auto begin = context.DeclarationNames.getIdentifier(&context.Idents.get("begin"));
    auto end = context.DeclarationNames.getIdentifier(&context.Idents.get("end"));
    auto members = lookup(range, begin);
    clang::QualType iterator;
    if(!members.empty() || !lookup(range, end).empty()) {
        llvm::SmallVector<const clang::NamedDecl*, 4> candidates(members.begin(), members.end());
        drop_by_arity(candidates, 0, /*member_call=*/true);
        /// `begin()` is called on the variable the range initializes.
        select_by_object(candidates, range);
        iterator = common_return_type(candidates, {});
    } else if(auto found = argument_dependent_lookup(begin,
                                                     {
                                                         {.type = range, .lvalue = true}
    })) {
        /// A class without member `begin` or `end` is iterated through the
        /// `begin(range)` argument-dependent lookup finds.
        iterator = common_return_type(*found,
                                      {
        },
                                      {{.type = range, .lvalue = true}});
    }
    return iterator.isNull() ? iterator : deref_type(resolve_object(iterator), false);
}

clang::QualType PseudoInstantiator::replace_auto(clang::QualType declared,
                                                 clang::QualType argument) {
    auto quals = declared.getLocalQualifiers();
    auto* T = declared.getTypePtr();
    if(llvm::isa<clang::AutoType>(T)) {
        return context.getQualifiedType(argument, quals);
    }

    clang::QualType result;
    if(auto* PT = llvm::dyn_cast<clang::PointerType>(T)) {
        auto* pointer = argument->getAs<clang::PointerType>();
        if(!pointer) {
            return clang::QualType();
        }
        result = replace_auto(PT->getPointeeType(), pointer->getPointeeType());
        if(!result.isNull()) {
            result = context.getQualifiedType(context.getPointerType(result), quals);
        }
    } else if(auto* LRT = llvm::dyn_cast<clang::LValueReferenceType>(T)) {
        result = replace_auto(LRT->getPointeeType(), argument);
        if(!result.isNull()) {
            result = context.getLValueReferenceType(result);
        }
    } else if(auto* RRT = llvm::dyn_cast<clang::RValueReferenceType>(T)) {
        result = replace_auto(RRT->getPointeeType(), argument);
        if(!result.isNull()) {
            result = context.getRValueReferenceType(result);
        }
    }
    return result;
}

clang::QualType PseudoInstantiator::subscript_type(const clang::ArraySubscriptExpr* expr) {
    auto array = operand(expr->getLHS());
    auto object = resolve_object(array.type);
    if(object.isNull()) {
        return object;
    }
    if(auto* PT = object->getAs<clang::PointerType>()) {
        return context.getLValueReferenceType(PT->getPointeeType());
    }
    /// An element of an xvalue array is an xvalue.
    if(auto* AT = context.getAsArrayType(object)) {
        return xvalue(expr->getLHS()) ? context.getRValueReferenceType(AT->getElementType())
                                      : context.getLValueReferenceType(AT->getElementType());
    }

    auto subscript = context.DeclarationNames.getCXXOperatorName(clang::OO_Subscript);
    auto members = lookup(object, subscript);
    llvm::SmallVector<const clang::NamedDecl*, 4> candidates(members.begin(), members.end());
    drop_by_arity(candidates, 1, /*member_call=*/true);
    select_by_object(candidates, object, array.rvalue);
    return common_return_type(candidates, {});
}

clang::QualType PseudoInstantiator::deref_type(clang::QualType object, bool rvalue) {
    if(object.isNull()) {
        return object;
    }
    if(auto* PT = object->getAs<clang::PointerType>()) {
        return context.getLValueReferenceType(PT->getPointeeType());
    }
    auto star = context.DeclarationNames.getCXXOperatorName(clang::OO_Star);
    auto members = lookup(object, star);
    llvm::SmallVector<const clang::NamedDecl*, 4> candidates(members.begin(), members.end());
    drop_by_arity(candidates, 0, /*member_call=*/true);
    select_by_object(candidates, object, rvalue);
    if(auto found = argument_dependent_lookup(star,
                                              {
                                                  {.type = object, .lvalue = !rvalue}
    })) {
        candidates.append(found->begin(), found->end());
    }
    return common_return_type(candidates,
                              {
    },
                              {{.type = object, .lvalue = !rvalue}},
                              /*operands=*/true);
}

bool PseudoInstantiator::rvalue(const clang::Expr* base) {
    base = base->IgnoreParenImpCasts();
    if(evaluated_call(base)) {
        auto type = type_of(base);
        return !type.isNull() && !type->isLValueReferenceType();
    }
    return !base->isLValue() || xvalue(base);
}

Operand PseudoInstantiator::operand(const clang::Expr* expr) {
    auto type = expr_type(expr);
    if(evaluated_call(expr->IgnoreParenImpCasts())) {
        return {.type = type, .rvalue = !type.isNull() && !type->isLValueReferenceType()};
    }
    return {.type = type, .rvalue = rvalue(expr)};
}

clang::QualType PseudoInstantiator::call_type(const clang::CallExpr* call) {
    auto* callee = call->getCallee()->IgnoreParenImpCasts();
    auto callees = call_candidates(call);
    if(!callees.complete) {
        return clang::QualType();
    }
    return common_return_type(callees.candidates,
                              decls::explicit_template_arguments(callee),
                              callees.arguments ? llvm::ArrayRef<CallArgument>(*callees.arguments)
                                                : llvm::ArrayRef<CallArgument>());
}

clang::QualType PseudoInstantiator::common_return_type(
    llvm::ArrayRef<const clang::NamedDecl*> candidates,
    llvm::ArrayRef<clang::TemplateArgumentLoc> template_arguments,
    llvm::ArrayRef<CallArgument> arguments,
    bool operands) {
    clang::QualType common;
    for(auto* candidate: candidates) {
        auto* method = llvm::dyn_cast_or_null<clang::CXXMethodDecl>(as_function(candidate));
        bool object = operands && method && method->isImplicitObjectMemberFunction();
        auto type =
            return_type(candidate, template_arguments, object ? arguments.drop_front() : arguments);
        if(auto* function = as_function(candidate);
           type.isNull() && function && template_arguments.empty() &&
           function->getDescribedFunctionTemplate() &&
           function->getReturnType()->getAs<clang::DecltypeType>()) {
            continue;
        }
        if(type.isNull() || (!common.isNull() && !context.hasSameType(type, common))) {
            return clang::QualType();
        }
        common = type;
    }
    return common;
}

clang::QualType
    PseudoInstantiator::return_type(const clang::NamedDecl* candidate,
                                    llvm::ArrayRef<clang::TemplateArgumentLoc> template_arguments,
                                    llvm::ArrayRef<CallArgument> arguments) {
    auto* function = as_function(candidate);
    if(!function) {
        return clang::QualType();
    }
    /// Lookup finds a member defined out of line as that definition,
    /// whose template parameters are its own; the declaration in the
    /// class is written in the class's.
    function = function->getCanonicalDecl();
    auto* FTD = function->getDescribedFunctionTemplate();
    if(!FTD) {
        return substitute(function->getReturnType());
    }

    auto* params = FTD->getTemplateParameters();
    llvm::SmallVector<clang::TemplateArgument, 4> bound;
    /// Explicit arguments reaching a pack parameter would need grouping;
    /// leave them all to the unbound case below.
    auto pack = ranges::find_if(*params, &clang::NamedDecl::isTemplateParameterPack);
    if(template_arguments.size() <= static_cast<std::size_t>(pack - params->begin())) {
        for(auto& argument: template_arguments) {
            bound.push_back(argument.getArgument());
        }
        /// The call deduces what the explicit arguments leave out, before
        /// any default applies; none stands in for a deduction that fails.
        bool deduced = true;
        if(!arguments.empty() && bound.size() < params->size()) {
            auto from_call = deduce_call(FTD, function, arguments);
            deduced = !from_call.empty();
            for(auto i = bound.size(); i < from_call.size(); i += 1) {
                if(!from_call[i].isNull()) {
                    bound.push_back(from_call[i]);
                    continue;
                }
                /// One the call leaves undeduced takes its default, which
                /// may read those before it.
                llvm::SmallVector<clang::TemplateArgument, 4> completed;
                fill_defaults(FTD, bound, completed);
                if(completed.size() <= i) {
                    break;
                }
                bound.push_back(completed[i]);
            }
        }
        if(llvm::SmallVector<clang::TemplateArgument, 4> completed;
           deduced && fill_defaults(FTD, bound, completed)) {
            bound = std::move(completed);
        }
    }
    /// Unbound, they would be resolved through the rest of the stack:
    /// a `decltype` of a call on them names whatever that call returns
    /// on anything.
    if(bound.empty() && mentions_parameters(function->getReturnType(), params)) {
        return clang::QualType();
    }
    stack.push(FTD, params, bound);
    auto type = substitute(function->getReturnType());
    return mentions_parameters(type, params) ? clang::QualType() : type;
}

std::optional<llvm::SmallVector<CallArgument, 4>>
    PseudoInstantiator::call_arguments(llvm::ArrayRef<const clang::Expr*> arguments) {
    if(ranges::any_of(arguments, llvm::IsaPred<clang::PackExpansionExpr>)) {
        return std::nullopt;
    }
    llvm::SmallVector<CallArgument, 4> passed;
    for(auto* argument: arguments) {
        auto evaluated = operand(argument);
        auto type = resolve(evaluated.type);
        if(type.isNull()) {
            return std::nullopt;
        }
        /// An expression of lvalue reference type, resolved, is an lvalue:
        /// `static_cast<T&&>(v)` with `T = Item&` collapses.
        bool lvalue = type->isLValueReferenceType() || !evaluated.rvalue;
        passed.push_back({.type = type.getNonReferenceType(), .lvalue = lvalue, .expr = argument});
    }
    return passed;
}

const clang::Decl* PseudoInstantiator::referenced_decl(const clang::Expr* expr) {
    if(auto* DSME = llvm::dyn_cast<clang::CXXDependentScopeMemberExpr>(expr)) {
        return preferred(lookup_member(member_object(DSME), DSME->getMember()));
    }
    if(auto* DSDRE = llvm::dyn_cast<clang::DependentScopeDeclRefExpr>(expr)) {
        return preferred(lookup(DSDRE->getQualifier(), DSDRE->getDeclName()));
    }
    if(auto* DRE = llvm::dyn_cast<clang::DeclRefExpr>(expr)) {
        return DRE->getDecl();
    }
    if(auto* ME = llvm::dyn_cast<clang::MemberExpr>(expr)) {
        return ME->getMemberDecl();
    }
    return nullptr;
}

clang::QualType PseudoInstantiator::arrow_pointee(clang::QualType type) {
    auto arrow = context.DeclarationNames.getCXXOperatorName(clang::OO_Arrow);
    for(unsigned hop = 0; hop < 8; hop += 1) {
        type = resolve_object(type);
        if(auto* PT = type->getAs<clang::PointerType>()) {
            return PT->getPointeeType();
        }
        auto frames = stack.size();
        auto members = lookup(type, arrow);
        llvm::SmallVector<const clang::NamedDecl*, 4> candidates(members.begin(), members.end());
        select_by_object(candidates, type);
        /// The lookup leaves its deduction frames in place, so a return
        /// type written in the class's own parameters (`T*`, `pointer`)
        /// comes back with the specialization's arguments filled in.
        type = common_return_type(candidates, {});
        stack.truncate(frames);
        if(type.isNull()) {
            return type;
        }
    }
    return clang::QualType();
}

clang::QualType PseudoInstantiator::resolve_object(clang::QualType type) {
    auto resolved = resolve(type);
    return resolved.isNull() ? resolved : resolved.getNonReferenceType();
}

clang::QualType PseudoInstantiator::member_object(clang::QualType base, bool arrow) {
    if(base.isNull() || !arrow) {
        return base;
    }
    return arrow_pointee(base);
}

clang::QualType PseudoInstantiator::member_object(const clang::CXXDependentScopeMemberExpr* expr) {
    auto base = expr->isImplicitAccess() ? expr->getBaseType() : expr_type(expr->getBase());
    return member_object(base, expr->isArrow());
}

}  // namespace clice::types
