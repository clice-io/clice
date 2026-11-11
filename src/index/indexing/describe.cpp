module clice;

import :index.indexing;
import :index.indexing.builder;
import :index.symbol;
import :semantic.decls;
import :semantic.display;
import :semantic.identity;
import :semantic.symbol;
import :semantic.token_class;

namespace clice::index {

namespace {

/// Whether an anonymous namespace sits between a declaration and the scope
/// its qualified name starts from, past the scopes such a name skips.
bool in_anonymous_namespace(const clang::NamedDecl* decl) {
    if(auto* shadow = llvm::dyn_cast<clang::UsingShadowDecl>(decl)) {
        decl = shadow->getTargetDecl();
    }
    for(const auto* context = decl->getDeclContext(); !context->isTranslationUnit();
        context = context->getParent()) {
        if(const auto* ns = llvm::dyn_cast<clang::NamespaceDecl>(context)) {
            if(ns->isAnonymousNamespace()) {
                return true;
            }
            if(!ns->isInline()) {
                return false;
            }
        } else if(const auto* record = llvm::dyn_cast<clang::RecordDecl>(context);
                  record && record->isAnonymousStructOrUnion()) {
            continue;
        } else if(const auto* enumeration = llvm::dyn_cast<clang::EnumDecl>(context);
                  enumeration && !enumeration->isScoped() && !enumeration->getIdentifier()) {
            continue;
        } else if(llvm::isa<clang::NamedDecl>(context)) {
            return false;
        }
    }
    return false;
}

NameForm name_form_of(clang::DeclarationName name) {
    switch(name.getNameKind()) {
        case clang::DeclarationName::Identifier: return NameForm::Identifier;
        case clang::DeclarationName::CXXConstructorName: return NameForm::Constructor;
        case clang::DeclarationName::CXXDestructorName: return NameForm::Destructor;
        case clang::DeclarationName::CXXConversionFunctionName: return NameForm::Conversion;
        case clang::DeclarationName::CXXOperatorName: return NameForm::Operator;
        case clang::DeclarationName::CXXLiteralOperatorName: return NameForm::Literal;
        case clang::DeclarationName::CXXDeductionGuideName:
        case clang::DeclarationName::CXXUsingDirective: return NameForm::Other;
        case clang::DeclarationName::ObjCZeroArgSelector:
        case clang::DeclarationName::ObjCOneArgSelector:
        case clang::DeclarationName::ObjCMultiArgSelector: return NameForm::Selector;
    }
    std::unreachable();
}

/// Instantiations never reach the symbol table (decls::normalize folds
/// them into their pattern), so a specialization node here was written.
bool is_specialization(const clang::NamedDecl* decl) {
    if(llvm::isa<clang::ClassTemplateSpecializationDecl, clang::VarTemplateSpecializationDecl>(
           decl)) {
        return true;
    }
    if(auto* function = llvm::dyn_cast<clang::FunctionDecl>(decl)) {
        return function->getTemplateSpecializationKind() == clang::TSK_ExplicitSpecialization;
    }
    return false;
}

/// clangd's rule for what unqualified completion may offer from an index:
/// namespace-scope declarations, and the enumerators of unscoped enums at
/// namespace or class scope. Members are Sema's after `.`, and a
/// specialization shares its template's name.
bool is_completable(const clang::NamedDecl* decl) {
    if(is_specialization(decl)) {
        return false;
    }
    auto namespace_scope = [](const clang::DeclContext* context) {
        return context->isTranslationUnit() || context->isNamespace() ||
               llvm::isa<clang::LinkageSpecDecl, clang::ExportDecl>(context);
    };
    const clang::DeclContext* context = decl->getDeclContext();
    if(namespace_scope(context)) {
        return true;
    }
    if(auto* enumeration = llvm::dyn_cast<clang::EnumDecl>(context)) {
        const clang::DeclContext* outer = enumeration->getDeclContext();
        return !enumeration->isScoped() &&
               (namespace_scope(outer) || llvm::isa<clang::CXXRecordDecl>(outer));
    }
    return false;
}

}  // namespace

Symbol Builder::describe(const clang::NamedDecl* decl) {
    Symbol symbol;
    auto flags = SymbolFlags::None;
    auto name = display::symbol_name(decl);
    symbol.name = std::move(name.text);
    if(name.unnamed) {
        flags |= SymbolFlags::Unnamed;
    }
    symbol.args = display::template_args(*decl);
    if(const auto* function = llvm::dyn_cast<clang::FunctionDecl>(decl)) {
        symbol.signature = display::call_signature(*function,
                                                   {
                                                       .suppress_unwritten_scope = true,
                                                       .polish_for_declaration = true,
                                                       .clean_uglified = true,
                                                       .readable = &readable,
                                                       .scope = function->getDeclContext(),
                                                   });
    }
    symbol.parent = unit.get<EntityTable>().parent(decl);
    symbol.kind = SymbolKind::from(decl);
    symbol.scope = scope_of(decl);
    if(decls::is_templated(decl)) {
        flags |= SymbolFlags::Template;
    }
    if(is_specialization(decl)) {
        flags |= SymbolFlags::Specialization;
    }
    // Attributes accumulate along the redeclaration chain; the last
    // declaration carries them all.
    if(decl->getMostRecentDecl()->getAvailability() == clang::AR_Deprecated) {
        flags |= SymbolFlags::Deprecated;
    }
    if(auto* ns = llvm::dyn_cast<clang::NamespaceDecl>(decl); ns && ns->isInline()) {
        flags |= SymbolFlags::InlineNamespace;
    }
    if(auto* record = llvm::dyn_cast<clang::RecordDecl>(decl);
       record && record->isAnonymousStructOrUnion()) {
        flags |= SymbolFlags::AnonymousScope;
    }
    if(auto* enumeration = llvm::dyn_cast<clang::EnumDecl>(decl);
       enumeration && !enumeration->isScoped() && !enumeration->getIdentifier()) {
        flags |= SymbolFlags::AnonymousScope;
    }
    if(in_anonymous_namespace(decl)) {
        flags |= SymbolFlags::AnonymousNamespace;
    }
    if(decl->isTemplateParameter()) {
        flags |= SymbolFlags::TemplateParameter;
    }
    // `FWD(Expr)` forward-declares a class defined by hand.
    if(ranges::all_of(decl->redecls(), [](const clang::Decl* redecl) {
           return redecl->getLocation().isMacroID();
       })) {
        flags |= SymbolFlags::SpelledInMacro;
    }
    if(unit.in_library(decl->getLocation())) {
        flags |= SymbolFlags::SystemHeader;
    }
    if(is_completable(decl)) {
        flags |= SymbolFlags::Completable;
    }
    if(decls::is_exported(decl)) {
        flags |= SymbolFlags::Exported;
    }
    symbol.flags = with_form(flags, name_form_of(decl->getDeclName()));
    symbol.token_class = token_class(decl);
    return symbol;
}

}  // namespace clice::index
