#include "semantic/unifier.h"

#include "clang/AST/ExprCXX.h"

namespace clice {

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
            case clang::Type::Elaborated: {
                type = llvm::cast<clang::ElaboratedType>(T)->getNamedType();
                continue;
            }
            case clang::Type::Paren: {
                type = llvm::cast<clang::ParenType>(T)->getInnerType();
                continue;
            }
            case clang::Type::Using: {
                type = llvm::cast<clang::UsingType>(T)->getUnderlyingType();
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

bool TypeUnifier::bind(unsigned index, const clang::TemplateArgument& argument) {
    if(index >= bindings.size()) {
        return false;
    }

    auto& existing = bindings[index];
    if(existing.isNull()) {
        existing = argument;
        return true;
    }

    auto lhs = context.getCanonicalTemplateArgument(existing);
    auto rhs = context.getCanonicalTemplateArgument(argument);
    if(!lhs.structurallyEquals(rhs)) {
        return false;
    }

    /// Same argument bound twice; keep the more sugared spelling.
    if(existing.getKind() == clang::TemplateArgument::Type &&
       argument.getKind() == clang::TemplateArgument::Type) {
        auto type = existing.getAsType();
        if(type == type.getCanonicalType() && argument.getAsType() != rhs.getAsType()) {
            existing = argument;
        }
    }
    return true;
}

bool TypeUnifier::collect(unsigned index, const clang::TemplateArgument& argument) {
    if(index >= bindings.size()) {
        return false;
    }
    if(elements.size() < bindings.size()) {
        elements.resize(bindings.size());
    }
    elements[index].push_back(argument);
    return true;
}

bool TypeUnifier::template_id(clang::QualType type,
                              clang::TemplateName& name,
                              TemplateArguments& arguments) const {
    clang::Qualifiers quals;
    type = peel(type, quals);

    if(auto ICNT = llvm::dyn_cast<clang::InjectedClassNameType>(type)) {
        type = ICNT->getInjectedSpecializationType();
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

bool TypeUnifier::unify(clang::QualType pattern, clang::QualType argument) {
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

    /// A pack expansion on the argument side cannot be matched structurally
    /// (its element count is unknown); treat it as non-deduced.
    if(llvm::isa<clang::PackExpansionType>(argument)) {
        return true;
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

            /// noexcept participates in the function type; a mismatch rejects
            /// the match unless either side's specification is still value
            /// dependent (`noexcept(B)` with unbound B).
            if(PF->getExceptionSpecType() != clang::EST_DependentNoexcept &&
               AF->getExceptionSpecType() != clang::EST_DependentNoexcept &&
               PF->isNothrow() != AF->isNothrow()) {
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
            auto pattern_cls = PM->getQualifier() ? PM->getQualifier()->getAsType() : nullptr;
            auto argument_cls = AM->getQualifier() ? AM->getQualifier()->getAsType() : nullptr;
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

        case clang::Type::DependentSizedArray: {
            auto PA = llvm::cast<clang::DependentSizedArrayType>(pattern);

            /// `T[N]`: deduce N from a constant array bound.
            if(auto NTTP = referenced_nttp(PA->getSizeExpr()); NTTP && NTTP->getDepth() == depth) {
                if(auto AA = llvm::dyn_cast<clang::ConstantArrayType>(argument)) {
                    llvm::APSInt size(AA->getSize());
                    size.setIsUnsigned(NTTP->getType()->isUnsignedIntegerType());
                    if(!bind(NTTP->getIndex(),
                             clang::TemplateArgument(context, size, NTTP->getType()))) {
                        return false;
                    }
                    return unify(PA->getElementType(), AA->getElementType());
                }
            }

            if(auto AA = llvm::dyn_cast<clang::ArrayType>(argument)) {
                return unify(PA->getElementType(), AA->getElementType());
            }
            return false;
        }

        /// Dependent forms we cannot look into are non-deduced contexts:
        /// they constrain nothing.
        case clang::Type::DependentName:
        case clang::Type::DependentTemplateSpecialization:
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

bool TypeUnifier::unify(const clang::TemplateArgument& pattern,
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
            /// is a non-deduced context. Constant expression arguments are
            /// normalized to Integral so downstream substitution (e.g. array
            /// bounds) sees a value, not an expression.
            if(auto NTTP = referenced_nttp(pattern.getAsExpr());
               NTTP && NTTP->getDepth() == depth) {
                auto bound = argument;
                if(argument.getKind() == clang::TemplateArgument::Expression) {
                    auto expr = argument.getAsExpr();
                    if(!expr->isValueDependent()) {
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
            return true;
        }

        case clang::TemplateArgument::Integral: {
            if(argument.getKind() == clang::TemplateArgument::Integral) {
                return llvm::APSInt::isSameValue(pattern.getAsIntegral(), argument.getAsIntegral());
            }
            /// As-written value arguments (`pick<false, ...>`) arrive as
            /// expressions; evaluate constants so value-specialized partials
            /// can match.
            if(argument.getKind() == clang::TemplateArgument::Expression) {
                auto expr = argument.getAsExpr();
                if(!expr->isValueDependent()) {
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

bool TypeUnifier::unify(TemplateArguments patterns, TemplateArguments arguments) {
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
    for(auto& pattern: flat_patterns) {
        if(pattern.isPackExpansion()) {
            /// A trailing pack expansion matches all remaining arguments
            /// element-wise: pack parameters inside the pattern accumulate
            /// one binding per argument (`box<Us>...` against
            /// `box<int>, box<X>` deduces `Us = {int, X}`), while non-pack
            /// parameters must deduce consistently across elements. Nested
            /// expansions stay non-deduced.
            if(expanding) {
                return true;
            }

            auto inner = pattern.getPackExpansionPattern();
            elements.clear();
            elements.resize(bindings.size());
            expanding = true;
            bool matched = true;
            for(unsigned j = i; j < flat.size(); j += 1) {
                if(!unify(inner, flat[j])) {
                    matched = false;
                    break;
                }
            }
            expanding = false;
            if(!matched) {
                return false;
            }

            for(auto [index, group]: llvm::enumerate(elements)) {
                if(!group.empty() &&
                   !bind(index, clang::TemplateArgument::CreatePackCopy(context, group))) {
                    return false;
                }
            }
            return true;
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

bool deduce_arguments(clang::ASTContext& context,
                      clang::TemplateParameterList* params,
                      llvm::ArrayRef<clang::TemplateArgument> patterns,
                      llvm::ArrayRef<clang::TemplateArgument> arguments,
                      llvm::SmallVectorImpl<clang::TemplateArgument>& deduced) {
    TypeUnifier unifier(context, params->getDepth(), params->size());
    if(!unifier.unify(patterns, arguments)) {
        return false;
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

        return false;
    }
    return true;
}

bool more_specialized(clang::ASTContext& context,
                      clang::ClassTemplatePartialSpecializationDecl* left,
                      clang::ClassTemplatePartialSpecializationDecl* right) {
    auto matches = [&](clang::ClassTemplatePartialSpecializationDecl* pattern,
                       clang::ClassTemplatePartialSpecializationDecl* argument) {
        auto params = pattern->getTemplateParameters();
        TypeUnifier unifier(context, params->getDepth(), params->size());
        return unifier.unify(pattern->getTemplateArgs().asArray(),
                             argument->getTemplateArgs().asArray());
    };

    return matches(right, left) && !matches(left, right);
}

}  // namespace clice
