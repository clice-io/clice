module clice;

import :semantic.template_resolver;
import :semantic.semantics;
import :unittests.utils.tester;

namespace clice::testing {

namespace {

struct InputFinder : clang::RecursiveASTVisitor<InputFinder> {
    CompilationUnit& unit;
    clang::QualType input;

    using Base = clang::RecursiveASTVisitor<InputFinder>;

    InputFinder(CompilationUnit& unit) : unit(unit) {}

    bool TraverseDecl(clang::Decl* decl) {
        if(decl && (llvm::isa<clang::TranslationUnitDecl>(decl) ||
                    unit.file_id(decl->getLocation()) == unit.focus_file().file())) {
            Base::TraverseDecl(decl);
        }

        return true;
    }

    bool VisitTypedefNameDecl(const clang::TypedefNameDecl* decl) {
        if(decl->getName() == "input") {
            input = decl->getUnderlyingType();
        }

        return true;
    }
};

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

    struct Scanner : clang::RecursiveASTVisitor<Scanner> {
        types::TemplateResolver& resolver;
        unsigned visited = 0;

        Scanner(types::TemplateResolver& resolver) : resolver(resolver) {}

        bool VisitTypedefNameDecl(clang::TypedefNameDecl* decl) {
            auto type = decl->getUnderlyingType();
            if(!type.isNull() && type->isDependentType()) {
                resolver.resolve(type);
                visited += 1;
            }
            return true;
        }

        bool VisitCXXDependentScopeMemberExpr(clang::CXXDependentScopeMemberExpr* expr) {
            resolver.lookup(expr);
            visited += 1;
            return true;
        }

        bool VisitDependentScopeDeclRefExpr(clang::DependentScopeDeclRefExpr* expr) {
            resolver.lookup(expr);
            visited += 1;
            return true;
        }
    } scanner(unit->get<types::TemplateResolver>());

    scanner.TraverseAST(unit->context());
    ZEXPECT(scanner.visited > 0);
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

    struct Scanner : clang::RecursiveASTVisitor<Scanner> {
        types::TemplateResolver& resolver;
        unsigned visited = 0;

        Scanner(types::TemplateResolver& resolver) : resolver(resolver) {}

        bool VisitTypedefNameDecl(clang::TypedefNameDecl* decl) {
            auto type = decl->getUnderlyingType();
            if(!type.isNull() && type->isDependentType()) {
                resolver.resolve(type);
                visited += 1;
            }
            return true;
        }

        bool VisitCXXDependentScopeMemberExpr(clang::CXXDependentScopeMemberExpr* expr) {
            resolver.lookup(expr);
            visited += 1;
            return true;
        }

        bool VisitDependentScopeDeclRefExpr(clang::DependentScopeDeclRefExpr* expr) {
            resolver.lookup(expr);
            visited += 1;
            return true;
        }
    } scanner(unit->get<types::TemplateResolver>());

    scanner.TraverseAST(unit->context());
    ZEXPECT(scanner.visited > 0);
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

    struct VariableFinder : clang::RecursiveASTVisitor<VariableFinder> {
        const clang::VarDecl* first = nullptr;

        bool VisitVarDecl(const clang::VarDecl* decl) {
            if(decl->getName() == "first") {
                first = decl;
            }
            return true;
        }
    } variables;

    variables.TraverseAST(unit->context());
    ZASSERT(variables.first);

    InputFinder finder(*unit);
    finder.TraverseAST(unit->context());
    ZASSERT(!finder.input.isNull());

    auto& resolver = unit->get<types::TemplateResolver>();
    auto deduced = resolver.deduce(variables.first);
    ZASSERT(!deduced.isNull());
    ZEXPECT(deduced.getCanonicalType() == unit->context().IntTy);

    auto input = resolver.resolve(finder.input);
    ZASSERT(!input.isNull());
    ZEXPECT(input->isDependentType());
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

    struct Finder : clang::RecursiveASTVisitor<Finder> {
        const clang::CallExpr* call = nullptr;
        const clang::VarDecl* value = nullptr;

        bool VisitCallExpr(const clang::CallExpr* expr) {
            if(llvm::isa<clang::CXXDependentScopeMemberExpr>(
                   expr->getCallee()->IgnoreParenImpCasts())) {
                call = expr;
            }
            return true;
        }

        bool VisitVarDecl(const clang::VarDecl* decl) {
            if(decl->getName() == "value") {
                value = decl;
            }
            return true;
        }
    } finder;

    finder.TraverseAST(unit->context());
    ZASSERT(finder.call);
    ZASSERT(finder.value);

    auto& resolver = unit->get<types::TemplateResolver>();
    ZEXPECT(resolver.callees(finder.call).size() == 1);
    auto deduced = resolver.deduce(finder.value);
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

    struct Finder : clang::RecursiveASTVisitor<Finder> {
        const clang::CallExpr* call = nullptr;
        const clang::CXXDependentScopeMemberExpr* member = nullptr;

        bool VisitCallExpr(const clang::CallExpr* expr) {
            if(llvm::isa<clang::UnresolvedLookupExpr>(expr->getCallee()->IgnoreParenImpCasts())) {
                call = expr;
            }
            return true;
        }

        bool VisitCXXDependentScopeMemberExpr(const clang::CXXDependentScopeMemberExpr* expr) {
            member = expr;
            return true;
        }
    } finder;

    finder.TraverseAST(unit->context());
    ZASSERT(finder.call);
    ZASSERT(finder.member);

    auto& resolver = unit->get<types::TemplateResolver>();
    resolver.callees(finder.call);
    ZEXPECT(resolver.lookup(finder.member).empty());
}

};  // ZEST_SUITE(TemplateResolver)

}  // namespace

}  // namespace clice::testing
