module;

#include "modules/prelude.h"
/// Parts of this file (only_instantiation, proto_type_loc) are ported from
/// clangd's AST.cpp and InlayHints.cpp (llvmorg-21.1.8), and
/// resolve_forwarding_params follows clangd's resolveForwardingParameters;
/// LLVM project, licensed under Apache License v2.0 with LLVM Exceptions.
/// See https://llvm.org/LICENSE.txt for license information.

module clice;

import :semantic.decls;
import :semantic.unifier;

namespace clice::decls {

bool is_templated(const clang::Decl* decl) {
    assert(decl);
    if(decl->getDescribedTemplate()) {
        return true;
    }

    if(llvm::isa<clang::TemplateDecl,
                 clang::ClassTemplatePartialSpecializationDecl,
                 clang::VarTemplatePartialSpecializationDecl>(decl)) {
        return true;
    }

    return false;
}

bool is_exported(const clang::Decl* decl) {
    // A concept's or alias template's parameters sit in the enclosing
    // context, the `export` block included.
    if(decl->isTemplateParameter() ||
       !decl->getDeclContext()->getRedeclContext()->isFileContext()) {
        return false;
    }
    // Clang marks what a named module exports visible to importers — a
    // namespace too once it holds an exported declaration.
    return llvm::any_of(decl->redecls(), [](const clang::Decl* redecl) {
        auto* module = redecl->getOwningModule();
        return module && module->isNamedModule() &&
               redecl->getModuleOwnershipKind() ==
                   clang::Decl::ModuleOwnershipKind::VisibleWhenImported;
    });
}

namespace {

template <class T>
bool is_template_specialization_kind(const clang::NamedDecl* decl,
                                     clang::TemplateSpecializationKind kind) {
    if(const auto* td = dyn_cast<T>(decl))
        return td->getTemplateSpecializationKind() == kind;
    return false;
}

inline bool is_template_specialization_kind(const clang::NamedDecl* decl,
                                            clang::TemplateSpecializationKind kind) {
    return is_template_specialization_kind<clang::FunctionDecl>(decl, kind) ||
           is_template_specialization_kind<clang::CXXRecordDecl>(decl, kind) ||
           is_template_specialization_kind<clang::VarDecl>(decl, kind);
}

}  // namespace

bool is_implicit_instantiation(const clang::NamedDecl* decl) {
    assert(decl);
    return is_template_specialization_kind(decl, clang::TSK_ImplicitInstantiation);
}

bool is_instantiation(const clang::Decl* decl) {
    if(const auto* named = llvm::dyn_cast<clang::NamedDecl>(decl);
       named && is_implicit_instantiation(named)) {
        return true;
    }

    if(const auto* spec = llvm::dyn_cast<clang::ClassTemplateSpecializationDecl>(decl)) {
        return !llvm::isa<clang::ClassTemplatePartialSpecializationDecl>(spec) &&
               clang::isTemplateInstantiation(spec->getSpecializationKind());
    }
    if(const auto* function = llvm::dyn_cast<clang::FunctionDecl>(decl)) {
        return clang::isTemplateInstantiation(function->getTemplateSpecializationKind());
    }
    if(const auto* var = llvm::dyn_cast<clang::VarDecl>(decl)) {
        return clang::isTemplateInstantiation(var->getTemplateSpecializationKind());
    }
    /// A member class of a class template specialization, instantiated
    /// along with it or explicitly (`template struct Outer<int>::Inner;`).
    if(const auto* record = llvm::dyn_cast<clang::CXXRecordDecl>(decl)) {
        return clang::isTemplateInstantiation(record->getTemplateSpecializationKind());
    }
    return false;
}

namespace {

/// The pattern an undeclared specialization would be instantiated from:
/// match the partial specializations against the written arguments the way
/// real instantiation would. Falls back to the primary template when no
/// partial matches, the match is ambiguous, or the winner's match could not
/// be verified (see deduce_arguments).
template <typename Partial, typename Spec>
const clang::NamedDecl* undeclared_pattern(const Spec* spec) {
    auto* primary = spec->getSpecializedTemplate();
    auto& context = spec->getASTContext();
    auto arguments = spec->getTemplateArgs().asArray();

    llvm::SmallVector<Partial*> partials;
    primary->getPartialSpecializations(partials);

    llvm::SmallVector<types::PartialMatch<Partial>, 4> matched;
    for(auto* partial: partials) {
        llvm::SmallVector<clang::TemplateArgument> deduced;
        auto deduction = types::deduce_arguments(context,
                                                 partial->getTemplateParameters(),
                                                 partial->getTemplateArgs().asArray(),
                                                 arguments,
                                                 deduced);
        if(deduction != types::Deduction::Failed) {
            matched.push_back(
                {.partial = partial, .verified = deduction == types::Deduction::Matched});
        }
    }

    auto choice = types::select_partial(context, matched);
    if(choice.verdict != types::PartialVerdict::Selected) {
        return primary->getTemplatedDecl();
    }
    return choice.winner;
}

const clang::CXXRecordDecl* getDeclContextForTemplateInstationPattern(const clang::Decl* D) {
    if(const auto* CTSD = dyn_cast<clang::ClassTemplateSpecializationDecl>(D->getDeclContext())) {
        return CTSD->getTemplateInstantiationPattern();
    }

    if(const auto* RD = dyn_cast<clang::CXXRecordDecl>(D->getDeclContext())) {
        return RD->getInstantiatedFromMemberClass();
    }

    return nullptr;
}

}  // namespace

/// An explicit specialization named before it is declared keeps that
/// first, undeclared node as its canonical declaration.
template <typename Spec>
bool explicitly_specialized(const Spec* spec) {
    for(auto* redecl: spec->redecls()) {
        if(llvm::cast<Spec>(redecl)->getSpecializationKind() == clang::TSK_ExplicitSpecialization) {
            return true;
        }
    }
    return false;
}

auto instantiated_from(const clang::NamedDecl* decl) -> const clang::NamedDecl* {
    assert(decl);
    if(auto CTSD = llvm::dyn_cast<clang::ClassTemplateSpecializationDecl>(decl)) {
        auto kind = CTSD->getTemplateSpecializationKind();
        if(kind == clang::TSK_Undeclared && explicitly_specialized(CTSD)) {
            return CTSD;
        }
        if(kind == clang::TSK_Undeclared) {
            /// Instantiation is lazy: an undeclared specialization carries no
            /// pattern link yet. Select the pattern instantiation would use so
            /// the identity matches the instantiated case.
            return undeclared_pattern<clang::ClassTemplatePartialSpecializationDecl>(CTSD);
        } else if(kind == clang::TSK_ExplicitSpecialization) {
            /// If the decl is an full specialization, return itself.
            return CTSD;
        }

        return CTSD->getTemplateInstantiationPattern();
    }

    if(auto FD = llvm::dyn_cast<clang::FunctionDecl>(decl)) {
        /// If the decl is an full specialization, return itself.
        if(FD->getTemplateSpecializationKind() == clang::TSK_ExplicitSpecialization) {
            return FD;
        }

        return FD->getTemplateInstantiationPattern();
    }

    if(auto VTSD = llvm::dyn_cast<clang::VarTemplateSpecializationDecl>(decl)) {
        if(VTSD->getSpecializationKind() == clang::TSK_Undeclared) {
            if(explicitly_specialized(VTSD)) {
                return VTSD;
            }
            return undeclared_pattern<clang::VarTemplatePartialSpecializationDecl>(VTSD);
        }
    }

    if(auto VD = llvm::dyn_cast<clang::VarDecl>(decl)) {
        /// If the decl is an full specialization, return itself.
        if(VD->getTemplateSpecializationKind() == clang::TSK_ExplicitSpecialization) {
            return VD;
        }

        return VD->getTemplateInstantiationPattern();
    }

    if(auto CRD = llvm::dyn_cast<clang::CXXRecordDecl>(decl)) {
        /// An explicitly specialized member (`template <> struct Outer<char>::Inner`)
        /// is its own entity, just like a full specialization.
        if(CRD->getTemplateSpecializationKind() == clang::TSK_ExplicitSpecialization) {
            return CRD;
        }
        return CRD->getInstantiatedFromMemberClass();
    }

    /// For `FieldDecl` and `TypedefNameDecl`, clang will not store their instantiation information
    /// in the unit. So we need to look up the original decl manually.
    if(llvm::isa<clang::FieldDecl, clang::TypedefNameDecl>(decl)) {
        /// FIXME: figure out the context.
        if(auto context = getDeclContextForTemplateInstationPattern(decl)) {
            for(auto member: context->lookup(decl->getDeclName())) {
                if(member->isImplicit()) {
                    continue;
                }

                if(member->getKind() == decl->getKind()) {
                    return member;
                }
            }
        }
    }

    if(auto ED = llvm::dyn_cast<clang::EnumDecl>(decl)) {
        if(auto* info = ED->getMemberSpecializationInfo();
           info && info->getTemplateSpecializationKind() == clang::TSK_ExplicitSpecialization) {
            return ED;
        }
        return ED->getInstantiatedFromMemberEnum();
    }

    if(auto ECD = llvm::dyn_cast<clang::EnumConstantDecl>(decl)) {
        auto ED = llvm::cast<clang::EnumDecl>(ECD->getDeclContext());
        if(auto context = ED->getInstantiatedFromMemberEnum()) {
            for(auto member: context->lookup(ECD->getDeclName())) {
                return member;
            }
        }
    }

    return nullptr;
}

auto normalize(const clang::NamedDecl* decl) -> const clang::NamedDecl* {
    assert(decl);

    decl = llvm::cast<clang::NamedDecl>(decl->getCanonicalDecl());

    if(auto ND = instantiated_from(llvm::cast<clang::NamedDecl>(decl))) {
        return llvm::cast<clang::NamedDecl>(ND->getCanonicalDecl());
    }

    return decl;
}

namespace {

template <typename TemplateDeclTy>
clang::NamedDecl* only_instantiation_impl(TemplateDeclTy* TD) {
    clang::NamedDecl* Only = nullptr;
    for(auto* Spec: TD->specializations()) {
        if(Spec->getTemplateSpecializationKind() == clang::TSK_ExplicitSpecialization)
            continue;
        if(Only != nullptr)
            return nullptr;
        Only = Spec;
    }
    return Only;
}

}  // namespace

auto only_instantiation(clang::NamedDecl* TemplatedDecl) -> clang::NamedDecl* {
    assert(TemplatedDecl);
    if(auto* TD = TemplatedDecl->getDescribedTemplate()) {
        if(auto* CTD = llvm::dyn_cast<clang::ClassTemplateDecl>(TD))
            return only_instantiation_impl(CTD);
        if(auto* FTD = llvm::dyn_cast<clang::FunctionTemplateDecl>(TD))
            return only_instantiation_impl(FTD);
        if(auto* VTD = llvm::dyn_cast<clang::VarTemplateDecl>(TD))
            return only_instantiation_impl(VTD);
    }
    return nullptr;
}

auto only_instantiation(clang::ParmVarDecl* decl) -> clang::ParmVarDecl* {
    assert(decl);
    auto* TemplateFunction = llvm::dyn_cast<clang::FunctionDecl>(decl->getDeclContext());
    if(!TemplateFunction)
        return nullptr;
    auto* InstantiatedFunction =
        llvm::dyn_cast_or_null<clang::FunctionDecl>(only_instantiation(TemplateFunction));
    if(!InstantiatedFunction)
        return nullptr;

    unsigned ParamIdx = 0;
    for(auto* Param: TemplateFunction->parameters()) {
        // Can't reason about param indexes in the presence of preceding packs.
        // And if this param is a pack, it may expand to multiple params.
        if(Param->isParameterPack())
            return nullptr;
        if(Param == decl)
            break;
        ++ParamIdx;
    }
    assert(ParamIdx < TemplateFunction->getNumParams() && "Couldn't find param in list?");
    assert(ParamIdx < InstantiatedFunction->getNumParams() &&
           "Instantiated function has fewer (non-pack) parameters?");
    return InstantiatedFunction->getParamDecl(ParamIdx);
}

namespace {

// Returns the template parameter pack type from an instantiated function
// template, if it exists, nullptr otherwise.
auto function_pack_type(const clang::FunctionDecl* callee) -> const clang::TemplateTypeParmType* {
    // returns true for `X` in `template <typename... X> void foo()`
    auto is_type_pack = [](clang::NamedDecl* decl) {
        if(const auto* TTPD = llvm::dyn_cast<clang::TemplateTypeParmDecl>(decl)) {
            return TTPD->isParameterPack();
        }
        return false;
    };

    if(const auto* decl = callee->getPrimaryTemplate()) {
        auto template_params = decl->getTemplateParameters()->asArray();
        // find the template parameter pack from the back
        const auto it =
            std::ranges::find_if(template_params.rbegin(), template_params.rend(), is_type_pack);
        if(it != template_params.rend()) {
            const auto* TTPD = llvm::dyn_cast<clang::TemplateTypeParmDecl>(*it);
            return TTPD->getTypeForDecl()->castAs<clang::TemplateTypeParmType>();
        }
    }

    return nullptr;
}

}  // namespace

// Returns the template parameter pack type that this parameter was expanded
// from (if in the Args... or Args&... or Args&&... form), if this is the case,
// nullptr otherwise.
auto underlying_pack_type(const clang::ParmVarDecl* param) -> const clang::TemplateTypeParmType* {
    assert(param);
    const auto* type = param->getType().getTypePtr();
    if(auto* ref_type = llvm::dyn_cast<clang::ReferenceType>(type)) {
        type = ref_type->getPointeeTypeAsWritten().getTypePtr();
    }

    if(const auto* subst_type = llvm::dyn_cast<clang::SubstTemplateTypeParmType>(type)) {
        const auto* decl = subst_type->getReplacedParameter();
        if(decl->isParameterPack()) {
            return decl->getTypeForDecl()->castAs<clang::TemplateTypeParmType>();
        }
    }

    return nullptr;
}

namespace {

bool is_std_forward(const clang::FunctionDecl* callee) {
    auto* name = callee->getIdentifier();
    return name && name->getName() == "forward" && callee->isInStdNamespace();
}

/// The parameter `arg` passes on unchanged: named directly, through
/// `std::forward`, or through the copy or move building a by-value
/// parameter, implicit nodes aside.
const clang::ParmVarDecl* passed_param(const clang::Expr* arg) {
    arg = arg->IgnoreImplicitAsWritten();
    if(auto* construct = llvm::dyn_cast<clang::CXXConstructExpr>(arg);
       construct && construct->getConstructor()->isCopyOrMoveConstructor()) {
        arg = construct->getArg(0)->IgnoreImplicitAsWritten();
    }
    if(auto* call = llvm::dyn_cast<clang::CallExpr>(arg)) {
        if(auto* callee = call->getDirectCallee(); callee && is_std_forward(callee)) {
            arg = call->getArg(0)->IgnoreImplicitAsWritten();
        }
    }
    auto* ref = llvm::dyn_cast<clang::DeclRefExpr>(arg);
    return ref ? llvm::dyn_cast<clang::ParmVarDecl>(ref->getDecl()) : nullptr;
}

/// The run of `params`, parameters of `fn`, that `fn`'s trailing type
/// parameter pack expanded to; empty, at the end of `params`, when none.
auto expanded_pack(const clang::FunctionDecl* fn, llvm::ArrayRef<const clang::ParmVarDecl*> params)
    -> llvm::ArrayRef<const clang::ParmVarDecl*> {
    auto* pack = function_pack_type(fn);
    auto expanded = [pack](const clang::ParmVarDecl* param) {
        return pack && underlying_pack_type(param) == pack;
    };
    return params.drop_until(expanded).take_while(expanded);
}

/// Finds the first call in a function body that passes `pack`, parameters
/// of that function, on as consecutive arguments, and the callee
/// parameters receiving them. For example, in
///
///   template <typename T, typename... Args>
///   auto make_unique(Args... args) {
///     return unique_ptr<T>(new T(args...));
///   }
///
/// `make_unique<std::string>(2, 'x')` passes its two parameters on to the
/// constructor `std::string(int, char)`.
struct PackForwardFinder : clang::RecursiveASTVisitor<PackForwardFinder> {
    llvm::ArrayRef<const clang::ParmVarDecl*> pack;

    const clang::FunctionDecl* callee = nullptr;
    llvm::ArrayRef<const clang::ParmVarDecl*> params;

    bool VisitCallExpr(clang::CallExpr* call) {
        auto* fn = call->getDirectCallee();
        if(!fn) {
            return true;
        }
        llvm::ArrayRef<const clang::Expr*> args(call->getArgs(), call->getNumArgs());
        // The object a member operator is called on binds a parameter only
        // when it is the explicit object.
        if(auto* method = llvm::dyn_cast<clang::CXXMethodDecl>(fn);
           method && !method->isExplicitObjectMemberFunction() &&
           llvm::isa<clang::CXXOperatorCallExpr>(call)) {
            args = args.drop_front();
        }
        match(fn, args);
        return !callee;
    }

    bool VisitCXXConstructExpr(clang::CXXConstructExpr* construct) {
        match(construct->getConstructor(), {construct->getArgs(), construct->getNumArgs()});
        return !callee;
    }

    /// `args` bind `fn`'s parameters in order.
    void match(const clang::FunctionDecl* fn, llvm::ArrayRef<const clang::Expr*> args) {
        // Arguments past the named parameters go to a C variadic `...`.
        args = args.take_front(fn->getNumParams());
        // A written expansion, left in a dependent call, stands for any
        // number of arguments: the ones after it bind unknown parameters.
        if(llvm::any_of(args, llvm::IsaPred<clang::PackExpansionExpr>)) {
            return;
        }
        auto passes = [](const clang::Expr* arg, const clang::ParmVarDecl* param) {
            return passed_param(arg) == param;
        };
        for(std::size_t start = 0; start + pack.size() <= args.size(); start += 1) {
            if(llvm::equal(args.slice(start, pack.size()), pack, passes)) {
                callee = fn;
                params = fn->parameters().slice(start, pack.size());
                return;
            }
        }
    }
};

}  // namespace

auto resolve_forwarding_params(const clang::FunctionDecl* decl, unsigned max_depth)
    -> llvm::SmallVector<const clang::ParmVarDecl*> {
    assert(decl);
    llvm::ArrayRef<const clang::ParmVarDecl*> own = decl->parameters();
    llvm::SmallVector<const clang::ParmVarDecl*> resolved(own.begin(), own.end());
    // The pack still being followed, as parameters of `function`, and where
    // its first element stands in `resolved`.
    const clang::FunctionDecl* function = decl;
    auto pack = expanded_pack(decl, own);
    std::size_t offset = pack.data() - own.data();
    llvm::SmallPtrSet<const clang::FunctionTemplateDecl*, 4> seen;
    if(auto* primary = decl->getPrimaryTemplate()) {
        seen.insert(primary);
    }
    for(unsigned depth = 0; !pack.empty() && depth < max_depth; depth += 1) {
        PackForwardFinder finder{.pack = pack};
        finder.TraverseStmt(function->getBody());
        if(!finder.callee) {
            break;
        }
        // A template reached again recurses over its own pack (`f(rest...)`
        // peeling one argument per step): no parameter is its target.
        if(auto* primary = finder.callee->getPrimaryTemplate();
           primary && !seen.insert(primary).second) {
            return {own.begin(), own.end()};
        }
        llvm::copy(finder.params, resolved.begin() + offset);
        auto next = expanded_pack(finder.callee, finder.params);
        offset += next.data() - finder.params.data();
        pack = next;
        function = finder.callee;
    }
    return resolved;
}

bool binds_mutable_reference(const clang::ParmVarDecl* param, const clang::ParmVarDecl* forwarded) {
    auto forwarded_type = forwarded->getType();
    return param->getType()->isLValueReferenceType() && forwarded_type->isLValueReferenceType() &&
           !forwarded_type.getNonReferenceType().isConstQualified() &&
           !underlying_pack_type(forwarded);
}

auto proto_type_loc(clang::Expr* expr) -> clang::FunctionProtoTypeLoc {
    assert(expr);
    clang::TypeLoc target;
    clang::Expr* naked_fn = expr->IgnoreParenCasts();

    if(const auto* T = naked_fn->getType().getTypePtr()->getAs<clang::TypedefType>()) {
        target = T->getDecl()->getTypeSourceInfo()->getTypeLoc();
    } else if(const auto* DR = llvm::dyn_cast<clang::DeclRefExpr>(naked_fn)) {
        const auto* D = DR->getDecl();
        if(const auto* const VD = llvm::dyn_cast<clang::VarDecl>(D)) {
            target = VD->getTypeSourceInfo()->getTypeLoc();
        }
    }

    if(!target) {
        return {};
    }

    // Unwrap types that may be wrapping the function type
    while(true) {
        if(auto p = target.getAs<clang::PointerTypeLoc>()) {
            target = p.getPointeeLoc();
            continue;
        }

        if(auto a = target.getAs<clang::AttributedTypeLoc>()) {
            target = a.getModifiedLoc();
            continue;
        }

        if(auto p = target.getAs<clang::ParenTypeLoc>()) {
            target = p.getInnerLoc();
            continue;
        }

        break;
    }

    if(auto f = target.getAs<clang::FunctionProtoTypeLoc>()) {
        return f;
    }

    return {};
}

}  // namespace clice::decls
