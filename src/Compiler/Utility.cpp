#include "Compiler/Utility.h"

#include "clang/Basic/SourceManager.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/DeclObjC.h"
#include "clang/AST/ASTContext.h"

namespace clice {

template <class T>
bool is_template_specialization_kind(const clang::NamedDecl* decl,
                                     clang::TemplateSpecializationKind Kind) {
    if(const auto* TD = dyn_cast<T>(decl))
        return TD->getTemplateSpecializationKind() == Kind;
    return false;
}

inline bool is_template_specialization_kind(const clang::NamedDecl* decl,
                                            clang::TemplateSpecializationKind Kind) {
    return is_template_specialization_kind<clang::FunctionDecl>(decl, Kind) ||
           is_template_specialization_kind<clang::CXXRecordDecl>(decl, Kind) ||
           is_template_specialization_kind<clang::VarDecl>(decl, Kind);
}

inline bool is_implicit_template_instantiation(const clang::NamedDecl* decl) {
    return is_template_specialization_kind(decl, clang::TSK_ImplicitInstantiation);
}

bool is_inside_main_file(clang::SourceLocation loc, const clang::SourceManager& sm) {
    if(!loc.isValid())
        return false;
    clang::FileID fid = sm.getFileID(sm.getExpansionLoc(loc));
    return fid == sm.getMainFileID() || fid == sm.getPreambleFileID();
};

bool is_clangd_top_level_decl(const clang::Decl* decl) {
    auto& sm = decl->getASTContext().getSourceManager();
    if(!is_inside_main_file(decl->getLocation(), sm))
        return false;
    if(const clang::NamedDecl* named_decl = dyn_cast<clang::NamedDecl>(decl))
        if(is_implicit_template_instantiation(named_decl))
            return false;

    // ObjCMethodDecl are not actually top-level decls.
    if(isa<clang::ObjCMethodDecl>(decl))
        return false;
    return true;
}

}  // namespace clice
