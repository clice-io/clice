module clice;

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

    /// The innermost namespace enclosing `decl`, and the one enclosing it
    /// when that is inline: lookup there finds what the inline one holds.
    void enclosing(const clang::Decl* decl) {
        const auto* scope = decl->getDeclContext()->getEnclosingNamespaceContext();
        namespaces.insert(scope->getPrimaryContext());
        if(const auto* inline_scope = llvm::dyn_cast<clang::NamespaceDecl>(scope);
           inline_scope && inline_scope->isInline()) {
            namespaces.insert(inline_scope->getParent()->getEnclosingNamespaceContext());
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
    argument_dependent_lookup(clang::DeclarationName name, llvm::ArrayRef<clang::QualType> types) {
    Associated associated;
    for(auto type: types) {
        if(type.isNull() || type->isDependentType()) {
            return std::nullopt;
        }
        associated.add(type);
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
            if(function && visible(function) && seen.insert(decl->getCanonicalDecl()).second) {
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
                                      llvm::ArrayRef<const clang::Expr*> arguments) {
    if(candidates.size() < 2 || ranges::any_of(arguments, [](const clang::Expr* argument) {
           return argument->isTypeDependent();
       })) {
        return;
    }
    auto same = [&](clang::QualType parameter, const clang::Expr* argument) {
        return context.hasSameUnqualifiedType(parameter.getNonReferenceType(), argument->getType());
    };
    /// Whether `decl` takes every argument as is; nullopt when that waits
    /// for instantiation.
    auto exact = [&](const clang::NamedDecl* decl) -> std::optional<bool> {
        auto* FD = as_function(decl);
        if(!FD) {
            return std::nullopt;
        }
        unsigned offset = 0;
        if(auto method = llvm::dyn_cast<clang::CXXMethodDecl>(FD);
           method && method->isExplicitObjectMemberFunction()) {
            offset = 1;
        }
        if(arguments.size() + offset > FD->getNumParams()) {
            return false;
        }
        for(auto [index, argument]: views::enumerate(arguments)) {
            auto type = FD->getParamDecl(static_cast<unsigned>(index) + offset)->getType();
            if(type->isDependentType()) {
                return std::nullopt;
            }
            if(!same(type, argument)) {
                return false;
            }
        }
        return true;
    };
    const clang::NamedDecl* chosen = nullptr;
    for(auto* candidate: candidates) {
        auto taken = exact(candidate);
        if(!taken) {
            return;
        }
        if(*taken) {
            if(chosen) {
                return;
            }
            chosen = candidate;
        }
    }
    if(chosen) {
        candidates.assign({chosen});
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
            if(auto other_score = rank(sibling);
               other_score >= 0 && (score < 0 || other_score < score)) {
                return true;
            }
        }
        return siblings && score < 0;
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
    if(!pack) {
        prefer_exact(callees.candidates, arguments);
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

std::optional<llvm::SmallVector<CallArgument, 4>>
    PseudoInstantiator::call_arguments(llvm::ArrayRef<const clang::Expr*> arguments) {
    if(expands_pack(arguments)) {
        return std::nullopt;
    }
    llvm::SmallVector<CallArgument, 4> passed;
    for(auto* argument: arguments) {
        auto type = resolve_object(expr_type(argument));
        if(type.isNull()) {
            return std::nullopt;
        }
        passed.push_back({.type = type, .lvalue = !rvalue(argument)});
    }
    return passed;
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
                    if(llvm::isa<clang::UnresolvedUsingValueDecl>(decl) &&
                       !ranges::contains(candidates, decl)) {
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
            if(auto passed = call_arguments(arguments)) {
                auto types = ranges::to<llvm::SmallVector<clang::QualType, 4>>(
                    views::transform(*passed, &CallArgument::type));
                found = argument_dependent_lookup(ULE->getName(), types);
            }
            if(found) {
                for(auto* decl: *found) {
                    if(ranges::none_of(candidates, [&](const clang::NamedDecl* candidate) {
                           return candidate->getCanonicalDecl() == decl->getCanonicalDecl();
                       })) {
                        candidates.push_back(decl);
                    }
                }
            } else {
                callees.complete = false;
            }
        }
    } else if(auto DSME = llvm::dyn_cast<clang::CXXDependentScopeMemberExpr>(callee)) {
        object = member_object(DSME);
        rvalue_object = !DSME->isArrow() && !DSME->isImplicitAccess() && rvalue(DSME->getBase());
        auto members = lookup_member(object, DSME->getMember());
        candidates.append(members.begin(), members.end());
    } else if(auto DSDRE = llvm::dyn_cast<clang::DependentScopeDeclRefExpr>(callee)) {
        auto members = lookup(DSDRE->getQualifier(), DSDRE->getDeclName());
        candidates.append(members.begin(), members.end());
    }

    /// A using-declaration naming a member of a dependent base stands for
    /// what it names there, beside the class's own overloads.
    llvm::SmallVector<const clang::NamedDecl*, 4> named;
    auto add = [&](const clang::NamedDecl* decl) {
        if(ranges::none_of(named, [&](const clang::NamedDecl* other) {
               return other->getCanonicalDecl() == decl->getCanonicalDecl();
           })) {
            named.push_back(decl);
        }
    };
    for(auto* candidate: candidates) {
        if(auto* used = llvm::dyn_cast<clang::UnresolvedUsingValueDecl>(candidate)) {
            ranges::for_each(lookup(used->getQualifier(), used->getDeclName()), add);
        } else {
            add(candidate);
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
               viable(FD, member_call);
    });
    select_by_object(candidates, object, rvalue_object);
    return callees;
}

}  // namespace clice::types
