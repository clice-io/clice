module clice;

import :semantic.template_resolver.instantiator;
import :support.logging;

namespace clice::types {

namespace {

/// What a standard library class template's `type` is, by the standard.
enum class Rule : std::uint8_t {
    Conditional,
    EnableIf,
    TypeIdentity,
    RemoveReference,
    RemoveConst,
    RemoveVolatile,
    RemoveCV,
    RemoveCVRef,
    RemovePointer,
    AddPointer,
    AddLValueReference,
    AddRValueReference,
    AddConst,
    AddVolatile,
    AddCV,
    Decay,
};

/// The class templates of namespace std whose `type` the resolver answers by
/// rule. Reading their definitions costs every query a deduction through
/// their partial specializations, written differently by each library, and
/// the answer is the same in all of them.
constexpr std::pair<llvm::StringLiteral, Rule> rules[] = {
    {"conditional",          Rule::Conditional       },
    {"enable_if",            Rule::EnableIf          },
    {"type_identity",        Rule::TypeIdentity      },
    {"remove_reference",     Rule::RemoveReference   },
    {"remove_const",         Rule::RemoveConst       },
    {"remove_volatile",      Rule::RemoveVolatile    },
    {"remove_cv",            Rule::RemoveCV          },
    {"remove_cvref",         Rule::RemoveCVRef       },
    {"remove_pointer",       Rule::RemovePointer     },
    {"add_pointer",          Rule::AddPointer        },
    {"add_lvalue_reference", Rule::AddLValueReference},
    {"add_rvalue_reference", Rule::AddRValueReference},
    {"add_const",            Rule::AddConst          },
    {"add_volatile",         Rule::AddVolatile       },
    {"add_cv",               Rule::AddCV             },
    {"decay",                Rule::Decay             },
};

/// The rule `DNT`, a `type` of a standard library class template's
/// specialization, is answered by, with the specialization as written.
std::optional<Rule> rule_of(const clang::DependentNameType* DNT,
                            const clang::TemplateSpecializationType*& written) {
    auto* name = DNT->getIdentifier();
    auto qualifier = DNT->getQualifier();
    if(!name || !name->isStr("type") ||
       qualifier.getKind() != clang::NestedNameSpecifier::Kind::Type) {
        return std::nullopt;
    }
    written = qualifier.getAsType()->getAs<clang::TemplateSpecializationType>();
    auto* CTD = written ? llvm::dyn_cast_or_null<clang::ClassTemplateDecl>(
                              written->getTemplateName().getAsTemplateDecl())
                        : nullptr;
    if(!CTD || !CTD->getIdentifier() || !CTD->isInStdNamespace()) {
        return std::nullopt;
    }
    for(auto [spelling, rule]: rules) {
        if(CTD->getName() == spelling) {
            return rule;
        }
    }
    return std::nullopt;
}

/// Whether nothing tells how `type` is built: a parameter, a dependent name,
/// a `decltype` may turn out a reference, cv-qualified or an array.
bool opaque(clang::QualType type) {
    auto* canonical = type.getCanonicalType().getTypePtr();
    if(!canonical->isDependentType()) {
        return false;
    }
    if(auto* TST = llvm::dyn_cast<clang::TemplateSpecializationType>(canonical)) {
        return !llvm::isa_and_present<clang::ClassTemplateDecl>(
            TST->getTemplateName().getAsTemplateDecl());
    }
    return !llvm::isa<clang::PointerType,
                      clang::ReferenceType,
                      clang::ArrayType,
                      clang::FunctionType,
                      clang::MemberPointerType,
                      clang::InjectedClassNameType,
                      clang::RecordType>(canonical);
}

}  // namespace

std::optional<bool> PseudoInstantiator::std_condition(const clang::TemplateArgument& argument) {
    std::optional<llvm::APSInt> value;
    if(argument.getKind() == clang::TemplateArgument::Integral) {
        value = argument.getAsIntegral();
    } else if(argument.getKind() == clang::TemplateArgument::Expression) {
        value = evaluate(argument.getAsExpr());
    }
    if(!value) {
        return std::nullopt;
    }
    return value->getBoolValue();
}

std::optional<clang::QualType> PseudoInstantiator::std_type(const clang::DependentNameType* DNT) {
    const clang::TemplateSpecializationType* written = nullptr;
    auto rule = rule_of(DNT, written);
    if(!rule) {
        return std::nullopt;
    }
    llvm::SmallVector<clang::TemplateArgument, 4> arguments;
    rewrite_arguments(written->template_arguments(), arguments, Policy::Resolve);
    auto type_at = [&](unsigned index) {
        if(index >= arguments.size() ||
           arguments[index].getKind() != clang::TemplateArgument::Type ||
           arguments[index].getAsType()->getAs<clang::PackExpansionType>()) {
            return clang::QualType();
        }
        return arguments[index].getAsType();
    };
    auto unknown = clang::QualType(DNT, 0);

    if(*rule == Rule::Conditional || *rule == Rule::EnableIf) {
        if(arguments.empty()) {
            return std::nullopt;
        }
        auto condition = std_condition(arguments.front());
        if(*rule == Rule::EnableIf) {
            /// Where the condition is false the library's definition has no
            /// `type`: what tells a constraint from a member it lacks stays
            /// the definition's.
            if(condition == false) {
                return std::nullopt;
            }
            auto type = arguments.size() > 1 ? type_at(1) : context.VoidTy;
            return type.isNull() ? std::nullopt : std::optional(type);
        }
        if(!condition) {
            auto chosen = type_at(1);
            auto other = type_at(2);
            return !chosen.isNull() && !other.isNull() && context.hasSameType(chosen, other)
                       ? chosen
                       : unknown;
        }
        auto type = type_at(*condition ? 1 : 2);
        return type.isNull() ? std::nullopt : std::optional(type);
    }

    auto type = type_at(0);
    if(type.isNull()) {
        return std::nullopt;
    }
    switch(*rule) {
        case Rule::TypeIdentity: return type;
        case Rule::AddLValueReference:
            return type->isVoidType() ? type : context.getLValueReferenceType(type);
        case Rule::AddRValueReference:
            return type->isVoidType() ? type : context.getRValueReferenceType(type);
        case Rule::AddConst: return type.withConst();
        case Rule::AddVolatile: return type.withVolatile();
        case Rule::AddCV: return type.withConst().withVolatile();
        default: break;
    }
    /// A layer the argument writes is the one the rule removes; one only a
    /// parameter may carry waits for the parameter.
    auto without = [&](clang::QualType type, clang::Qualifiers::TQ removed) {
        clang::Qualifiers quals;
        auto unqualified = context.getUnqualifiedArrayType(type, quals);
        if(!(quals.getCVRQualifiers() & removed) &&
           opaque(context.getBaseElementType(unqualified))) {
            return unknown;
        }
        quals.removeCVRQualifiers(removed);
        return context.getQualifiedType(unqualified, quals);
    };
    auto referred =
        !type->isReferenceType() && opaque(type) ? clang::QualType() : type.getNonReferenceType();
    constexpr auto cv =
        static_cast<clang::Qualifiers::TQ>(clang::Qualifiers::Const | clang::Qualifiers::Volatile);
    switch(*rule) {
        case Rule::RemoveReference: return referred.isNull() ? unknown : referred;
        case Rule::RemoveConst: return without(type, clang::Qualifiers::Const);
        case Rule::RemoveVolatile: return without(type, clang::Qualifiers::Volatile);
        case Rule::RemoveCV: return without(type, cv);
        case Rule::RemoveCVRef: return referred.isNull() ? unknown : without(referred, cv);
        case Rule::RemovePointer: {
            if(auto* pointer = type->getAs<clang::PointerType>()) {
                return pointer->getPointeeType();
            }
            return opaque(type) ? unknown : type;
        }
        case Rule::AddPointer:
            return referred.isNull() ? unknown : context.getPointerType(referred);
        case Rule::Decay: {
            type = type.getNonReferenceType();
            if(opaque(type)) {
                return unknown;
            }
            if(type->isArrayType()) {
                return context.getArrayDecayedType(type);
            }
            if(type->isFunctionType()) {
                return context.getPointerType(type);
            }
            return type.getUnqualifiedType();
        }
        default: std::unreachable();
    }
}

llvm::SmallVector<clang::QualType, 2>
    PseudoInstantiator::std_alternatives(const clang::DependentNameType* DNT) {
    const clang::TemplateSpecializationType* written = nullptr;
    if(rule_of(DNT, written) != Rule::Conditional) {
        return {};
    }
    llvm::SmallVector<clang::TemplateArgument, 4> arguments;
    rewrite_arguments(written->template_arguments(), arguments, Policy::Resolve);
    if(arguments.size() != 3 || std_condition(arguments[0]) ||
       arguments[1].getKind() != clang::TemplateArgument::Type ||
       arguments[2].getKind() != clang::TemplateArgument::Type) {
        return {};
    }
    return {arguments[1].getAsType(), arguments[2].getAsType()};
}

PseudoInstantiator::lookup_result
    PseudoInstantiator::lookup_through_alternatives(llvm::ArrayRef<clang::QualType> alternatives,
                                                    clang::DeclarationName name) {
    /// Alternatives inside an alternative multiply: MSVC's special member
    /// layers nest a conditional base in each of three choices, four deep.
    /// Past the first, a condition not known leaves the member unfound, as
    /// the library's definition does.
    if(through_alternatives) {
        give_up(Trouble::Undecided);
        return lookup_result();
    }
    through_alternatives = true;
    auto outermost = llvm::make_scope_exit([&] { through_alternatives = false; });
    auto frames = stack.size();
    std::optional<Found> common;
    for(auto [index, alternative]: views::enumerate(alternatives)) {
        auto members = lookup(alternative, name);
        if(members.empty()) {
            stack.truncate(frames);
            return lookup_result();
        }
        auto here = found_by(members, frames);
        if(common && !same_found(*common, here)) {
            stack.truncate(frames);
            return lookup_result();
        }
        common = std::move(here);
        if(static_cast<std::size_t>(index) + 1 < alternatives.size()) {
            stack.truncate(frames);
        }
    }
    LOG_DEBUG("{}found '{}' alike in each alternative", pad(), name.getAsString());
    return common->members;
}

}  // namespace clice::types
