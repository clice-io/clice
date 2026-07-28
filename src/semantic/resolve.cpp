#include "semantic/resolve.h"

#include "semantic/ast_utility.h"

#include "clang/AST/Decl.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/DeclTemplate.h"
#include "clang/AST/Expr.h"
#include "clang/AST/ExprCXX.h"
#include "clang/AST/ExprConcepts.h"
#include "clang/AST/TypeLoc.h"

namespace clice {

namespace {

/// The per-node-kind extraction below is ported verbatim from the former
/// SemanticVisitor: the same decls, the same roles, the same locations —
/// including its asymmetries (e.g. using-decl occurrences name the shadow
/// while the mirrored relation is keyed by the using-decl itself), which the
/// serialized index format pins.
struct FactSink {
    OccurrenceCallback occurrence;
    RelationCallback relation;

    void occur(const clang::NamedDecl* decl, RelationKind kind, clang::SourceLocation location) {
        if(decl && location.isValid()) {
            occurrence(decl, kind, location);
        }
    }

    void relate(const clang::NamedDecl* decl,
                RelationKind kind,
                const clang::NamedDecl* target,
                clang::SourceRange range) {
        if(decl && target && range.isValid()) {
            relation(decl, kind, target, range);
        }
    }
};

void resolve_decl(const clang::Decl* D, FactSink& sink) {
    /// namespace Foo = Bar
    ///            ^     ^~~~ reference
    ///            ^~~~ definition
    if(auto* NAD = llvm::dyn_cast<clang::NamespaceAliasDecl>(D)) {
        sink.occur(NAD, RelationKind::Definition, NAD->getLocation());
        sink.relate(NAD, RelationKind::Definition, NAD, NAD->getLocation());
        sink.occur(NAD->getNamespace(), RelationKind::Reference, NAD->getTargetNameLoc());
        sink.relate(NAD->getNamespace(),
                    RelationKind::Reference,
                    NAD->getNamespace(),
                    NAD->getTargetNameLoc());
        return;
    }

    /// namespace Foo { }
    ///            ^~~~ definition
    if(auto* ND = llvm::dyn_cast<clang::NamespaceDecl>(D)) {
        sink.occur(ND, RelationKind::Definition, ND->getLocation());
        sink.relate(ND, RelationKind::Definition, ND, ND->getLocation());
        return;
    }

    /// using namespace Foo
    ///                  ^~~~~~~ reference
    if(auto* UDD = llvm::dyn_cast<clang::UsingDirectiveDecl>(D)) {
        sink.occur(UDD->getNominatedNamespace(), RelationKind::Reference, UDD->getLocation());
        sink.relate(UDD, RelationKind::Reference, UDD->getNominatedNamespace(), UDD->getLocation());
        return;
    }

    /// label:
    ///   ^~~~ definition
    if(auto* LD = llvm::dyn_cast<clang::LabelDecl>(D)) {
        sink.occur(LD, RelationKind::Definition, LD->getLocation());
        sink.relate(LD, RelationKind::Definition, LD, LD->getLocation());
        return;
    }

    /// struct X { int foo; };
    ///                 ^~~~ definition
    if(auto* FD = llvm::dyn_cast<clang::FieldDecl>(D)) {
        sink.occur(FD, RelationKind::Definition, FD->getLocation());
        sink.relate(FD, RelationKind::Definition, FD, FD->getLocation());

        if(auto target = ast::decl_of(FD->getType())) {
            sink.relate(FD, RelationKind::TypeDefinition, target, FD->getLocation());
        }
        return;
    }

    /// enum Foo { bar };
    ///             ^~~~ definition
    if(auto* ECD = llvm::dyn_cast<clang::EnumConstantDecl>(D)) {
        sink.occur(ECD, RelationKind::Definition, ECD->getLocation());
        sink.relate(ECD, RelationKind::Definition, ECD, ECD->getLocation());
        sink.relate(ECD,
                    RelationKind::TypeDefinition,
                    llvm::cast<clang::NamedDecl>(ECD->getDeclContext()),
                    ECD->getLocation());
        return;
    }

    /// using Foo::bar;
    ///             ^~~~ reference
    if(auto* UD = llvm::dyn_cast<clang::UsingDecl>(D)) {
        for(auto shadow: UD->shadows()) {
            sink.occur(shadow, RelationKind::WeakReference, UD->getLocation());
            sink.relate(UD, RelationKind::WeakReference, UD, UD->getLocation());
        }
        return;
    }

    /// auto [a, b] = std::make_tuple(1, 2);
    ///       ^~~~ definition
    if(auto* BD = llvm::dyn_cast<clang::BindingDecl>(D)) {
        sink.occur(BD, RelationKind::Definition, BD->getLocation());
        sink.relate(BD, RelationKind::Definition, BD, BD->getLocation());

        if(auto target = ast::decl_of(BD->getType())) {
            sink.relate(BD, RelationKind::TypeDefinition, target, BD->getLocation());
        }
        return;
    }

    /// template <int N>
    ///               ^~~~ definition
    if(auto* NTTP = llvm::dyn_cast<clang::NonTypeTemplateParmDecl>(D)) {
        sink.occur(NTTP, RelationKind::Definition, NTTP->getLocation());
        sink.relate(NTTP, RelationKind::Definition, NTTP, NTTP->getLocation());

        if(auto target = ast::decl_of(NTTP->getType())) {
            sink.relate(NTTP, RelationKind::TypeDefinition, target, NTTP->getLocation());
        }
        return;
    }

    /// template <typename T> / template <template <typename> class T>
    ///                    ^~~~ definition
    if(llvm::isa<clang::TemplateTypeParmDecl, clang::TemplateTemplateParmDecl>(D)) {
        auto* ND = llvm::cast<clang::NamedDecl>(D);
        sink.occur(ND, RelationKind::Definition, ND->getLocation());
        sink.relate(ND, RelationKind::Definition, ND, ND->getLocation());
        return;
    }

    /// template <typename T> concept Foo = ...;
    ///                                ^~~~ definition
    if(auto* CD = llvm::dyn_cast<clang::ConceptDecl>(D)) {
        sink.occur(CD, RelationKind::Definition, CD->getLocation());
        sink.relate(CD, RelationKind::Definition, CD, CD->getLocation());
        return;
    }

    /// The template decl itself produces no facts; the templated decl
    /// inside it does.
    if(llvm::isa<clang::ClassTemplateDecl, clang::FunctionTemplateDecl, clang::VarTemplateDecl>(
           D)) {
        return;
    }

    /// struct/class/union/enum Foo { ... };
    ///                          ^~~~ declaration/definition
    if(auto* TD = llvm::dyn_cast<clang::TagDecl>(D)) {
        if(auto* CTSD = llvm::dyn_cast<clang::ClassTemplateSpecializationDecl>(TD)) {
            switch(CTSD->getSpecializationKind()) {
                /// Unlike the old filtered traversal, the semantic map may
                /// record implicit instantiations; they produce no written
                /// facts.
                case clang::TSK_Undeclared:
                case clang::TSK_ImplicitInstantiation: {
                    return;
                }

                case clang::TSK_ExplicitSpecialization: {
                    break;
                }

                case clang::TSK_ExplicitInstantiationDeclaration:
                case clang::TSK_ExplicitInstantiationDefinition: {
                    auto decl = ast::instantiated_from(CTSD);
                    sink.occur(decl, RelationKind::Reference, CTSD->getLocation());
                    sink.relate(decl, RelationKind::Reference, decl, CTSD->getLocation());
                    return;
                }
            }
        }

        RelationKind kind = TD->isThisDeclarationADefinition() ? RelationKind::Definition
                                                               : RelationKind::Declaration;
        sink.occur(TD, kind, TD->getLocation());
        sink.relate(TD, kind, TD, TD->getLocation());

        if(auto* CRD = llvm::dyn_cast<clang::CXXRecordDecl>(TD)) {
            if(auto* def = CRD->getDefinition()) {
                for(auto& base: CRD->bases()) {
                    /// FIXME: Handle dependent base class.
                    if(auto target = ast::decl_of(base.getType())) {
                        sink.relate(def, RelationKind::Base, target, base.getSourceRange());
                        sink.relate(target, RelationKind::Derived, def, base.getSourceRange());
                    }
                }
            }
        }
        return;
    }

    /// void foo() { ... }
    ///       ^~~~ declaration/definition
    if(auto* FD = llvm::dyn_cast<clang::FunctionDecl>(D)) {
        switch(FD->getTemplateSpecializationKind()) {
            case clang::TSK_ImplicitInstantiation: {
                return;
            }

            /// FIXME: Clang currently doesn't record source location of explicit
            /// instantiation of function template correctly. Skip it temporarily.
            case clang::TSK_ExplicitInstantiationDeclaration:
            case clang::TSK_ExplicitInstantiationDefinition: {
                return;
            }

            case clang::TSK_Undeclared:
            case clang::TSK_ExplicitSpecialization: {
                break;
            }
        }

        RelationKind kind = FD->isThisDeclarationADefinition() ? RelationKind::Definition
                                                               : RelationKind::Declaration;
        sink.occur(FD, kind, FD->getLocation());
        sink.relate(FD, kind, FD, FD->getLocation());

        /// FIXME: Handle `CXXConversionDecl` and `CXXDeductionGuide`.

        if(auto* method = llvm::dyn_cast<clang::CXXMethodDecl>(FD)) {
            for(auto* override: method->overridden_methods()) {
                sink.relate(method, RelationKind::Interface, override, FD->getLocation());
                sink.relate(override, RelationKind::Implementation, method, FD->getLocation());
            }

            if(auto* ctor = llvm::dyn_cast<clang::CXXConstructorDecl>(method)) {
                sink.relate(ctor,
                            RelationKind::TypeDefinition,
                            ctor->getParent(),
                            FD->getLocation());
                sink.relate(ctor->getParent(), RelationKind::Constructor, ctor, FD->getLocation());
            }

            if(auto* dtor = llvm::dyn_cast<clang::CXXDestructorDecl>(method)) {
                sink.relate(dtor,
                            RelationKind::TypeDefinition,
                            dtor->getParent(),
                            FD->getLocation());
                sink.relate(dtor->getParent(), RelationKind::Destructor, dtor, FD->getLocation());
            }
        }
        return;
    }

    /// using Foo = int;
    ///        ^~~~ definition
    if(auto* TND = llvm::dyn_cast<clang::TypedefNameDecl>(D)) {
        sink.occur(TND, RelationKind::Definition, TND->getLocation());
        sink.relate(TND, RelationKind::Definition, TND, TND->getLocation());

        if(auto target = ast::decl_of(TND->getUnderlyingType())) {
            sink.relate(TND, RelationKind::TypeDefinition, target, TND->getLocation());
        }
        return;
    }

    /// int foo = 2;
    ///      ^~~~ declaration/definition
    if(auto* VD = llvm::dyn_cast<clang::VarDecl>(D)) {
        if(auto* VTSD = llvm::dyn_cast<clang::VarTemplateSpecializationDecl>(VD)) {
            switch(VTSD->getSpecializationKind()) {
                case clang::TSK_ImplicitInstantiation:
                /// FIXME: Clang currently doesn't record source location of explicit
                /// instantiation of variable template correctly. Skip it temporarily.
                case clang::TSK_ExplicitInstantiationDeclaration:
                case clang::TSK_ExplicitInstantiationDefinition: {
                    return;
                }

                case clang::TSK_Undeclared:
                case clang::TSK_ExplicitSpecialization: {
                    break;
                }
            }
        }

        RelationKind kind = VD->isThisDeclarationADefinition() ? RelationKind::Definition
                                                               : RelationKind::Declaration;
        sink.occur(VD, kind, VD->getLocation());
        sink.relate(VD, kind, VD, VD->getLocation());

        if(auto target = ast::decl_of(VD->getType())) {
            sink.relate(VD, RelationKind::TypeDefinition, target, VD->getLocation());
        }
        return;
    }
}

void resolve_type_loc(clang::TypeLoc TL, FactSink& sink) {
    /// struct Foo foo;
    ///         ^~~~ reference
    if(auto TTL = TL.getAs<clang::TagTypeLoc>()) {
        sink.occur(TTL.getDecl(), RelationKind::Reference, TTL.getNameLoc());
        sink.relate(TTL.getDecl(), RelationKind::Reference, TTL.getDecl(), TTL.getNameLoc());
        return;
    }

    /// using Foo = int; Foo foo;
    ///                   ^~~~ reference
    if(auto TTL = TL.getAs<clang::TypedefTypeLoc>()) {
        sink.occur(TTL.getTypedefNameDecl(), RelationKind::Reference, TTL.getNameLoc());
        sink.relate(TTL.getTypedefNameDecl(),
                    RelationKind::Reference,
                    TTL.getTypedefNameDecl(),
                    TTL.getNameLoc());
        return;
    }

    /// template <typename T> void foo(T t)
    ///                                ^~~~ reference
    if(auto TTPL = TL.getAs<clang::TemplateTypeParmTypeLoc>()) {
        sink.occur(TTPL.getDecl(), RelationKind::Reference, TTPL.getNameLoc());
        sink.relate(TTPL.getDecl(), RelationKind::Reference, TTPL.getDecl(), TTPL.getNameLoc());
        return;
    }

    /// std::vector<int>
    ///        ^~~~ reference
    if(auto TSTL = TL.getAs<clang::TemplateSpecializationTypeLoc>()) {
        if(TSTL.getTypePtr()->isDependentType()) {
            /// FIXME: for dependent type, use the template resolver to
            /// resolve the template decl.
            return;
        }

        auto decl = ast::decl_of(TSTL.getType());
        sink.occur(decl, RelationKind::Reference, TSTL.getTemplateNameLoc());
        sink.relate(decl, RelationKind::Reference, decl, TSTL.getTemplateNameLoc());
        return;
    }

    /// std::vector<T>::value_type / std::allocator<T>::rebind<U>
    /// FIXME: dependent names await the template resolver.
}

void resolve_nns_loc(clang::NestedNameSpecifierLoc NNSL, FactSink& sink) {
    auto* NNS = NNSL.getNestedNameSpecifier();
    switch(NNS->getKind()) {
        case clang::NestedNameSpecifier::Namespace: {
            auto* decl = NNS->getAsNamespace();
            sink.occur(decl, RelationKind::Reference, NNSL.getLocalBeginLoc());
            sink.relate(decl, RelationKind::Reference, decl, NNSL.getLocalBeginLoc());
            break;
        }

        case clang::NestedNameSpecifier::NamespaceAlias: {
            auto* decl = NNS->getAsNamespaceAlias();
            sink.occur(decl, RelationKind::Reference, NNSL.getLocalBeginLoc());
            sink.relate(decl, RelationKind::Reference, decl, NNSL.getLocalBeginLoc());
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
}

void resolve_stmt(const clang::Stmt* S, FactSink& sink, const clang::NamedDecl* enclosing) {
    /// foo = 1
    ///  ^~~~ reference
    if(auto* DRE = llvm::dyn_cast<clang::DeclRefExpr>(S)) {
        sink.occur(DRE->getDecl(), RelationKind::Reference, DRE->getLocation());
        sink.relate(DRE->getDecl(), RelationKind::Reference, DRE->getDecl(), DRE->getLocation());
        return;
    }

    /// foo.bar
    ///      ^~~~ reference
    if(auto* ME = llvm::dyn_cast<clang::MemberExpr>(S)) {
        auto location = ME->getMemberLoc();
        if(location.isInvalid()) {
            /// An invalid member location means an implicit member expr, e.g.
            /// the implicit `operator bool` call in `if(x)`.
            return;
        }

        sink.occur(ME->getMemberDecl(), RelationKind::Reference, location);
        sink.relate(ME->getMemberDecl(), RelationKind::Reference, ME->getMemberDecl(), location);
        return;
    }

    if(auto* CE = llvm::dyn_cast<clang::CallExpr>(S)) {
        const clang::NamedDecl* caller = llvm::dyn_cast_if_present<clang::FunctionDecl>(enclosing);
        const clang::NamedDecl* callee = nullptr;

        if(auto* decl = CE->getCalleeDecl()) {
            callee = llvm::dyn_cast<clang::NamedDecl>(decl);
        }

        if(callee && caller) {
            sink.relate(caller, RelationKind::Callee, callee, CE->getSourceRange());
            sink.relate(callee, RelationKind::Caller, caller, CE->getSourceRange());
        }
        return;
    }

    /// std::is_same<T, U>::value etc.
    /// FIXME: dependent expressions await the template resolver.
}

}  // namespace

void resolve_facts(const SemanticNode& node,
                   OccurrenceCallback occurrence,
                   RelationCallback relation,
                   const clang::NamedDecl* enclosing_function) {
    FactSink sink{occurrence, relation};

    switch(node.kind()) {
        case SemanticNode::Kind::Decl: {
            resolve_decl(node.get<clang::Decl>(), sink);
            break;
        }

        case SemanticNode::Kind::Stmt: {
            resolve_stmt(node.get<clang::Stmt>(), sink, enclosing_function);
            break;
        }

        case SemanticNode::Kind::TypeLoc: {
            resolve_type_loc(*node.get<clang::TypeLoc>(), sink);
            break;
        }

        case SemanticNode::Kind::NestedNameSpecifierLoc: {
            resolve_nns_loc(*node.get<clang::NestedNameSpecifierLoc>(), sink);
            break;
        }

        case SemanticNode::Kind::ConceptReference: {
            /// requires Foo<T>;
            ///            ^~~~ reference
            auto* reference = node.get<clang::ConceptReference>();
            sink.occur(reference->getNamedConcept(),
                       RelationKind::Reference,
                       reference->getConceptNameLoc());
            sink.relate(reference->getNamedConcept(),
                        RelationKind::Reference,
                        reference->getNamedConcept(),
                        reference->getConceptNameLoc());
            break;
        }

        default: {
            break;
        }
    }
}

void resolve_occurrences(const SemanticNode& node, OccurrenceCallback callback) {
    resolve_facts(
        node,
        callback,
        [](const clang::NamedDecl*, RelationKind, const clang::NamedDecl*, clang::SourceRange) {});
}

}  // namespace clice
