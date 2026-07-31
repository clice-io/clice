/// Parts of this file (only_instantiation, resolve_forwarding_params,
/// proto_type_loc and the forwarding-call analysis) are ported from
/// clangd's AST.cpp and InlayHints.cpp (llvmorg-21.1.8), part of the LLVM
/// project, licensed under Apache License v2.0 with LLVM Exceptions. See
/// https://llvm.org/LICENSE.txt for license information.

#include "semantic/decls.h"

#include "semantic/unifier.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallSet.h"
#include "clang/AST/Decl.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/DeclTemplate.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/AST/Type.h"

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

namespace {

/// The pattern an undeclared specialization would be instantiated from:
/// match the partial specializations against the written arguments the way
/// real instantiation would. Falls back to the primary template when no
/// partial matches, the match is ambiguous, or the winner is constrained
/// (constraint satisfaction needs Sema).
template <typename Partial, typename Spec>
const clang::NamedDecl* undeclared_pattern(const Spec* spec) {
    auto* primary = spec->getSpecializedTemplate();
    auto& context = spec->getASTContext();
    auto arguments = spec->getTemplateArgs().asArray();

    llvm::SmallVector<Partial*> partials;
    primary->getPartialSpecializations(partials);

    auto matches = [&](Partial* partial) {
        llvm::SmallVector<clang::TemplateArgument> deduced;
        return types::deduce_arguments(context,
                                       partial->getTemplateParameters(),
                                       partial->getTemplateArgs().asArray(),
                                       arguments,
                                       deduced);
    };

    Partial* best = nullptr;
    llvm::SmallVector<Partial*, 4> matched;
    for(auto* partial: partials) {
        if(matches(partial)) {
            matched.push_back(partial);
            if(!best || types::more_specialized(context, partial, best)) {
                best = partial;
            }
        }
    }

    if(!best) {
        return primary->getTemplatedDecl();
    }

    /// Real instantiation diagnoses ambiguity; degrade to the primary
    /// rather than picking arbitrarily.
    for(auto* partial: matched) {
        if(partial != best && !types::more_specialized(context, best, partial)) {
            return primary->getTemplatedDecl();
        }
    }

    if(best->getTemplateParameters()->hasAssociatedConstraints()) {
        return primary->getTemplatedDecl();
    }

    return best;
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

auto instantiated_from(const clang::NamedDecl* decl) -> const clang::NamedDecl* {
    assert(decl);
    if(auto CTSD = llvm::dyn_cast<clang::ClassTemplateSpecializationDecl>(decl)) {
        auto kind = CTSD->getTemplateSpecializationKind();
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

// This visitor walks over the body of an instantiated function template.
// The template accepts a parameter pack and the visitor records whether
// the pack parameters were forwarded to another call. For example, given:
//
// template <typename T, typename... Args>
// auto make_unique(Args... args) {
//   return unique_ptr<T>(new T(args...));
// }
//
// When called as `make_unique<std::string>(2, 'x')` this yields a function
// `make_unique<std::string, int, char>` with two parameters.
// The visitor records that those two parameters are forwarded to the
// `constructor std::string(int, char);`.
//
// This information is recorded in the `ForwardingInfo` split into fully
// resolved parameters (passed as argument to a parameter that is not an
// expanded template type parameter pack) and forwarding parameters (passed to a
// parameter that is an expanded template type parameter pack).
class ForwardingCallVisitor : public clang::RecursiveASTVisitor<ForwardingCallVisitor> {
public:
    ForwardingCallVisitor(llvm::ArrayRef<const clang::ParmVarDecl*> Parameters) :
        Parameters{Parameters}, PackType{underlying_pack_type(Parameters.front())} {}

    bool VisitCallExpr(clang::CallExpr* E) {
        auto* Callee = getCalleeDeclOrUniqueOverload(E);
        if(Callee) {
            handleCall(Callee, E->arguments());
        }
        return !Info.has_value();
    }

    bool VisitCXXConstructExpr(clang::CXXConstructExpr* E) {
        auto* Callee = E->getConstructor();
        if(Callee) {
            handleCall(Callee, E->arguments());
        }
        return !Info.has_value();
    }

    // The expanded parameter pack to be resolved
    llvm::ArrayRef<const clang::ParmVarDecl*> Parameters;
    // The type of the parameter pack
    const clang::TemplateTypeParmType* PackType;

    struct ForwardingInfo {
        // If the parameters were resolved to another FunctionDecl, these are its
        // first non-variadic parameters (i.e. the first entries of the parameter
        // pack that are passed as arguments bound to a non-pack parameter.)
        llvm::ArrayRef<const clang::ParmVarDecl*> Head;
        // If the parameters were resolved to another FunctionDecl, these are its
        // variadic parameters (i.e. the entries of the parameter pack that are
        // passed as arguments bound to a pack parameter.)
        llvm::ArrayRef<const clang::ParmVarDecl*> Pack;
        // If the parameters were resolved to another FunctionDecl, these are its
        // last non-variadic parameters (i.e. the last entries of the parameter pack
        // that are passed as arguments bound to a non-pack parameter.)
        llvm::ArrayRef<const clang::ParmVarDecl*> Tail;
        // If the parameters were resolved to another FunctionDecl, this
        // is it.
        std::optional<clang::FunctionDecl*> PackTarget;
    };

    // The output of this visitor
    std::optional<ForwardingInfo> Info;

private:
    // inspects the given callee with the given args to check whether it
    // contains Parameters, and sets Info accordingly.
    void handleCall(clang::FunctionDecl* Callee, typename clang::CallExpr::arg_range Args) {
        // Skip functions with less parameters, they can't be the target.
        if(Callee->parameters().size() < Parameters.size())
            return;
        if(llvm::any_of(Args,
                        [](const clang::Expr* E) { return isa<clang::PackExpansionExpr>(E); })) {
            return;
        }
        auto PackLocation = findPack(Args);
        if(!PackLocation)
            return;
        llvm::ArrayRef<clang::ParmVarDecl*> MatchingParams =
            Callee->parameters().slice(*PackLocation, Parameters.size());
        // Check whether the function has a parameter pack as the last template
        // parameter
        if(const auto* TTPT = function_pack_type(Callee)) {
            // In this case: Separate the parameters into head, pack and tail
            auto IsExpandedPack = [&](const clang::ParmVarDecl* P) {
                return underlying_pack_type(P) == TTPT;
            };
            ForwardingInfo FI;
            FI.Head = MatchingParams.take_until(IsExpandedPack);
            FI.Pack = MatchingParams.drop_front(FI.Head.size()).take_while(IsExpandedPack);
            FI.Tail = MatchingParams.drop_front(FI.Head.size() + FI.Pack.size());
            FI.PackTarget = Callee;
            Info = FI;
            return;
        }
        // Default case: assume all parameters were fully resolved
        ForwardingInfo FI;
        FI.Head = MatchingParams;
        Info = FI;
    }

    // Returns the beginning of the expanded pack represented by Parameters
    // in the given arguments, if it is there.
    std::optional<size_t> findPack(typename clang::CallExpr::arg_range Args) {
        // find the argument directly referring to the first parameter
        assert(Parameters.size() <= static_cast<size_t>(llvm::size(Args)));
        for(auto Begin = Args.begin(), End = Args.end() - Parameters.size() + 1; Begin != End;
            ++Begin) {
            if(const auto* RefArg = unwrapForward(*Begin)) {
                if(Parameters.front() != RefArg->getDecl())
                    continue;
                // Check that this expands all the way until the last parameter.
                // It's enough to look at the last parameter, because it isn't possible
                // to expand without expanding all of them.
                auto ParamEnd = Begin + Parameters.size() - 1;
                RefArg = unwrapForward(*ParamEnd);
                if(!RefArg || Parameters.back() != RefArg->getDecl())
                    continue;
                return std::distance(Args.begin(), Begin);
            }
        }
        return std::nullopt;
    }

    static clang::FunctionDecl* getCalleeDeclOrUniqueOverload(clang::CallExpr* E) {
        clang::Decl* CalleeDecl = E->getCalleeDecl();
        auto* Callee = llvm::dyn_cast_or_null<clang::FunctionDecl>(CalleeDecl);
        if(!Callee) {
            if(auto* Lookup = dyn_cast<clang::UnresolvedLookupExpr>(E->getCallee())) {
                Callee = resolveOverload(Lookup, E);
            }
        }
        // Ignore the callee if the number of arguments is wrong (deal with va_args)
        if(Callee && Callee->getNumParams() == E->getNumArgs())
            return Callee;
        return nullptr;
    }

    static clang::FunctionDecl* resolveOverload(clang::UnresolvedLookupExpr* Lookup,
                                                clang::CallExpr* E) {
        clang::FunctionDecl* MatchingDecl = nullptr;
        if(!Lookup->requiresADL()) {
            // Check whether there is a single overload with this number of
            // parameters
            for(auto* Candidate: Lookup->decls()) {
                if(auto* FuncCandidate = llvm::dyn_cast_or_null<clang::FunctionDecl>(Candidate)) {
                    if(FuncCandidate->getNumParams() == E->getNumArgs()) {
                        if(MatchingDecl) {
                            // there are multiple candidates - abort
                            return nullptr;
                        }
                        MatchingDecl = FuncCandidate;
                    }
                }
            }
        }
        return MatchingDecl;
    }

    // Tries to get to the underlying argument by unwrapping implicit nodes and
    // std::forward.
    const static clang::DeclRefExpr* unwrapForward(const clang::Expr* E) {
        auto is_std_forward = [](const clang::FunctionDecl* Callee) {
            if(!Callee) {
                return false;
            }
            if(Callee->getBuiltinID() == clang::Builtin::BIforward) {
                return true;
            }
            const auto* callee_name = Callee->getIdentifier();
            if(!callee_name || callee_name->getName() != "forward") {
                return false;
            }
            // Walk up through inline namespaces (e.g. std::__1::forward).
            for(const clang::DeclContext* DC = Callee->getDeclContext(); DC; DC = DC->getParent()) {
                if(const auto* NS = llvm::dyn_cast<clang::NamespaceDecl>(DC)) {
                    if(NS->getName() == "std" && NS->getParent()->isTranslationUnit()) {
                        return true;
                    }
                }
            }
            return false;
        };

        E = E->IgnoreImplicitAsWritten();
        // There might be an implicit copy/move constructor call on top of the
        // forwarded arg.
        // FIXME: Maybe mark implicit calls in the AST to properly filter here.
        if(const auto* Const = llvm::dyn_cast<clang::CXXConstructExpr>(E))
            if(Const->getConstructor()->isCopyOrMoveConstructor())
                E = Const->getArg(0)->IgnoreImplicitAsWritten();
        if(const auto* Call = llvm::dyn_cast<clang::CallExpr>(E)) {
            if(is_std_forward(Call->getDirectCallee())) {
                return llvm::dyn_cast<clang::DeclRefExpr>(
                    Call->getArg(0)->IgnoreImplicitAsWritten());
            }
        }
        return llvm::dyn_cast<clang::DeclRefExpr>(E);
    }
};

}  // namespace

auto resolve_forwarding_params(const clang::FunctionDecl* D, unsigned MaxDepth)
    -> llvm::SmallVector<const clang::ParmVarDecl*> {
    assert(D);
    auto params = D->parameters();

    // If the function has a template parameter pack
    if(const auto* TTPT = function_pack_type(D)) {
        // Split the parameters into head, pack and tail
        auto IsExpandedPack = [TTPT](const clang::ParmVarDecl* P) {
            return underlying_pack_type(P) == TTPT;
        };
        llvm::ArrayRef<const clang::ParmVarDecl*> Head = params.take_until(IsExpandedPack);
        llvm::ArrayRef<const clang::ParmVarDecl*> Pack =
            params.drop_front(Head.size()).take_while(IsExpandedPack);
        llvm::ArrayRef<const clang::ParmVarDecl*> Tail =
            params.drop_front(Head.size() + Pack.size());
        llvm::SmallVector<const clang::ParmVarDecl*> Result(params.size());
        // Fill in non-pack parameters
        auto* HeadIt = std::copy(Head.begin(), Head.end(), Result.begin());
        auto TailIt = std::copy(Tail.rbegin(), Tail.rend(), Result.rbegin());
        // Recurse on pack parameters

        size_t Depth = 0;

        const clang::FunctionDecl* CurrentFunction = D;
        llvm::SmallSet<const clang::FunctionTemplateDecl*, 4> SeenTemplates;
        if(const auto* Template = D->getPrimaryTemplate()) {
            SeenTemplates.insert(Template);
        }

        while(!Pack.empty() && CurrentFunction && Depth < MaxDepth) {
            // Find call expressions involving the pack
            ForwardingCallVisitor V{Pack};
            V.TraverseStmt(CurrentFunction->getBody());
            if(!V.Info) {
                break;
            }
            // If we found something: Fill in non-pack parameters
            auto Info = *V.Info;
            HeadIt = std::copy(Info.Head.begin(), Info.Head.end(), HeadIt);
            TailIt = std::copy(Info.Tail.rbegin(), Info.Tail.rend(), TailIt);
            // Prepare next recursion level
            Pack = Info.Pack;
            CurrentFunction = Info.PackTarget.value_or(nullptr);
            Depth++;
            // If we are recursing into a previously encountered function: Abort
            if(CurrentFunction) {
                if(const auto* Template = CurrentFunction->getPrimaryTemplate()) {
                    bool NewFunction = SeenTemplates.insert(Template).second;
                    if(!NewFunction) {
                        return {params.begin(), params.end()};
                    }
                }
            }
        }

        // Fill in the remaining unresolved pack parameters
        HeadIt = std::copy(Pack.begin(), Pack.end(), HeadIt);
        assert(TailIt.base() == HeadIt);
        return Result;
    }
    return {params.begin(), params.end()};
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
