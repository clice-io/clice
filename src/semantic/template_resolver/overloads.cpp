module clice;

import :semantic.decls;
import :semantic.template_resolver.instantiator;
import :semantic.types;

namespace clice::types {

namespace {

/// The arguments a call passing `count` gives `FD`: a member-syntax call
/// does not spell the explicit object argument (`s.foo(1)` with
/// `foo(this S&, int)`), but the declaration counts it.
unsigned passed(const clang::FunctionDecl* FD, unsigned count, bool member_call) {
    if(auto method = llvm::dyn_cast<clang::CXXMethodDecl>(FD);
       member_call && method && method->isExplicitObjectMemberFunction()) {
        return count + 1;
    }
    return count;
}

/// Does `FD` take as many as `count` arguments? C-style variadics and
/// parameter packs lift the maximum.
bool takes(const clang::FunctionDecl* FD, unsigned count) {
    if(count <= FD->getNumParams() || FD->isVariadic()) {
        return true;
    }
    return ranges::any_of(FD->parameters(),
                          [](const clang::ParmVarDecl* param) { return param->isParameterPack(); });
}

/// Can `FD` accept a call with `count` arguments? Default arguments
/// lower the minimum.
bool arity_viable(const clang::FunctionDecl* FD, unsigned count, bool member_call) {
    count = passed(FD, count, member_call);
    return count >= FD->getMinRequiredArguments() && takes(FD, count);
}

/// Whether every one of `arguments`, resolved, may initialize its
/// parameter of `FD`: none certain to fail (never_initializes), matched by
/// position up to a parameter pack. An argument clang already typed is
/// checked as written instead (accepts), where a null pointer constant, a
/// string literal or a braced list shows.
bool may_take(const clang::FunctionDecl* FD,
              llvm::ArrayRef<CallArgument> arguments,
              bool member_call) {
    auto parameters = FD->parameters().drop_front(passed(FD, 0, member_call));
    for(auto [parameter, argument]: views::zip(parameters, arguments)) {
        if(parameter->isParameterPack()) {
            break;
        }
        if(argument.expr && (!argument.expr->isTypeDependent() ||
                             llvm::isa<clang::InitListExpr>(argument.expr->IgnoreParens()))) {
            continue;
        }
        if(never_initializes(FD->getASTContext(),
                             argument.type.getNonReferenceType(),
                             argument.lvalue,
                             parameter->getType())) {
            return false;
        }
    }
    return true;
}

/// Whether an argument expands a pack (`f(xs...)`), which may instantiate
/// to any number of arguments: fixed-arity filtering would remove viable
/// overloads.
bool expands_pack(llvm::ArrayRef<const clang::Expr*> arguments) {
    return ranges::any_of(arguments, llvm::IsaPred<clang::PackExpansionExpr>);
}

/// Drop the function overloads `viable` rejects. Non-function candidates
/// (e.g. a callable object's variable) stay: arity says nothing about
/// them.
void drop_unviable(llvm::SmallVectorImpl<const clang::NamedDecl*>& candidates,
                   llvm::function_ref<bool(const clang::FunctionDecl*)> viable) {
    auto removed = ranges::remove_if(candidates, [&](const clang::NamedDecl* decl) {
        auto FD = as_function(decl);
        return FD && !viable(FD);
    });
    candidates.erase(removed.begin(), removed.end());
}

/// The namespaces and classes argument-dependent lookup associates with
/// the types of a call's arguments ([basic.lookup.argdep]).
struct Associated {
    llvm::SmallSetVector<const clang::DeclContext*, 4> namespaces;

    llvm::SmallPtrSet<const clang::CXXRecordDecl*, 4> classes;

    /// The innermost namespace enclosing `decl`, and each enclosing it
    /// through inline ones: lookup there finds what the inline ones hold.
    void enclosing(const clang::Decl* decl) {
        const auto* scope = decl->getDeclContext()->getEnclosingNamespaceContext();
        namespaces.insert(scope->getPrimaryContext());
        while(const auto* inline_scope = llvm::dyn_cast<clang::NamespaceDecl>(scope)) {
            if(!inline_scope->isInline()) {
                break;
            }
            scope = inline_scope->getParent()->getEnclosingNamespaceContext();
            namespaces.insert(scope->getPrimaryContext());
        }
    }

    void add(const clang::CXXRecordDecl* record) {
        record = record->getDefinition() ? record->getDefinition() : record;
        if(!classes.insert(record->getCanonicalDecl()).second) {
            return;
        }
        enclosing(record);
        if(const auto* outer = llvm::dyn_cast<clang::CXXRecordDecl>(record->getDeclContext())) {
            classes.insert(outer->getCanonicalDecl());
        }
        if(record->hasDefinition()) {
            for(const auto& base: record->bases()) {
                add(base.getType());
            }
        }
        if(const auto* specialization =
               llvm::dyn_cast<clang::ClassTemplateSpecializationDecl>(record)) {
            for(const auto& argument: specialization->getTemplateArgs().asArray()) {
                add(argument);
            }
        }
    }

    void add(const clang::TemplateArgument& argument) {
        switch(argument.getKind()) {
            case clang::TemplateArgument::Type: add(argument.getAsType()); break;
            case clang::TemplateArgument::Template: {
                if(const auto* decl = argument.getAsTemplate().getAsTemplateDecl()) {
                    enclosing(decl);
                    if(const auto* outer =
                           llvm::dyn_cast<clang::CXXRecordDecl>(decl->getDeclContext())) {
                        classes.insert(outer->getCanonicalDecl());
                    }
                }
                break;
            }
            case clang::TemplateArgument::Pack: {
                for(const auto& element: argument.pack_elements()) {
                    add(element);
                }
                break;
            }
            default: break;
        }
    }

    void add(clang::QualType type) {
        type = type.getCanonicalType().getNonReferenceType();
        const auto* T = type.getTypePtr();
        if(const auto* pointer = T->getAs<clang::PointerType>()) {
            add(pointer->getPointeeType());
        } else if(const auto* array = llvm::dyn_cast<clang::ArrayType>(T)) {
            add(array->getElementType());
        } else if(const auto* member = llvm::dyn_cast<clang::MemberPointerType>(T)) {
            add(member->getPointeeType());
            if(const auto* owner = member->getMostRecentCXXRecordDecl()) {
                add(owner);
            }
        } else if(const auto* function = llvm::dyn_cast<clang::FunctionProtoType>(T)) {
            add(function->getReturnType());
            for(auto parameter: function->getParamTypes()) {
                add(parameter);
            }
        } else if(const auto* record = T->getAsCXXRecordDecl()) {
            add(record);
        } else if(const auto* enumeration = T->getAs<clang::EnumType>()) {
            const auto* decl = enumeration->getDecl();
            enclosing(decl);
            if(const auto* outer = llvm::dyn_cast<clang::CXXRecordDecl>(decl->getDeclContext())) {
                classes.insert(outer->getCanonicalDecl());
            }
        }
    }
};

}  // namespace

std::optional<llvm::SmallVector<const clang::NamedDecl*, 4>>
    argument_dependent_lookup(clang::DeclarationName name, llvm::ArrayRef<CallArgument> arguments) {
    Associated associated;
    for(auto& argument: arguments) {
        if(argument.type.isNull() || argument.type->isDependentType()) {
            return std::nullopt;
        }
        associated.add(argument.type);
    }

    /// A friend declared only in a class is visible to the lookup through
    /// that class alone.
    auto visible = [&](const clang::FunctionDecl* function) {
        return ranges::any_of(function->redecls(), [&](const clang::FunctionDecl* redecl) {
            if(redecl->getFriendObjectKind() == clang::Decl::FOK_None) {
                return true;
            }
            const auto* befriending =
                llvm::dyn_cast<clang::CXXRecordDecl>(redecl->getLexicalDeclContext());
            return befriending && associated.classes.contains(befriending->getCanonicalDecl());
        });
    };
    llvm::SmallVector<const clang::NamedDecl*, 4> found;
    llvm::SmallPtrSet<const clang::Decl*, 8> seen;
    for(const auto* scope: associated.namespaces) {
        for(auto* decl: scope->lookup(name)) {
            const auto* function = as_function(decl);
            if(function && visible(function) &&
               arity_viable(function, arguments.size(), /*member_call=*/false) &&
               may_take(function, arguments, /*member_call=*/false) &&
               seen.insert(decl->getCanonicalDecl()).second) {
                found.push_back(decl);
            }
        }
    }
    return found;
}

void drop_by_arity(llvm::SmallVectorImpl<const clang::NamedDecl*>& candidates,
                   unsigned count,
                   bool member_call) {
    drop_unviable(candidates, [&](const clang::FunctionDecl* FD) {
        return arity_viable(FD, count, member_call);
    });
}

bool PseudoInstantiator::accepts(const clang::FunctionDecl* FD,
                                 llvm::ArrayRef<const clang::Expr*> arguments,
                                 bool member_call) {
    if(FD->isInvalidDecl()) {
        return true;
    }
    unsigned offset = 0;
    if(auto method = llvm::dyn_cast<clang::CXXMethodDecl>(FD);
       method && method->isExplicitObjectMemberFunction()) {
        if(!member_call) {
            return true;
        }
        offset = 1;
    }
    for(unsigned index = 0; index < arguments.size(); index += 1) {
        auto argument = arguments[index];
        if(llvm::isa<clang::PackExpansionExpr>(argument) || index + offset >= FD->getNumParams()) {
            break;
        }
        auto param = FD->getParamDecl(index + offset);
        if(param->isParameterPack()) {
            break;
        }
        if(never_initializes(context, *argument, param->getType())) {
            return false;
        }
    }
    return true;
}

void PseudoInstantiator::prefer_exact(llvm::SmallVectorImpl<const clang::NamedDecl*>& candidates,
                                      llvm::ArrayRef<CallArgument> arguments,
                                      bool member_call) {
    if(candidates.size() < 2) {
        return;
    }
    /// How the argument converts to the parameter at the rank of an exact
    /// match: 0 as is or decayed, 1 adding qualifiers a reference or a
    /// pointer binds — a worse sequence of the same rank; none when it needs
    /// more.
    auto rank = [&](clang::QualType parameter, clang::QualType type) -> std::optional<unsigned> {
        auto target = parameter.getNonReferenceType();
        auto added = [](clang::QualType to, clang::QualType from) -> std::optional<unsigned> {
            auto extra = from.getCVRQualifiers() & ~to.getCVRQualifiers();
            if(extra != 0) {
                return std::nullopt;
            }
            return to.getCVRQualifiers() != from.getCVRQualifiers() ? 1 : 0;
        };
        if(parameter->isReferenceType()) {
            return context.hasSameUnqualifiedType(target, type) ? added(target, type)
                                                                : std::nullopt;
        }
        if(context.hasSameUnqualifiedType(target, type)) {
            return 0;
        }
        auto decayed = context.getAdjustedParameterType(type);
        if(target->isPointerType() && decayed->isPointerType() &&
           context.hasSameUnqualifiedType(target->getPointeeType(), decayed->getPointeeType())) {
            return added(target->getPointeeType(), decayed->getPointeeType());
        }
        return context.hasSameUnqualifiedType(target, decayed) ? std::optional(0u) : std::nullopt;
    };
    /// The rank of each argument `decl` takes at the rank of an exact match;
    /// empty when one needs more, nullopt when that waits for
    /// instantiation. A function template's parameters are read with what
    /// deduction gives them; an argument the C-style ellipsis takes is the
    /// worst match there is.
    using Ranks = llvm::SmallVector<unsigned, 4>;
    /// Those an argument cannot initialize their parameter with once
    /// deduction made it concrete: `const U&&` takes no lvalue.
    llvm::SmallPtrSet<const clang::NamedDecl*, 4> unviable;
    auto exact = [&](const clang::NamedDecl* decl) -> std::optional<Ranks> {
        auto* FD = as_function(decl);
        if(!FD) {
            return std::nullopt;
        }
        /// A member defined out of line is written in its own template
        /// parameters, its declaration in the class in the class's.
        FD = FD->getCanonicalDecl();
        auto frames = stack.size();
        auto read = llvm::make_scope_exit([&] { stack.truncate(frames); });
        if(auto* FTD = FD->getDescribedFunctionTemplate()) {
            auto deduced = deduce_call(FTD, FD, arguments);
            if(deduced.empty()) {
                return Ranks{};
            }
            stack.push(FTD, FTD->getTemplateParameters(), deduced);
            /// Its constraints decide whether it is a candidate at all: one
            /// that fails drops it, one not decided leaves the set as it is.
            llvm::SmallVector<clang::AssociatedConstraint, 2> constraints;
            FTD->getAssociatedConstraints(constraints);
            for(auto& constraint: constraints) {
                auto satisfied = evaluate(constraint.ConstraintExpr);
                if(!satisfied) {
                    return std::nullopt;
                }
                if(satisfied->isZero()) {
                    unviable.insert(decl);
                    return Ranks{};
                }
            }
        }
        Ranks ranks;
        auto parameters = FD->parameters().drop_front(passed(FD, 0, member_call));
        for(auto [index, argument]: views::enumerate(arguments)) {
            if(static_cast<std::size_t>(index) >= parameters.size()) {
                return Ranks{};
            }
            auto* parameter = parameters[index];
            if(parameter->isParameterPack()) {
                break;
            }
            auto type = substitute(parameter->getType());
            if(type.isNull() || type->isDependentType()) {
                return std::nullopt;
            }
            /// An argument clang typed is checked as written, where a null
            /// pointer constant or a braced list shows.
            if(argument.expr && !argument.expr->isTypeDependent()
                   ? never_initializes(context, *argument.expr, type)
                   : never_initializes(context, argument.type, argument.lvalue, type)) {
                unviable.insert(decl);
                return Ranks{};
            }
            auto taken = rank(type, argument.type);
            if(!taken) {
                return Ranks{};
            }
            ranks.push_back(*taken);
        }
        if(ranks.size() < arguments.size() && !ranks.empty()) {
            ranks.resize(arguments.size(), 0);
        }
        return ranks.empty() && !arguments.empty() ? Ranks(arguments.size(), 0) : ranks;
    };
    struct Exact {
        const clang::NamedDecl* decl;
        Ranks ranks;
    };
    llvm::SmallVector<Exact, 4> exacts;
    bool waits = false;
    for(auto* candidate: candidates) {
        auto taken = exact(candidate);
        waits |= !taken;
        if(taken && taken->size() == arguments.size() && !(taken->empty() && !arguments.empty())) {
            exacts.push_back({candidate, std::move(*taken)});
        }
    }
    llvm::erase_if(candidates,
                   [&](const clang::NamedDecl* decl) { return unviable.contains(decl); });
    if(waits) {
        return;
    }
    /// One no worse for any argument and better for some is better.
    auto better = [](const Ranks& lhs, const Ranks& rhs) {
        return ranges::equal(lhs, rhs, std::less_equal<>()) && lhs != rhs;
    };
    llvm::erase_if(exacts, [&](const Exact& candidate) {
        return ranges::any_of(exacts, [&](const Exact& other) {
            return better(other.ranks, candidate.ranks);
        });
    });
    /// Of exact matches no better than one another, the one that is no
    /// template specialization wins.
    if(exacts.size() > 1 && ranges::all_of(exacts, [&](const Exact& candidate) {
           return candidate.ranks == exacts.front().ranks;
       })) {
        llvm::erase_if(exacts, [](const Exact& candidate) {
            return as_function(candidate.decl)->getDescribedFunctionTemplate() != nullptr;
        });
    }
    if(exacts.size() == 1) {
        candidates.assign({exacts.front().decl});
    }
}

void
    PseudoInstantiator::select_by_object(llvm::SmallVectorImpl<const clang::NamedDecl*>& candidates,
                                         clang::QualType object,
                                         bool rvalue) {
    if(object.isNull() || candidates.size() < 2) {
        return;
    }
    auto method_of = [](const clang::NamedDecl* decl) -> const clang::CXXMethodDecl* {
        auto* method = llvm::dyn_cast_or_null<clang::CXXMethodDecl>(as_function(decl));
        return method && method->isImplicitObjectMemberFunction() ? method : nullptr;
    };
    auto alike = [&](const clang::CXXMethodDecl* lhs, const clang::CXXMethodDecl* rhs) {
        return !lhs->getDescribedFunctionTemplate() == !rhs->getDescribedFunctionTemplate() &&
               ranges::equal(lhs->parameters(),
                             rhs->parameters(),
                             [&](const clang::ParmVarDecl* left, const clang::ParmVarDecl* right) {
                                 return context.hasSameType(left->getType(), right->getType());
                             });
    };

    auto quals = resolve_object(object).getCVRQualifiers();
    /// How well the method binds the object, lower better; -1 when it
    /// cannot.
    auto rank = [&](const clang::CXXMethodDecl* method) {
        auto method_quals = method->getMethodQualifiers().getCVRQualifiers();
        auto ref = method->getRefQualifier();
        if((method_quals & quals) != quals ||
           (rvalue ? ref == clang::RQ_LValue && !(method_quals & clang::Qualifiers::Const)
                   : ref == clang::RQ_RValue)) {
            return -1;
        }
        return std::popcount(method_quals & ~quals) * 2 +
               (rvalue && ref == clang::RQ_LValue ? 1 : 0);
    };
    bool bindable = ranges::any_of(candidates, [&](const clang::NamedDecl* decl) {
        auto* method = method_of(decl);
        return method && rank(method) >= 0;
    });
    auto outranked = [&](const clang::NamedDecl* decl) {
        auto* method = method_of(decl);
        if(!method) {
            return false;
        }
        auto score = rank(method);
        bool siblings = false;
        for(auto* other: candidates) {
            auto* sibling = method_of(other);
            if(!sibling || sibling == method || !alike(method, sibling)) {
                continue;
            }
            siblings = true;
            if(auto other_score = rank(sibling); other_score >= 0 && other_score < score) {
                return true;
            }
        }
        return score < 0 && (bindable || siblings);
    };
    llvm::SmallVector<const clang::NamedDecl*, 4> kept;
    for(auto* candidate: candidates) {
        if(!outranked(candidate)) {
            kept.push_back(candidate);
        }
    }
    candidates.assign(kept.begin(), kept.end());
}

Callees PseudoInstantiator::call_candidates(const clang::CallExpr* expr) {
    if(auto* callee = llvm::dyn_cast_or_null<clang::NamedDecl>(expr->getCalleeDecl())) {
        return {.candidates = {callee}};
    }
    if(llvm::isa<clang::CXXOperatorCallExpr>(expr)) {
        return {};
    }
    llvm::ArrayRef<const clang::Expr*> arguments(expr->getArgs(), expr->getNumArgs());
    bool pack = expands_pack(arguments);
    auto callees =
        callee_candidates(expr->getCallee(),
                          arguments,
                          [&](const clang::FunctionDecl* FD, bool member) {
                              return (pack || arity_viable(FD, arguments.size(), member)) &&
                                     accepts(FD, arguments, member);
                          });
    /// Deduction here leaves explicit template arguments out.
    auto* callee = expr->getCallee()->IgnoreParenImpCasts();
    if(callees.arguments && decls::explicit_template_arguments(callee).empty()) {
        bool member =
            llvm::isa<clang::UnresolvedMemberExpr, clang::CXXDependentScopeMemberExpr>(callee);
        prefer_exact(callees.candidates, *callees.arguments, member);
    }
    return callees;
}

llvm::SmallVector<const clang::NamedDecl*, 4>
    PseudoInstantiator::written_call_candidates(const clang::Expr* callee,
                                                llvm::ArrayRef<const clang::Expr*> written) {
    bool pack = expands_pack(written);
    return callee_candidates(callee,
                             written,
                             [&](const clang::FunctionDecl* FD, bool member) {
                                 return written.empty() || pack ||
                                        takes(FD, passed(FD, written.size() + 1, member));
                             })
        .candidates;
}

Callees PseudoInstantiator::callee_candidates(
    const clang::Expr* callee,
    llvm::ArrayRef<const clang::Expr*> arguments,
    llvm::function_ref<bool(const clang::FunctionDecl*, bool)> viable) {
    Callees callees;
    auto& candidates = callees.candidates;
    callee = callee->IgnoreParenImpCasts();
    bool member_call =
        llvm::isa<clang::UnresolvedMemberExpr, clang::CXXDependentScopeMemberExpr>(callee);
    /// Resolved before a lookup leaves the frames of the callee's class on
    /// the stack.
    callees.arguments = call_arguments(arguments);
    clang::QualType object;
    bool rvalue_object = false;
    if(auto OE = llvm::dyn_cast<clang::OverloadExpr>(callee)) {
        candidates.append(OE->decls_begin(), OE->decls_end());
        if(auto UME = llvm::dyn_cast<clang::UnresolvedMemberExpr>(OE)) {
            object = member_object(UME->getBaseType(), UME->isArrow());
            rvalue_object = !UME->isArrow() && !UME->isImplicitAccess() && rvalue(UME->getBase());
            /// Clang leaves a using-declaration naming a member of a
            /// dependent base out of the set it found.
            if(auto* naming = UME->getNamingClass()) {
                for(auto* decl: naming->lookup(UME->getName())) {
                    if(llvm::isa<clang::UnresolvedUsingValueDecl>(decl)) {
                        candidates.push_back(decl);
                    }
                }
            }
        }
        /// Argument-dependent lookup adds what instantiation finds for the
        /// arguments' types; while one of them stays dependent the visible
        /// overloads are all there is to tell, and the call may call more.
        if(auto* ULE = llvm::dyn_cast<clang::UnresolvedLookupExpr>(OE);
           ULE && ULE->requiresADL() && !arguments.empty()) {
            std::optional<llvm::SmallVector<const clang::NamedDecl*, 4>> found;
            if(callees.arguments) {
                found = argument_dependent_lookup(ULE->getName(), *callees.arguments);
            }
            if(found) {
                candidates.append(found->begin(), found->end());
            } else {
                callees.complete = false;
            }
        }
    } else if(auto DSME = llvm::dyn_cast<clang::CXXDependentScopeMemberExpr>(callee)) {
        auto base = DSME->isImplicitAccess() ? Operand{.type = DSME->getBaseType(), .rvalue = false}
                                             : operand(DSME->getBase());
        object = member_object(base.type, DSME->isArrow());
        rvalue_object = !DSME->isArrow() && base.rvalue;
        auto members = lookup_member(object, DSME->getMember());
        candidates.append(members.begin(), members.end());
    } else if(auto DSDRE = llvm::dyn_cast<clang::DependentScopeDeclRefExpr>(callee)) {
        auto members = lookup(DSDRE->getQualifier(), DSDRE->getDeclName());
        candidates.append(members.begin(), members.end());
    }

    /// A using-declaration naming a member of a dependent base stands for
    /// what it names there, beside the class's own overloads.
    llvm::SmallVector<const clang::NamedDecl*, 4> named;
    llvm::DenseMap<const clang::Decl*, const clang::Decl*> reached_through;
    auto add = [&](const clang::NamedDecl* decl, const clang::Decl* used) {
        auto [found, added] = reached_through.try_emplace(decl->getCanonicalDecl(), used);
        if(added) {
            named.push_back(decl);
        } else if(used && found->second && found->second != used) {
            /// One declaration through two specializations of its class
            /// (`using Base<T>::get; using Base<T*>::get;`): the frames of
            /// the two lookups are not kept apart.
            callees.complete = false;
        }
    };
    for(auto* candidate: candidates) {
        if(auto* used = llvm::dyn_cast<clang::UnresolvedUsingValueDecl>(candidate)) {
            for(auto* decl: lookup(used->getQualifier(), used->getDeclName())) {
                add(decl, used->getCanonicalDecl());
            }
        } else {
            add(candidate, nullptr);
        }
    }
    candidates.assign(named.begin(), named.end());

    /// Explicit template arguments (`rows.template pick<int>()`) name the
    /// templates of the set alone.
    bool explicit_arguments = false;
    if(auto* OE = llvm::dyn_cast<clang::OverloadExpr>(callee)) {
        explicit_arguments = OE->hasExplicitTemplateArgs();
    } else if(auto* DSME = llvm::dyn_cast<clang::CXXDependentScopeMemberExpr>(callee)) {
        explicit_arguments = DSME->hasExplicitTemplateArgs();
    } else if(auto* DSDRE = llvm::dyn_cast<clang::DependentScopeDeclRefExpr>(callee)) {
        explicit_arguments = DSDRE->hasExplicitTemplateArgs();
    }
    drop_unviable(candidates, [&](const clang::FunctionDecl* FD) {
        return (!explicit_arguments || FD->getDescribedFunctionTemplate()) &&
               viable(FD, member_call) &&
               (!callees.arguments || may_take(FD, *callees.arguments, member_call));
    });
    select_by_object(candidates, object, rvalue_object);
    return callees;
}

}  // namespace clice::types
