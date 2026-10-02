#include "test/tester.h"
#include "semantic/decls.h"

#include "clang/AST/DeclTemplate.h"

namespace clice::testing {

namespace {

ZEST_SUITE(decls, Tester) {
    auto class_template(llvm::StringRef name) -> clang::ClassTemplateDecl* {
        for(auto* decl: unit->context().getTranslationUnitDecl()->decls()) {
            if(auto* ctd = llvm::dyn_cast<clang::ClassTemplateDecl>(decl)) {
                if(ctd->getName() == name) {
                    return ctd;
                }
            }
        }
        return nullptr;
    }

    /// The specialization of `name` whose first template argument prints as
    /// `arg`, e.g. specialization("A", "int *").
    auto specialization(llvm::StringRef name, llvm::StringRef arg)
        -> clang::ClassTemplateSpecializationDecl* {
        auto* ctd = class_template(name);
        if(!ctd) {
            return nullptr;
        }
        for(auto* spec: ctd->specializations()) {
            if(spec->getTemplateArgs()[0].getAsType().getAsString() == arg) {
                return spec;
            }
        }
        return nullptr;
    }

    ZEST_CASE(UndeclaredPartialMatch) {
        add_main("main.cpp", R"cpp(
template <typename T> struct A { };
template <typename T> struct A<T*> { };

A<int*>* undeclared_partial_use;
A<double>* undeclared_primary_use;
A<char*> instantiated_partial;
)cpp");
        ASSERT(compile());

        auto* undeclared = specialization("A", "int *");
        auto* instantiated = specialization("A", "char *");
        auto* primary_use = specialization("A", "double");
        ASSERT((undeclared && instantiated && primary_use));
        ASSERT(undeclared->getSpecializationKind() == clang::TSK_Undeclared);
        ASSERT(instantiated->getSpecializationKind() == clang::TSK_ImplicitInstantiation);

        /// The undeclared specialization anchors to the same pattern real
        /// instantiation selects: the partial specialization.
        EXPECT(decls::normalize(undeclared) == decls::normalize(instantiated));
        EXPECT(
            llvm::isa<clang::ClassTemplatePartialSpecializationDecl>(decls::normalize(undeclared)));

        /// Arguments matching no partial fall back to the primary pattern.
        EXPECT(decls::normalize(primary_use) ==
               class_template("A")->getTemplatedDecl()->getCanonicalDecl());
    };

    ZEST_CASE(AmbiguousPartialsPrimary) {
        add_main("main.cpp", R"cpp(
template <typename T, typename U> struct B { };
template <typename T> struct B<T, int> { };
template <typename U> struct B<int, U> { };

B<int, int>* ambiguous_use;
)cpp");
        ASSERT(compile());

        auto* ambiguous = specialization("B", "int");
        ASSERT(ambiguous);
        ASSERT(ambiguous->getSpecializationKind() == clang::TSK_Undeclared);

        /// Neither partial dominates; degrade to the primary rather than
        /// picking arbitrarily.
        EXPECT(decls::normalize(ambiguous) ==
               class_template("B")->getTemplatedDecl()->getCanonicalDecl());
    };

    ZEST_CASE(MemberSpecIdentity) {
        add_main("main.cpp", R"cpp(
template <typename T> struct Outer {
    struct Inner { };
};
template <> struct Outer<char>::Inner {
    int special;
};

Outer<char>::Inner use_special;
Outer<int>::Inner use_generic;
)cpp");
        ASSERT(compile());

        auto member = [&](llvm::StringRef arg) -> clang::CXXRecordDecl* {
            auto* spec = specialization("Outer", arg);
            if(!spec) {
                return nullptr;
            }
            for(auto* decl: spec->decls()) {
                if(auto* record = llvm::dyn_cast<clang::CXXRecordDecl>(decl)) {
                    if(record->getName() == "Inner") {
                        return record;
                    }
                }
            }
            return nullptr;
        };

        auto* specialized = member("char");
        auto* generic = member("int");
        ASSERT((specialized && generic));
        ASSERT(specialized->getTemplateSpecializationKind() == clang::TSK_ExplicitSpecialization);

        /// The explicitly specialized member keeps its own identity instead of
        /// folding into the generic pattern.
        EXPECT(decls::normalize(specialized) != decls::normalize(generic));
        EXPECT(decls::normalize(specialized) == specialized->getCanonicalDecl());

        /// The implicit member still anchors to the pattern in the primary.
        EXPECT(decls::normalize(generic) ==
               generic->getInstantiatedFromMemberClass()->getCanonicalDecl());
    };

};  // ZEST_SUITE(decls)

}  // namespace

}  // namespace clice::testing
