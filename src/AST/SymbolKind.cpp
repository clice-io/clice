#include "AST/SymbolKind.h"

#include "Compiler/Compilation.h"

namespace clice {

SymbolKind SymbolKind::from(const clang::Decl* decl) {
    if(llvm::isa<clang::NamespaceDecl, clang::NamespaceAliasDecl>(decl)) {
        return SymbolKind::Namespace;
    } else if(llvm::isa<clang::TypedefNameDecl,
                        clang::TemplateTypeParmDecl,
                        clang::TemplateTemplateParmDecl>(decl)) {
        return SymbolKind::Type;
    } else if(auto TD = llvm::dyn_cast<clang::TagDecl>(decl)) {
        return TD->isEnum()     ? SymbolKind::Enum
               : TD->isStruct() ? SymbolKind::Struct
               : TD->isUnion()  ? SymbolKind::Union
                                : SymbolKind::Class;
    } else if(llvm::isa<clang::FieldDecl>(decl)) {
        return SymbolKind::Field;
    } else if(llvm::isa<clang::EnumConstantDecl>(decl)) {
        return SymbolKind::EnumMember;
    } else if(auto FD = llvm::dyn_cast<clang::FunctionDecl>(decl)) {
        return FD->isOverloadedOperator() ? SymbolKind::Operator
               : FD->isCXXClassMember()   ? SymbolKind::Method
                                          : SymbolKind::Function;
    } else if(llvm::isa<clang::ParmVarDecl>(decl)) {
        return SymbolKind::Parameter;
    } else if(llvm::isa<clang::VarDecl, clang::BindingDecl, clang::NonTypeTemplateParmDecl>(decl)) {
        return SymbolKind::Variable;
    } else if(llvm::isa<clang::LabelDecl>(decl)) {
        return SymbolKind::Label;
    } else if(llvm::isa<clang::ConceptDecl>(decl)) {
        return SymbolKind::Concept;
    } else {
        return SymbolKind::Invalid;
    }
}

SymbolKind SymbolKind::from(const clang::tok::TokenKind kind) {
    return {};
}

}  // namespace clice
