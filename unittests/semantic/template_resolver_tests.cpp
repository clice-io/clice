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

ZEST_CASE(UsingDeclarationPerView) {
    /// util.h's using-declaration names both erase_if overloads the unit
    /// declared before it; util.h on its own sees lib.h's alone.
    add_file("lib.h", R"code(
        #pragma once
        namespace ns {
        template <class T> struct vec {};
        template <class T> int erase_if(vec<T>& c) { return 0; }
        }
    )code");
    add_file("util.h", R"code(
        #pragma once
        #include "lib.h"
        namespace app {
        using ns::erase_if;
        template <class C> int remove_bad(C& c) { return erase_if(c); }
        }
    )code");
    add_focus("main.cpp", R"code(
        #include "lib.h"
        namespace ns {
        template <class T> struct list {};
        template <class T> int erase_if(list<T>& c) { return 1; }
        }
        #include "util.h"
    )code");
    ZASSERT(compile());

    Nodes nodes(unit->context());
    auto* call = nodes.call_through<clang::UnresolvedLookupExpr>();
    ZASSERT(call);

    auto& resolver = unit->get<types::TemplateResolver>();
    resolver.see_from(call->getBeginLoc());
    ZEXPECT(resolver.callees(call).size() == 1);
}

ZEST_CASE(InjectedClassKeepsParameters) {
    /// Through its own name the class reads its parameters as written. The
    /// canonical form names one by depth and index alone, which `all`'s own
    /// parameter at the same place would be taken for, `wrap<Ts>` nested
    /// into itself.
    add_focus("main.cpp", R"code(
        template <class T> struct wrap {};
        template <class... Ts> struct all {};
        template <class F, class... R> struct all<F, R...> { using type = F; };
        template <class... Ts> struct holder {
            using first = typename all<wrap<Ts>...>::type;
        };
        template <class... Ts> struct user : holder<Ts...> {
            void f() { typename user::first x; }
        };
    )code");
    ZASSERT(compile());

    Nodes nodes(unit->context());
    auto* x = nodes.named<clang::VarDecl>("x");
    ZASSERT(x);

    auto resolved = unit->get<types::TemplateResolver>().resolve(x->getType());
    auto* wrap = resolved.isNull() ? nullptr : resolved->getAs<clang::TemplateSpecializationType>();
    ZASSERT(wrap);
    ZASSERT(wrap->template_arguments().size() == 1);
    auto argument = wrap->template_arguments()[0].getAsType();
    if(auto* expansion = argument->getAs<clang::PackExpansionType>()) {
        argument = expansion->getPattern();
    }
    auto* parameter = argument->getAs<clang::TemplateTypeParmType>();
    ZASSERT(parameter);
    ZEXPECT(parameter->getDecl() != nullptr);
}

ZEST_CASE(InjectedPartialKeepsParameters) {
    /// A partial specialization names its own parameters as written too.
    add_focus("main.cpp", R"code(
        template <class T> struct wrap {};
        template <class... Ts> struct all {};
        template <class F, class... R> struct all<F, R...> { using type = F; };
        template <class... Ts> struct holder {
            using first = typename all<wrap<Ts>...>::type;
        };
        template <class... Ts> struct user;
        template <class... Ts> struct user<int, Ts...> : holder<Ts...> {
            void f() { typename user::first x; }
        };
    )code");
    ZASSERT(compile());

    Nodes nodes(unit->context());
    auto* x = nodes.named<clang::VarDecl>("x");
    ZASSERT(x);

    auto resolved = unit->get<types::TemplateResolver>().resolve(x->getType());
    auto* wrap = resolved.isNull() ? nullptr : resolved->getAs<clang::TemplateSpecializationType>();
    ZASSERT(wrap);
    ZASSERT(wrap->template_arguments().size() == 1);
    auto argument = wrap->template_arguments()[0].getAsType();
    if(auto* expansion = argument->getAs<clang::PackExpansionType>()) {
        argument = expansion->getPattern();
    }
    auto* parameter = argument->getAs<clang::TemplateTypeParmType>();
    ZASSERT(parameter);
    ZEXPECT(parameter->getDecl() != nullptr);
}

ZEST_CASE(ViewsBetweenIncludes) {
    /// A header sees the partial specialization extra.h declares only past
    /// its include; one name asked from both places answers each alike.
    add_file("lib.h", R"code(
        #pragma once
        namespace ns {
        template <class T> struct traits { using type = int; };
        }
    )code");
    add_file("extra.h", R"code(
        #pragma once
        #include "lib.h"
        namespace ns {
        template <class T> struct traits<T*> { using type = long; };
        }
    )code");
    add_file("util.h", R"code(
        #pragma once
        #include "lib.h"
        template <class C> using kind = typename ns::traits<C*>::type;
        #include "extra.h"
        template <class C> struct after {};
    )code");
    add_focus("main.cpp", R"code(
        #include "util.h"
    )code");
    ZASSERT(compile());

    Nodes nodes(unit->context());
    auto* kind = nodes.named<clang::TypeAliasDecl>("kind");
    ZASSERT(kind);
    auto* after = nodes.named<clang::ClassTemplateDecl>("after");
    ZASSERT(after);

    auto& resolver = unit->get<types::TemplateResolver>();
    auto& context = unit->context();
    for(auto from: {kind->getLocation(), after->getLocation(), kind->getLocation()}) {
        resolver.see_from(from);
        auto resolved = resolver.resolve(kind->getUnderlyingType());
        ZASSERT(!resolved.isNull());
        bool past = from == after->getLocation();
        ZEXPECT(resolved.getCanonicalType() == (past ? context.LongTy : context.IntTy));
    }
}

ZEST_CASE(CanonicalNamesKeepTheirBindings) {
    /// `C::value_type` and `T::value_type` share their canonical form; the
    /// only instantiation binds `C` alone.
    add_focus("main.cpp", R"code(
        template <class X> struct box { using value_type = int; };
        template <class C> void sum(const C&) { typename C::value_type total{}; }
        template <class T> struct wrapper { typename T::value_type held; };
        void use() { sum(box<int>{}); }
    )code");
    ZASSERT(compile());

    Nodes nodes(unit->context());
    auto* total = nodes.named<clang::VarDecl>("total");
    ZASSERT(total);
    auto* held = nodes.named<clang::FieldDecl>("held");
    ZASSERT(held);
    auto& resolver = unit->get<types::TemplateResolver>();
    auto resolved = resolver.resolve(total->getType());
    ZASSERT(!resolved.isNull());
    ZEXPECT(resolved.getCanonicalType() == unit->context().IntTy);
    auto free = resolver.resolve(held->getType());
    ZASSERT(!free.isNull());
    ZEXPECT(free->getAs<clang::DependentNameType>() != nullptr);
}

ZEST_CASE(MemberValueUnderBindings) {
    /// A value argument written in the caller's parameters (`is_int<B>`)
    /// folds under the caller's bindings.
    add_focus("main.cpp", R"code(
        template <class T> struct is_int { static constexpr bool value = false; };
        template <> struct is_int<int> { static constexpr bool value = true; };
        template <bool B> struct flag { static constexpr bool value = B; };
        template <bool B> struct pick { using type = long; };
        template <> struct pick<true> { using type = int; };
        template <class A, class B> struct traits {
            static constexpr bool ok = flag<is_int<B>::value>::value;
        };
        template <class T> struct user {
            using t = typename pick<traits<T, int>::ok>::type;
        };
    )code");
    ZASSERT(compile());

    Nodes nodes(unit->context());
    auto* t = nodes.named<clang::TypedefNameDecl>("t");
    ZASSERT(t);
    auto resolved = unit->get<types::TemplateResolver>().resolve(t->getUnderlyingType());
    ZASSERT(!resolved.isNull());
    ZEXPECT(resolved.getCanonicalType() == unit->context().IntTy);
}

ZEST_CASE(DefaultUnderBindings) {
    /// The default of `put`'s second parameter reads the class's `A`, bound
    /// to `float` by the object: it cannot form, and `put` is no candidate.
    add_focus("main.cpp", R"code(
        template <class T> struct is_int { static constexpr bool value = false; };
        template <> struct is_int<int> { static constexpr bool value = true; };
        template <bool B, class R = void> struct enable_if {};
        template <class R> struct enable_if<true, R> { using type = R; };
        template <class A, class B> struct holder {
            template <class U, class = typename enable_if<is_int<A>::value>::type> int put(U);
            template <class U> long put(U, int = 0);
        };
        template <class T> void use(holder<float, T> h) { h.put(1); }
    )code");
    ZASSERT(compile());

    Nodes nodes(unit->context());
    auto* call = nodes.call_through<clang::CXXDependentScopeMemberExpr>();
    ZASSERT(call);
    ZEXPECT(unit->get<types::TemplateResolver>().callees(call).size() == 1);
}

ZEST_CASE(StandardConditionalBase) {
    /// A base chosen by `conditional` whose condition is not known (MSVC's
    /// variant) has a member both choices give alike.
    add_focus("main.cpp", R"code(
        namespace std {
        template <bool B, class T, class F> struct conditional { using type = T; };
        template <class T, class F> struct conditional<false, T, F> { using type = F; };
        template <class T> constexpr bool trivial = __is_trivially_destructible(T);
        template <class... Ts> struct base { void reset(); };
        template <class... Ts> struct layer : base<Ts...> {};
        template <class... Ts>
        struct variant : conditional<(trivial<Ts> && ...), base<Ts...>, layer<Ts...>>::type {
            void f() { this->reset(); }
        };
        }
    )code");
    ZASSERT(compile());

    Nodes nodes(unit->context());
    ZASSERT(!nodes.members.empty());
    auto& resolver = unit->get<types::TemplateResolver>();
    auto found = resolver.lookup(nodes.members.back());
    ZEXPECT(found.isSingleResult());
}

ZEST_CASE(StandardConditionalBasesNested) {
    /// Each layer is chosen by a condition not known, between the layer
    /// below and a class over it (MSVC's special member layers): looking
    /// through every choice at every layer would double the work per layer.
    add_focus("main.cpp", R"code(
        namespace std {
        template <bool B, class T, class F> struct conditional { using type = T; };
        template <class T, class F> struct conditional<false, T, F> { using type = F; };
        template <class T> constexpr bool trivial = __is_trivially_destructible(T);
        template <class... Ts> struct base { void reset(); };
        template <class B> struct over : B {};
        template <class B, class... Ts>
        using pick = typename conditional<(trivial<Ts> && ...), B, over<B>>::type;
        template <class B, class... Ts>
        using pick3 = pick<pick<pick<B, Ts...>, Ts...>, Ts...>;
        template <class... Ts>
        struct variant : pick3<pick3<pick3<pick3<base<Ts...>, Ts...>, Ts...>, Ts...>, Ts...> {
            void f() { this->reset(); }
        };
        }
    )code");
    ZASSERT(compile());

    Nodes nodes(unit->context());
    ZASSERT(!nodes.members.empty());
    types::TemplateResolver resolver(*unit, {.audit = true});
    resolver.lookup(nodes.members.back());
    ZASSERT(resolver.audits().size() == 1);
    auto& audit = resolver.audits().front();
    ZEXPECT(audit.troubles[static_cast<std::size_t>(types::Trouble::Steps)] == 0);
}

ZEST_CASE(StandardTypeRules) {
    /// The standard's traits answer by rule. One of a parameter that may be
    /// anything waits for it.
    add_focus("main.cpp", R"code(
        namespace std {
        template <class T> struct remove_reference { using type = T; };
        template <class T> struct decay { using type = T; };
        template <bool B, class T, class F> struct conditional { using type = T; };
        template <bool B, class T = void> struct enable_if {};
        template <class T> struct enable_if<true, T> { using type = T; };
        template <class T> struct type_identity { using type = T; };
        template <class T> struct add_lvalue_reference { using type = T&; };
        template <class T> struct remove_const { using type = T; };
        template <class T> struct remove_cvref { using type = T; };
        template <class T> struct remove_pointer { using type = T; };
        template <class T> struct add_pointer { using type = T*; };
        }
        template <class T> struct uses {
            using stripped = typename std::remove_reference<T&>::type;
            using decayed = typename std::decay<const T[3]>::type;
            using chosen = typename std::conditional<true, T, int>::type;
            using enabled = typename std::enable_if<sizeof(T) == 4, T*>::type;
            using waiting = typename std::remove_reference<T>::type;
            using identity = typename std::type_identity<T>::type;
            using void_ref = typename std::add_lvalue_reference<
                typename std::conditional<true, void, T>::type>::type;
            using unconst = typename std::remove_const<const T[3]>::type;
            using unwritten = typename std::remove_const<T[3]>::type;
            using bare = typename std::remove_cvref<const T&>::type;
            using pointee = typename std::remove_pointer<T*>::type;
            using pointer = typename std::add_pointer<T&>::type;
            using function = typename std::decay<void(T)>::type;
            using alike = typename std::conditional<sizeof(T) == 4, T, T>::type;
            using disabled = typename std::enable_if<false, T>::type;
        };
    )code");
    ZASSERT(compile());

    Nodes nodes(unit->context());
    auto* T = nodes.named<clang::TemplateTypeParmDecl>("T");
    ZASSERT(T);
    auto& context = unit->context();
    auto parameter = context.getTypeDeclType(T);
    auto& resolver = unit->get<types::TemplateResolver>();
    auto resolve = [&](llvm::StringRef name) {
        auto* alias = nodes.named<clang::TypedefNameDecl>(name);
        return alias ? resolver.resolve(alias->getUnderlyingType()) : clang::QualType();
    };
    auto same = [&](clang::QualType lhs, clang::QualType rhs) {
        return !lhs.isNull() && context.hasSameType(lhs, rhs);
    };
    ZEXPECT(same(resolve("stripped"), parameter));
    ZEXPECT(same(resolve("decayed"), context.getPointerType(parameter.withConst())));
    ZEXPECT(same(resolve("chosen"), parameter));
    ZEXPECT(same(resolve("enabled"), context.getPointerType(parameter)));
    auto waiting = resolve("waiting");
    ZASSERT(!waiting.isNull());
    ZEXPECT(waiting->getAs<clang::DependentNameType>() != nullptr);
    ZEXPECT(same(resolve("identity"), parameter));
    ZEXPECT(same(resolve("void_ref"), context.VoidTy));
    ZEXPECT(same(resolve("unconst"),
                 context.getConstantArrayType(parameter,
                                              llvm::APInt(64, 3),
                                              nullptr,
                                              clang::ArraySizeModifier::Normal,
                                              0)));
    auto unwritten = resolve("unwritten");
    ZASSERT(!unwritten.isNull());
    ZEXPECT(unwritten->getAs<clang::DependentNameType>() != nullptr);
    ZEXPECT(same(resolve("bare"), parameter));
    ZEXPECT(same(resolve("pointee"), parameter));
    ZEXPECT(same(resolve("pointer"), context.getPointerType(parameter)));
    auto function = resolve("function");
    ZASSERT(!function.isNull());
    ZEXPECT(function->isFunctionPointerType());
    ZEXPECT(same(resolve("alike"), parameter));
    /// Where `enable_if` holds no `type`, the library's definition says so.
    auto disabled = resolve("disabled");
    ZASSERT(!disabled.isNull());
    ZEXPECT(disabled->getAs<clang::DependentNameType>() != nullptr);
}

ZEST_CASE(StandardConditionalBaseDiffers) {
    /// Where the two bases a `conditional` may choose give different
    /// members, the member is neither.
    add_focus("main.cpp", R"code(
        namespace std {
        template <bool B, class T, class F> struct conditional { using type = T; };
        template <class T> constexpr bool trivial = __is_trivially_destructible(T);
        template <class... Ts> struct base { void reset(); };
        template <class... Ts> struct layer : base<Ts...> { void reset(); };
        template <class... Ts>
        struct variant : conditional<(trivial<Ts> && ...), base<Ts...>, layer<Ts...>>::type {
            void f() { this->reset(); }
        };
        }
    )code");
    ZASSERT(compile());

    Nodes nodes(unit->context());
    ZASSERT(!nodes.members.empty());
    auto& resolver = unit->get<types::TemplateResolver>();
    ZEXPECT(resolver.lookup(nodes.members.back()).empty());
}

ZEST_CASE(UnknownOperandOfAnd) {
    /// `false` decides `&&` on either side of an operand not known.
    add_focus("main.cpp", R"code(
        template <bool B, class T, class F> struct choose { using type = T; };
        template <class T, class F> struct choose<false, T, F> { using type = F; };
        template <class T> struct both {
            using left = typename choose<(sizeof(T) > 4) && false, int, char>::type;
            using right = typename choose<false && (sizeof(T) > 4), int, char>::type;
        };
    )code");
    ZASSERT(compile());

    Nodes nodes(unit->context());
    auto& resolver = unit->get<types::TemplateResolver>();
    auto& context = unit->context();
    for(auto name: {"left", "right"}) {
        auto* alias = nodes.named<clang::TypedefNameDecl>(name);
        ZASSERT(alias);
        auto resolved = resolver.resolve(alias->getUnderlyingType());
        ZASSERT(!resolved.isNull());
        ZEXPECT(context.hasSameType(resolved, context.CharTy));
    }
}

ZEST_CASE(UnknownLengthExpansion) {
    /// An expansion whose length is not known may hold none of the elements
    /// a partial specialization needs: the patterns disagree on `type`.
    add_focus("main.cpp", R"code(
        template <class... Ts> struct pick { using type = void; };
        template <class T, class... Rest> struct pick<T, Rest...> { using type = T; };
        template <class... Ts> struct user { using first = typename pick<Ts...>::type; };
    )code");
    ZASSERT(compile());

    Nodes nodes(unit->context());
    auto* first = nodes.named<clang::TypedefNameDecl>("first");
    ZASSERT(first);
    auto& resolver = unit->get<types::TemplateResolver>();
    auto resolved = resolver.resolve(first->getUnderlyingType());
    ZASSERT(!resolved.isNull());
    ZEXPECT(resolved->getAs<clang::DependentNameType>() != nullptr);
}

ZEST_CASE(RecursiveValueUnknown) {
    /// A recursive value whose operands are not known folds down a chain per
    /// argument, not a tree: past an unknown operand the other is not folded.
    add_focus("main.cpp", R"code(
        template <long X, long Y> constexpr long gcd = gcd<Y, X % Y>;
        template <long X> constexpr long gcd<X, 0> = X;
        template <bool B, class T = void> struct enable_if {};
        template <class T> struct enable_if<true, T> { using type = T; };
        template <class R> struct reduced {
            using type = typename enable_if<gcd<R::num, R::den> != 0, int>::type;
        };
    )code");
    ZASSERT(compile());

    Nodes nodes(unit->context());
    auto* type = nodes.named<clang::TypedefNameDecl>("type");
    ZASSERT(type);
    types::TemplateResolver resolver(*unit, {.audit = true});
    resolver.resolve(type->getUnderlyingType());
    ZASSERT(resolver.audits().size() == 1);
    auto& audit = resolver.audits().front();
    ZEXPECT(audit.troubles[static_cast<std::size_t>(types::Trouble::Evaluation)] < 32);
}

};  // ZEST_SUITE(TemplateResolver)

}  // namespace

}  // namespace clice::testing
