module;

#include "modules/prelude.h"
/// Parts of this file (deduced_type and its helpers) are ported from
/// clangd's AST.cpp (llvmorg-21.1.8), part of the LLVM project, licensed
/// under Apache License v2.0 with LLVM Exceptions. See
/// https://llvm.org/LICENSE.txt for license information.

module clice;

import :semantic.decls;
import :semantic.resolver;
import :semantic.types;

namespace clice::types {

namespace {

template <typename Ty>
    requires requires(Ty* T) { T->getDecl(); }
const clang::NamedDecl* decl_of_impl(const Ty* T) {
    return T->getDecl();
}

const clang::NamedDecl* decl_of_impl(const void* T) {
    return nullptr;
}

}  // namespace

auto decls_of(clang::QualType type, TemplateResolver* resolver)
    -> llvm::SmallVector<const clang::NamedDecl*, 1> {
    /// Sugar spelling no name of its own gives way to the type it wraps.
    while(!type.isNull()) {
        const clang::Type* written = type.getTypePtr();
        if(auto* subst = llvm::dyn_cast<clang::SubstTemplateTypeParmType>(written)) {
            type = subst->getReplacementType();
        } else if(auto* paren = llvm::dyn_cast<clang::ParenType>(written)) {
            type = paren->getInnerType();
        } else if(auto* attributed = llvm::dyn_cast<clang::AttributedType>(written)) {
            type = attributed->getModifiedType();
        } else if(auto* macro = llvm::dyn_cast<clang::MacroQualifiedType>(written)) {
            type = macro->getUnderlyingType();
        } else {
            break;
        }
    }

    if(type.isNull()) {
        return {};
    }

    /// A type alias is what the type names, whatever it stands for.
    if(const auto* TT = llvm::dyn_cast<clang::TypedefType>(type.getTypePtr())) {
        return {TT->getDecl()};
    }

    /// using ns::Widget; — the written name imports the type it names.
    if(const auto* UT = llvm::dyn_cast<clang::UsingType>(type.getTypePtr())) {
        return {UT->getDecl()->getTargetDecl()};
    }

    /// Dependent names carry no declaration structurally; resolve them
    /// heuristically when a resolver is available.
    if(const auto* DNT = type->getAs<clang::DependentNameType>()) {
        llvm::SmallVector<const clang::NamedDecl*, 1> result;
        if(resolver) {
            for(auto* target: resolver->lookup(DNT)) {
                result.push_back(target);
            }
        }
        return result;
    }

    /// using typename B<T>::type — the unresolved-using declaration itself
    /// first, then the resolver's candidates for what it imports.
    if(const auto* UUT = type->getAs<clang::UnresolvedUsingType>()) {
        llvm::SmallVector<const clang::NamedDecl*, 1> result;
        result.push_back(UUT->getDecl());
        if(resolver) {
            if(auto* UD = llvm::dyn_cast<clang::UnresolvedUsingTypenameDecl>(UUT->getDecl())) {
                for(auto* target: resolver->lookup(UD)) {
                    result.push_back(target);
                }
            }
        }
        return result;
    }

    if(auto TST = type->getAs<clang::TemplateSpecializationType>()) {
        auto decl = TST->getTemplateName().getAsTemplateDecl();

        /// A dependent template name (`T::template rebind<U>`) has no
        /// declaration structurally; resolve it heuristically.
        if(!decl) {
            llvm::SmallVector<const clang::NamedDecl*, 1> result;
            if(resolver) {
                for(auto* target: resolver->lookup(TST)) {
                    result.push_back(target);
                }
            }
            return result;
        }

        if(type->isDependentType()) {
            return {decl};
        }

        /// For a template specialization type, the template name is possibly a `ClassTemplateDecl`
        ///  `TypeAliasTemplateDecl` or `TemplateTemplateParmDecl` and `BuiltinTemplateDecl`.
        if(llvm::isa<clang::TypeAliasTemplateDecl>(decl)) {
            return {decl->getTemplatedDecl()};
        }

        if(llvm::isa<clang::TemplateTemplateParmDecl, clang::BuiltinTemplateDecl>(decl)) {
            return {decl};
        }

        if(const auto* record = TST->getAsCXXRecordDecl()) {
            if(const auto* pattern = decls::instantiated_from(record)) {
                return {pattern};
            }
        }
        return {};
    }

    const clang::NamedDecl* decl = nullptr;
    switch(type->getTypeClass()) {
#define ABSTRACT_TYPE(TY, BASE)
#define TYPE(TY, BASE)                                                                             \
    case clang::Type::TY: decl = decl_of_impl(llvm::cast<clang::TY##Type>(type)); break;
#include "clang/AST/TypeNodes.inc"
    }

    if(decl) {
        return {decl};
    }
    return {};
}

auto decl_of(clang::QualType type, TemplateResolver* resolver) -> const clang::NamedDecl* {
    auto decls = decls_of(type, resolver);
    return decls.empty() ? nullptr : decls.front();
}

auto unwrap(clang::TypeLoc type, bool unwrap_function_type) -> clang::TypeLoc {
    while(true) {
        if(auto qualified = type.getAs<clang::QualifiedTypeLoc>()) {
            type = qualified.getUnqualifiedLoc();
        } else if(auto reference = type.getAs<clang::ReferenceTypeLoc>()) {
            type = reference.getPointeeLoc();
        } else if(auto pointer = type.getAs<clang::PointerTypeLoc>()) {
            type = pointer.getPointeeLoc();
        } else if(auto paren = type.getAs<clang::ParenTypeLoc>()) {
            type = paren.getInnerLoc();
        } else if(auto array = type.getAs<clang::ConstantArrayTypeLoc>()) {
            type = array.getElementLoc();
        } else if(auto proto = type.getAs<clang::FunctionProtoTypeLoc>();
                  proto && unwrap_function_type) {
            type = proto.getReturnLoc();
        } else {
            break;
        }
    }
    return type;
}

auto unwrap(clang::QualType type) -> clang::QualType {
    while(!type.isNull()) {
        const clang::Type* written = type.getTypePtr();
        if(auto* paren = llvm::dyn_cast<clang::ParenType>(written)) {
            type = paren->getInnerType();
        } else if(auto* adjusted = llvm::dyn_cast<clang::AdjustedType>(written)) {
            type = adjusted->getOriginalType();
        } else if(auto* pointer = llvm::dyn_cast<clang::PointerType>(written)) {
            type = pointer->getPointeeType();
        } else if(auto* reference = llvm::dyn_cast<clang::ReferenceType>(written)) {
            type = reference->getPointeeType();
        } else if(auto* array = llvm::dyn_cast<clang::ArrayType>(written)) {
            type = array->getElementType();
        } else if(auto* deduced = llvm::dyn_cast<clang::DeducedType>(written);
                  deduced && deduced->getDeducedKind() == clang::DeducedKind::Deduced) {
            type = deduced->getDeducedType();
        } else if(auto* decltype_type = llvm::dyn_cast<clang::DecltypeType>(written);
                  decltype_type && decltype_type->isSugared()) {
            type = decltype_type->getUnderlyingType();
        } else {
            break;
        }
    }
    return type;
}

auto destructor_of(clang::QualType type) -> const clang::CXXDestructorDecl* {
    auto* RD = type->getAsCXXRecordDecl();
    if(!RD || !RD->hasDefinition() || RD->hasTrivialDestructor()) {
        return nullptr;
    }
    return RD->getDestructor();
}

auto declared_type(const clang::TypeDecl* decl) -> clang::QualType {
    assert(decl);
    clang::ASTContext& context = decl->getASTContext();
    if(const auto* spec = llvm::dyn_cast<clang::ClassTemplateSpecializationDecl>(decl)) {
        if(const auto* args = spec->getTemplateArgsAsWritten()) {
            return context.getTemplateSpecializationType(
                clang::ElaboratedTypeKeyword::None,
                clang::TemplateName(spec->getSpecializedTemplate()),
                args->arguments(),
                /*CanonicalArgs=*/{});
        }
    }
    return context.getTypeDeclType(decl);
}

namespace {

/// Returns the TemplateTypeParmTypeLoc of the implicit template type
/// parameter introduced by an `auto` typed function parameter, if any.
auto contained_auto_param_type(clang::TypeLoc type) -> clang::TemplateTypeParmTypeLoc {
    if(auto qualified = type.getAs<clang::QualifiedTypeLoc>()) {
        return contained_auto_param_type(qualified.getUnqualifiedLoc());
    }

    if(llvm::isa<clang::PointerType, clang::ReferenceType, clang::ParenType>(type.getTypePtr())) {
        return contained_auto_param_type(type.getNextTypeLoc());
    }

    if(auto function = type.getAs<clang::FunctionTypeLoc>()) {
        return contained_auto_param_type(function.getReturnLoc());
    }

    if(auto param = type.getAs<clang::TemplateTypeParmTypeLoc>()) {
        if(param.getTypePtr()->getDecl()->isImplicit()) {
            return param;
        }
    }

    return {};
}

/// The deduced return type of a function whose return type holds the
/// placeholder:
/// - auto foo() {}
/// - auto& foo() {}
/// - auto foo() -> int {}
/// - auto foo() -> decltype(1+1) {}
/// - operator auto() const { return 10; }
auto deduced_return_type(const clang::FunctionDecl* function) -> clang::QualType {
    auto returned = function->getReturnType();
    if(const auto* AT = returned->getContainedAutoType();
       AT && AT->getDeducedKind() == clang::DeducedKind::Deduced) {
        return AT->getDeducedType();
    }
    /// auto in a trailing return type just points to a DecltypeType and
    /// getContainedAutoType does not unwrap it.
    if(const auto* DT = llvm::dyn_cast<clang::DecltypeType>(returned)) {
        return DT->getUnderlyingType();
    }
    return returned;
}

/// The type an abbreviated template's `auto` parameter took, when the
/// template was instantiated exactly once.
auto deduced_param_type(const clang::ParmVarDecl* param, clang::TemplateTypeParmTypeLoc written)
    -> clang::QualType {
    /// We expect the TTP to be attached to this function template.
    const auto* templated = llvm::dyn_cast<clang::FunctionDecl>(param->getDeclContext());
    auto* template_decl = templated ? templated->getDescribedFunctionTemplate() : nullptr;
    if(!template_decl) {
        return {};
    }

    auto* params = template_decl->getTemplateParameters();
    auto parameters = params->asArray();
    auto* found = std::ranges::find(parameters, written.getDecl());
    assert(found != params->end() && "auto TTP is not from enclosing function?");

    // The helper keeps its clangd signature, which speaks mutable pointers;
    // it only reads through them.
    auto* instantiation = llvm::dyn_cast_or_null<clang::FunctionDecl>(
        decls::only_instantiation(const_cast<clang::FunctionDecl*>(templated)));
    if(!instantiation) {
        return {};
    }

    const auto* args = instantiation->getTemplateSpecializationArgs();
    if(args->size() != params->size()) {
        /// No weird variadic stuff.
        return {};
    }
    return args->get(found - params->begin()).getAsType();
}

}  // namespace

auto deduced_type(clang::DynTypedNode written, const clang::Decl* owner) -> clang::QualType {
    if(const auto* loc = written.get<clang::TypeLoc>()) {
        if(auto decltype_loc = loc->getAs<clang::DecltypeTypeLoc>()) {
            /// A DecltypeType's underlying type can be another DecltypeType! E.g.
            ///   int I = 0;
            ///   decltype(I) J = I;
            ///   decltype(J) K = J;
            clang::QualType deduced;
            const auto* type = decltype_loc.getTypePtr();
            while(type && !type->getUnderlyingType().isNull()) {
                deduced = type->getUnderlyingType();
                type = llvm::dyn_cast<clang::DecltypeType>(deduced.getTypePtr());
            }
            return deduced;
        }
    }

    const auto* declarator = llvm::dyn_cast<clang::DeclaratorDecl>(owner);
    if(!declarator || !declarator->getTypeSourceInfo()) {
        return {};
    }

    /// The leading `auto` of a trailing return type belongs to the function
    /// itself.
    if(written.get<clang::Decl>() == owner) {
        const auto* function = llvm::dyn_cast<clang::FunctionDecl>(owner);
        const auto* proto =
            function ? function->getType()->getAs<clang::FunctionProtoType>() : nullptr;
        return proto && proto->hasTrailingReturn() ? deduced_return_type(function)
                                                   : clang::QualType();
    }

    const auto* loc = written.get<clang::TypeLoc>();
    if(!loc) {
        return {};
    }
    auto declared = declarator->getTypeSourceInfo()->getTypeLoc();

    /// 'auto' of a parameter does not name an AutoType, but an implicit
    /// template parameter.
    if(auto param_loc = loc->getAs<clang::TemplateTypeParmTypeLoc>()) {
        const auto* param = llvm::dyn_cast<clang::ParmVarDecl>(declarator);
        if(!param || contained_auto_param_type(declared) != param_loc) {
            return {};
        }
        return deduced_param_type(param, param_loc);
    }

    auto auto_loc = loc->getAs<clang::AutoTypeLoc>();
    if(!auto_loc) {
        return {};
    }
    /// The placeholder of `owner`'s own type, not one below it (`new auto`
    /// in an initializer or a body).
    auto declares = [&](clang::TypeLoc type) {
        auto contained = type.getContainedAutoTypeLoc();
        return contained && contained.getNameLoc() == auto_loc.getNameLoc();
    };

    if(const auto* function = llvm::dyn_cast<clang::FunctionDecl>(declarator)) {
        /// `operator auto()` spells its placeholder in the name; a function
        /// declared through an alias (`id<decltype(auto(0))()> f;`) has no
        /// return type of its own written.
        clang::TypeLoc returned;
        if(llvm::isa<clang::CXXConversionDecl>(function)) {
            returned = function->getNameInfo().getNamedTypeInfo()->getTypeLoc();
        } else if(auto proto = function->getFunctionTypeLoc()) {
            returned = proto.getReturnLoc();
        }
        return returned && declares(returned) ? deduced_return_type(function) : clang::QualType();
    }

    if(!declares(declared)) {
        return {};
    }
    // A structured binding of an array declares the array type itself.
    const auto* type = declarator->getType()->getContainedAutoType();
    return type ? type->desugar() : clang::QualType();
}

auto accessed_specialization(const clang::CXXDependentScopeMemberExpr& access,
                             TemplateResolver& resolver)
    -> const clang::TemplateSpecializationType* {
    if(access.isImplicitAccess()) {
        return nullptr;
    }
    auto object = resolver.member_object(access.getBase(), access.isArrow());
    return object.isNull() ? nullptr : object->getAs<clang::TemplateSpecializationType>();
}

auto writable(clang::QualType type) -> bool {
    struct Finder : clang::RecursiveASTVisitor<Finder> {
        bool VisitBuiltinType(clang::BuiltinType* type) {
            return type->getKind() != clang::BuiltinType::Dependent;
        }

        bool VisitDecltypeType(clang::DecltypeType* type) {
            return !type->isDependentType();
        }

        bool VisitAutoType(clang::AutoType* type) {
            return type->getDeducedKind() == clang::DeducedKind::Deduced;
        }

        bool VisitTemplateTypeParmType(clang::TemplateTypeParmType* type) {
            return type->getDecl() && !type->getDecl()->isImplicit();
        }
    };

    return Finder().TraverseType(type);
}

namespace {

/// What initializes a parameter: the argument, or what a conversion
/// function returns.
struct Value {
    clang::QualType type;
    bool lvalue = false;
    /// A null pointer constant (`0`, `NULL`).
    bool null = false;
    /// A string literal, which clang still converts to a pointer to
    /// non-const `char` (`wchar_t` when wide), with a warning.
    bool literal = false;
};

/// The non-class types whose standard conversions are decided here. The
/// rest — member and function pointers, vectors, `_Complex`, `_BitInt`,
/// atomics — stay undecided.
enum class Kind : std::uint8_t {
    Arithmetic,
    UnscopedEnum,
    ScopedEnum,
    Pointer,
    NullPointer,
    Other
};

Kind kind_of(clang::QualType type) {
    if(const auto* builtin = type->getAs<clang::BuiltinType>()) {
        if(builtin->isInteger() || builtin->isFloatingPoint()) {
            return Kind::Arithmetic;
        }
        return builtin->getKind() == clang::BuiltinType::NullPtr ? Kind::NullPointer : Kind::Other;
    }
    if(type->isEnumeralType()) {
        return type->isScopedEnumeralType() ? Kind::ScopedEnum : Kind::UnscopedEnum;
    }
    return type->isPointerType() ? Kind::Pointer : Kind::Other;
}

/// An arithmetic, enumeration or `std::nullptr_t` type.
bool plain(clang::QualType type) {
    auto kind = kind_of(type);
    return kind != Kind::Pointer && kind != Kind::Other;
}

/// What a pointer whose conversions are decided here points to.
bool decided_pointee(clang::QualType type) {
    return type->isVoidType() || type->isRecordType() || plain(type);
}

/// A non-class type whose reference bindings are decided here.
bool decided_referee(clang::QualType type) {
    return plain(type) || (type->isPointerType() && decided_pointee(type->getPointeeType()));
}

/// The definition of the class `type` names, null while it has none.
const clang::CXXRecordDecl* class_of(clang::QualType type) {
    const auto* record = type->getAsCXXRecordDecl();
    if(!record || type->isDependentType() || !record->hasDefinition()) {
        return nullptr;
    }
    record = record->getDefinition();
    return record->isInvalidDecl() ? nullptr : record;
}

bool same_or_derived(const clang::CXXRecordDecl& from, const clang::CXXRecordDecl& to) {
    return from.getCanonicalDecl() == to.getCanonicalDecl() || from.isDerivedFrom(&to);
}

/// Whether `outer` carries every cv-qualifier of `inner`.
bool includes_qualifiers(clang::QualType outer, clang::QualType inner) {
    return (inner.getCVRQualifiers() & ~outer.getCVRQualifiers()) == 0;
}

/// Whether only an lvalue binds `reference` with no conversion: a
/// reference to non-const, or to volatile.
bool binds_lvalue_only(clang::QualType reference) {
    auto referred = reference.getNonReferenceType();
    return reference->isLValueReferenceType() &&
           (!referred.isConstQualified() || referred.isVolatileQualified());
}

/// Whether no standard conversion takes `from`, of non-class type, to the
/// non-class `to`.
bool never_converts(clang::ASTContext& context, const Value& from, clang::QualType to) {
    auto type = from.type;
    if(type->isArrayType()) {
        type = context.getArrayDecayedType(type);
    }
    if(context.hasSameUnqualifiedType(type, to)) {
        return false;
    }
    auto kind = kind_of(type);
    switch(kind_of(to)) {
        case Kind::Arithmetic: {
            /// `std::nullptr_t` converts to `bool` by direct-initialization
            /// only.
            if(to->isBooleanType()) {
                return kind == Kind::ScopedEnum || kind == Kind::NullPointer;
            }
            return kind == Kind::ScopedEnum || kind == Kind::Pointer || kind == Kind::NullPointer;
        }
        case Kind::UnscopedEnum:
        case Kind::ScopedEnum: return kind != Kind::Other;
        case Kind::Pointer: break;
        default: return false;
    }

    if(from.null || kind == Kind::NullPointer) {
        return false;
    }
    if(kind != Kind::Pointer) {
        return kind != Kind::Other;
    }
    auto target = to->getPointeeType();
    auto pointee = type->getPointeeType();
    if(!decided_pointee(target) || !decided_pointee(pointee)) {
        return false;
    }
    bool writable_literal = from.literal && target.getCVRQualifiers() == 0 &&
                            (target->isCharType() || target->isWideCharType());
    if(!includes_qualifiers(target, pointee) && !writable_literal) {
        return true;
    }
    if(target->isVoidType()) {
        return false;
    }
    if(pointee->isVoidType()) {
        return true;
    }
    if(context.hasSameUnqualifiedType(target, pointee)) {
        return false;
    }
    if(target->isRecordType() && pointee->isRecordType()) {
        const auto* base = class_of(target);
        const auto* derived = class_of(pointee);
        return base && derived && !same_or_derived(*derived, *base);
    }
    return true;
}

/// Whether no standard conversion sequence, reference binding included,
/// takes `from` to `to`: all a converting constructor's parameter or a
/// conversion function's result allows, on its side of the one
/// user-defined conversion.
bool never_standard(clang::ASTContext& context, const Value& from, clang::QualType to) {
    auto target = to.getNonReferenceType();
    if(target->isFunctionType() || from.type->isFunctionType()) {
        return false;
    }
    if(target->isRecordType() != from.type->isRecordType()) {
        return true;
    }

    bool reference = to->isReferenceType();
    if(target->isRecordType()) {
        const auto* base = class_of(target);
        const auto* derived = class_of(from.type);
        if(!base || !derived) {
            return false;
        }
        if(!same_or_derived(*derived, *base)) {
            return true;
        }
        return reference && (!includes_qualifiers(target, from.type) ||
                             (from.lvalue ? to->isRValueReferenceType() : binds_lvalue_only(to)));
    }

    if(!reference) {
        return never_converts(context, from, target);
    }
    bool decided = decided_referee(target) && decided_referee(from.type);
    bool related = decided && context.hasSimilarType(target, from.type);
    bool same = related && context.hasSameUnqualifiedType(target, from.type);
    if(binds_lvalue_only(to)) {
        if(!from.lvalue) {
            return true;
        }
        return decided && (!related || (same && !includes_qualifiers(target, from.type)));
    }
    if(related && from.lvalue && to->isRValueReferenceType()) {
        return true;
    }
    if(same) {
        return !includes_qualifiers(target, from.type);
    }
    return never_converts(context, from, target.getUnqualifiedType());
}

/// Whether a converting constructor of `record` may take `from`.
bool constructs(clang::ASTContext& context, const clang::CXXRecordDecl& record, const Value& from) {
    auto name =
        context.DeclarationNames.getCXXConstructorName(context.getCanonicalTagType(&record));
    for(const auto* found: record.lookup(name)) {
        /// A constructor template, or one inherited by a using-declaration.
        const auto* constructor = llvm::dyn_cast<clang::CXXConstructorDecl>(found);
        if(!constructor) {
            return true;
        }
        if(!constructor->isConvertingConstructor(/*AllowExplicit=*/false)) {
            continue;
        }
        /// `S(...)` takes anything.
        if(constructor->getNumParams() == 0 ||
           !never_standard(context, from, constructor->getParamDecl(0)->getType())) {
            return true;
        }
    }
    return false;
}

/// Whether one user-defined conversion — a converting constructor of the
/// parameter's class, a conversion function of the argument's — may take
/// `from` to `to`; true wherever unsure.
bool may_convert(clang::ASTContext& context, const Value& from, clang::QualType to) {
    auto target = to.getNonReferenceType();
    bool lvalue_only = binds_lvalue_only(to);
    const clang::CXXRecordDecl* source = nullptr;
    if(from.type->isRecordType()) {
        source = class_of(from.type);
        if(!source) {
            return true;
        }
    }
    if(target->isRecordType()) {
        const auto* record = class_of(target);
        if(!record) {
            return source || !lvalue_only;
        }
        /// A class binds a reference to itself or a base directly, or not
        /// at all.
        if(source && same_or_derived(*source, *record)) {
            return false;
        }
        if(!lvalue_only && constructs(context, *record, from)) {
            return true;
        }
    }
    if(!source) {
        return false;
    }

    for(const auto* found: source->getVisibleConversionFunctions()) {
        const auto* conversion =
            llvm::dyn_cast<clang::CXXConversionDecl>(found->getUnderlyingDecl());
        if(!conversion) {
            return true;
        }
        if(conversion->isExplicit()) {
            continue;
        }
        auto type = conversion->getConversionType();
        if(type->isDependentType() || type->isUndeducedType()) {
            return true;
        }
        /// A reference only an lvalue binds takes what a conversion
        /// function returns by lvalue reference, else a temporary.
        if(lvalue_only && !type->isLValueReferenceType()) {
            continue;
        }
        Value result{.type = type.getNonReferenceType(), .lvalue = type->isLValueReferenceType()};
        if(!never_standard(context, result, lvalue_only ? to : target.getUnqualifiedType())) {
            return true;
        }
    }
    return false;
}

}  // namespace

auto never_initializes(clang::ASTContext& context,
                       const clang::Expr& argument,
                       clang::QualType parameter) -> bool {
    auto type = argument.getType();
    if(argument.isTypeDependent() || argument.containsErrors() || parameter->isDependentType() ||
       type->isPlaceholderType() || type->isUndeducedType() ||
       llvm::isa<clang::InitListExpr>(argument.IgnoreParens())) {
        return false;
    }
    Value from{
        .type = type,
        .lvalue = argument.isLValue(),
        .null = argument.isNullPointerConstant(context, clang::Expr::NPC_ValueDependentIsNull) !=
                clang::Expr::NPCK_NotNull,
        .literal = llvm::isa<clang::StringLiteral>(argument.IgnoreParenImpCasts()),
    };
    return never_standard(context, from, parameter) && !may_convert(context, from, parameter);
}

}  // namespace clice::types
