module clice;

import :semantic.decls;
import :unittests.utils.tester;

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
    add_focus("main.cpp", R"cpp(
template <typename T> struct A { };
template <typename T> struct A<T*> { };

A<int*>* undeclared_partial_use;
A<double>* undeclared_primary_use;
A<char*> instantiated_partial;
)cpp");
    ZASSERT(compile());

    auto* undeclared = specialization("A", "int *");
    auto* instantiated = specialization("A", "char *");
    auto* primary_use = specialization("A", "double");
    ZASSERT((undeclared && instantiated && primary_use));
    ZASSERT(undeclared->getSpecializationKind() == clang::TSK_Undeclared);
    ZASSERT(instantiated->getSpecializationKind() == clang::TSK_ImplicitInstantiation);

    /// The undeclared specialization anchors to the same pattern real
    /// instantiation selects: the partial specialization.
    ZEXPECT(decls::normalize(undeclared) == decls::normalize(instantiated));
    ZEXPECT(llvm::isa<clang::ClassTemplatePartialSpecializationDecl>(decls::normalize(undeclared)));

    /// Arguments matching no partial fall back to the primary pattern.
    ZEXPECT(decls::normalize(primary_use) ==
            class_template("A")->getTemplatedDecl()->getCanonicalDecl());
};

ZEST_CASE(UndeclaredDetectionPartial) {
    add_focus("main.cpp", R"cpp(
template <typename...> using void_t = void;
template <typename T, typename = void> struct has_x { };
template <typename T> struct has_x<T, void_t<typename T::x>> { };

struct With { using x = int; };
struct Without { };

has_x<With>* detected_use;
has_x<Without>* missing_use;
)cpp");
    ZASSERT(compile());

    auto of = [&](llvm::StringRef name) -> clang::ClassTemplateSpecializationDecl* {
        for(auto* spec: class_template("has_x")->specializations()) {
            auto* record = spec->getTemplateArgs()[0].getAsType()->getAsCXXRecordDecl();
            if(record && record->getName() == name) {
                return spec;
            }
        }
        return nullptr;
    };
    auto* detected = of("With");
    auto* missing = of("Without");
    ZASSERT((detected && missing));
    ZASSERT(missing->getSpecializationKind() == clang::TSK_Undeclared);

    /// Substituting `Without` into the partial's pattern fails: real
    /// instantiation uses the primary, and so does the identity.
    ZEXPECT(llvm::isa<clang::ClassTemplatePartialSpecializationDecl>(decls::normalize(detected)));
    ZEXPECT(decls::normalize(missing) ==
            class_template("has_x")->getTemplatedDecl()->getCanonicalDecl());
};

ZEST_CASE(UndeclaredDetectionThroughUsing) {
    add_focus("main.cpp", R"cpp(
template <typename...> using void_t = void;
template <typename T, typename = void> struct has_x { };
template <typename T> struct has_x<T, void_t<typename T::x>> { };

struct Base {
protected:
    using x = int;
};
struct Exposed : Base {
    using Base::x;
};

has_x<Exposed>* exposed_use;
)cpp");
    ZASSERT(compile());

    clang::ClassTemplateSpecializationDecl* exposed = nullptr;
    for(auto* spec: class_template("has_x")->specializations()) {
        exposed = spec;
    }
    ZASSERT(exposed);
    ZASSERT(exposed->getSpecializationKind() == clang::TSK_Undeclared);

    /// The public using-declaration makes the protected alias usable from
    /// outside: real instantiation takes the partial, and so does the
    /// identity.
    ZEXPECT(llvm::isa<clang::ClassTemplatePartialSpecializationDecl>(decls::normalize(exposed)));
};

ZEST_CASE(UndeclaredDetectionHiddenByUsing) {
    add_focus("main.cpp", R"cpp(
template <typename...> using void_t = void;
template <typename T, typename = void> struct has_x { };
template <typename T> struct has_x<T, void_t<typename T::x>> { };

struct Base {
    using x = int;
};
struct Hidden : Base {
private:
    using Base::x;
};

has_x<Hidden>* hidden_use;
)cpp");
    ZASSERT(compile());

    clang::ClassTemplateSpecializationDecl* hidden = nullptr;
    for(auto* spec: class_template("has_x")->specializations()) {
        hidden = spec;
    }
    ZASSERT(hidden);
    ZASSERT(hidden->getSpecializationKind() == clang::TSK_Undeclared);

    /// The private using-declaration hides Base's public alias: real
    /// instantiation uses the primary, and so does the identity.
    ZEXPECT(decls::normalize(hidden) ==
            class_template("has_x")->getTemplatedDecl()->getCanonicalDecl());
};

ZEST_CASE(AmbiguousPartialsPrimary) {
    add_focus("main.cpp", R"cpp(
template <typename T, typename U> struct B { };
template <typename T> struct B<T, int> { };
template <typename U> struct B<int, U> { };

B<int, int>* ambiguous_use;
)cpp");
    ZASSERT(compile());

    auto* ambiguous = specialization("B", "int");
    ZASSERT(ambiguous);
    ZASSERT(ambiguous->getSpecializationKind() == clang::TSK_Undeclared);

    /// Neither partial dominates; degrade to the primary rather than
    /// picking arbitrarily.
    ZEXPECT(decls::normalize(ambiguous) ==
            class_template("B")->getTemplatedDecl()->getCanonicalDecl());
};

ZEST_CASE(MemberSpecIdentity) {
    add_focus("main.cpp", R"cpp(
template <typename T> struct Outer {
    struct Inner { };
};
template <> struct Outer<char>::Inner {
    int special;
};

Outer<char>::Inner use_special;
Outer<int>::Inner use_generic;
)cpp");
    ZASSERT(compile());

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
    ZASSERT((specialized && generic));
    ZASSERT(specialized->getTemplateSpecializationKind() == clang::TSK_ExplicitSpecialization);

    /// The explicitly specialized member keeps its own identity instead of
    /// folding into the generic pattern.
    ZEXPECT(decls::normalize(specialized) != decls::normalize(generic));
    ZEXPECT(decls::normalize(specialized) == specialized->getCanonicalDecl());

    /// The implicit member still anchors to the pattern in the primary.
    ZEXPECT(decls::normalize(generic) ==
            generic->getInstantiatedFromMemberClass()->getCanonicalDecl());
};

};  // ZEST_SUITE(decls)

}  // namespace

}  // namespace clice::testing
