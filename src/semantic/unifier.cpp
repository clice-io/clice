#include "semantic/unifier.h"

#include "clang/AST/ExprCXX.h"
#include "clang/AST/RecursiveASTVisitor.h"

namespace clice::types {

namespace {

/// Strip local qualifiers and one-step sugar until a structural node is
/// reached, accumulating qualifiers into `quals`. Child sugar is preserved:
/// only the current level is desugared, so template arguments and pointees
/// keep the form the user wrote.
clang::QualType peel(clang::QualType type, clang::Qualifiers& quals) {
    while(true) {
        if(type.hasLocalQualifiers()) {
            quals.addQualifiers(type.getLocalQualifiers());
            type = type.getLocalUnqualifiedType();
            continue;
        }

        const clang::Type* T = type.getTypePtr();
        switch(T->getTypeClass()) {
            case clang::Type::Paren: {
                type = llvm::cast<clang::ParenType>(T)->getInnerType();
                continue;
            }
            case clang::Type::Using: {
                type = llvm::cast<clang::UsingType>(T)->desugar();
                continue;
            }
            case clang::Type::Typedef: {
                type = llvm::cast<clang::TypedefType>(T)->desugar();
                continue;
            }
            case clang::Type::SubstTemplateTypeParm: {
                type = llvm::cast<clang::SubstTemplateTypeParmType>(T)->getReplacementType();
                continue;
            }
            case clang::Type::MacroQualified: {
                type = llvm::cast<clang::MacroQualifiedType>(T)->getUnderlyingType();
                continue;
            }
            case clang::Type::Attributed: {
                type = llvm::cast<clang::AttributedType>(T)->getEquivalentType();
                continue;
            }
            case clang::Type::TemplateSpecialization: {
                auto TST = llvm::cast<clang::TemplateSpecializationType>(T);
                /// Alias specializations are sugar for the substituted
                /// underlying type; structural matching sees through them.
                if(TST->isTypeAlias()) {
                    type = TST->desugar();
                    continue;
                }
                return type;
            }
            default: {
                return type;
            }
        }
    }
}

/// Can `TD` be bound to the template template parameter `TTP`? Approximated
/// by arity (defaults and packs included) plus positional parameter kinds: a
/// unary parameter must not accept a binary template, and a type slot must
/// not accept a non-type one. Nested template-template lists are not
/// compared recursively.
bool template_compatible(const clang::TemplateTemplateParmDecl* TTP, clang::TemplateDecl* TD) {
    auto params = TTP->getTemplateParameters();
    auto candidate = TD->getTemplateParameters();

    auto kinds_match = [&](unsigned count) {
        for(unsigned i = 0; i < count; i += 1) {
            auto lhs = params->getParam(i);
            auto rhs = candidate->getParam(i);
            if(lhs->getKind() != rhs->getKind()) {
                return false;
            }
        }
        return true;
    };

    if(params->hasParameterPack()) {
        /// The pack absorbs every candidate slot past the fixed prefix, so
        /// each of those slots must agree with the pack's kind (a type pack
        /// cannot cover a non-type slot, defaulted or not) — and the
        /// candidate must be able to take the fixed prefix at all.
        unsigned fixed = params->size() - 1;
        if(candidate->size() < fixed && !candidate->hasParameterPack()) {
            return false;
        }
        if(!kinds_match(std::min(fixed, candidate->size()))) {
            return false;
        }
        auto pack_kind = params->getParam(params->size() - 1)->getKind();
        for(unsigned i = fixed; i < candidate->size(); i += 1) {
            if(candidate->getParam(i)->getKind() != pack_kind) {
                return false;
            }
        }
        return true;
    }
    if(params->size() < candidate->getMinRequiredArguments() ||
       (params->size() > candidate->size() && !candidate->hasParameterPack())) {
        return false;
    }
    return kinds_match(std::min(params->size(), candidate->size()));
}

/// Indices of the deduction-depth parameter packs referenced inside an
/// expansion pattern; a zero-length expansion must still bind them (to the
/// empty pack) so cardinality conflicts with other occurrences surface.
struct PackIndexCollector : clang::RecursiveASTVisitor<PackIndexCollector> {
    unsigned depth;
    llvm::SmallVector<unsigned, 2> indices;

    explicit PackIndexCollector(unsigned depth) : depth(depth) {}

    bool VisitTemplateTypeParmType(clang::TemplateTypeParmType* T) {
        if(T->isParameterPack() && T->getDepth() == depth) {
            indices.push_back(T->getIndex());
        }
        return true;
    }

    bool VisitDeclRefExpr(clang::DeclRefExpr* expr) {
        if(auto NTTP = llvm::dyn_cast<clang::NonTypeTemplateParmDecl>(expr->getDecl());
           NTTP && NTTP->isParameterPack() && NTTP->getDepth() == depth) {
            indices.push_back(NTTP->getIndex());
        }
        return true;
    }

    bool TraverseTemplateName(clang::TemplateName name) {
        if(auto TTP =
               llvm::dyn_cast_or_null<clang::TemplateTemplateParmDecl>(name.getAsTemplateDecl());
           TTP && TTP->isParameterPack() && TTP->getDepth() == depth) {
            indices.push_back(TTP->getIndex());
        }
        return RecursiveASTVisitor::TraverseTemplateName(name);
    }

    void traverse(const clang::TemplateArgument& argument) {
        switch(argument.getKind()) {
            case clang::TemplateArgument::Type: {
                TraverseType(argument.getAsType());
                break;
            }
            case clang::TemplateArgument::Expression: {
                TraverseStmt(argument.getAsExpr());
                break;
            }
            case clang::TemplateArgument::Template:
            case clang::TemplateArgument::TemplateExpansion: {
                TraverseTemplateName(argument.getAsTemplateOrTemplatePattern());
                break;
            }
            default: {
                break;
            }
        }
    }
};

}  // namespace

const clang::NonTypeTemplateParmDecl* referenced_nttp(const clang::Expr* expr) {
    if(!expr) {
        return nullptr;
    }
    if(auto DRE = llvm::dyn_cast<clang::DeclRefExpr>(expr->IgnoreParenImpCasts())) {
        return llvm::dyn_cast<clang::NonTypeTemplateParmDecl>(DRE->getDecl());
    }
    return nullptr;
}

bool Unifier::equivalent(const clang::TemplateArgument& lhs,
                         const clang::TemplateArgument& rhs) const {
    if(context.getCanonicalTemplateArgument(lhs).structurallyEquals(
           context.getCanonicalTemplateArgument(rhs))) {
        return true;
    }

    /// Dependent expression arguments keep distinct node identities even
    /// when they spell the same thing; two bare references to one
    /// parameter (`A<X, X>`) are the same value.
    if(lhs.getKind() == clang::TemplateArgument::Expression &&
       rhs.getKind() == clang::TemplateArgument::Expression) {
        auto* left = referenced_nttp(lhs.getAsExpr());
        return left && left == referenced_nttp(rhs.getAsExpr());
    }
    return false;
}

bool Unifier::bind(unsigned index, const clang::TemplateArgument& argument) {
    if(index >= bindings.size()) {
        return false;
    }

    auto& existing = bindings[index];
    if(existing.isNull()) {
        existing = argument;
        return true;
    }

    if(!equivalent(existing, argument)) {
        return false;
    }

    /// Same argument bound twice; keep the more sugared spelling.
    if(existing.getKind() == clang::TemplateArgument::Type &&
       argument.getKind() == clang::TemplateArgument::Type) {
        auto type = existing.getAsType();
        if(type == type.getCanonicalType() &&
           argument.getAsType() != argument.getAsType().getCanonicalType()) {
            existing = argument;
        }
    }
    return true;
}

bool Unifier::collect(unsigned index, const clang::TemplateArgument& argument) {
    if(index >= bindings.size()) {
        return false;
    }
    if(elements.size() < bindings.size()) {
        elements.resize(bindings.size());
    }

    auto& group = elements[index];

    /// A pack referenced twice in one expansion element (`pair<Ts, Ts>...`)
    /// must see the same value at every occurrence.
    if(group.size() == element_ordinal + 1) {
        return equivalent(group.back(), argument);
    }

    /// A pack that skipped an earlier element cannot re-synchronize.
    if(group.size() != element_ordinal) {
        return false;
    }

    group.push_back(argument);
    return true;
}

bool Unifier::template_id(clang::QualType type,
                          clang::TemplateName& name,
                          TemplateArguments& arguments) const {
    clang::Qualifiers quals;
    type = peel(type, quals);

    if(auto ICNT = llvm::dyn_cast<clang::InjectedClassNameType>(type)) {
        type = ICNT->getDecl()->getCanonicalTemplateSpecializationType(context);
    }

    if(auto TST = llvm::dyn_cast<clang::TemplateSpecializationType>(type)) {
        name = TST->getTemplateName();
        arguments = TST->template_arguments();
        return true;
    }

    if(auto RT = llvm::dyn_cast<clang::RecordType>(type)) {
        if(auto CTSD = llvm::dyn_cast<clang::ClassTemplateSpecializationDecl>(RT->getDecl())) {
            name = clang::TemplateName(CTSD->getSpecializedTemplate());
            arguments = CTSD->getTemplateArgs().asArray();
            return true;
        }
    }

    return false;
}

bool Unifier::unify(clang::QualType pattern, clang::QualType argument) {
    if(pattern.isNull() || argument.isNull()) {
        return false;
    }

    clang::Qualifiers pattern_quals;
    clang::Qualifiers argument_quals;
    pattern = peel(pattern, pattern_quals);
    argument = peel(argument, argument_quals);

    /// `cv-list T`: the parameter absorbs the qualifiers the pattern doesn't
    /// mention, so its qualifiers must be a subset of the argument's.
    ///
    /// Only parameters at the deduction depth bind; a parameter of an
    /// enclosing template is a concrete type from this deduction's point of
    /// view and falls through to structural comparison below — treating it
    /// as a wildcard would let `Inner<pair<O, U>>` match any first element.
    if(auto TTPT = llvm::dyn_cast<clang::TemplateTypeParmType>(pattern);
       TTPT && TTPT->getDepth() == depth) {
        if(!argument_quals.isStrictSupersetOf(pattern_quals) && argument_quals != pattern_quals) {
            return false;
        }
        auto remaining = argument_quals;
        remaining.removeQualifiers(pattern_quals);
        auto bound = context.getQualifiedType(argument, remaining);
        if(expanding && TTPT->isParameterPack()) {
            return collect(TTPT->getIndex(), clang::TemplateArgument(bound));
        }
        return bind(TTPT->getIndex(), clang::TemplateArgument(bound));
    }

    /// A pack expansion on the argument side has an unknown element count,
    /// but a fixed pattern must still be structurally admissible against one
    /// element (`void(int)` can never match `void(Us*...)`).
    if(auto APET = llvm::dyn_cast<clang::PackExpansionType>(argument)) {
        return unify(pattern, APET->getPattern());
    }

    /// Anything else matches structurally: qualifiers must agree exactly.
    if(pattern_quals != argument_quals) {
        return false;
    }

    switch(pattern->getTypeClass()) {
        case clang::Type::Pointer: {
            auto AP = llvm::dyn_cast<clang::PointerType>(argument);
            return AP && unify(llvm::cast<clang::PointerType>(pattern)->getPointeeType(),
                               AP->getPointeeType());
        }

        case clang::Type::LValueReference:
        case clang::Type::RValueReference: {
            if(pattern->getTypeClass() != argument->getTypeClass()) {
                return false;
            }
            return unify(llvm::cast<clang::ReferenceType>(pattern)->getPointeeType(),
                         llvm::cast<clang::ReferenceType>(argument)->getPointeeType());
        }

        case clang::Type::TemplateSpecialization:
        case clang::Type::InjectedClassName:
        case clang::Type::Record: {
            /// A specialization of a dependent template name is a
            /// non-deduced context: it constrains nothing.
            if(auto TST = llvm::dyn_cast<clang::TemplateSpecializationType>(pattern);
               TST && TST->getTemplateName().getAsDependentTemplateName()) {
                return true;
            }

            clang::TemplateName pattern_name, argument_name;
            TemplateArguments pattern_args, argument_args;
            if(!template_id(pattern, pattern_name, pattern_args)) {
                /// A plain record with no template head matches only itself.
                return context.hasSameUnqualifiedType(pattern, argument);
            }
            if(!template_id(argument, argument_name, argument_args)) {
                return false;
            }

            /// A template template parameter in the head deduces the
            /// argument's template, e.g. matching `X<TT<Us...>>`.
            if(auto TTP = llvm::dyn_cast_or_null<clang::TemplateTemplateParmDecl>(
                   pattern_name.getAsTemplateDecl());
               TTP && TTP->getDepth() == depth) {
                if(auto TD = argument_name.getAsTemplateDecl();
                   TD && !template_compatible(TTP, TD)) {
                    return false;
                }
                auto head = clang::TemplateArgument(argument_name);
                if(expanding && TTP->isParameterPack() ? !collect(TTP->getIndex(), head)
                                                       : !bind(TTP->getIndex(), head)) {
                    return false;
                }
            } else if(!context.hasSameTemplateName(pattern_name, argument_name)) {
                return false;
            }

            return unify(pattern_args, argument_args);
        }

        case clang::Type::FunctionProto: {
            auto PF = llvm::cast<clang::FunctionProtoType>(pattern);
            auto AF = llvm::dyn_cast<clang::FunctionProtoType>(argument);
            if(!AF || PF->isVariadic() != AF->isVariadic()) {
                return false;
            }

            /// The extended info (calling convention, regparm, ...) is part
            /// of the function type: an ABI-specific pattern must not accept
            /// the default ABI.
            if(PF->getExtInfo() != AF->getExtInfo()) {
                return false;
            }

            /// noexcept participates in the function type; a mismatch rejects
            /// the match unless either side's specification is still value
            /// dependent (`noexcept(B)` with unbound B).
            if(PF->getExceptionSpecType() != clang::EST_DependentNoexcept &&
               AF->getExceptionSpecType() != clang::EST_DependentNoexcept &&
               PF->isNothrow() != AF->isNothrow()) {
                return false;
            }

            /// `noexcept(B)` with a bare parameter deduces B from the
            /// argument's specification, like an array bound.
            if(PF->getExceptionSpecType() == clang::EST_DependentNoexcept) {
                if(auto NTTP = referenced_nttp(PF->getNoexceptExpr());
                   NTTP && NTTP->getDepth() == depth) {
                    clang::TemplateArgument bound;
                    if(AF->getExceptionSpecType() == clang::EST_DependentNoexcept &&
                       AF->getNoexceptExpr()) {
                        bound = clang::TemplateArgument(AF->getNoexceptExpr(),
                                                        /*IsCanonical=*/false);
                    } else if(!NTTP->getType()->isDependentType()) {
                        bound = clang::TemplateArgument(
                            context,
                            llvm::APSInt(llvm::APInt(1, AF->isNothrow() ? 1 : 0), true),
                            NTTP->getType());
                    }
                    if(!bound.isNull() && !bind(NTTP->getIndex(), bound)) {
                        return false;
                    }
                }
            }

            /// Method cv/ref qualifiers are part of the type as well:
            /// `R (C::*)(As...) const` must not accept a non-const one.
            if(PF->getMethodQuals() != AF->getMethodQuals() ||
               PF->getRefQualifier() != AF->getRefQualifier()) {
                return false;
            }
            if(!unify(PF->getReturnType(), AF->getReturnType())) {
                return false;
            }

            /// Parameter lists reuse the argument-list machinery so a trailing
            /// `As...` deduces element-wise like a trailing pack argument.
            llvm::SmallVector<clang::TemplateArgument, 4> pattern_params;
            for(auto type: PF->getParamTypes()) {
                pattern_params.emplace_back(type);
            }
            llvm::SmallVector<clang::TemplateArgument, 4> argument_params;
            for(auto type: AF->getParamTypes()) {
                argument_params.emplace_back(type);
            }
            return unify(pattern_params, argument_params);
        }

        case clang::Type::MemberPointer: {
            auto PM = llvm::cast<clang::MemberPointerType>(pattern);
            auto AM = llvm::dyn_cast<clang::MemberPointerType>(argument);
            if(!AM) {
                return false;
            }
            auto type_of = [](clang::NestedNameSpecifier NNS) -> const clang::Type* {
                return NNS && NNS.getKind() == clang::NestedNameSpecifier::Kind::Type
                           ? NNS.getAsType()
                           : nullptr;
            };
            auto pattern_cls = type_of(PM->getQualifier());
            auto argument_cls = type_of(AM->getQualifier());
            if(!pattern_cls || !argument_cls) {
                return false;
            }
            return unify(clang::QualType(pattern_cls, 0), clang::QualType(argument_cls, 0)) &&
                   unify(PM->getPointeeType(), AM->getPointeeType());
        }

        case clang::Type::ConstantArray: {
            auto PA = llvm::cast<clang::ConstantArrayType>(pattern);
            auto AA = llvm::dyn_cast<clang::ConstantArrayType>(argument);
            return AA && PA->getSize() == AA->getSize() &&
                   unify(PA->getElementType(), AA->getElementType());
        }

        case clang::Type::IncompleteArray: {
            auto AA = llvm::dyn_cast<clang::IncompleteArrayType>(argument);
            return AA && unify(llvm::cast<clang::IncompleteArrayType>(pattern)->getElementType(),
                               AA->getElementType());
        }

        case clang::Type::DependentSizedArray: {
            auto PA = llvm::cast<clang::DependentSizedArrayType>(pattern);

            /// `T[N]`: deduce N from a constant array bound. An integral
            /// argument needs a concrete type: `auto` bounds deduce as the
            /// array bound type per the language rules.
            if(auto NTTP = referenced_nttp(PA->getSizeExpr()); NTTP && NTTP->getDepth() == depth) {
                if(auto AA = llvm::dyn_cast<clang::ConstantArrayType>(argument)) {
                    auto type = NTTP->getType();
                    if(type->isDependentType()) {
                        type = context.getSizeType();
                    }
                    llvm::APSInt size(AA->getSize());
                    size.setIsUnsigned(type->isUnsignedIntegerType());

                    /// The bound must be representable in the parameter's
                    /// type: `bool N` never deduces from `U[2]`.
                    auto converted = size.extOrTrunc(context.getIntWidth(type));
                    if(!llvm::APSInt::isSameValue(converted.extOrTrunc(size.getBitWidth()), size)) {
                        return false;
                    }

                    if(!bind(NTTP->getIndex(), clang::TemplateArgument(context, converted, type))) {
                        return false;
                    }
                    return unify(PA->getElementType(), AA->getElementType());
                }
            }

            /// `T[N]` against another dependent-sized array deduces N from
            /// the argument's bound expression; the types must agree for the
            /// value to survive conversion.
            if(auto NTTP = referenced_nttp(PA->getSizeExpr()); NTTP && NTTP->getDepth() == depth) {
                if(auto AA = llvm::dyn_cast<clang::DependentSizedArrayType>(argument);
                   AA && AA->getSizeExpr()) {
                    if(!NTTP->getType()->isDependentType() &&
                       !AA->getSizeExpr()->getType()->isDependentType() &&
                       !context.hasSameUnqualifiedType(NTTP->getType(),
                                                       AA->getSizeExpr()->getType())) {
                        return false;
                    }
                    if(!bind(NTTP->getIndex(),
                             clang::TemplateArgument(AA->getSizeExpr(), /*IsCanonical=*/false))) {
                        return false;
                    }
                    return unify(PA->getElementType(), AA->getElementType());
                }
            }

            /// A bounded pattern (`U[N]`) only matches arrays that have a
            /// bound; `X[]` stays with the primary.
            if(llvm::isa<clang::ConstantArrayType, clang::DependentSizedArrayType>(argument)) {
                return unify(PA->getElementType(),
                             llvm::cast<clang::ArrayType>(argument)->getElementType());
            }
            return false;
        }

        /// Dependent forms we cannot look into are non-deduced contexts:
        /// they constrain nothing.
        case clang::Type::DependentName:
        case clang::Type::Decltype:
        case clang::Type::UnresolvedUsing:
        case clang::Type::PackExpansion: {
            return true;
        }

        default: {
            return context.hasSameUnqualifiedType(pattern, argument);
        }
    }
}

bool Unifier::unify(const clang::TemplateArgument& pattern,
                    const clang::TemplateArgument& argument) {
    switch(pattern.getKind()) {
        case clang::TemplateArgument::Type: {
            if(argument.getKind() != clang::TemplateArgument::Type) {
                return false;
            }
            return unify(pattern.getAsType(), argument.getAsType());
        }

        case clang::TemplateArgument::Expression: {
            /// A bare reference to an NTTP deduces it; any other expression
            /// is a non-deduced context, checked once the bindings are
            /// known (see Deferred). Constant expression arguments are
            /// normalized to Integral so downstream substitution (e.g. array
            /// bounds) sees a value, not an expression.
            if(auto NTTP = referenced_nttp(pattern.getAsExpr());
               NTTP && NTTP->getDepth() == depth) {
                auto bound = argument;
                if(argument.getKind() == clang::TemplateArgument::Expression) {
                    auto expr = argument.getAsExpr();

                    /// Value deduction requires the types to agree: a `bool`
                    /// pattern parameter never matches an `int` argument, no
                    /// matter the eventual value.
                    if(!NTTP->getType()->isDependentType() && !expr->getType()->isDependentType() &&
                       !context.hasSameUnqualifiedType(NTTP->getType(), expr->getType())) {
                        return false;
                    }

                    /// An integral TemplateArgument requires a concrete type;
                    /// a parameter typed by an earlier parameter (`T N`) stays
                    /// an expression until that type is substituted.
                    if(!expr->isValueDependent() && !NTTP->getType()->isDependentType()) {
                        if(auto value = expr->getIntegerConstantExpr(context)) {
                            bound = clang::TemplateArgument(context, *value, NTTP->getType());
                        }
                    }
                }
                if(expanding && NTTP->isParameterPack()) {
                    return collect(NTTP->getIndex(), bound);
                }
                return bind(NTTP->getIndex(), bound);
            }
            deferred_checks.push_back({.pattern = pattern.getAsExpr(), .argument = argument});
            return true;
        }

        case clang::TemplateArgument::Integral: {
            if(argument.getKind() == clang::TemplateArgument::Integral) {
                return context.hasSameType(pattern.getIntegralType(), argument.getIntegralType()) &&
                       llvm::APSInt::isSameValue(pattern.getAsIntegral(), argument.getAsIntegral());
            }
            /// As-written value arguments (`pick<false, ...>`) arrive as
            /// expressions; evaluate constants so value-specialized partials
            /// can match. The value's type is part of the identity for `auto`
            /// parameters (`trait<1, B>` must not swallow `trait<1L, Y>`).
            if(argument.getKind() == clang::TemplateArgument::Expression) {
                auto expr = argument.getAsExpr();
                if(!expr->isValueDependent() &&
                   context.hasSameType(pattern.getIntegralType(), expr->getType())) {
                    if(auto value = expr->getIntegerConstantExpr(context)) {
                        return llvm::APSInt::isSameValue(pattern.getAsIntegral(), *value);
                    }
                }
            }
            return false;
        }

        case clang::TemplateArgument::Template: {
            if(auto TTP = llvm::dyn_cast_or_null<clang::TemplateTemplateParmDecl>(
                   pattern.getAsTemplate().getAsTemplateDecl());
               TTP && TTP->getDepth() == depth) {
                if(argument.getKind() == clang::TemplateArgument::Template) {
                    if(auto TD = argument.getAsTemplate().getAsTemplateDecl();
                       TD && !template_compatible(TTP, TD)) {
                        return false;
                    }
                }
                if(expanding && TTP->isParameterPack()) {
                    return collect(TTP->getIndex(), argument);
                }
                return bind(TTP->getIndex(), argument);
            }
            if(argument.getKind() != clang::TemplateArgument::Template) {
                return false;
            }
            return context.hasSameTemplateName(pattern.getAsTemplate(), argument.getAsTemplate());
        }

        default: {
            auto lhs = context.getCanonicalTemplateArgument(pattern);
            auto rhs = context.getCanonicalTemplateArgument(argument);
            return lhs.structurallyEquals(rhs);
        }
    }
}

bool Unifier::unify(TemplateArguments patterns, TemplateArguments arguments) {
    /// Flatten Pack entries on both sides so positional matching lines up:
    /// converted argument lists (injected arguments, partial specialization
    /// patterns) group a pack's arguments as `Pack{...}`, and substitution
    /// may produce Pack entries inline.
    llvm::SmallVector<clang::TemplateArgument, 4> flat_patterns;
    for(auto& pattern: patterns) {
        if(pattern.getKind() == clang::TemplateArgument::Pack) {
            flat_patterns.append(pattern.pack_begin(), pattern.pack_end());
        } else {
            flat_patterns.push_back(pattern);
        }
    }

    llvm::SmallVector<clang::TemplateArgument, 4> flat;
    for(auto& argument: arguments) {
        if(argument.getKind() == clang::TemplateArgument::Pack) {
            flat.append(argument.pack_begin(), argument.pack_end());
        } else {
            flat.push_back(argument);
        }
    }

    unsigned i = 0;
    for(unsigned p = 0; p < flat_patterns.size(); p += 1) {
        auto& pattern = flat_patterns[p];
        if(pattern.isPackExpansion()) {
            /// Nested expansions stay non-deduced.
            if(expanding) {
                return true;
            }

            /// A pack expansion matches arguments element-wise: pack
            /// parameters inside the pattern accumulate one binding per
            /// element (`box<Us>...` against `box<int>, box<X>` deduces
            /// `Us = {int, X}`), while non-pack parameters must deduce
            /// consistently across elements. The element count is fixed by
            /// the arity — whatever the fixed suffix (`void(Ts..., int)`)
            /// does not claim belongs to the expansion. A second expansion
            /// has no unique split and rejects.
            unsigned suffix = flat_patterns.size() - p - 1;
            for(unsigned q = p + 1; q < flat_patterns.size(); q += 1) {
                if(flat_patterns[q].isPackExpansion()) {
                    return false;
                }
            }
            if(flat.size() < i + suffix) {
                return false;
            }
            unsigned length = flat.size() - i - suffix;

            auto inner = pattern.getPackExpansionPattern();
            elements.clear();
            elements.resize(bindings.size());
            expanding = true;
            bool matched = true;
            for(unsigned j = i; j < i + length; j += 1) {
                element_ordinal = j - i;
                if(!unify(inner, flat[j])) {
                    matched = false;
                    break;
                }
            }
            expanding = false;
            if(!matched) {
                return false;
            }

            /// A zero-length expansion collected nothing, but its packs must
            /// still bind to the empty pack: `tuple<Ts...>` against `tuple<>`
            /// conflicts with `Ts = {X}` from another occurrence. With a
            /// non-zero length, empty groups belong to packs guarded away by
            /// nested expansions and stay unbound.
            if(length == 0) {
                PackIndexCollector collector(depth);
                collector.traverse(inner);
                for(auto index: collector.indices) {
                    if(!bind(index, clang::TemplateArgument::CreatePackCopy(context, {}))) {
                        return false;
                    }
                }
            }
            for(auto [index, group]: llvm::enumerate(elements)) {
                if(!group.empty() &&
                   !bind(index, clang::TemplateArgument::CreatePackCopy(context, group))) {
                    return false;
                }
            }
            i += length;
            continue;
        }

        if(i >= flat.size()) {
            return false;
        }
        if(!unify(pattern, flat[i])) {
            return false;
        }
        i += 1;
    }

    return i == flat.size();
}

namespace {

llvm::APSInt truth(bool value) {
    return llvm::APSInt(llvm::APInt(1, value ? 1 : 0), /*isUnsigned=*/true);
}

struct Folder {
    clang::ASTContext& context;
    llvm::function_ref<std::optional<llvm::APSInt>(const clang::Expr*)> value_of;

    /// A node whose type is still dependent (`Config<T>::k + 1`) keeps the
    /// width its operands gave it.
    std::optional<llvm::APSInt> typed(const llvm::APSInt& value, clang::QualType type) {
        return type->isDependentType() ? value : convert_integral(context, value, type);
    }

    std::optional<llvm::APSInt> fold(const clang::Expr* expr) {
        if(!expr->isValueDependent()) {
            return expr->getIntegerConstantExpr(context);
        }

        expr = expr->IgnoreParens();
        if(auto* cast = llvm::dyn_cast<clang::CastExpr>(expr)) {
            auto value = fold(cast->getSubExpr());
            return value ? convert_integral(context, *value, cast->getType()) : std::nullopt;
        }

        if(auto* unary = llvm::dyn_cast<clang::UnaryOperator>(expr)) {
            auto value = fold(unary->getSubExpr());
            if(!value) {
                return std::nullopt;
            }
            switch(unary->getOpcode()) {
                case clang::UO_Plus: break;
                case clang::UO_Minus: *value = -*value; break;
                case clang::UO_Not: *value = ~*value; break;
                case clang::UO_LNot: *value = truth(value->isZero()); break;
                default: return std::nullopt;
            }
            return typed(*value, unary->getType());
        }

        if(auto* binary = llvm::dyn_cast<clang::BinaryOperator>(expr)) {
            return fold_binary(binary);
        }

        if(auto* conditional = llvm::dyn_cast<clang::ConditionalOperator>(expr)) {
            auto condition = fold(conditional->getCond());
            if(!condition) {
                return std::nullopt;
            }
            auto value = fold(condition->isZero() ? conditional->getFalseExpr()
                                                  : conditional->getTrueExpr());
            return value ? typed(*value, conditional->getType()) : std::nullopt;
        }

        auto value = value_of(expr);
        return value ? typed(*value, expr->getType()) : std::nullopt;
    }

    std::optional<llvm::APSInt> fold_binary(const clang::BinaryOperator* binary) {
        auto op = binary->getOpcode();
        auto lhs = fold(binary->getLHS());

        /// Either operand of a logical operator can settle it alone
        /// (`unknown && false`); folding has no side effects to preserve.
        if(op == clang::BO_LAnd || op == clang::BO_LOr) {
            bool absorbing = op == clang::BO_LOr;
            if(lhs && lhs->getBoolValue() == absorbing) {
                return typed(truth(absorbing), binary->getType());
            }
            auto rhs = fold(binary->getRHS());
            if(rhs && rhs->getBoolValue() == absorbing) {
                return typed(truth(absorbing), binary->getType());
            }
            if(!lhs || !rhs) {
                return std::nullopt;
            }
            return typed(truth(!absorbing), binary->getType());
        }

        auto rhs = fold(binary->getRHS());
        if(!lhs || !rhs) {
            return std::nullopt;
        }

        if(op == clang::BO_Shl || op == clang::BO_Shr) {
            if(rhs->isNegative() || rhs->uge(lhs->getBitWidth())) {
                return std::nullopt;
            }
            auto amount = static_cast<unsigned>(rhs->getZExtValue());
            auto result = op == clang::BO_Shl ? *lhs << amount : *lhs >> amount;
            return typed(result, binary->getType());
        }

        /// Sema has already brought both operands to their common type; a
        /// mismatch means an operand we folded through an unmodeled path.
        if(lhs->getBitWidth() != rhs->getBitWidth() || lhs->isUnsigned() != rhs->isUnsigned()) {
            return std::nullopt;
        }

        llvm::APSInt result;
        switch(op) {
            case clang::BO_Mul: result = *lhs * *rhs; break;
            case clang::BO_Div:
            case clang::BO_Rem: {
                if(rhs->isZero()) {
                    return std::nullopt;
                }
                result = op == clang::BO_Div ? *lhs / *rhs : *lhs % *rhs;
                break;
            }
            case clang::BO_Add: result = *lhs + *rhs; break;
            case clang::BO_Sub: result = *lhs - *rhs; break;
            case clang::BO_And: result = *lhs & *rhs; break;
            case clang::BO_Xor: result = *lhs ^ *rhs; break;
            case clang::BO_Or: result = *lhs | *rhs; break;
            case clang::BO_LT: result = truth(*lhs < *rhs); break;
            case clang::BO_GT: result = truth(*lhs > *rhs); break;
            case clang::BO_LE: result = truth(*lhs <= *rhs); break;
            case clang::BO_GE: result = truth(*lhs >= *rhs); break;
            case clang::BO_EQ: result = truth(*lhs == *rhs); break;
            case clang::BO_NE: result = truth(*lhs != *rhs); break;
            default: return std::nullopt;
        }
        return typed(result, binary->getType());
    }
};

/// The value of a converted argument, when it is one.
std::optional<llvm::APSInt> constant_value(clang::ASTContext& context,
                                           const clang::TemplateArgument& argument) {
    if(argument.getKind() == clang::TemplateArgument::Integral) {
        return argument.getAsIntegral();
    }
    if(argument.getKind() == clang::TemplateArgument::Expression &&
       !argument.getAsExpr()->isValueDependent()) {
        return argument.getAsExpr()->getIntegerConstantExpr(context);
    }
    return std::nullopt;
}

bool same_profile(clang::ASTContext& context, const clang::Expr* lhs, const clang::Expr* rhs) {
    llvm::FoldingSetNodeID left;
    llvm::FoldingSetNodeID right;
    lhs->Profile(left, context, /*Canonical=*/true);
    rhs->Profile(right, context, /*Canonical=*/true);
    return left == right;
}

/// Is `pattern`, with the bindings of `params` substituted in, the same
/// expression as `argument`? Compared node by node over the operators the
/// folding models; a bound parameter matches the subexpression its binding
/// denotes. Anything else is not provably the same.
bool same_expression(clang::ASTContext& context,
                     clang::TemplateParameterList* params,
                     llvm::ArrayRef<clang::TemplateArgument> bindings,
                     const clang::Expr* pattern,
                     const clang::Expr* argument) {
    pattern = pattern->IgnoreParenImpCasts();
    argument = argument->IgnoreParenImpCasts();

    if(auto NTTP = referenced_nttp(pattern);
       NTTP && NTTP->getDepth() == params->getDepth() && NTTP->getIndex() < bindings.size()) {
        auto& bound = bindings[NTTP->getIndex()];
        if(bound.getKind() == clang::TemplateArgument::Expression) {
            return same_profile(context, bound.getAsExpr()->IgnoreParenImpCasts(), argument);
        }
        auto expected = constant_value(context, bound);
        auto actual =
            argument->isValueDependent() ? std::nullopt : argument->getIntegerConstantExpr(context);
        return expected && actual && llvm::APSInt::isSameValue(*expected, *actual);
    }

    if(!pattern->isValueDependent() && !argument->isValueDependent()) {
        auto expected = pattern->getIntegerConstantExpr(context);
        auto actual = argument->getIntegerConstantExpr(context);
        return expected && actual && llvm::APSInt::isSameValue(*expected, *actual);
    }

    if(pattern->getStmtClass() != argument->getStmtClass()) {
        return false;
    }
    auto same = [&](const clang::Expr* lhs, const clang::Expr* rhs) {
        return same_expression(context, params, bindings, lhs, rhs);
    };
    if(auto* binary = llvm::dyn_cast<clang::BinaryOperator>(pattern)) {
        auto* other = llvm::cast<clang::BinaryOperator>(argument);
        return binary->getOpcode() == other->getOpcode() &&
               same(binary->getLHS(), other->getLHS()) && same(binary->getRHS(), other->getRHS());
    }
    if(auto* unary = llvm::dyn_cast<clang::UnaryOperator>(pattern)) {
        auto* other = llvm::cast<clang::UnaryOperator>(argument);
        return unary->getOpcode() == other->getOpcode() &&
               same(unary->getSubExpr(), other->getSubExpr());
    }
    if(auto* conditional = llvm::dyn_cast<clang::ConditionalOperator>(pattern)) {
        auto* other = llvm::cast<clang::ConditionalOperator>(argument);
        return same(conditional->getCond(), other->getCond()) &&
               same(conditional->getTrueExpr(), other->getTrueExpr()) &&
               same(conditional->getFalseExpr(), other->getFalseExpr());
    }
    return false;
}

}  // namespace

std::optional<llvm::APSInt> convert_integral(clang::ASTContext& context,
                                             const llvm::APSInt& value,
                                             clang::QualType type) {
    if(type->isDependentType() || !type->isIntegralOrEnumerationType()) {
        return std::nullopt;
    }
    auto width = context.getIntWidth(type);
    if(type->isBooleanType()) {
        return llvm::APSInt(llvm::APInt(width, value.getBoolValue() ? 1 : 0), /*isUnsigned=*/true);
    }
    auto result = value.extOrTrunc(width);
    result.setIsUnsigned(type->isUnsignedIntegerOrEnumerationType());
    return result;
}

std::optional<llvm::APSInt> evaluate_integral(
    clang::ASTContext& context,
    const clang::Expr* expr,
    llvm::function_ref<std::optional<llvm::APSInt>(const clang::Expr*)> value_of) {
    return Folder{.context = context, .value_of = value_of}.fold(expr);
}

Deduction deduce_arguments(clang::ASTContext& context,
                           clang::TemplateParameterList* params,
                           llvm::ArrayRef<clang::TemplateArgument> patterns,
                           llvm::ArrayRef<clang::TemplateArgument> arguments,
                           llvm::SmallVectorImpl<clang::TemplateArgument>& deduced) {
    Unifier unifier(context, params->getDepth(), params->size());
    if(!unifier.unify(patterns, arguments)) {
        return Deduction::Failed;
    }

    deduced.assign(unifier.results().begin(), unifier.results().end());
    for(auto [i, argument]: llvm::enumerate(deduced)) {
        if(!argument.isNull()) {
            continue;
        }

        /// An unbound pack deduces as empty.
        if(params->getParam(i)->isTemplateParameterPack()) {
            argument = clang::TemplateArgument::CreatePackCopy(context, {});
            continue;
        }

        return Deduction::Failed;
    }

    auto value_of = [&](const clang::Expr* expr) -> std::optional<llvm::APSInt> {
        auto NTTP = referenced_nttp(expr);
        if(!NTTP || NTTP->getDepth() != params->getDepth() || NTTP->getIndex() >= deduced.size()) {
            return std::nullopt;
        }
        return constant_value(context, deduced[NTTP->getIndex()]);
    };

    auto verdict = Deduction::Matched;
    for(auto& check: unifier.deferred()) {
        auto expected = evaluate_integral(context, check.pattern, value_of);
        auto actual = constant_value(context, check.argument);
        if(expected && actual) {
            if(!llvm::APSInt::isSameValue(*expected, *actual)) {
                return Deduction::Failed;
            }
            continue;
        }
        if(check.argument.getKind() == clang::TemplateArgument::Expression &&
           same_expression(context, params, deduced, check.pattern, check.argument.getAsExpr())) {
            continue;
        }
        verdict = Deduction::Unverified;
    }

    llvm::SmallVector<clang::AssociatedConstraint, 2> constraints;
    params->getAssociatedConstraints(constraints);
    for(auto& constraint: constraints) {
        auto satisfied = evaluate_integral(context, constraint.ConstraintExpr, value_of);
        if(!satisfied) {
            verdict = Deduction::Unverified;
        } else if(satisfied->isZero()) {
            return Deduction::Failed;
        }
    }
    return verdict;
}

namespace {

/// Partial ordering via symmetric deduction: `left` is more specialized than
/// `right` iff right's pattern matches left's and not vice versa — or the
/// patterns are equivalent and only `left` is constrained.
template <typename Partial>
bool more_specialized(clang::ASTContext& context, Partial* left, Partial* right) {
    auto matches = [&](Partial* pattern, Partial* argument) {
        auto params = pattern->getTemplateParameters();
        Unifier unifier(context, params->getDepth(), params->size());
        return unifier.unify(pattern->getTemplateArgs().asArray(),
                             argument->getTemplateArgs().asArray());
    };

    bool forward = matches(right, left);
    if(forward != matches(left, right)) {
        return forward;
    }
    return forward && left->getTemplateParameters()->hasAssociatedConstraints() &&
           !right->getTemplateParameters()->hasAssociatedConstraints();
}

template <typename Partial>
PartialChoice<Partial> select_partial_impl(clang::ASTContext& context,
                                           llvm::ArrayRef<PartialMatch<Partial>> viable) {
    const PartialMatch<Partial>* best = nullptr;
    for(auto& match: viable) {
        if(!best || more_specialized(context, match.partial, best->partial)) {
            best = &match;
        }
    }
    if(!best) {
        return {};
    }
    for(auto& match: viable) {
        if(&match != best && !more_specialized(context, best->partial, match.partial)) {
            return {.verdict = PartialVerdict::Ambiguous};
        }
    }
    if(!best->verified) {
        return {.verdict = PartialVerdict::Unverified};
    }
    return {.verdict = PartialVerdict::Selected, .winner = best->partial};
}

}  // namespace

PartialChoice<clang::ClassTemplatePartialSpecializationDecl> select_partial(
    clang::ASTContext& context,
    llvm::ArrayRef<PartialMatch<clang::ClassTemplatePartialSpecializationDecl>> viable) {
    return select_partial_impl(context, viable);
}

PartialChoice<clang::VarTemplatePartialSpecializationDecl> select_partial(
    clang::ASTContext& context,
    llvm::ArrayRef<PartialMatch<clang::VarTemplatePartialSpecializationDecl>> viable) {
    return select_partial_impl(context, viable);
}

}  // namespace clice::types
