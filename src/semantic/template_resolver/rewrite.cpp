module clice;

import :semantic.decls;
import :semantic.template_resolver;
import :semantic.template_resolver.deduction;
import :semantic.template_resolver.instantiator;
import :support.logging;

namespace clice::types {

clang::QualType PseudoInstantiator::resolve(clang::QualType type) {
    return rewrite(type, Policy::Resolve);
}

clang::QualType PseudoInstantiator::substitute(clang::QualType type) {
    return rewrite(type, Policy::Substitute);
}

clang::QualType PseudoInstantiator::rewrite(clang::QualType type, Policy policy) {
    if(type.isNull() || !type->isDependentType()) {
        return type;
    }
    steps += 1;
    if(depth > 16 || steps > step_budget) {
        inconclusive = true;
        return type;
    }
    depth += 1;
    auto result = rewrite_type(type, policy);
    depth -= 1;
    return result.isNull() ? type : result;
}

clang::QualType PseudoInstantiator::rewrite_type(clang::QualType type, Policy policy) {
    auto quals = type.getLocalQualifiers();
    const clang::Type* T = type.getLocalUnqualifiedType().getTypePtr();

    clang::QualType result;
    switch(T->getTypeClass()) {
        case clang::Type::TemplateTypeParm: {
            result = rewrite_parameter(llvm::cast<clang::TemplateTypeParmType>(T), policy);
            break;
        }

        /// Sugar nodes: rewrite what they point at; the wrapper is dropped,
        /// which is fine because consumers compare canonically or look
        /// through sugar.
        case clang::Type::Paren: {
            result = rewrite(llvm::cast<clang::ParenType>(T)->getInnerType(), policy);
            break;
        }
        case clang::Type::Using: {
            result = rewrite(llvm::cast<clang::UsingType>(T)->desugar(), policy);
            break;
        }
        case clang::Type::MacroQualified: {
            result = rewrite(llvm::cast<clang::MacroQualifiedType>(T)->getUnderlyingType(), policy);
            break;
        }
        case clang::Type::SubstTemplateTypeParm: {
            result = rewrite(llvm::cast<clang::SubstTemplateTypeParmType>(T)->getReplacementType(),
                             policy);
            break;
        }

        /// Dependent typedefs expand under Policy::Substitute regardless of
        /// the current policy — the invariant that breaks typedef ↔ lookup
        /// cycles lives on this single line.
        case clang::Type::Typedef: {
            auto TND = llvm::cast<clang::TypedefType>(T)->getDecl();
            auto underlying = TND->getUnderlyingType();
            if(underlying->isDependentType()) {
                result = substitute(underlying);
            }
            break;
        }

        case clang::Type::InjectedClassName: {
            auto ICT = llvm::cast<clang::InjectedClassNameType>(T);
            result =
                rewrite(ICT->getDecl()->getCanonicalTemplateSpecializationType(context), policy);
            break;
        }

        /// A member class of a template is written in the template's
        /// parameters through the class enclosing it: the type takes that
        /// class, rewritten, as its qualifier, through which lookup binds
        /// the parameters again.
        case clang::Type::Record: {
            auto* RT = llvm::cast<clang::RecordType>(T);
            auto qualifier = RT->getQualifier();
            if(!qualifier) {
                auto* parent =
                    llvm::dyn_cast<clang::CXXRecordDecl>(RT->getDecl()->getDeclContext());
                if(!parent || !parent->isDependentContext()) {
                    break;
                }
                auto enclosing =
                    parent->getDescribedClassTemplate() ||
                            llvm::isa<clang::ClassTemplatePartialSpecializationDecl>(parent)
                        ? parent->getCanonicalTemplateSpecializationType(context)
                        : context.getCanonicalTagType(parent);
                qualifier = clang::NestedNameSpecifier(enclosing.getTypePtr());
            }
            if(auto rewritten = rewrite_specifier(qualifier, policy); rewritten != qualifier) {
                result = context.getTagType(RT->getKeyword(),
                                            rewritten,
                                            RT->getDecl(),
                                            /*OwnsTag=*/false);
            }
            break;
        }

        /// `using typename Base::name;` names what `typename
        /// Base::name` would.
        case clang::Type::UnresolvedUsing: {
            auto* decl = llvm::cast<clang::UnresolvedUsingType>(T)->getDecl();
            result = rewrite(context.getDependentNameType(clang::ElaboratedTypeKeyword::Typename,
                                                          decl->getQualifier(),
                                                          decl->getIdentifier()),
                             policy);
            break;
        }

        case clang::Type::TemplateSpecialization: {
            auto TST = llvm::cast<clang::TemplateSpecializationType>(T);
            auto dependent = TST->getTemplateName().getAsDependentTemplateName();
            if(dependent && policy == Policy::Resolve) {
                result = resolve_dependent_template(TST);
            } else if(dependent) {
                auto name = rewrite_template_name(TST->getTemplateName(), policy);
                if(!name) {
                    break;
                }
                llvm::SmallVector<clang::TemplateArgument, 4> arguments;
                bool changed = rewrite_arguments(TST->template_arguments(), arguments, policy);
                if(!name->getAsDependentTemplateName()) {
                    result = make_specialization(*name, arguments);
                } else if(*name != TST->getTemplateName() || changed) {
                    result = context.getTemplateSpecializationType(TST->getKeyword(),
                                                                   *name,
                                                                   arguments,
                                                                   /*CanonicalArgs=*/{});
                }
            } else {
                result = rewrite_template(TST, policy);
            }
            break;
        }

        case clang::Type::DependentName: {
            auto DNT = llvm::cast<clang::DependentNameType>(T);
            if(policy == Policy::Resolve) {
                result = resolve_dependent_name(DNT);
            } else {
                auto NNS = rewrite_specifier(DNT->getQualifier(), policy);
                if(NNS != DNT->getQualifier()) {
                    result =
                        context.getDependentNameType(DNT->getKeyword(), NNS, DNT->getIdentifier());
                }
            }
            break;
        }

        case clang::Type::Pointer: {
            auto pointee = rewrite(llvm::cast<clang::PointerType>(T)->getPointeeType(), policy);
            /// A substituted reference pointee makes the pointer
            /// ill-formed (`P<X&>::type` with `type = T*`); degrade
            /// rather than fabricate a malformed node.
            if(pointee->isReferenceType()) {
                break;
            }
            result = context.getPointerType(pointee);
            break;
        }

        /// Substituted reference pointees collapse per the language
        /// rules: `&` wins over anything, `&& &&` stays `&&`. Pointees a
        /// reference cannot bind to (`void`, qualified function types)
        /// degrade instead of fabricating an invalid node.
        case clang::Type::LValueReference: {
            auto pointee = rewrite(llvm::cast<clang::ReferenceType>(T)->getPointeeType(), policy);
            if(!pointee.getNonReferenceType().isReferenceable()) {
                break;
            }
            result = context.getLValueReferenceType(pointee.getNonReferenceType());
            break;
        }

        case clang::Type::RValueReference: {
            auto pointee = rewrite(llvm::cast<clang::ReferenceType>(T)->getPointeeType(), policy);
            if(!pointee.getNonReferenceType().isReferenceable()) {
                break;
            }
            if(pointee->isLValueReferenceType()) {
                result = context.getLValueReferenceType(pointee.getNonReferenceType());
            } else {
                result = context.getRValueReferenceType(pointee.getNonReferenceType());
            }
            break;
        }

        case clang::Type::Attributed: {
            auto AT = llvm::cast<clang::AttributedType>(T);
            auto modified = rewrite(AT->getModifiedType(), policy);
            auto equivalent = rewrite(AT->getEquivalentType(), policy);
            if(modified == AT->getModifiedType() && equivalent == AT->getEquivalentType()) {
                break;
            }
            result = context.getAttributedType(AT->getAttrKind(), modified, equivalent);
            break;
        }

        case clang::Type::Atomic: {
            auto AT = llvm::cast<clang::AtomicType>(T);
            auto value = rewrite(AT->getValueType(), policy);
            if(value == AT->getValueType()) {
                break;
            }
            /// Atomic operands must be unqualified, non-reference object
            /// types; anything else degrades.
            if(value->isReferenceType() || value->isArrayType() || value->isFunctionType() ||
               value->isAtomicType() || value.hasQualifiers()) {
                break;
            }
            result = context.getAtomicType(value);
            break;
        }

        case clang::Type::Decayed: {
            /// A parameter written as a dependent array or function type
            /// stores its adjusted form in a DecayedType; rebuild from
            /// the substituted original.
            auto DT = llvm::cast<clang::DecayedType>(T);
            auto original = rewrite(DT->getOriginalType(), policy);
            if(original == DT->getOriginalType()) {
                break;
            }
            result = context.getAdjustedParameterType(original);
            break;
        }

        case clang::Type::UnaryTransform: {
            auto UTT = llvm::cast<clang::UnaryTransformType>(T);
            auto base = rewrite(UTT->getBaseType(), policy);
            if(auto computed = transformed(UTT->getUTTKind(), base); !computed.isNull()) {
                result = computed;
                break;
            }
            /// Rebuilding with a non-dependent operand would fabricate a
            /// node whose canonical type is the operand itself — a wrong
            /// answer for every transform transformed() does not
            /// compute. Degrade to the unrewritten node instead.
            if(base == UTT->getBaseType() || !base->isDependentType()) {
                break;
            }
            result = context.getUnaryTransformType(base, base, UTT->getUTTKind());
            break;
        }

        case clang::Type::MemberPointer: {
            auto MPT = llvm::cast<clang::MemberPointerType>(T);
            auto qualifier = MPT->getQualifier();
            if(!qualifier || qualifier.getKind() != clang::NestedNameSpecifier::Kind::Type) {
                break;
            }
            auto cls = qualifier.getAsType();
            auto pointee = rewrite(MPT->getPointeeType(), policy);
            auto rewritten_cls = rewrite(clang::QualType(cls, 0), policy);
            if(pointee == MPT->getPointeeType() && rewritten_cls.getTypePtr() == cls) {
                break;
            }
            /// Pointers to reference or void members do not exist, and
            /// the owner must stay a class type (or still-dependent);
            /// degrade.
            if(pointee->isReferenceType() || pointee->isVoidType()) {
                break;
            }
            if(!rewritten_cls->isDependentType() && !rewritten_cls->isRecordType()) {
                break;
            }
            result =
                context.getMemberPointerType(pointee,
                                             clang::NestedNameSpecifier(rewritten_cls.getTypePtr()),
                                             rewritten_cls->getAsCXXRecordDecl());
            break;
        }

        case clang::Type::PackExpansion: {
            auto PET = llvm::cast<clang::PackExpansionType>(T);
            auto pattern = rewrite(PET->getPattern(), policy);
            if(pattern == PET->getPattern()) {
                break;
            }
            if(pattern->containsUnexpandedParameterPack()) {
                result = context.getPackExpansionType(pattern, PET->getNumExpansions());
            } else {
                /// The pack was substituted with a concrete (single)
                /// argument; the expansion collapses to it.
                result = pattern;
            }
            break;
        }

        /// `decltype(e)` of an unparenthesized name, member access, call
        /// or subscript is the named declaration's type (the callee's
        /// return type, `operator[]`'s for a subscript).
        /// Looking those up is resolution; substitution only sees
        /// through a plain variable.
        case clang::Type::Decltype: {
            auto expr = llvm::cast<clang::DecltypeType>(T)->getUnderlyingExpr();
            if(policy == Policy::Resolve && !llvm::isa<clang::ParenExpr>(expr)) {
                result = type_of(expr);
            } else if(auto DRE = llvm::dyn_cast<clang::DeclRefExpr>(expr)) {
                if(auto decl = DRE->getDecl(); llvm::isa<clang::VarDecl>(decl)) {
                    result = rewrite(decl->getType(), policy);
                }
            }
            break;
        }

        case clang::Type::DependentSizedArray: {
            auto DSAT = llvm::cast<clang::DependentSizedArrayType>(T);
            auto element = rewrite(DSAT->getElementType(), policy);
            /// Array elements must be object types (no references,
            /// void, or function types); degrade otherwise.
            if(element->isReferenceType() || element->isVoidType() || element->isFunctionType()) {
                break;
            }

            /// `T[N]` with a known N collapses to a constant array; a
            /// bound that is itself a dependent expression (`N = M` from
            /// deduction) is threaded through instead of left stale.
            if(auto NTTP = referenced_nttp(DSAT->getSizeExpr())) {
                auto* argument = bound_argument(NTTP, NTTP->getDepth(), NTTP->getIndex());
                if(argument && argument->getKind() == clang::TemplateArgument::Integral) {
                    /// A negative bound is ill-formed; fabricating an
                    /// array from its two's-complement bits would be a
                    /// wrong answer. Degrade.
                    if(argument->getAsIntegral().isNegative()) {
                        break;
                    }
                    result = context.getConstantArrayType(element,
                                                          argument->getAsIntegral(),
                                                          nullptr,
                                                          DSAT->getSizeModifier(),
                                                          DSAT->getIndexTypeCVRQualifiers());
                    break;
                }
                if(argument && argument->getKind() == clang::TemplateArgument::Expression) {
                    result = context.getDependentSizedArrayType(element,
                                                                argument->getAsExpr(),
                                                                DSAT->getSizeModifier(),
                                                                DSAT->getIndexTypeCVRQualifiers());
                    break;
                }
            }

            if(element != DSAT->getElementType()) {
                result = context.getDependentSizedArrayType(element,
                                                            DSAT->getSizeExpr(),
                                                            DSAT->getSizeModifier(),
                                                            DSAT->getIndexTypeCVRQualifiers());
            }
            break;
        }

        case clang::Type::ConstantArray: {
            auto CAT = llvm::cast<clang::ConstantArrayType>(T);
            auto element = rewrite(CAT->getElementType(), policy);
            if(element->isReferenceType() || element->isVoidType() || element->isFunctionType()) {
                break;
            }
            if(element != CAT->getElementType()) {
                result = context.getConstantArrayType(element,
                                                      CAT->getSize(),
                                                      CAT->getSizeExpr(),
                                                      CAT->getSizeModifier(),
                                                      CAT->getIndexTypeCVRQualifiers());
            }
            break;
        }

        case clang::Type::IncompleteArray: {
            auto IAT = llvm::cast<clang::IncompleteArrayType>(T);
            auto element = rewrite(IAT->getElementType(), policy);
            if(element->isReferenceType() || element->isVoidType() || element->isFunctionType()) {
                break;
            }
            if(element != IAT->getElementType()) {
                result = context.getIncompleteArrayType(element,
                                                        IAT->getSizeModifier(),
                                                        IAT->getIndexTypeCVRQualifiers());
            }
            break;
        }

        case clang::Type::FunctionProto: {
            auto FPT = llvm::cast<clang::FunctionProtoType>(T);
            auto ret = rewrite(FPT->getReturnType(), policy);
            llvm::SmallVector<clang::QualType, 4> params;
            bool changed = ret != FPT->getReturnType();
            bool representable = true;
            for(auto param: FPT->getParamTypes()) {
                /// A parameter pack (`void(Ts...)`) splices its bound
                /// elements into the parameter list.
                if(auto PET = param->getAs<clang::PackExpansionType>()) {
                    auto splice = [&](llvm::ArrayRef<clang::TemplateArgument> elements) {
                        for(auto& element: elements) {
                            if(element.getKind() != clang::TemplateArgument::Type ||
                               element.getAsType()->isVoidType()) {
                                representable = false;
                                return;
                            }
                            params.push_back(context.getAdjustedParameterType(element.getAsType()));
                        }
                        changed = true;
                    };

                    auto pattern = PET->getPattern();
                    const clang::TemplateArgument* bound = nullptr;
                    if(auto TTPT = pattern->getAs<clang::TemplateTypeParmType>()) {
                        bound = bound_argument(TTPT->getDecl(), TTPT->getDepth(), TTPT->getIndex());
                    }
                    if(bound && bound->getKind() == clang::TemplateArgument::Pack) {
                        splice(bound->getPackAsArray());
                        continue;
                    }
                    if(auto expanded = expand_pack_pattern(pattern, policy)) {
                        splice(*expanded);
                        continue;
                    }
                }

                auto rewritten = rewrite(param, policy);
                changed |= rewritten != param;
                /// Parameters decay (`void(T)` with `T = int[2]` is
                /// `void(int*)`), so a substituted type must be adjusted
                /// before it enters the prototype; a substituted `void`
                /// parameter cannot exist and degrades.
                if(rewritten->isVoidType()) {
                    representable = false;
                    break;
                }
                params.push_back(context.getAdjustedParameterType(rewritten));
            }
            if(!representable) {
                break;
            }
            /// Functions cannot return arrays or functions; degrade.
            if(ret->isArrayType() || ret->isFunctionType()) {
                break;
            }

            auto EPI = FPT->getExtProtoInfo();

            /// `noexcept(B)` with a bound bare parameter substitutes at
            /// the specification level: an expression binding replaces
            /// the operand whole, a value binding selects the concrete
            /// specification — no expression is ever rebuilt.
            if(FPT->getExceptionSpecType() == clang::EST_DependentNoexcept) {
                if(auto NTTP = referenced_nttp(FPT->getNoexceptExpr())) {
                    auto* bound = bound_argument(NTTP, NTTP->getDepth(), NTTP->getIndex());
                    if(bound && bound->getKind() == clang::TemplateArgument::Integral) {
                        EPI.ExceptionSpec = {};
                        EPI.ExceptionSpec.Type = bound->getAsIntegral().getBoolValue()
                                                     ? clang::EST_BasicNoexcept
                                                     : clang::EST_None;
                        changed = true;
                    } else if(bound && bound->getKind() == clang::TemplateArgument::Expression) {
                        EPI.ExceptionSpec.NoexceptExpr = bound->getAsExpr();
                        changed = true;
                    }
                }
            }

            if(changed) {
                /// Splicing changes the arity; per-parameter ABI info
                /// cannot be carried over.
                if(params.size() != FPT->getNumParams()) {
                    EPI.ExtParameterInfos = nullptr;
                }
                result = context.getFunctionType(ret, params, EPI);
            }
            break;
        }

        default: {
            break;
        }
    }

    if(result.isNull()) {
        return clang::QualType();
    }
    /// cv-qualifiers applied through substitution are ignored when the
    /// substituted type is a reference or function type (`const T` with
    /// `T = X&` is just `X&`).
    if(quals.hasQualifiers() && !result->isReferenceType() && !result->isFunctionType()) {
        result = context.getQualifiedType(result, quals);
    }
    return result;
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

clang::QualType PseudoInstantiator::transformed(clang::UnaryTransformType::UTTKind kind,
                                                clang::QualType base) {
    using Kind = clang::UnaryTransformType;
    auto shaped = [&] {
        switch(base.getCanonicalType()->getTypeClass()) {
            case clang::Type::Builtin: return !base->isDependentType();
            case clang::Type::Pointer:
            case clang::Type::MemberPointer:
            case clang::Type::LValueReference:
            case clang::Type::RValueReference:
            case clang::Type::Record:
            case clang::Type::Enum:
            case clang::Type::InjectedClassName:
            case clang::Type::TemplateSpecialization:
            case clang::Type::ConstantArray:
            case clang::Type::IncompleteArray:
            case clang::Type::DependentSizedArray:
            case clang::Type::FunctionProto: return true;
            default: return false;
        }
    };
    if(!shaped()) {
        return clang::QualType();
    }
    auto value = base.getNonReferenceType();
    auto without = [&](unsigned removed) {
        auto quals = base.getQualifiers();
        quals.removeCVRQualifiers(removed);
        return context.getQualifiedType(base.getUnqualifiedType(), quals);
    };
    /// A function type with qualifiers of its own (`int() const`) has
    /// no pointer nor reference; void has no reference.
    auto* function = base->getAs<clang::FunctionProtoType>();
    bool referenceable = !function || (function->getMethodQuals().empty() &&
                                       function->getRefQualifier() == clang::RQ_None);
    switch(kind) {
        case Kind::AddPointer: return referenceable ? context.getPointerType(value) : base;
        case Kind::AddLvalueReference:
            return base->isVoidType() || !referenceable ? base
                                                        : context.getLValueReferenceType(value);
        case Kind::AddRvalueReference:
            if(base->isVoidType() || !referenceable || base->isLValueReferenceType()) {
                return base;
            }
            return context.getRValueReferenceType(value);
        case Kind::RemoveReference: return value;
        case Kind::RemoveCVRef: return value.getUnqualifiedType();
        case Kind::RemoveCV: return without(clang::Qualifiers::Const | clang::Qualifiers::Volatile);
        case Kind::RemoveConst: return without(clang::Qualifiers::Const);
        case Kind::RemoveVolatile: return without(clang::Qualifiers::Volatile);
        case Kind::RemoveRestrict: return without(clang::Qualifiers::Restrict);
        case Kind::RemovePointer: {
            auto* pointer = base->getAs<clang::PointerType>();
            return pointer ? pointer->getPointeeType() : base;
        }
        case Kind::RemoveExtent: {
            auto* array = context.getAsArrayType(base);
            return array ? array->getElementType() : base;
        }
        case Kind::RemoveAllExtents: return context.getBaseElementType(base);
        case Kind::Decay: {
            if(context.getAsArrayType(value)) {
                return context.getArrayDecayedType(value);
            }
            if(value->isFunctionType()) {
                return context.getPointerType(value);
            }
            return value.getUnqualifiedType();
        }
        case Kind::EnumUnderlyingType: {
            auto* ET = base->getAs<clang::EnumType>();
            auto* definition = ET ? ET->getDecl()->getDefinition() : nullptr;
            return definition ? definition->getIntegerType() : clang::QualType();
        }
        case Kind::MakeSigned:
        case Kind::MakeUnsigned: return clang::QualType();
    }
    std::unreachable();
}

clang::QualType PseudoInstantiator::rewrite_parameter(const clang::TemplateTypeParmType* TTPT,
                                                      Policy policy) {
    if(const auto* argument = bound_argument(TTPT->getDecl(), TTPT->getDepth(), TTPT->getIndex())) {
        clang::QualType type;

        if(argument->getKind() == clang::TemplateArgument::Type) {
            type = argument->getAsType();
        } else if(argument->getKind() == clang::TemplateArgument::Pack) {
            auto pack = argument->getPackAsArray();
            if(pack.size() == 1 && pack[0].getKind() == clang::TemplateArgument::Type) {
                type = pack[0].getAsType();
            }
            /// Multi-element packs are spliced at the template argument
            /// list level (rewrite_arguments); a bare parameter cannot
            /// stand for several types at once.
        }

        return type;
    }

    // A specialization named without the argument takes the parameter's
    // default. This enables resolution chains like:
    //   template<typename T, typename Alloc = allocator<T>> struct vector;
    // where Alloc's default depends on T.
    if(stack.owns(TTPT->getDecl(), TTPT->getDepth()) && policy == Policy::Resolve) {
        if(clang::TemplateTypeParmDecl* TTPD = TTPT->getDecl();
           TTPD && TTPD->hasDefaultArgument()) {
            const auto& argument = TTPD->getDefaultArgument().getArgument();
            if(argument.getKind() == clang::TemplateArgument::Type) {
                return rewrite(argument.getAsType(), policy);
            }
        }
    }

    return clang::QualType();
}

clang::QualType PseudoInstantiator::resolve_dependent_name(const clang::DependentNameType* DNT) {
    LOG_DEBUG(
        "{}"
        "resolve '{}'",
        pad(),
        clang::QualType(DNT, 0).getAsString());
    indent += 1;

    // Check cache.
    if(pack_narrowing == 0) {
        if(auto iter = resolved.find(DNT); iter != resolved.end()) {
            LOG_DEBUG(
                "{}"
                "→ '{}' (cached)",
                pad(),
                iter->second.getAsString());
            indent -= 1;
            return iter->second;
        }
    }

    // Cycle detection: if we're already resolving this DNT, bail out.
    if(!resolving.insert(DNT).second) {
        LOG_DEBUG("{}→ <cycle detected, returning original>", pad());
        indent -= 1;
        return clang::QualType(DNT, 0);
    }

    auto stack_size = stack.size();
    auto* decl = preferred(lookup(DNT->getQualifier(), DNT->getIdentifier()));
    clang::QualType type;
    if(auto* alias = llvm::dyn_cast_or_null<clang::TypedefNameDecl>(decl)) {
        type = alias->getUnderlyingType();
    } else if(auto* tag = llvm::dyn_cast_or_null<clang::TagDecl>(decl)) {
        type = context.getCanonicalTagType(tag);
    }

    clang::QualType result;
    if(!type.isNull()) {
        const char* decl_kind = "decl";
        if(llvm::isa<clang::TypedefNameDecl>(decl))
            decl_kind = "typedef";
        else if(llvm::isa<clang::TagDecl>(decl))
            decl_kind = "tag";
        auto decl_name = llvm::dyn_cast<clang::NamedDecl>(decl)
                             ? llvm::dyn_cast<clang::NamedDecl>(decl)->getNameAsString()
                             : "?";
        LOG_DEBUG(
            "{}"
            "found {} '{}' = '{}'",
            pad(),
            decl_kind,
            decl_name,
            type.getAsString());

        // Step 1: substitute params (expand typedefs, no lookup).
        result = substitute(type);
        LOG_DEBUG(
            "{}"
            "substitute → '{}'",
            pad(),
            result.getAsString());

        // Pop lookup frames BEFORE further resolution. The substitute step already
        // used the full stack for parameter substitution. Resolution should only
        // see the outer context to avoid polluting free variables (e.g. T) with
        // mappings from intermediate lookup frames.
        stack.truncate(stack_size);

        // Step 2: if still dependent, do full resolution (may trigger more lookups).
        if(!result.isNull() && result->isDependentType()) {
            result = rewrite(result, Policy::Resolve);
        }
    } else {
        stack.truncate(stack_size);
    }

    resolving.erase(DNT);

    if(!result.isNull()) {
        LOG_DEBUG(
            "{}"
            "→ '{}'",
            pad(),
            result.getAsString());
        indent -= 1;
        if(pack_narrowing == 0 && !inconclusive) {
            resolved.try_emplace(DNT, result);
        }
        return result;
    }

    LOG_DEBUG("{}→ <unresolved>", pad());
    indent -= 1;
    return clang::QualType(DNT, 0);
}

clang::QualType
    PseudoInstantiator::resolve_dependent_template(const clang::TemplateSpecializationType* TST) {
    LOG_DEBUG(
        "{}"
        "resolve TST '{}'",
        pad(),
        clang::QualType(TST, 0).getAsString());
    indent += 1;

    auto& template_name = *TST->getTemplateName().getAsDependentTemplateName();
    /// Pack-narrowed resolutions depend on context the node pointer does
    /// not capture; they may not be cached.
    bool cacheable = pack_narrowing == 0;

    if(cacheable) {
        if(auto iter = resolved.find(TST); iter != resolved.end()) {
            indent -= 1;
            return iter->second;
        }
    }

    auto NNS = rewrite_specifier(template_name.getQualifier(), Policy::Resolve);

    llvm::SmallVector<clang::TemplateArgument, 4> arguments;
    rewrite_arguments(TST->template_arguments(), arguments, Policy::Resolve);

    auto* name = template_name.getName().getIdentifier();
    if(!name) {
        LOG_DEBUG("{}→ <unresolved TST>", pad());
        indent -= 1;
        return clang::QualType(TST, 0);
    }

    auto stack_size = stack.size();
    if(auto* decl = preferred(lookup(NNS, name))) {
        if(auto* TATD = llvm::dyn_cast<clang::TypeAliasTemplateDecl>(decl)) {
            if(deduce_template_arguments(TATD, arguments) != Deduction::Failed) {
                auto type = substitute(TATD->getTemplatedDecl()->getUnderlyingType());
                // Pop lookup frames before further resolution.
                stack.truncate(stack_size);
                if(!type.isNull() && type->isDependentType()) {
                    type = rewrite(type, Policy::Resolve);
                }
                if(!type.isNull()) {
                    LOG_DEBUG(
                        "{}"
                        "→ '{}' (alias)",
                        pad(),
                        type.getAsString());
                    indent -= 1;
                    if(cacheable && !inconclusive) {
                        resolved.try_emplace(TST, type);
                    }
                    return type;
                }
            }
        } else if(auto* CTD = llvm::dyn_cast<clang::ClassTemplateDecl>(decl)) {
            // Resolve TST to a concrete TemplateSpecializationType.
            // e.g. __alloc_traits<allocator<T>>::rebind<T> → rebind<T> (a TST)
            // This allows subsequent lookup of members (like "other") to work.
            // Keep lookup frames on stack — the caller (e.g. rewrite_specifier
            // processing A<X>::B<Y>::C<Z>) needs them for parameter substitution.
            auto result = make_specialization(clang::TemplateName(CTD), arguments);
            LOG_DEBUG(
                "{}"
                "→ TST '{}' (class)",
                pad(),
                result.getAsString());
            indent -= 1;
            if(cacheable && !inconclusive) {
                resolved.try_emplace(TST, result);
            }
            return result;
        }
    }
    stack.truncate(stack_size);

    LOG_DEBUG("{}→ <unresolved TST>", pad());
    indent -= 1;
    auto fallback = clang::QualType(TST, 0);
    /// Only a conclusive failure may be cached: an exhausted step budget
    /// or a tripped recursion guard proves nothing, and the cache is
    /// TU-wide — a truncated query must not poison this node for later
    /// queries that could still resolve it.
    if(cacheable && !inconclusive) {
        resolved.try_emplace(TST, fallback);
    }
    return fallback;
}

}  // namespace clice::types
