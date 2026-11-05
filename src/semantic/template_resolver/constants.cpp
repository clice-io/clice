module clice;

import :semantic.template_resolver.deduction;

namespace clice::types {

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

    /// Sema promotes the operands of an arithmetic node whose type is known;
    /// under a still-dependent one (`-Config<T>::n`) the folding applies the
    /// integral promotion itself.
    llvm::APSInt promoted(const llvm::APSInt& value, clang::QualType type) {
        auto width = context.getIntWidth(context.IntTy);
        if(!type->isDependentType() || value.getBitWidth() >= width) {
            return value;
        }
        return llvm::APSInt(value.extend(width), /*isUnsigned=*/false);
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
            *value = promoted(*value, unary->getType());
            switch(unary->getOpcode()) {
                case clang::UO_Plus: break;
                case clang::UO_Minus: {
                    if(value->isSigned() && value->isMinSignedValue()) {
                        return std::nullopt;
                    }
                    *value = -*value;
                    break;
                }
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
            auto* chosen =
                condition->isZero() ? conditional->getFalseExpr() : conditional->getTrueExpr();
            auto value = fold(chosen);
            if(!value) {
                return std::nullopt;
            }
            auto type = conditional->getType();
            if(!type->isDependentType()) {
                return typed(*value, type);
            }

            /// Under a still-dependent type Sema has not brought the branches
            /// to their common type; the other branch must already agree.
            auto other = fold(chosen == conditional->getTrueExpr() ? conditional->getFalseExpr()
                                                                   : conditional->getTrueExpr());
            if(!other) {
                return std::nullopt;
            }
            auto result = promoted(*value, type);
            auto rest = promoted(*other, type);
            if(result.getBitWidth() != rest.getBitWidth() ||
               result.isUnsigned() != rest.isUnsigned()) {
                return std::nullopt;
            }
            return result;
        }

        auto value = value_of(expr);
        return value ? typed(*value, expr->getType()) : std::nullopt;
    }

    std::optional<llvm::APSInt> fold_binary(const clang::BinaryOperator* binary) {
        auto op = binary->getOpcode();
        auto lhs = fold(binary->getLHS());
        auto rhs = fold(binary->getRHS());

        /// An operand that does not fold may stand for a substitution
        /// failure, which leaves the whole expression false: `false` decides
        /// `&&` regardless, `true` decides `||` only beside a known operand.
        if(op == clang::BO_LAnd && ((lhs && lhs->isZero()) || (rhs && rhs->isZero()))) {
            return typed(truth(false), binary->getType());
        }
        if(!lhs || !rhs) {
            return std::nullopt;
        }
        if(op == clang::BO_LAnd || op == clang::BO_LOr) {
            return typed(truth(op == clang::BO_LAnd ? lhs->getBoolValue() && rhs->getBoolValue()
                                                    : lhs->getBoolValue() || rhs->getBoolValue()),
                         binary->getType());
        }
        lhs = promoted(*lhs, binary->getType());
        rhs = promoted(*rhs, binary->getType());

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

        /// Signed overflow leaves the expression no constant at all.
        bool overflow = false;
        bool is_signed = lhs->isSigned();
        auto signed_result = [](llvm::APInt value) {
            return llvm::APSInt(std::move(value), /*isUnsigned=*/false);
        };
        llvm::APSInt result;
        switch(op) {
            case clang::BO_Mul: {
                result = is_signed ? signed_result(lhs->smul_ov(*rhs, overflow)) : *lhs * *rhs;
                break;
            }
            case clang::BO_Div:
            case clang::BO_Rem: {
                if(rhs->isZero()) {
                    return std::nullopt;
                }
                overflow = is_signed && lhs->isMinSignedValue() && rhs->isAllOnes();
                result = op == clang::BO_Div ? *lhs / *rhs : *lhs % *rhs;
                break;
            }
            case clang::BO_Add: {
                result = is_signed ? signed_result(lhs->sadd_ov(*rhs, overflow)) : *lhs + *rhs;
                break;
            }
            case clang::BO_Sub: {
                result = is_signed ? signed_result(lhs->ssub_ov(*rhs, overflow)) : *lhs - *rhs;
                break;
            }
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
        if(overflow) {
            return std::nullopt;
        }
        return typed(result, binary->getType());
    }
};

/// The category a type's shape puts it in, for the traits it decides.
enum class Category : std::uint8_t {
    /// A template parameter or a dependent name: it may become anything.
    Unknown,
    Void,
    NullPointer,
    Integral,
    Floating,
    Enum,
    Class,
    Union,
    Pointer,
    MemberPointer,
    LValueReference,
    RValueReference,
    Array,
    Function,
};

Category category_of(clang::QualType type) {
    const auto* canonical = type.getCanonicalType().getTypePtr();
    switch(canonical->getTypeClass()) {
        case clang::Type::Builtin: {
            if(canonical->isDependentType()) {
                return Category::Unknown;
            }
            if(canonical->isVoidType()) {
                return Category::Void;
            }
            if(canonical->isNullPtrType()) {
                return Category::NullPointer;
            }
            if(canonical->isIntegerType()) {
                return Category::Integral;
            }
            return canonical->isFloatingType() ? Category::Floating : Category::Unknown;
        }
        case clang::Type::Pointer: return Category::Pointer;
        case clang::Type::MemberPointer: return Category::MemberPointer;
        case clang::Type::LValueReference: return Category::LValueReference;
        case clang::Type::RValueReference: return Category::RValueReference;
        case clang::Type::ConstantArray:
        case clang::Type::IncompleteArray:
        case clang::Type::DependentSizedArray: return Category::Array;
        case clang::Type::FunctionProto:
        case clang::Type::FunctionNoProto: return Category::Function;
        case clang::Type::Enum: return Category::Enum;
        case clang::Type::Record:
        case clang::Type::InjectedClassName: {
            auto* record = canonical->getAsRecordDecl();
            return record && record->isUnion() ? Category::Union : Category::Class;
        }
        case clang::Type::TemplateSpecialization: {
            auto* specialization = llvm::cast<clang::TemplateSpecializationType>(canonical);
            auto* pattern = llvm::dyn_cast_or_null<clang::ClassTemplateDecl>(
                specialization->getTemplateName().getAsTemplateDecl());
            if(!pattern) {
                return Category::Unknown;
            }
            return pattern->getTemplatedDecl()->isUnion() ? Category::Union : Category::Class;
        }
        default: return Category::Unknown;
    }
}

/// What the standard type-category trait `name` (`is_object`, without
/// `_v`) says of a type of `category`; none for another trait or an unknown
/// category.
std::optional<bool> category_trait(llvm::StringRef name, Category category) {
    if(category == Category::Unknown) {
        return std::nullopt;
    }
    bool arithmetic = category == Category::Integral || category == Category::Floating;
    bool fundamental =
        arithmetic || category == Category::Void || category == Category::NullPointer;
    bool reference = category == Category::LValueReference || category == Category::RValueReference;
    return llvm::StringSwitch<std::optional<bool>>(name)
        .Case("is_void", category == Category::Void)
        .Case("is_null_pointer", category == Category::NullPointer)
        .Case("is_integral", category == Category::Integral)
        .Case("is_floating_point", category == Category::Floating)
        .Case("is_array", category == Category::Array)
        .Case("is_enum", category == Category::Enum)
        .Case("is_union", category == Category::Union)
        .Case("is_class", category == Category::Class)
        .Case("is_function", category == Category::Function)
        .Case("is_pointer", category == Category::Pointer)
        .Case("is_lvalue_reference", category == Category::LValueReference)
        .Case("is_rvalue_reference", category == Category::RValueReference)
        .Case("is_member_pointer", category == Category::MemberPointer)
        .Case("is_reference", reference)
        .Case("is_arithmetic", arithmetic)
        .Case("is_fundamental", fundamental)
        .Case("is_compound", !fundamental)
        .Case("is_object",
              !reference && category != Category::Void && category != Category::Function)
        .Case("is_scalar",
              arithmetic || category == Category::Enum || category == Category::Pointer ||
                  category == Category::MemberPointer || category == Category::NullPointer)
        .Default(std::nullopt);
}

}  // namespace

std::optional<bool> type_trait(const clang::Expr* expr,
                               llvm::function_ref<clang::QualType(clang::QualType)> bound) {
    expr = expr->IgnoreParenImpCasts();
    auto standard = [](const clang::Decl* decl) {
        return decl && decl->getDeclContext()->isStdNamespace();
    };
    auto single_type = [](llvm::ArrayRef<clang::TemplateArgumentLoc> arguments) {
        return arguments.size() == 1 &&
                       arguments.front().getArgument().getKind() == clang::TemplateArgument::Type
                   ? arguments.front().getArgument().getAsType()
                   : clang::QualType();
    };

    llvm::StringRef name;
    clang::QualType operand;
    if(auto* trait = llvm::dyn_cast<clang::TypeTraitExpr>(expr)) {
        if(trait->getNumArgs() != 1) {
            return std::nullopt;
        }
        /// The builtin is the trait's name after `__`: `__is_object`.
        name = clang::getTraitSpelling(trait->getTrait());
        name.consume_front("__");
        operand = trait->getArg(0)->getType();
    } else if(auto* lookup = llvm::dyn_cast<clang::UnresolvedLookupExpr>(expr)) {
        if(lookup->getNumDecls() == 0 || !standard(*lookup->decls_begin())) {
            return std::nullopt;
        }
        name = lookup->getName().getAsIdentifierInfo()
                   ? lookup->getName().getAsIdentifierInfo()->getName()
                   : llvm::StringRef();
        if(!name.consume_back("_v")) {
            return std::nullopt;
        }
        operand = single_type(lookup->template_arguments());
    } else if(auto* member = llvm::dyn_cast<clang::DependentScopeDeclRefExpr>(expr)) {
        auto qualifier = member->getQualifier();
        if(member->getDeclName().getAsString() != "value" ||
           qualifier.getKind() != clang::NestedNameSpecifier::Kind::Type) {
            return std::nullopt;
        }
        auto* specialization = qualifier.getAsType()->getAs<clang::TemplateSpecializationType>();
        auto* trait =
            specialization ? specialization->getTemplateName().getAsTemplateDecl() : nullptr;
        if(!standard(trait) || specialization->template_arguments().size() != 1 ||
           specialization->template_arguments().front().getKind() !=
               clang::TemplateArgument::Type) {
            return std::nullopt;
        }
        name = trait->getName();
        operand = specialization->template_arguments().front().getAsType();
    }
    if(operand.isNull()) {
        return std::nullopt;
    }
    return category_trait(name, category_of(bound(operand)));
}

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

}  // namespace clice::types
