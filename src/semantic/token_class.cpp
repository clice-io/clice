module;

#include "modules/prelude.h"

module clice;

import :semantic.decls;
import :semantic.token_class;
import :syntax.lexer;

namespace clice {

namespace {

using enum SemanticTokenKind;
using enum SemanticTokenModifier;

SemanticTokenKind kind_of(const clang::NamedDecl* decl) {
    if(llvm::isa<clang::NamespaceDecl, clang::NamespaceAliasDecl>(decl)) {
        return Namespace;
    }
    if(llvm::isa<clang::TemplateTypeParmDecl, clang::TemplateTemplateParmDecl>(decl)) {
        return TypeParameter;
    }
    if(llvm::isa<clang::NonTypeTemplateParmDecl>(decl)) {
        return TemplateParameter;
    }
    if(llvm::isa<clang::ConceptDecl>(decl)) {
        return Concept;
    }
    if(llvm::isa<clang::BuiltinTemplateDecl>(decl)) {
        return TypeAlias;
    }
    if(const auto* tmpl = llvm::dyn_cast<clang::TemplateDecl>(decl)) {
        return kind_of(tmpl->getTemplatedDecl());
    }
    if(llvm::isa<clang::TypedefNameDecl, clang::UnresolvedUsingTypenameDecl>(decl)) {
        return TypeAlias;
    }
    if(const auto* tag = llvm::dyn_cast<clang::TagDecl>(decl)) {
        // One kind for every redeclaration: `struct S;` may forward a
        // `class S`.
        const auto* defined = tag->getDefinition();
        switch((defined ? defined : tag->getCanonicalDecl())->getTagKind()) {
            case clang::TagTypeKind::Enum: return Enum;
            case clang::TagTypeKind::Union: return Union;
            case clang::TagTypeKind::Class: return Class;
            case clang::TagTypeKind::Struct:
            case clang::TagTypeKind::Interface: return Struct;
        }
        std::unreachable();
    }
    if(llvm::isa<clang::FieldDecl, clang::IndirectFieldDecl, clang::MSPropertyDecl>(decl)) {
        return Field;
    }
    if(llvm::isa<clang::EnumConstantDecl>(decl)) {
        return EnumMember;
    }
    if(const auto* guide = llvm::dyn_cast<clang::CXXDeductionGuideDecl>(decl)) {
        return kind_of(guide->getDeducedTemplate());
    }
    if(const auto* function = llvm::dyn_cast<clang::FunctionDecl>(decl)) {
        if(function->isOverloadedOperator() || function->getLiteralIdentifier()) {
            return Operator;
        }
        return llvm::isa<clang::CXXMethodDecl>(function) ? Method : Function;
    }
    if(llvm::isa<clang::ParmVarDecl>(decl)) {
        return Parameter;
    }
    if(llvm::isa<clang::VarDecl, clang::BindingDecl>(decl)) {
        return Variable;
    }
    if(llvm::isa<clang::LabelDecl>(decl)) {
        return Label;
    }
    if(llvm::isa<clang::UnresolvedUsingValueDecl, clang::UnresolvedUsingIfExistsDecl>(decl)) {
        return Unknown;
    }
    return None;
}

/// Where `decl` is visible from: a function, a class, its file, or
/// everywhere; nothing for a template parameter without a meaningful
/// context (a variable template's).
std::optional<SemanticTokenModifier> scope_of(const clang::NamedDecl* decl) {
    const clang::DeclContext* context = decl->getDeclContext();
    // The class name injected into a class belongs to the class's scope.
    if(const auto* record = llvm::dyn_cast<clang::CXXRecordDecl>(decl);
       record && record->isInjectedClassName()) {
        context = context->getParent();
    }
    // A lambda's captures are its function's locals.
    if(const auto* record = llvm::dyn_cast<clang::RecordDecl>(context);
       record && record->isLambda() && llvm::isa<clang::FieldDecl>(decl)) {
        return Local;
    }
    for(; !context->isFileContext(); context = context->getParent()) {
        if(context->isFunctionOrMethod()) {
            return Local;
        }
        if(context->isRecord()) {
            return Member;
        }
    }
    if(context->isTranslationUnit() && decl->isTemplateParameter()) {
        return std::nullopt;
    }
    // Visible, not external, linkage: a C enumerator has no linkage yet
    // reaches every file its enum does.
    if(!clang::isExternallyVisible(decl->getLinkageInternal())) {
        return Internal;
    }
    return Global;
}

/// Whether an object of `type` cannot be modified through its name: const
/// itself, a reference to const, an array of const elements.
bool is_readonly(clang::QualType type) {
    if(type.isNull()) {
        return false;
    }
    type = type.getNonReferenceType();
    if(type.isConstQualified()) {
        return true;
    }
    if(const auto* array = type->getAsArrayTypeUnsafe()) {
        return is_readonly(array->getElementType());
    }
    return false;
}

bool is_readonly(const clang::NamedDecl* decl) {
    if(llvm::isa<clang::EnumConstantDecl, clang::NonTypeTemplateParmDecl>(decl)) {
        return true;
    }
    if(const auto* property = llvm::dyn_cast<clang::MSPropertyDecl>(decl)) {
        return !property->hasSetter() || is_readonly(property->getType());
    }
    if(llvm::isa<clang::FieldDecl, clang::VarDecl, clang::BindingDecl>(decl)) {
        return is_readonly(llvm::cast<clang::ValueDecl>(decl)->getType());
    }
    if(const auto* method = llvm::dyn_cast<clang::CXXMethodDecl>(decl)) {
        return method->isConst();
    }
    return false;
}

bool is_static(const clang::NamedDecl* decl) {
    if(const auto* method = llvm::dyn_cast<clang::CXXMethodDecl>(decl)) {
        return method->isStatic();
    }
    if(const auto* var = llvm::dyn_cast<clang::VarDecl>(decl)) {
        return var->isStaticDataMember() || var->isStaticLocal();
    }
    return false;
}

/// Whether a value of `type` can be called: a function pointer or
/// reference, a pointer to member function, a lambda, a class with an
/// `operator()`.
bool is_callable(clang::QualType type) {
    if(type.isNull() || type->isDependentType()) {
        return false;
    }
    type = type.getNonReferenceType();
    if(type->isFunctionPointerType() || type->isMemberFunctionPointerType() ||
       type->isFunctionType()) {
        return true;
    }
    const auto* record = type->getAsCXXRecordDecl();
    if(!record || !record->hasDefinition()) {
        return false;
    }
    auto call = record->getASTContext().DeclarationNames.getCXXOperatorName(clang::OO_Call);
    return record->isLambda() || !record->lookup(call).empty();
}

bool is_constexpr(const clang::NamedDecl* decl) {
    if(const auto* function = llvm::dyn_cast<clang::FunctionDecl>(decl)) {
        return function->isConstexpr();
    }
    if(const auto* var = llvm::dyn_cast<clang::VarDecl>(decl)) {
        return var->isConstexpr() || var->hasAttr<clang::ConstInitAttr>();
    }
    return llvm::isa<clang::NonTypeTemplateParmDecl>(decl);
}

bool is_default_library(const clang::NamedDecl* decl) {
    if(llvm::isa<clang::BuiltinTemplateDecl>(decl)) {
        return true;
    }
    if(const auto* function = llvm::dyn_cast<clang::FunctionDecl>(decl);
       function && function->getBuiltinID() != 0) {
        return true;
    }
    auto location = decl->getCanonicalDecl()->getLocation();
    return location.isValid() &&
           decl->getASTContext().getSourceManager().isInSystemHeader(location);
}

/// The keyword tables of the newest C and C++ dialects, where a keyword of
/// any dialect is found.
struct KeywordTables {
    clang::LangOptions cxx = raw_dialect(clang::Language::CXX, clang::LangStandard::lang_cxx26);
    clang::LangOptions c = raw_dialect(clang::Language::C, clang::LangStandard::lang_c23);
    clang::IdentifierTable cxx_keywords{cxx};
    clang::IdentifierTable c_keywords{c};
};

}  // namespace

TokenClass token_class(const clang::NamedDecl* decl) {
    if(const auto* shadow = llvm::dyn_cast<clang::UsingShadowDecl>(decl)) {
        decl = shadow->getTargetDecl();
    }
    if(const auto* guide = llvm::dyn_cast<clang::CXXDeductionGuideDecl>(decl)) {
        decl = guide->getDeducedTemplate();
    }

    TokenClass result{.kind = kind_of(decl)};
    if(result.kind == None) {
        return result;
    }
    auto add = [&](SemanticTokenModifier modifier) {
        result.modifiers |= mask(modifier);
    };

    if(decls::is_templated(decl) ||
       llvm::isa<clang::ClassTemplateSpecializationDecl, clang::VarTemplateSpecializationDecl>(
           decl)) {
        add(Template);
    }
    if(const auto* function = llvm::dyn_cast<clang::FunctionDecl>(decl);
       function && function->getPrimaryTemplate()) {
        add(Template);
    }
    // The facts below are the templated declaration's: a class template is
    // abstract when its class is.
    if(const auto* tmpl = llvm::dyn_cast<clang::TemplateDecl>(decl);
       tmpl && tmpl->getTemplatedDecl() && !llvm::isa<clang::TemplateTemplateParmDecl>(decl)) {
        decl = tmpl->getTemplatedDecl();
    }

    if(auto scope = scope_of(decl)) {
        add(*scope);
    }
    if(is_readonly(decl)) {
        add(Readonly);
    }
    if(is_static(decl)) {
        add(Static);
    }
    if(decl->getMostRecentDecl()->getAvailability() == clang::AR_Deprecated) {
        add(Deprecated);
    }
    if(is_default_library(decl)) {
        add(DefaultLibrary);
    }
    if(is_constexpr(decl)) {
        add(Constexpr);
    }
    if(llvm::isa<clang::UnresolvedUsingValueDecl,
                 clang::UnresolvedUsingTypenameDecl,
                 clang::UnresolvedUsingIfExistsDecl>(decl)) {
        add(Dependent);
    }

    if(const auto* method = llvm::dyn_cast<clang::CXXMethodDecl>(decl)) {
        if(method->isVirtual()) {
            add(Virtual);
        }
        if(method->isPureVirtual()) {
            add(Abstract);
        }
        if(llvm::isa<clang::CXXConstructorDecl>(method)) {
            add(Constructor);
        }
        if(llvm::isa<clang::CXXDestructorDecl>(method)) {
            add(Destructor);
        }
    }
    if(const auto* record = llvm::dyn_cast<clang::CXXRecordDecl>(decl)) {
        if(const auto* definition = record->getDefinition()) {
            if(definition->isAbstract()) {
                add(Abstract);
            }
            if(definition->isPolymorphic()) {
                add(Virtual);
            }
        }
    }
    if(const auto* function = llvm::dyn_cast<clang::FunctionDecl>(decl)) {
        if(function->isNoReturn()) {
            add(ControlFlow);
        }
        if(result.kind == Operator) {
            add(UserDefined);
        }
    }
    if(llvm::isa<clang::VarDecl, clang::FieldDecl, clang::BindingDecl>(decl) &&
       is_callable(llvm::cast<clang::ValueDecl>(decl)->getType())) {
        add(Callable);
    }
    return result;
}

TokenClass token_class(const clang::MacroInfo& macro, const clang::SourceManager& sources) {
    TokenClass result{.kind = Macro};
    auto add = [&](SemanticTokenModifier modifier) {
        result.modifiers |= mask(modifier);
    };
    if(macro.isFunctionLike()) {
        add(Callable);
    }
    if(macro.tokens_empty()) {
        add(Empty);
    }
    for(const auto& token: macro.tokens()) {
        if(token.isOneOf(clang::tok::kw_return,
                         clang::tok::kw_break,
                         clang::tok::kw_continue,
                         clang::tok::kw_goto,
                         clang::tok::kw_throw,
                         clang::tok::kw_co_return)) {
            add(ControlFlow);
            break;
        }
    }
    auto location = macro.getDefinitionLoc();
    if(macro.isBuiltinMacro() || location.isInvalid() || sources.isInSystemHeader(location) ||
       sources.isWrittenInBuiltinFile(location)) {
        add(DefaultLibrary);
    }
    return result;
}

TokenClass keyword_class(clang::tok::TokenKind kind) {
    switch(kind) {
        // Fundamental and Clang/GNU builtin types; `__fp16` lexes as
        // `kw_half`.
        case clang::tok::kw_bool:
        case clang::tok::kw_char:
        case clang::tok::kw_wchar_t:
        case clang::tok::kw_char8_t:
        case clang::tok::kw_char16_t:
        case clang::tok::kw_char32_t:
        case clang::tok::kw_double:
        case clang::tok::kw_float:
        case clang::tok::kw_int:
        case clang::tok::kw_long:
        case clang::tok::kw_short:
        case clang::tok::kw_signed:
        case clang::tok::kw_unsigned:
        case clang::tok::kw_void:
        case clang::tok::kw_half:
        case clang::tok::kw__BitInt:
        case clang::tok::kw__Bool:
        case clang::tok::kw__Complex:
        case clang::tok::kw__Decimal128:
        case clang::tok::kw__Decimal32:
        case clang::tok::kw__Decimal64:
        case clang::tok::kw__ExtInt:
        case clang::tok::kw__Float16:
        case clang::tok::kw__Imaginary:
        case clang::tok::kw___bf16:
        case clang::tok::kw___float128:
        case clang::tok::kw___ibm128:
        case clang::tok::kw___int64:
        case clang::tok::kw___int128: return {.kind = Primitive};

        case clang::tok::kw_sizeof:
        case clang::tok::kw_alignof:
        case clang::tok::kw__Alignof:
        case clang::tok::kw_typeid:
        case clang::tok::kw_new:
        case clang::tok::kw_delete:
        case clang::tok::kw_static_cast:
        case clang::tok::kw_dynamic_cast:
        case clang::tok::kw_const_cast:
        case clang::tok::kw_reinterpret_cast:
        case clang::tok::kw_addrspace_cast: return {.kind = Operator};

        case clang::tok::kw_if:
        case clang::tok::kw_else:
        case clang::tok::kw_switch:
        case clang::tok::kw_case:
        case clang::tok::kw_for:
        case clang::tok::kw_while:
        case clang::tok::kw_do:
        case clang::tok::kw_break:
        case clang::tok::kw_continue:
        case clang::tok::kw_return:
        case clang::tok::kw_goto:
        case clang::tok::kw_try:
        case clang::tok::kw_catch:
        case clang::tok::kw_throw:
        case clang::tok::kw_co_await:
        case clang::tok::kw_co_return:
        case clang::tok::kw_co_yield:
        case clang::tok::kw___try:
        case clang::tok::kw___finally:
        case clang::tok::kw___leave: return {.kind = Keyword, .modifiers = mask(ControlFlow)};

        case clang::tok::kw_true:
        case clang::tok::kw_false:
        case clang::tok::kw_nullptr:
        case clang::tok::kw___null: return {.kind = Keyword, .modifiers = mask(Literal)};

        default: return {.kind = Keyword};
    }
}

clang::tok::TokenKind keyword_of(llvm::StringRef name) {
    static const KeywordTables tables;
    for(const auto* keywords: {&tables.cxx_keywords, &tables.c_keywords}) {
        if(auto it = keywords->find(name); it != keywords->end()) {
            return it->second->getTokenID();
        }
    }
    return clang::tok::identifier;
}

}  // namespace clice
