module clice;

import :semantic.template_resolver;
import :semantic.semantics;
import :unittests.utils.tester;

namespace clice::testing {

namespace {

/// The nodes of a unit the cases ask the resolver about, gathered in one
/// traversal. Every RecursiveASTVisitor a file instantiates adds seconds to
/// its compile: the file has this one.
struct Nodes : clang::RecursiveASTVisitor<Nodes> {
    llvm::SmallVector<const clang::NamedDecl*> decls;

    llvm::SmallVector<const clang::CallExpr*> calls;

    llvm::SmallVector<const clang::CXXDependentScopeMemberExpr*> members;

    llvm::SmallVector<const clang::DependentScopeDeclRefExpr*> qualified;

    explicit Nodes(clang::ASTContext& context) {
        TraverseAST(context);
    }

    bool VisitNamedDecl(const clang::NamedDecl* decl) {
        decls.push_back(decl);
        return true;
    }

    bool VisitCallExpr(const clang::CallExpr* expr) {
        calls.push_back(expr);
        return true;
    }

    bool VisitCXXDependentScopeMemberExpr(const clang::CXXDependentScopeMemberExpr* expr) {
        members.push_back(expr);
        return true;
    }

    bool VisitDependentScopeDeclRefExpr(const clang::DependentScopeDeclRefExpr* expr) {
        qualified.push_back(expr);
        return true;
    }

    /// The last declaration of kind `Decl` named `name`.
    template <typename Decl>
    const Decl* named(llvm::StringRef name) const {
        for(auto* decl: llvm::reverse(decls)) {
            auto* found = llvm::dyn_cast<Decl>(decl);
            if(found && found->getIdentifier() && found->getName() == name) {
                return found;
            }
        }
        return nullptr;
    }

    /// The last call whose callee is a `Callee`.
    template <typename Callee>
    const clang::CallExpr* call_through() const {
        for(auto* call: llvm::reverse(calls)) {
            if(llvm::isa<Callee>(call->getCallee()->IgnoreParenImpCasts())) {
                return call;
            }
        }
        return nullptr;
    }
};

/// Feed every dependent typedef and dependent name of the unit to its
/// resolver; how many there were.
unsigned scan(CompilationUnit& unit) {
    Nodes nodes(unit.context());
    auto& resolver = unit.get<types::TemplateResolver>();
    unsigned visited = 0;
    for(auto* decl: nodes.decls) {
        auto* alias = llvm::dyn_cast<clang::TypedefNameDecl>(decl);
        auto type = alias ? alias->getUnderlyingType() : clang::QualType();
        if(!type.isNull() && type->isDependentType()) {
            resolver.resolve(type);
            visited += 1;
        }
    }
    for(auto* member: nodes.members) {
        resolver.lookup(member);
        visited += 1;
    }
    for(auto* name: nodes.qualified) {
        resolver.lookup(name);
        visited += 1;
    }
    return visited;
}

ZEST_SUITE(TemplateResolver, Tester) {

ZEST_CASE(CallArityOccurrences) {
    /// The production occurrence path must apply the arity filter: the
    /// two-argument overload never appears for a one-argument call.
    add_focus("main.cpp", R"code(
        template <typename T>
        struct S {
            int foo(int);
            int foo(int, int);
            int foo(T);

            void call(T t) {
                foo(t);
            }
        };
    )code");
    ZASSERT(compile());

    auto semantics = Semantics::build(*unit);
    auto entries = semantics.node_entries();

    bool found = false;
    for(std::uint32_t i = 0; i < entries.size(); i += 1) {
        if(entries[i].node.get<clang::UnresolvedMemberExpr>()) {
            auto occurrences =
                resolve_occurrences(semantics, i, &unit->get<types::TemplateResolver>());
            ZEXPECT(occurrences.size() == 2);
            found = true;
        }
    }
    ZEXPECT(found);
}

ZEST_CASE(BrokenCodeScan) {
    /// Error-recovery ASTs are the resolver's everyday hostile input in an
    /// LSP; resolving anything in a broken TU must degrade, never crash.
    add_focus("main.cpp", R"code(
        template <typename T>
        struct A {
            using type = typename T::missing;
            using bad = typename unknown_ns::thing<T>::nested;
            using worse = typename A<typename T::gone>::type;
            T member;

            void f() {
                auto x = undeclared_function(member);
                member.no_such_member();
                member.get()[0][1].leaf;
            }
        };

        template <typename T>
        struct A<T*>;

        template <typename... Ts>
        struct B {
            using type = typename A<mistyped<Ts...>>::type;
            using other = typename B::unknown;
        };

        struct C : A<not_a_type> {
            using inherited = typename A<not_a_type>::type;
        };
    )code");
    prepare();
    ZASSERT(try_compile());

    ZEXPECT(scan(*unit) > 0);
}

ZEST_CASE(StandardScan) {
    /// Crash-safety scan: feed every dependent typedef and dependent
    /// expression in a libstdc++-heavy TU through the resolver. Results are
    /// irrelevant — under the assertion-enabled ASan Debug build, surviving
    /// the scan is the assertion.
    add_focus("main.cpp", R"code(
        #include <functional>
        #include <map>
        #include <memory>
        #include <vector>
    )code");
    ZASSERT(compile_driver());

    ZEXPECT(scan(*unit) > 0);
}

ZEST_CASE(ScopedBindingStaysLocal) {
    add_focus("main.cpp", R"code(
        template <typename T>
        struct Holder {
            template <typename U>
            struct Inner {
                using type = T;
            };

            template <typename U>
            void reset() {
                auto first = typename Inner<U>::type();
                using input = typename Inner<U>::type;
            }
        };

        template struct Holder<int>;
    )code");
    ZASSERT(compile());

    Nodes nodes(unit->context());
    auto* first = nodes.named<clang::VarDecl>("first");
    ZASSERT(first);
    auto* input = nodes.named<clang::TypedefNameDecl>("input");
    ZASSERT(input);

    auto& resolver = unit->get<types::TemplateResolver>();
    auto deduced = resolver.deduce(first);
    ZASSERT(!deduced.isNull());
    ZEXPECT(deduced.getCanonicalType() == unit->context().IntTy);

    auto resolved = resolver.resolve(input->getUnderlyingType());
    ZASSERT(!resolved.isNull());
    ZEXPECT(resolved->isDependentType());
}

ZEST_CASE(KeptCallBindings) {
    /// A later query takes a call's candidates as an earlier one found them,
    /// with the bindings of the lookup that found them: the type of the
    /// call reads `A` through them.
    add_focus("main.cpp", R"code(
        template <typename A, typename B>
        struct Pair {
            A first();
        };

        template <typename T>
        void use(Pair<int, T> pair) {
            auto value = pair.first();
        }
    )code");
    ZASSERT(compile());

    Nodes nodes(unit->context());
    auto* call = nodes.call_through<clang::CXXDependentScopeMemberExpr>();
    ZASSERT(call);
    auto* value = nodes.named<clang::VarDecl>("value");
    ZASSERT(value);

    auto& resolver = unit->get<types::TemplateResolver>();
    ZEXPECT(resolver.callees(call).size() == 1);
    auto deduced = resolver.deduce(value);
    ZASSERT(!deduced.isNull());
    ZEXPECT(deduced.getCanonicalType() == unit->context().IntTy);
}

ZEST_CASE(KeptOpenDefaults) {
    /// The first query opens `N = T::value` for the first `make()`, then
    /// again for the second, which a later query takes as kept: it opens
    /// the default too, so `Bar<0>` may apply and the two `x` disagree.
    add_focus("main.cpp", R"code(
        template <int N>
        struct Bar {
            int x;
        };

        template <>
        struct Bar<0> {
            double x;
        };

        template <typename T, int N = T::value>
        struct Foo {
            static Bar<N> make();
        };

        template <typename A, typename B>
        void g(A, B);

        template <typename T>
        void use() {
            g(Foo<T>::make(), Foo<T>::make().x);
        }
    )code");
    ZASSERT(compile());

    Nodes nodes(unit->context());
    auto* call = nodes.call_through<clang::UnresolvedLookupExpr>();
    ZASSERT(call);
    ZASSERT(!nodes.members.empty());

    auto& resolver = unit->get<types::TemplateResolver>();
    resolver.callees(call);
    ZEXPECT(resolver.lookup(nodes.members.back()).empty());
}

ZEST_CASE(KeptPerView) {
    /// main.cpp declares a second erase_if and a partial specialization of
    /// traits before it includes util.h, which on its own sees lib.h's
    /// declarations alone. What a query keeps holds only for the places that
    /// see alike: from util.h the call names one candidate and `kind` is
    /// `int`, from main.cpp two and `long`, whichever place asked first.
    add_file("lib.h", R"code(
        #pragma once
        namespace ns {
        template <class T> struct vec {};
        template <class T> int erase_if(vec<T>& c) { return 0; }
        template <class T> struct traits { using type = int; };
        }
    )code");
    add_file("util.h", R"code(
        #pragma once
        #include "lib.h"
        template <class C> int remove_bad(C& c) { return ns::erase_if(c); }
        template <class C> using kind = typename ns::traits<C*>::type;
    )code");
    add_focus("main.cpp", R"code(
        #include "lib.h"
        namespace ns {
        template <class T> struct list {};
        template <class T> int erase_if(list<T>& c) { return 1; }
        template <class T> struct traits<T*> { using type = long; };
        }
        #include "util.h"
    )code");
    ZASSERT(compile());

    Nodes nodes(unit->context());
    auto* call = nodes.call_through<clang::UnresolvedLookupExpr>();
    ZASSERT(call);
    auto* kind = nodes.named<clang::TypeAliasDecl>("kind");
    ZASSERT(kind);
    auto* list = nodes.named<clang::ClassTemplateDecl>("list");
    ZASSERT(list);

    auto& resolver = unit->get<types::TemplateResolver>();
    auto& context = unit->context();
    for(auto from: {list->getLocation(), call->getBeginLoc(), list->getLocation()}) {
        resolver.see_from(from);
        bool in_main = from == list->getLocation();
        ZEXPECT(resolver.callees(call).size() == (in_main ? 2 : 1));
        auto resolved = resolver.resolve(kind->getUnderlyingType());
        ZASSERT(!resolved.isNull());
        ZEXPECT(resolved.getCanonicalType() == (in_main ? context.LongTy : context.IntTy));
    }
}

};  // ZEST_SUITE(TemplateResolver)

}  // namespace

}  // namespace clice::testing
