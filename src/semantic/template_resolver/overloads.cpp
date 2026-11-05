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

}  // namespace

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

llvm::SmallVector<const clang::NamedDecl*, 4>
    PseudoInstantiator::call_candidates(const clang::CallExpr* expr) {
    if(auto* callee = llvm::dyn_cast_or_null<clang::NamedDecl>(expr->getCalleeDecl())) {
        return {callee};
    }
    if(llvm::isa<clang::CXXOperatorCallExpr>(expr)) {
        return {};
    }
    llvm::ArrayRef<const clang::Expr*> arguments(expr->getArgs(), expr->getNumArgs());
    bool pack = expands_pack(arguments);
    return callee_candidates(expr->getCallee(), [&](const clang::FunctionDecl* FD, bool member) {
        return (pack || arity_viable(FD, arguments.size(), member)) &&
               accepts(FD, arguments, member);
    });
}

llvm::SmallVector<const clang::NamedDecl*, 4>
    PseudoInstantiator::written_call_candidates(const clang::Expr* callee,
                                                llvm::ArrayRef<const clang::Expr*> written) {
    bool pack = expands_pack(written);
    return callee_candidates(callee, [&](const clang::FunctionDecl* FD, bool member) {
        return written.empty() || pack || takes(FD, passed(FD, written.size() + 1, member));
    });
}

llvm::SmallVector<const clang::NamedDecl*, 4> PseudoInstantiator::callee_candidates(
    const clang::Expr* callee,
    llvm::function_ref<bool(const clang::FunctionDecl*, bool)> viable) {
    llvm::SmallVector<const clang::NamedDecl*, 4> candidates;
    callee = callee->IgnoreParenImpCasts();
    bool member_call =
        llvm::isa<clang::UnresolvedMemberExpr, clang::CXXDependentScopeMemberExpr>(callee);
    clang::QualType object;
    bool rvalue_object = false;
    /// FIXME: a call needing ADL keeps only what ordinary lookup saw at the
    /// definition: when the instantiation's ADL finds another overload (`g(t, 1)`
    /// reaching `N::g`), a lone visible `::g` still answers, its parameter names
    /// in inlay hints with it.
    if(auto OE = llvm::dyn_cast<clang::OverloadExpr>(callee)) {
        candidates.append(OE->decls_begin(), OE->decls_end());
        if(auto UME = llvm::dyn_cast<clang::UnresolvedMemberExpr>(OE)) {
            object = member_object(UME->getBaseType(), UME->isArrow());
            rvalue_object = !UME->isArrow() && !UME->isImplicitAccess() && rvalue(UME->getBase());
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
    /// FIXME: explicit template arguments (`rows.template pick<int>()`) do not
    /// drop the same-name non-template overloads: navigation lists them too, and a
    /// return type they disagree on leaves the call untyped.
    drop_unviable(candidates,
                  [&](const clang::FunctionDecl* FD) { return viable(FD, member_call); });
    select_by_object(candidates, object, rvalue_object);
    return candidates;
}

}  // namespace clice::types
