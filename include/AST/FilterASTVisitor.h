#pragma once

#include "AST/SourceCode.h"
#include "Compiler/CompilationUnit.h"

#include "clang/AST/RecursiveASTVisitor.h"

namespace clice {

/// A visitor class that extends clang::RecursiveASTVisitor to traverse
/// AST nodes with an additional filtering mechanism.
template <typename Derived>
class FilteredASTVisitor : public clang::RecursiveASTVisitor<Derived> {
public:
    using Base = clang::RecursiveASTVisitor<Derived>;

    FilteredASTVisitor(CompilationUnit& unit, bool interested_only) :
        unit(unit), interested_only(interested_only) {}

#define CHECK_DERIVED_IMPL(func)                                                                   \
    static_assert(std::same_as<decltype(&FilteredASTVisitor::func), decltype(&Derived::func)>,     \
                  "Derived class should not implement this method");

    Derived& getDerived() {
        return static_cast<Derived&>(*this);
    }

    bool TraverseDecl(clang::Decl* decl) {
        CHECK_DERIVED_IMPL(TraverseDecl);

        if(!decl) {
            return true;
        }

        if(llvm::isa<clang::TranslationUnitDecl>(decl)) {
            if(interested_only) {
                for(auto top: unit.top_level_decls()) {
                    if(!TraverseDecl(top)) {
                        return false;
                    }
                }
                return true;
            }

            return Base::TraverseDecl(decl);
        }

        if(decl->isImplicit()) {
            return true;
        }

        /// We don't want to visit implicit instantiation.
        if(auto SD = llvm::dyn_cast<clang::ClassTemplateSpecializationDecl>(decl)) {
            if(SD->getSpecializationKind() == clang::TSK_ImplicitInstantiation) {
                return true;
            }
        }

        if(auto SD = llvm::dyn_cast<clang::FunctionDecl>(decl)) {
            if(SD->getTemplateSpecializationKind() == clang::TSK_ImplicitInstantiation) {
                return true;
            }
        }

        if(auto SD = llvm::dyn_cast<clang::VarTemplateSpecializationDecl>(decl)) {
            if(SD->getSpecializationKind() == clang::TSK_ImplicitInstantiation) {
                return true;
            }
        }

        if constexpr(requires { getDerived().on_traverse_decl(decl, &Base::TraverseDecl); }) {
            return getDerived().on_traverse_decl(decl, &Base::TraverseDecl);
        } else {
            return Base::TraverseDecl(decl);
        }
    }

    bool TraverseStmt(clang::Stmt* stmt) {
        CHECK_DERIVED_IMPL(TraverseStmt);

        if(!stmt) {
            return true;
        }

        return Base::TraverseStmt(stmt);
    }

    /// FIXME: See https://github.com/llvm/llvm-project/issues/117687.
    bool TraverseAttributedStmt(clang::AttributedStmt* stmt) {
        CHECK_DERIVED_IMPL(TraverseAttributedStmt);

        if(!stmt) {
            return true;
        }

        for(auto attr: stmt->getAttrs()) {
            Base::TraverseAttr(const_cast<clang::Attr*>(attr));
        }

        return Base::TraverseAttributedStmt(stmt);
    }

    /// We don't want to node without location information.
    constexpr bool TraverseType [[gnu::always_inline]] (clang::QualType) {
        CHECK_DERIVED_IMPL(TraverseType);
        return true;
    }

    bool TraverseTypeLoc(clang::TypeLoc loc) {
        CHECK_DERIVED_IMPL(TraverseTypeLoc);

        if(!loc) {
            return true;
        }

        /// FIXME: Workaround for `QualifiedTypeLoc`.
        if(auto QL = loc.getAs<clang::QualifiedTypeLoc>()) {
            return Base::TraverseTypeLoc(QL.getUnqualifiedLoc());
        }

        return Base::TraverseTypeLoc(loc);
    }

    bool TraverseAttr(clang::Attr* attr) {
        CHECK_DERIVED_IMPL(TraverseAttr);

        if(!attr) {
            return true;
        }

        return Base::TraverseAttr(attr);
    }

    /// We don't want to node withou location information.
    constexpr bool TraverseNestedNameSpecifier
        [[gnu::always_inline]] (clang::NestedNameSpecifier*) {
        CHECK_DERIVED_IMPL(TraverseNestedNameSpecifier);
        return true;
    }

    /// Note that `RecursiveASTVisitor` doesn't have `VisitNestedNameSpecifier`,
    /// it is our own implementation.
    bool VisitNestedNameSpecifierLoc(clang::NestedNameSpecifierLoc loc) {
        return true;
    }

    bool TraverseNestedNameSpecifierLoc(clang::NestedNameSpecifierLoc loc) {
        CHECK_DERIVED_IMPL(TraverseNestedNameSpecifierLoc);

        if(!loc) {
            return true;
        }

        if(!getDerived().VisitNestedNameSpecifierLoc(loc)) {
            return false;
        }

        return Base::TraverseNestedNameSpecifierLoc(loc);
    }

    /// Note that `RecursiveASTVisitor` doesn't have `VisitNestedNameSpecifier`,
    /// it is our own implementation.
    bool VisitConceptReference(clang::ConceptReference* reference) {
        return true;
    }

    bool TraverseConceptReference(clang::ConceptReference* reference) {
        CHECK_DERIVED_IMPL(TraverseConceptReference);

        if(!reference) {
            return true;
        }

        if(!getDerived().VisitConceptReference(reference)) {
            return false;
        }

        return Base::TraverseConceptReference(reference);
    }

#undef CHECK_DERIVED_IMPL

protected:
    CompilationUnit& unit;
    bool interested_only;
};

}  // namespace clice
