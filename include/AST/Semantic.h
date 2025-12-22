#pragma once

#include "FilterASTVisitor.h"
#include "RelationKind.h"
#include "Resolver.h"
#include "SymbolKind.h"
#include "Utility.h"

namespace clice {

template <typename Derived>
class SemanticVisitor : public FilteredASTVisitor<SemanticVisitor<Derived>> {
public:
    using Base = FilteredASTVisitor<SemanticVisitor>;

    SemanticVisitor(CompilationUnit& unit, bool interested_only) :
        Base(unit, interested_only), unit(unit), resolver(unit.resolver()) {}

public:
    Derived& getDerived() {
        return static_cast<Derived&>(*this);
    }

    /// Invoked when a declaration occur is seen in source code.
    /// @param decl The decl corresponding to the symbol.
    /// @param kind The kind of the occurrence, such as declaration, definition, reference.
    /// @param location The location of the occurrence. Note that declaration name must be one
    /// token, so just one source location is enough.
    void handleDeclOccurrence(const clang::NamedDecl* decl,
                              RelationKind kind,
                              clang::SourceLocation location) {
        if(!decl || location.isInvalid()) {
            return;
        }

        assert(kind.is_one_of(RelationKind::Declaration,
                              RelationKind::Definition,
                              RelationKind::Reference,
                              RelationKind::WeakReference) &&
               "Invalid kind");

        /// Forwards to the derived class. Check whether the derived class has
        /// its own implementation to avoid infinite recursion.
        if constexpr(!std::same_as<decltype(&SemanticVisitor::handleDeclOccurrence),
                                   decltype(&Derived::handleDeclOccurrence)>) {
            getDerived().handleDeclOccurrence(decl, kind, location);
        }
    }

    /// Invoked when a macro occurrence is seen in source code.
    void handleMacroOccurrence(const clang::MacroInfo* def,
                               RelationKind kind,
                               clang::SourceLocation location) {
        assert(def && "Invalid macro");
        assert(kind.is_one_of(RelationKind::Definition, RelationKind::Reference) && "Invalid kind");
        assert(location.isValid() && "Invalid location");

        if constexpr(!std::same_as<decltype(&SemanticVisitor::handleMacroOccurrence),
                                   decltype(&Derived::handleMacroOccurrence)>) {
            getDerived().handleMacroOccurrence(def, kind, location);
        }
    }

    /// Invoked when a module occurrence is seen in source code.
    /// @param keyword The location of the `module` or `import` keyword.
    /// @param identifiers Tokens that make up the module name.
    void handleModuleOccurrence(clang::SourceLocation keyword,
                                llvm::ArrayRef<clang::syntax::Token> identifiers) {
        assert(keyword.isValid() && keyword.isFileID() && "Invalid keyword location");

        /// FIXME: Check whether identifiers are valid.

        if constexpr(!std::same_as<decltype(&SemanticVisitor::handleModuleOccurrence),
                                   decltype(&Derived::handleModuleOccurrence)>) {
            getDerived().handleModuleOccurrence(keyword, identifiers);
        }
    }

    void handleAttrOccurrence(const clang::Attr* attr, clang::SourceRange range) {
        if(!attr || range.isInvalid()) {
            return;
        }

        if constexpr(!std::same_as<decltype(&SemanticVisitor::handleAttrOccurrence),
                                   decltype(&Derived::handleAttrOccurrence)>) {
            getDerived().handleAttrOccurrence(attr, range);
        }
    }

    /// Invoked when a relation between two decls is seen in source code.
    /// @param decl The source decl.
    /// @param kind The kind of the relation.
    /// @param target The target decl, may same as the source decl.
    /// @param range The source range of the relation, may be invalid.
    void handleRelation(const clang::NamedDecl* decl,
                        RelationKind kind,
                        const clang::NamedDecl* target,
                        clang::SourceRange range) {
        if(!decl || !target || range.isInvalid()) {
            return;
        }

        if constexpr(!std::same_as<decltype(&SemanticVisitor::handleRelation),
                                   decltype(&Derived::handleRelation)>) {
            getDerived().handleRelation(decl, kind, target, range);
        }
    }

    void run() {
        if(Base::interested_only) {
            for(auto decl: unit.top_level_decls()) {
                Base::TraverseDecl(decl);
            }
        } else {
            Base::TraverseAST(unit.context());
        }

        for(auto directive: unit.directives()) {
            for(auto macro: directive.second.macros) {
                switch(macro.kind) {
                    case MacroRef::Kind::Def: {
                        handleMacroOccurrence(macro.macro, RelationKind::Definition, macro.loc);
                        break;
                    }

                    case MacroRef::Kind::Ref:
                    case MacroRef::Kind::Undef: {
                        handleMacroOccurrence(macro.macro, RelationKind::Reference, macro.loc);
                        break;
                    }
                }
            }
        }

        // if(auto module = unit.context().getCurrentNamedModule()) {
        //     auto keyword = module->DefinitionLoc;
        //     auto begin = TB.spelledTokenContaining(keyword);
        //     // assert(begin->kind() == clang::tok::identifier && begin->text(SM) == "module" &&
        //     //        "Invalid module declaration");
        //
        //    begin += 1;
        //    auto end = TB.spelledTokens(unit.file_id(keyword)).end();
        //
        //    for(auto iter = begin; iter != end; ++iter) {
        //        if(iter->kind() == clang::tok::identifier) {
        //            if(auto next = iter + 1; next != end && (next->kind() == clang::tok::period ||
        //                                                     next->kind() == clang::tok::colon)) {
        //                iter += 1;
        //                continue;
        //            }
        //
        //            end = iter + 1;
        //            break;
        //        }
        //
        //        std::unreachable();
        //    }
        //
        //    handleModuleOccurrence(keyword, llvm::ArrayRef<clang::syntax::Token>(begin, end));
        //}
    }

public:
    /// ============================================================================
    ///                                Declaration
    /// ============================================================================

#define VISIT_DECL(type) bool Visit##type(const clang::type* decl)
#define VISIT_STMT(type) bool Visit##type(const clang::type* stmt)
#define VISIT_EXPR(type) bool Visit##type(const clang::type* expr)
#define VISIT_TYPE(type) bool Visit##type(const clang::type* type)
#define VISIT_TYPELOC(type) bool Visit##type(clang::type loc)

    VISIT_DECL(ImportDecl) {
        /// FIXME:
        // auto tokens = TB.expandedTokens(decl->getSourceRange());
        //
        // assert(tokens.size() >= 2 && tokens[0].kind() == clang::tok::identifier &&
        //       tokens[0].text(SM) == "import" && "Invalid import declaration");
        // assert([&]() {
        //    auto range = tokens.drop_front(1);
        //    for(auto iter = range.begin(); iter != range.end(); ++iter) {
        //        if(iter->kind() == clang::tok::identifier) {
        //            if(auto next = iter + 1;
        //               next != range.end() && (next->kind() == clang::tok::coloncolon ||
        //                                       next->kind() == clang::tok::period)) {
        //                continue;
        //            }
        //            break;
        //        } else {
        //            return false;
        //        }
        //    }
        //    return true;
        //}() && "Invalid import declaration");
        //
        // handleModuleOccurrence(tokens[0].location(), tokens.drop_front(1));

        return true;
    }

    /// namespace Foo { }
    ///            ^~~~ definition
    VISIT_DECL(NamespaceDecl) {
        handleDeclOccurrence(decl, RelationKind::Definition, decl->getLocation());
        handleRelation(decl, RelationKind::Definition, decl, decl->getLocation());
        return true;
    }

    /// namespace Foo = Bar
    ///            ^     ^~~~ reference
    ///            ^~~~ definition
    VISIT_DECL(NamespaceAliasDecl) {
        handleDeclOccurrence(decl, RelationKind::Definition, decl->getLocation());
        handleRelation(decl, RelationKind::Definition, decl, decl->getLocation());
        handleDeclOccurrence(decl->getNamespace(),
                             RelationKind::Reference,
                             decl->getTargetNameLoc());
        handleRelation(decl->getNamespace(),
                       RelationKind::Reference,
                       decl->getNamespace(),
                       decl->getTargetNameLoc());
        return true;
    }

    /// using namespace Foo
    ///                  ^~~~~~~ reference
    VISIT_DECL(UsingDirectiveDecl) {
        handleDeclOccurrence(decl->getNominatedNamespace(),
                             RelationKind::Reference,
                             decl->getLocation());
        handleRelation(decl,
                       RelationKind::Reference,
                       decl->getNominatedNamespace(),
                       decl->getLocation());
        return true;
    }

    /// label:
    ///   ^~~~ definition
    VISIT_DECL(LabelDecl) {
        handleDeclOccurrence(decl, RelationKind::Definition, decl->getLocation());
        handleRelation(decl, RelationKind::Definition, decl, decl->getLocation());
        return true;
    }

    /// struct X { int foo; };
    ///                 ^~~~ definition
    VISIT_DECL(FieldDecl) {
        handleDeclOccurrence(decl, RelationKind::Definition, decl->getLocation());
        handleRelation(decl, RelationKind::Definition, decl, decl->getLocation());

        if(auto target = ast::decl_of(decl->getType())) {
            handleRelation(decl, RelationKind::TypeDefinition, target, decl->getLocation());
        }

        return true;
    }

    /// enum Foo { bar };
    ///             ^~~~ definition
    VISIT_DECL(EnumConstantDecl) {
        handleDeclOccurrence(decl, RelationKind::Definition, decl->getLocation());
        handleRelation(decl, RelationKind::Definition, decl, decl->getLocation());
        handleRelation(decl,
                       RelationKind::TypeDefinition,
                       llvm::cast<clang::NamedDecl>(decl->getDeclContext()),
                       decl->getLocation());
        return true;
    }

    /// using Foo::bar;
    ///             ^~~~ reference
    VISIT_DECL(UsingDecl) {
        for(auto shadow: decl->shadows()) {
            handleDeclOccurrence(shadow, RelationKind::WeakReference, decl->getLocation());
            handleRelation(decl, RelationKind::WeakReference, decl, decl->getLocation());
        }
        return true;
    }

    /// auto [a, b] = std::make_tuple(1, 2);
    ///       ^~~~ definition
    VISIT_DECL(BindingDecl) {
        handleDeclOccurrence(decl, RelationKind::Definition, decl->getLocation());
        handleRelation(decl, RelationKind::Definition, decl, decl->getLocation());

        if(auto target = ast::decl_of(decl->getType())) {
            handleRelation(decl, RelationKind::TypeDefinition, target, decl->getLocation());
        }

        return true;
    }

    /// template <typename T>
    ///                    ^~~~ definition
    VISIT_DECL(TemplateTypeParmDecl) {
        handleDeclOccurrence(decl, RelationKind::Definition, decl->getLocation());
        handleRelation(decl, RelationKind::Definition, decl, decl->getLocation());
        return true;
    }

    /// template <template <typename> class T>
    ///                                     ^~~~ definition
    VISIT_DECL(TemplateTemplateParmDecl) {
        handleDeclOccurrence(decl, RelationKind::Definition, decl->getLocation());
        handleRelation(decl, RelationKind::Definition, decl, decl->getLocation());
        return true;
    }

    /// template <int N>
    ///               ^~~~ definition
    VISIT_DECL(NonTypeTemplateParmDecl) {
        handleDeclOccurrence(decl, RelationKind::Definition, decl->getLocation());
        handleRelation(decl, RelationKind::Definition, decl, decl->getLocation());

        if(auto target = ast::decl_of(decl->getType())) {
            handleRelation(decl, RelationKind::TypeDefinition, target, decl->getLocation());
        }

        return true;
    }

    /// struct/class/union/enum Foo { ... };
    ///                          ^~~~ declaration/definition
    VISIT_DECL(TagDecl) {
        if(auto CTSD = llvm::dyn_cast<clang::ClassTemplateSpecializationDecl>(decl)) {
            switch(CTSD->getSpecializationKind()) {
                case clang::TSK_Undeclared:
                case clang::TSK_ImplicitInstantiation: {
                    std::unreachable();
                }

                case clang::TSK_ExplicitSpecialization: {
                    break;
                }

                case clang::TSK_ExplicitInstantiationDeclaration:
                case clang::TSK_ExplicitInstantiationDefinition: {
                    auto decl = ast::instantiated_from(CTSD);
                    handleDeclOccurrence(decl, RelationKind::Reference, CTSD->getLocation());
                    handleRelation(decl, RelationKind::Reference, decl, CTSD->getLocation());
                    return true;
                }
            }
        }

        RelationKind kind = decl->isThisDeclarationADefinition() ? RelationKind::Definition
                                                                 : RelationKind::Declaration;
        handleDeclOccurrence(decl, kind, decl->getLocation());
        handleRelation(decl, kind, decl, decl->getLocation());

        if(auto CRD = llvm::dyn_cast<clang::CXXRecordDecl>(decl)) {
            if(auto def = CRD->getDefinition()) {
                for(auto base: CRD->bases()) {
                    /// FIXME: Handle dependent base class.
                    if(auto target = ast::decl_of(base.getType())) {
                        handleRelation(def, RelationKind::Base, target, base.getSourceRange());
                        handleRelation(target, RelationKind::Derived, def, base.getSourceRange());
                    }
                }
            }
        }

        return true;
    }

    bool TraverseFunctionDecl(clang::FunctionDecl* decl) {
        if(!decl) {
            return true;
        }

        decls.emplace_back(decl);
        auto result = Base::TraverseFunctionDecl(decl);
        decls.pop_back();
        return result;
    }

    /// void foo() { ... }
    ///       ^~~~ declaration/definition
    VISIT_DECL(FunctionDecl) {
        switch(decl->getTemplateSpecializationKind()) {
            case clang::TSK_ImplicitInstantiation: {
                std::unreachable();
            }

            /// FIXME: Clang currently doesn't record source location of explicit
            /// instantiation of function template correctly. Skip it temporarily.
            case clang::TSK_ExplicitInstantiationDeclaration:
            case clang::TSK_ExplicitInstantiationDefinition: {
                return true;
            }

            case clang::TSK_Undeclared:
            case clang::TSK_ExplicitSpecialization: {
                break;
            }
        }

        RelationKind kind = decl->isThisDeclarationADefinition() ? RelationKind::Definition
                                                                 : RelationKind::Declaration;
        handleDeclOccurrence(decl, kind, decl->getLocation());
        handleRelation(decl, kind, decl, decl->getLocation());

        /// FIXME: Handle `CXXConversionDecl` and `CXXDeductionGuide`.

        if(auto method = llvm::dyn_cast<clang::CXXMethodDecl>(decl)) {
            for(auto override: method->overridden_methods()) {
                handleRelation(method, RelationKind::Interface, override, decl->getLocation());
                handleRelation(override, RelationKind::Implementation, method, decl->getLocation());
            }

            if(auto ctor = llvm::dyn_cast<clang::CXXConstructorDecl>(method)) {
                handleRelation(ctor,
                               RelationKind::TypeDefinition,
                               ctor->getParent(),
                               decl->getLocation());
                handleRelation(ctor->getParent(),
                               RelationKind::Constructor,
                               ctor,
                               decl->getLocation());
            }

            if(auto dtor = llvm::dyn_cast<clang::CXXDestructorDecl>(method)) {
                handleRelation(dtor,
                               RelationKind::TypeDefinition,
                               dtor->getParent(),
                               decl->getLocation());
                handleRelation(dtor->getParent(),
                               RelationKind::Destructor,
                               dtor,
                               decl->getLocation());
            }
        }

        return true;
    }

    /// using Foo = int;
    ///            ^~~~ definition
    VISIT_DECL(TypedefNameDecl) {
        handleDeclOccurrence(decl, RelationKind::Definition, decl->getLocation());
        handleRelation(decl, RelationKind::Definition, decl, decl->getLocation());

        if(auto target = ast::decl_of(decl->getUnderlyingType())) {
            handleRelation(decl, RelationKind::TypeDefinition, target, decl->getLocation());
        }

        return true;
    }

    /// int foo = 2;
    ///      ^~~~ declaration/definition
    VISIT_DECL(VarDecl) {
        if(auto TD = llvm::dyn_cast<clang::VarTemplateSpecializationDecl>(decl)) {
            switch(TD->getSpecializationKind()) {
                /// FIXME: Implicit instantiation should not occur in the lexical context. But
                /// clang currently wrongly adds it to the lexical context. Skip it here.
                case clang::TSK_ImplicitInstantiation:

                /// FIXME: Clang currently doesn't record source location of explicit
                /// instantiation of variable template correctly. Skip it temporarily.
                case clang::TSK_ExplicitInstantiationDeclaration:
                case clang::TSK_ExplicitInstantiationDefinition: {
                    return true;
                }

                case clang::TSK_Undeclared:
                case clang::TSK_ExplicitSpecialization: {
                    break;
                }
            }
        }

        RelationKind kind = decl->isThisDeclarationADefinition() ? RelationKind::Definition
                                                                 : RelationKind::Declaration;
        handleDeclOccurrence(decl, kind, decl->getLocation());
        handleRelation(decl, kind, decl, decl->getLocation());

        if(auto target = ast::decl_of(decl->getType())) {
            handleRelation(decl, RelationKind::TypeDefinition, target, decl->getLocation());
        }

        return true;
    }

    /// TODO: Traverse explicit and implicit instantiations to add occurrence
    /// to replacement template arguments.

    VISIT_DECL(ClassTemplateDecl) {
        return true;
    }

    VISIT_DECL(FunctionTemplateDecl) {
        return true;
    }

    VISIT_DECL(VarTemplateDecl) {
        return true;
    }

    /// template <typename T> concept Foo = ...;
    ///                                ^~~~ definition
    VISIT_DECL(ConceptDecl) {
        handleDeclOccurrence(decl, RelationKind::Definition, decl->getLocation());
        handleRelation(decl, RelationKind::Definition, decl, decl->getLocation());
        return true;
    }

    /// requires Foo<T>;
    ///            ^~~~ reference
    bool VisitConceptReference(clang::ConceptReference* reference) {
        auto decl = reference->getNamedConcept();
        auto location = reference->getConceptNameLoc();
        handleDeclOccurrence(decl, RelationKind::Reference, location);
        handleRelation(decl, RelationKind::Reference, decl, location);
        return true;
    }

    /// ============================================================================
    ///                                  TypeLoc
    /// ============================================================================

    /// unsigned int foo = 2;
    ///   ^~~~~~~~^~~~~~~~ reference
    VISIT_TYPELOC(BuiltinTypeLoc) {
        /// Should we handle builtin type separately? Note that it may
        /// be composed of multiple tokens.
        return true;
    }

    /// struct Foo foo;
    ///         ^~~~ reference
    VISIT_TYPELOC(TagTypeLoc) {
        auto decl = loc.getDecl();
        auto location = loc.getNameLoc();
        handleDeclOccurrence(decl, RelationKind::Reference, location);
        handleRelation(decl, RelationKind::Reference, decl, location);
        return true;
    }

    /// using Foo = int; Foo foo;
    ///                   ^~~~ reference
    VISIT_TYPELOC(TypedefTypeLoc) {
        auto decl = loc.getTypedefNameDecl();
        auto location = loc.getNameLoc();
        handleDeclOccurrence(decl, RelationKind::Reference, location);
        handleRelation(decl, RelationKind::Reference, decl, location);
        return true;
    }

    /// template <typename T> void foo(T t)
    ///                                ^~~~ reference
    VISIT_TYPELOC(TemplateTypeParmTypeLoc) {
        auto decl = loc.getDecl();
        auto location = loc.getNameLoc();
        handleDeclOccurrence(decl, RelationKind::Reference, location);
        handleRelation(decl, RelationKind::Reference, decl, location);
        return true;
    }

    /// std::vector<int>
    ///        ^~~~ reference
    VISIT_TYPELOC(TemplateSpecializationTypeLoc) {
        if(auto type = loc.getTypePtr(); type->isDependentType()) {
            /// FIXME: for dependent type, we always use the template resolver to
            /// resolve the template decl.
            return true;
        }

        auto decl = ast::decl_of(loc.getType());
        auto location = loc.getTemplateNameLoc();
        handleDeclOccurrence(decl, RelationKind::Reference, location);
        handleRelation(decl, RelationKind::Reference, decl, location);
        return true;
    }

    /// std::vector<T>::value_type
    ///                      ^~~~ reference
    VISIT_TYPELOC(DependentNameTypeLoc) {
        auto location = loc.getNameLoc();
        // for(auto decl: resolver.lookup(loc.getTypePtr())) {
        //     handleDeclOccurrence(decl, RelationKind::WeakReference, location);
        //     handleRelation(decl, RelationKind::WeakReference, decl, location);
        // }
        return true;
    }

    /// std::allocator<T>::rebind<U>
    ///                       ^~~~ reference
    VISIT_TYPELOC(DependentTemplateSpecializationTypeLoc) {
        auto location = loc.getTemplateNameLoc();
        // for(auto decl: resolver.lookup(loc.getTypePtr())) {
        //     handleDeclOccurrence(decl, RelationKind::WeakReference, location);
        //     handleRelation(decl, RelationKind::WeakReference, decl, location);
        // }
        return true;
    }

    /// ============================================================================
    ///                                Specifier
    /// ============================================================================

    bool VisitNestedNameSpecifierLoc(clang::NestedNameSpecifierLoc loc) {
        auto NNS = loc.getNestedNameSpecifier();
        switch(NNS->getKind()) {
            case clang::NestedNameSpecifier::Namespace: {
                auto decl = NNS->getAsNamespace();
                auto location = loc.getLocalBeginLoc();
                handleDeclOccurrence(decl, RelationKind::Reference, location);
                handleRelation(decl, RelationKind::Reference, decl, location);
                break;
            }

            case clang::NestedNameSpecifier::NamespaceAlias: {
                auto decl = NNS->getAsNamespaceAlias();
                auto location = loc.getLocalBeginLoc();
                handleDeclOccurrence(decl, RelationKind::Reference, location);
                handleRelation(decl, RelationKind::Reference, decl, location);
                break;
            }

            case clang::NestedNameSpecifier::Identifier: {
                assert(NNS->isDependent() && "Identifier NNS should be dependent");
                // FIXME: use TemplateResolver here.
                break;
            }

            case clang::NestedNameSpecifier::TypeSpec:
            case clang::NestedNameSpecifier::Global:
            case clang::NestedNameSpecifier::Super: {
                break;
            };
        }

        return true;
    }

    bool VisitAttr(clang::Attr* attr) {
        getDerived().handleAttrOccurrence(attr, attr->getRange());
        return true;
    }

    /// ============================================================================
    ///                                 Statement
    /// ============================================================================

    /// foo = 1
    ///  ^~~~ reference
    VISIT_EXPR(DeclRefExpr) {
        auto decl = expr->getDecl();
        auto location = expr->getLocation();
        handleDeclOccurrence(decl, RelationKind::Reference, location);
        handleRelation(decl, RelationKind::Reference, decl, location);
        return true;
    }

    /// foo.bar
    ///      ^~~~ reference
    VISIT_EXPR(MemberExpr) {
        auto decl = expr->getMemberDecl();
        auto location = expr->getMemberLoc();
        if(location.isInvalid()) {
            /// FIXME: if the location of member loc is invalid, this represents it is a
            /// implicit member expr, e.g. `if(x)`, implicit `if(x.operator bool())`. Try to
            /// use parens around the member loc for occurrence.
            ///
            /// Known implicit member expr:
            ///  - implicit constructor
            ///  - implicit conversion operator

            assert((llvm::isa<clang::CXXConstructorDecl, clang::CXXConversionDecl>(decl)) &&
                   "Invalid member location");

            return true;
        }

        handleDeclOccurrence(decl, RelationKind::Reference, location);
        handleRelation(decl, RelationKind::Reference, decl, location);
        return true;
    }

    /// std::is_same<T, U>::value
    ///           ^~~~ reference
    VISIT_EXPR(UnresolvedLookupExpr) {
        return true;
    }

    /// std::is_same<T, U>::value
    ///                       ^~~~ reference
    VISIT_EXPR(DependentScopeDeclRefExpr) {
        return true;
    }

    /// foo.T::value
    ///          ^~~~ reference
    VISIT_EXPR(CXXDependentScopeMemberExpr) {
        return true;
    }

    VISIT_EXPR(CallExpr) {
        /// FIXME: Is the function decls is empty, it means that
        /// the callee is a global variable decl or static assert decl.
        /// Should we handle it separately?
        if(decls.empty()) {
            return true;
        }

        auto back = decls.back();
        const clang::NamedDecl* caller = llvm::dyn_cast<clang::FunctionDecl>(back);
        const clang::NamedDecl* callee = nullptr;

        if(auto decl = expr->getCalleeDecl()) {
            if(auto ND = llvm::dyn_cast<clang::NamedDecl>(decl)) {
                callee = ND;
            }
        }

        if(callee && caller) {
            handleRelation(caller, RelationKind::Callee, callee, expr->getSourceRange());
            handleRelation(callee, RelationKind::Caller, caller, expr->getSourceRange());
        }

        return true;
    }

protected:
    CompilationUnit& unit;
    TemplateResolver& resolver;
    llvm::SmallVector<clang::Decl*> decls;
};

}  // namespace clice
