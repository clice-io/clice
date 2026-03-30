#include <string>

#include "test/test.h"
#include "test/tester.h"
#include "feature/feature.h"

namespace clice::testing {

namespace {

namespace protocol = eventide::ipc::protocol;

TEST_SUITE(InlayHint, Tester) {

std::vector<protocol::InlayHint> hints;
llvm::DenseMap<std::uint32_t, protocol::InlayHint> hints_map;

void run(llvm::StringRef code, std::source_location location = std::source_location::current()) {
    add_main("main.cpp", code);
    ASSERT_TRUE(compile_with_pch("-std=c++23"));

    LocalSourceRange range = LocalSourceRange(0, unit->interested_content().size());
    hints = feature::inlay_hints(*unit, range, {}, feature::PositionEncoding::UTF8);

    hints_map.clear();
    feature::PositionMapper converter(unit->interested_content(), feature::PositionEncoding::UTF8);
    for(auto& hint: hints) {
        hints_map[*converter.to_offset(hint.position)] = hint;
    }

    if(!unit->diagnostics().empty()) {
        for(auto& diagnostic: unit->diagnostics()) {
            std::println("{}", diagnostic.message);
        }
    }

    ASSERT_TRUE(unit->diagnostics().empty());
}

void EXPECT_SIZE(std::uint32_t size,
                 std::source_location location = std::source_location::current()) {
    ASSERT_EQ(hints.size(), size);
}

void EXPECT_HINT(llvm::StringRef pos,
                 llvm::StringRef name,
                 std::source_location location = std::source_location::current()) {
    auto offset = point(pos);
    auto it = hints_map.find(offset);
    ASSERT_TRUE(it != hints_map.end());

    std::string label;
    if(auto* plain = std::get_if<std::string>(&it->second.label)) {
        label = *plain;
    } else {
        for(const auto& part:
            std::get<std::vector<protocol::InlayHintLabelPart>>(it->second.label)) {
            label += part.value;
        }
    }
    ASSERT_EQ(label, name);
};

TEST_CASE(Parameters) {
    /// Name hint for normal param.
    run(R"c(
            int foo(int param);
            int x = foo($(0)42);
        )c");
    EXPECT_SIZE(1);
    EXPECT_HINT("0", "param:");

    // No hint for anonymous param.
    run(R"c(
            int foo(int);
            int x = foo(42);
        )c");
    EXPECT_SIZE(0);

    /// Reference hint for anonymous lvalue ref param.
    run(R"c(
            int foo(int&);
            int x = 1;
            int y = foo($(0)x);
        )c");
    EXPECT_SIZE(1);
    EXPECT_HINT("0", "&:");

    // No hint for anonymous const lvalue ref param.
    run(R"c(
            int foo(const int&);
            int x = foo(42);
        )c");
    EXPECT_SIZE(0);

    run(R"c(
            template <typename... Args>
            int foo(Args&& ...);
            int x = foo(42);
        )c");
    EXPECT_SIZE(0);

    run(R"c(
            namespace std {
                template <typename T> T&& forward(T&);
            }

            int foo(int);
            template <typename... Args>
            int bar(Args&&... args) { return foo(std::forward<Args>(args)...); }
            int x = bar(42);
        )c");
    EXPECT_SIZE(0);

    // No hint for anonymous r-value ref parameter
    run(R"c(
            int foo(int&&);
            int x = foo(42);
        )c");
    EXPECT_SIZE(0);

    // Parameter name picked up from definition if necessary
    run(R"c(
            int foo(int);
            int x = foo($(0)42);
            int foo(int param) {
                return 0;
            }
        )c");
    EXPECT_SIZE(1);
    EXPECT_HINT("0", "param:");

    // Parameter name picked up from definition if necessary
    run(R"c(
            int foo(int, int b);
            int x = foo($(0)42, $(1)42);
            int foo(int a, int) {
                return 0;
            }
        )c");
    EXPECT_SIZE(2);
    EXPECT_HINT("0", "a:");
    EXPECT_HINT("1", "b:");

    // Parameter name picked up from definition in a resolved forwarded parameter
    run(R"c(
            int foo(int, int);
            template <typename... Args>
            int bar(Args... args) {
                return foo(args...);
            }
            int x = bar($(0)42, $(1)42);
            int foo(int a, int b) {
                return 0;
            }
        )c");
    EXPECT_SIZE(2);
    EXPECT_HINT("0", "a:");
    EXPECT_HINT("1", "b:");

    // Prefer name from declaration
    run(R"c(
            int foo(int good);
            int x = foo($(0)42);
            int foo(int bad) {
                return 0;
            }
        )c");
    EXPECT_SIZE(1);
    EXPECT_HINT("0", "good:");

    // Only name hint for const l-value ref parameter
    run(R"c(
            int foo(const int& param);
            int x = foo($(0)42);
        )c");
    EXPECT_SIZE(1);
    EXPECT_HINT("0", "param:");

    // Only name hint for const l-value ref parameter via type alias
    run(R"c(
            using alias = const int&;
            int foo(alias param);
            int x = 1;
            int y = foo($(0)x);
        )c");
    EXPECT_SIZE(1);
    EXPECT_HINT("0", "param:");

    // Reference and name hint for l-value ref parameter
    run(R"c(
            int foo(int& param);
            int x = 1;
            int y = foo($(0)x);
        )c");
    EXPECT_SIZE(1);
    EXPECT_HINT("0", "&param:");

    // Reference and name hint for l-value ref parameter via type alias
    run(R"c(
            using alias = int&;
            int foo(alias param);
            int x = 1;
            int y = foo($(0)x);
        )c");
    EXPECT_SIZE(1);
    EXPECT_HINT("0", "&param:");

    // Only name hint for r-value ref parameter
    run(R"c(
            int foo(int&& param);
            int x = foo($(0)42);
        )c");
    EXPECT_SIZE(1);
    EXPECT_HINT("0", "param:");

    // Arg matches param
    run(R"c(
            void foo(int param);
            struct S {
                static const int param = 42;
            };
            void bar() {
                int param = 42;
                // Do not show redundant "param: param".
                foo(param);
                // But show it if the argument is qualified.
                foo($(0)S::param);
            }
            struct A {
                int param;
                void bar() {
                    // Do not show "param: param" for member-expr.
                    foo(param);
                }
            };
        )c");
    EXPECT_SIZE(1);
    EXPECT_HINT("0", "param:");

    // Arg matches param reference
    run(R"c(
            void foo(int& param);
            void foo2(const int& param);
            void bar() {
                int param;
                // show reference hint on mutable reference
                foo($(0)param);
                // but not on const reference
                foo2(param);
            }
        )c");
    EXPECT_SIZE(1);
    EXPECT_HINT("0", "&:");

    // Name hint for variadic parameter using std::forward in a constructor call
    run(R"c(
            namespace std { template <typename T> T&& forward(T&); }
            struct S { S(int a); };
            template <typename T, typename... Args>
            T bar(Args&&... args) { return T{std::forward<Args>(args)...}; }
            int x = 1;
            S y = bar<S>($(0)x);
        )c");
    EXPECT_SIZE(1);
    EXPECT_HINT("0", "a:");

    // Name hint for variadic parameter in a constructor call
    run(R"c(
            struct S { S(int a); };
            template <typename T, typename... Args>
            T bar(Args&&... args) { return T{args...}; }
            int x = 1;
            S y = bar<S>($(0)x);
        )c");
    EXPECT_SIZE(1);
    EXPECT_HINT("0", "a:");

    // Name for variadic parameter using std::forward
    run(R"c(
            namespace std { template <typename T> T&& forward(T&); }
            int foo(int a);
            template <typename... Args>
            int bar(Args&&... args) { return foo(std::forward<Args>(args)...); }
            int x = 1;
            int y = bar($(0)x);
        )c");
    EXPECT_SIZE(1);
    EXPECT_HINT("0", "a:");

    // Name hint for variadic parameter
    run(R"c(
            int foo(int a);
            template <typename... Args>
            int bar(Args&&... args) { return foo(args...); }
            int x = bar($(0)42);
        )c");
    EXPECT_SIZE(1);
    EXPECT_HINT("0", "a:");

    // Name hint for variadic parameter when the parameter pack is not the last template
    // parameter
    run(R"c(
            int foo(int a);
            template <typename... Args, typename Arg>
            int bar(Arg, Args&&... args) { return foo(args...); }
            int x = bar(1, $(0)42);
        )c");
    EXPECT_SIZE(1);
    EXPECT_HINT("0", "a:");

    // Name for variadic parameter that involves both head and tail parameters
    run(R"c(
            namespace std { template <typename T> T&& forward(T&); }
            int baz(int, int b, double);
            template <typename... Args>
            int foo(int a, Args&&... args) {
                return baz(1, std::forward<Args>(args)..., 1.0);
            }
            template <typename... Args>
            int bar(Args&&... args) { return foo(std::forward<Args>(args)...); }
            int x = bar($(0)32, $(1)42);
        )c");
    EXPECT_SIZE(2);
    EXPECT_HINT("0", "a:");
    EXPECT_HINT("1", "b:");

    // No hint for operator call with operator syntax
    run(R"c(
            struct S {};
            void operator+(S lhs, S rhs);
            void bar() {
                S a, b;
                a + b;
            }
        )c");
    EXPECT_SIZE(0);

    // Function call operator
    run(R"c(
            struct W {
                void operator()(int x);
            };

            struct S : W {
                using W::operator();
                static void operator()(int x, int y);
            };

            void bar() {
                auto l1 = [](int x) -> void {};
                auto l2 = [](int x) static -> void {};

                S s;
                s($(0)1);
                s.operator()($(1)1);
                s.operator()($(2)1, $(3)2);
                S::operator()($(4)1, $(5)2);

                l1($(6)1);
                l1.operator()($(7)1);
                l2($(8)1);
                l2.operator()($(9)1);

                void (*ptr)(int a, int b) = &S::operator();
                ptr($(10)1, $(11)2);
            }
        )c");

    EXPECT_HINT("0", "x:");
    EXPECT_HINT("1", "x:");
    EXPECT_HINT("2", "x:");
    EXPECT_HINT("3", "y:");
    EXPECT_HINT("4", "x:");
    EXPECT_HINT("5", "y:");
    EXPECT_HINT("6", "x:");
    EXPECT_HINT("7", "x:");
    EXPECT_HINT("8", "x:");
    EXPECT_HINT("9", "x:");
    EXPECT_HINT("10", "a:");
    EXPECT_HINT("11", "b:");

    // Deducing this
    run(R"c(
            struct S {
                template <typename This>
                int operator()(this This &&Self, int Param) {
                    return 42;
                }

                int function(this auto &Self, int Param) {
                    return Param;
                }
            };
            void work() {
                S s;
                s($(0)42);
                s.function($(1)42);
                S()($(2)42);
                auto lambda = [](this auto &Self, char C) -> void {
                    return Self(C);
                };
                lambda($(3)'A');
            }
        )c");

    EXPECT_HINT("0", "Param:");
    EXPECT_HINT("1", "Param:");
    EXPECT_HINT("2", "Param:");
    EXPECT_HINT("3", "C:");

    // Constructor with parentheses
    run(R"c(
            struct S {
                S(int param);
            };
            void bar() {
                S obj($(0)42);
            }
        )c");
    EXPECT_SIZE(1);
    EXPECT_HINT("0", "param:");

    // Constructor with braces
    run(R"c(
            struct S {
                S(int param);
            };
            void bar() {
                S obj{$(0)42};
            }
        )c");
    EXPECT_SIZE(1);
    EXPECT_HINT("0", "param:");

    // Member initialization
    run(R"c(
            struct S {
                S(int param);
            };

            struct T {
                S member;
                T() : member($(0)42) {}
            };
        )c");
    EXPECT_SIZE(1);
    EXPECT_HINT("0", "param:");

    // Function pointer
    run(R"c(
            void (*f1)(int param);
            void (*f2)(int param) noexcept;
            using f3_t = void(*)(int param);
            f3_t f3;
            using f4_t = void(*)(int param) noexcept;
            f4_t f4;

            void bar() {
                f1($(0)42);
                f2($(1)42);
                f3($(2)42);
                f4($(3)42);
            }
        )c");
    EXPECT_SIZE(4);
    EXPECT_HINT("0", "param:");
    EXPECT_HINT("1", "param:");
    EXPECT_HINT("2", "param:");
    EXPECT_HINT("3", "param:");

    // Leading underscore
    run(R"c(
            void foo(int p1, int _p2, int __p3);
            void bar() {
                foo($(0)41, $(1)42, $(2)43);
            }
        )c");
    EXPECT_SIZE(3);
    EXPECT_HINT("0", "p1:");
    EXPECT_HINT("1", "p2:");
    EXPECT_HINT("2", "p3:");

    // Variadic function
    run(R"c(
            template <typename... T>
            void foo(int fixed, T... variadic);

            void bar() {
                foo($(0)41, 42, 43);
            }
        )c");
    EXPECT_SIZE(1);
    EXPECT_HINT("0", "fixed:");

    // Varargs function
    run(R"c(
            void foo(int fixed, ...);

            void bar() {
                foo($(0)41, 42, 43);
            }
        )c");
    EXPECT_SIZE(1);
    EXPECT_HINT("0", "fixed:");

    // Do not show hint for parameter of copy or move constructor
    run(R"c(
            struct S {
                S();
                S(const S& other);
                S(S&& other);
            };

            void bar() {
                S a;
                S b = S(a);    // copy
                S c = S(S());  // move
            }
        )c");
    EXPECT_SIZE(0);

    // Do not hint call to user-defined literal operator
    run(R"c(
            long double operator ""_w(long double param);
            void bar() {
                1.2_w;
            }
        )c");
    EXPECT_SIZE(0);

    // Parameter name comment
    run(R"c(
            void foo(int param);
            void bar() {
                foo(/*param*/42);
                foo( /* param = */ 42);
                #define X 42
                #define Y X
                #define Z(...) Y
                foo(/*param=*/Z(a));
                foo($(0)Z(a));
                foo(/* the answer */$(1)42);
            }
        )c");
    EXPECT_SIZE(2);
    EXPECT_HINT("0", "param:");
    EXPECT_HINT("1", "param:");

    // Setter functions
    run(R"c(
            struct S {
                void setParent(S* parent);
                void set_parent(S* parent);
                void setTimeout(int timeoutMillis);
                void setTimeoutMillis(int timeout_millis);
            };
            void bar() {
                S s;
                // Parameter name matches setter name - omit hint.
                s.setParent(nullptr);
                // Support snake_case
                s.set_parent(nullptr);
                // Parameter name may contain extra info - show hint.
                s.setTimeout($(0)120);
                // FIXME: Ideally we'd want to omit this.
                s.setTimeoutMillis($(1)120);
            }
        )c");
    EXPECT_SIZE(2);
    EXPECT_HINT("0", "timeoutMillis:");
    EXPECT_HINT("1", "timeout_millis:");
};

TEST_CASE(Types) {
    // Basic type hint
    run(R"c(
            auto waldo$(0) = 42;
        )c");
    EXPECT_SIZE(1);
    EXPECT_HINT("0", ": int");

    // Decorations
    run(R"c(
            int x = 42;
            auto* var1$(0) = &x;
            auto&& var2$(1) = x;
            const auto& var3$(2) = x;
        )c");
    EXPECT_SIZE(3);
    EXPECT_HINT("0", ": int *");
    EXPECT_HINT("1", ": int &");
    EXPECT_HINT("2", ": const int &");

    // Decltype auto
    run(R"c(
            int x = 42;
            int& y = x;
            decltype(auto) z$(0) = y;
        )c");
    EXPECT_SIZE(1);
    EXPECT_HINT("0", ": int &");

    // No qualifiers
    run(R"c(
            namespace A {
                namespace B {
                    struct S1 {};
                    S1 foo();
                    auto x$(0) = foo();

                    struct S2 {
                        template <typename T>
                        struct Inner {};
                    };

                    S2::Inner<int> bar();
                    auto y$(1) = bar();
                }
            }
        )c");
    EXPECT_SIZE(2);
    EXPECT_HINT("0", ": S1");
    EXPECT_HINT("1", ": S2::Inner<int>");

    // Lambda
    run(R"c(
            void f() {
                int cap = 42;
                auto L$(0) = [cap, init$(1) = 1 + 1](int a)$(2) {
                    return a + cap + init;
                };
            }
        )c");
    EXPECT_SIZE(3);
    EXPECT_HINT("0", ": (lambda)");
    EXPECT_HINT("1", ": int");
    EXPECT_HINT("2", "-> int");

    // Lambda return hint shown even if no param list
    run(R"c(
            auto x$(0) = []$(1){return 42;};
        )c");
    EXPECT_SIZE(2);
    EXPECT_HINT("0", ": (lambda)");
    EXPECT_HINT("1", "-> int");

    // Structured bindings - public struct
    run(R"c(
            struct Point {
                int x;
                int y;
            };
            Point foo();
            auto [x$(0), y$(1)] = foo();
        )c");
    EXPECT_SIZE(2);
    EXPECT_HINT("0", ": int");
    EXPECT_HINT("1", ": int");

    // Structured bindings - array
    run(R"c(
            int arr[2];
            auto [x$(0), y$(1)] = arr;
        )c");
    EXPECT_SIZE(2);
    EXPECT_HINT("0", ": int");
    EXPECT_HINT("1", ": int");

    // Structured bindings - tuple-like
    run(R"c(
            struct IntPair {
                int a;
                int b;
            };

            namespace std {
                template <typename T>
                struct tuple_size {};

                template <>
                struct tuple_size<IntPair> {
                    constexpr static unsigned value = 2;
                };

                template <unsigned I, typename T>
                struct tuple_element {};

                template <unsigned I>
                struct tuple_element<I, IntPair> {
                    using type = int;
                };
            }

            template <unsigned I>
            int get(const IntPair& p) {
                if constexpr (I == 0) {
                    return p.a;
                } else if constexpr (I == 1) {
                    return p.b;
                }
            }

            IntPair bar();
            auto [x$(0), y$(1)] = bar();
        )c");
    EXPECT_SIZE(2);
    EXPECT_HINT("0", ": int");
    EXPECT_HINT("1", ": int");

    // Return type deduction
    run(R"c(
            auto f1(int x)$(0);  // Hint forward declaration too
            auto f1(int x)$(1) { return x + 1; }

            // Include pointer operators in hint
            int s;
            auto& f2()$(2) { return s; }

            // Do not hint `auto` for trailing return type.
            auto f3() -> int;

            // Do not hint when a trailing return type is specified.
            auto f4() -> auto* { return "foo"; }

            auto f5()$(3) {}

            // `auto` conversion operator
            struct A {
                operator auto()$(4) { return 42; }
            };
        )c");
    EXPECT_SIZE(5);
    EXPECT_HINT("0", "-> int");
    EXPECT_HINT("1", "-> int");
    EXPECT_HINT("2", "-> int &");
    EXPECT_HINT("3", "-> void");
    EXPECT_HINT("4", "-> int");

    // Decltype
    run(R"c(
            decltype(0)$(0) a;
            decltype(a)$(1) b;
            const decltype(0)$(2) &c = b;

            decltype(0)$(3) e();
            auto f() -> decltype(0)$(4);

            template <class, class> struct Foo;
            using G = Foo<decltype(0)$(5), float>;

            auto h$(6) = decltype(0)$(7){};
        )c");
    EXPECT_SIZE(8);
    EXPECT_HINT("0", ": int");
    EXPECT_HINT("1", ": int");
    EXPECT_HINT("2", ": int");
    EXPECT_HINT("3", ": int");
    EXPECT_HINT("4", ": int");
    EXPECT_HINT("5", ": int");
    EXPECT_HINT("6", ": int");
    EXPECT_HINT("7", ": int");

    // Long type name
    run(R"c(
            template <typename, typename, typename>
            struct A {};
            struct MultipleWords {};
            A<MultipleWords, MultipleWords, MultipleWords> foo();
            // Omit type hint past a certain length (currently 32)
            auto var = foo();
        )c");
    EXPECT_SIZE(0);

    // Default template args
    run(R"c(
            template <typename, typename = int>
            struct A {};
            A<float> foo();
            auto var$(0) = foo();
            A<float> bar[1];
            auto [binding$(1)] = bar;
        )c");
    EXPECT_SIZE(2);
    EXPECT_HINT("0", ": A<float>");
    EXPECT_HINT("1", ": A<float>");

    // Deduplication
    run(R"c(
            template <typename T>
            void foo() {
                auto var$(0) = 42;
            }

            template void foo<int>();
            template void foo<float>();
        )c");
    EXPECT_SIZE(1);
    EXPECT_HINT("0", ": int");

    // Singly instantiated template
    run(R"c(
            auto lambda$(0) = [](auto* param$(1), auto) { return 42; };
            int m = lambda("foo", 3);
        )c");
    EXPECT_HINT("0", ": (lambda)");
    EXPECT_HINT("1", ": const char *");

    // No hint for packs, or auto params following packs
    run(R"c(
            int x(auto a$(0), auto... b, auto c) { return 42; }
            int m = x<void*, char, float>(nullptr, 'c', 2.0, 2);
        )c");
    EXPECT_HINT("0", ": void *");
};

TEST_CASE(Designators, skip = true) {
    // Basic designator hints
    run(R"c(
            struct S { int x, y, z; };
            S s {$(0)1, $(1)2+2};

            int x[] = {$(2)0, $(3)1};
        )c");
    EXPECT_SIZE(4);
    EXPECT_HINT("0", ".x=");
    EXPECT_HINT("1", ".y=");
    EXPECT_HINT("2", "[0]=");
    EXPECT_HINT("3", "[1]=");

    // Nested designators
    run(R"c(
            struct Inner { int x, y; };
            struct Outer { Inner a, b; };
            Outer o{ $(0)a{ $(1)1, $(2)2 }, $(3)bx3 };
        )c");
    EXPECT_SIZE(4);
    EXPECT_HINT("0", ".a=");
    EXPECT_HINT("1", ".x=");
    EXPECT_HINT("2", ".y=");
    EXPECT_HINT("3", ".b.x=");

    // Anonymous record
    run(R"c(
            struct S {
                union {
                    struct {
                        struct {
                            int y;
                        };
                    } x;
                };
            };
            S s{$(0)xy42};
        )c");
    EXPECT_SIZE(1);
    EXPECT_HINT("0", ".x.y=");

    // Suppression
    run(R"c(
            struct Point { int a, b, c, d, e, f, g, h; };
            Point p{/*a=*/1, .c=2, /* .d = */3, $(0)4};
        )c");
    EXPECT_SIZE(1);
    EXPECT_HINT("0", ".e=");

    // Std array
    run(R"c(
            template <typename T, int N> struct Array { T __elements[N]; };
            Array<int, 2> x = {$(0)0, $(1)1};
        )c");
    EXPECT_SIZE(2);
    EXPECT_HINT("0", "[0]=");
    EXPECT_HINT("1", "[1]=");

    // Only aggregate init
    run(R"c(
            struct Copyable { int x; } c;
            Copyable d{c};

            struct Constructible { Constructible(int x); };
            Constructible x{42};
        )c");
    EXPECT_SIZE(0);

    // No crash
    run(R"c(
            struct A {};
            struct Foo {int a; int b;};
            void test() {
                Foo f{A(), $(0)1};
            }
        )c");
    EXPECT_SIZE(1);
    EXPECT_HINT("0", ".b=");
};

TEST_CASE(BlockEnd, skip = true) {
    // Functions
    run(R"c(
            int foo() {
                return 41;
            $(0)}

            template<int X>
            int bar() {
                // No hint for lambda for now
                auto f = []() {
                    return X;
                };
                return f();
            $(1)}

            // No hint because this isn't a definition
            int buz();

            struct S{};
            bool operator==(S, S) {
                return true;
            $(2)}
        )c");
    EXPECT_SIZE(3);
    EXPECT_HINT("0", " // foo");
    EXPECT_HINT("1", " // bar");
    EXPECT_HINT("2", " // operator==");

    // Methods
    run(R"c(
            struct Test {
                // No hint because there's no function body
                Test() = default;

                ~Test() {
                $(0)}

                void method1() {
                $(1)}

                // No hint because this isn't a definition
                void method2();

                template <typename T>
                void method3() {
                $(2)}

                // No hint because this isn't a definition
                template <typename T>
                void method4();

                Test operator+(int) const {
                    return *this;
                $(3)}

                operator bool() const {
                    return true;
                $(4)}

                // No hint because there's no function body
                operator int() const = delete;
            } x;

            void Test::method2() {
            $(5)}

            template <typename T>
            void Test::method4() {
            $(6)}
        )c");
    EXPECT_SIZE(7);
    EXPECT_HINT("0", " // ~Test");
    EXPECT_HINT("1", " // method1");
    EXPECT_HINT("2", " // method3");
    EXPECT_HINT("3", " // operator+");
    EXPECT_HINT("4", " // operator bool");
    EXPECT_HINT("5", " // Test::method2");
    EXPECT_HINT("6", " // Test::method4");

    // Namespaces
    run(R"c(
            namespace {
                void foo();
            $(0)}

            namespace ns {
                void bar();
            $(1)}
        )c");
    EXPECT_SIZE(2);
    EXPECT_HINT("0", " // namespace");
    EXPECT_HINT("1", " // namespace ns");

    // Types
    run(R"c(
            struct S {
            $(0)};

            class C {
            $(1)};

            union U {
            $(2)};

            enum E1 {
            $(3)};

            enum class E2 {
            $(4)};
        )c");
    EXPECT_SIZE(5);
    EXPECT_HINT("0", " // struct S");
    EXPECT_HINT("1", " // class C");
    EXPECT_HINT("2", " // union U");
    EXPECT_HINT("3", " // enum E1");
    EXPECT_HINT("4", " // enum class E2");

    // If statements
    run(R"c(
            void foo(bool cond) {
                if (cond)
                    ;

                if (cond) {
                $(0)}

                if (cond) {
                } else {
                $(1)}

                if (cond) {
                } else if (!cond) {
                $(2)}

                if (cond) {
                } else {
                    if (!cond) {
                    $(3)}
                $(4)}

                if (auto X = cond) {
                $(5)}

                if (int i = 0; i > 10) {
                $(6)}
            }
        )c");
    EXPECT_SIZE(7);
    EXPECT_HINT("0", " // if cond");
    EXPECT_HINT("1", " // if cond");
    EXPECT_HINT("2", " // if");
    EXPECT_HINT("3", " // if !cond");
    EXPECT_HINT("4", " // if cond");
    EXPECT_HINT("5", " // if X");
    EXPECT_HINT("6", " // if i > 10");

    // Loops
    run(R"c(
            void foo() {
                while (true)
                    ;

                while (true) {
                $(0)}

                do {
                } while (true);

                for (;true;) {
                $(1)}

                for (int I = 0; I < 10; ++I) {
                $(2)}

                int Vs[] = {1,2,3};
                for (auto V : Vs) {
                $(3)}
            }
        )c");
    EXPECT_SIZE(4);
    EXPECT_HINT("0", " // while true");
    EXPECT_HINT("1", " // for true");
    EXPECT_HINT("2", " // for I");
    EXPECT_HINT("3", " // for V");

    // Switch
    run(R"c(
            void foo(int I) {
                switch (I) {
                    case 0: break;
                $(0)}
            }
        )c");
    EXPECT_SIZE(1);
    EXPECT_HINT("0", " // switch I");

    // Print literals
    run(R"c(
            void foo() {
                while ("foo") {
                $(0)}

                while ("foo but this time it is very long") {
                $(1)}

                while (true) {
                $(2)}

                while (1) {
                $(3)}

                while (1.5) {
                $(4)}
            }
        )c");
    EXPECT_SIZE(5);
    EXPECT_HINT("0", " // while \"foo\"");
    EXPECT_HINT("1", " // while \"foo but...\"");
    EXPECT_HINT("2", " // while true");
    EXPECT_HINT("3", " // while 1");
    EXPECT_HINT("4", " // while 1.5");

    // Print refs
    run(R"c(
            namespace ns {
                int Var;
                int func();
                struct S {
                    int Field;
                    int method() const;
                };
            }
            void foo() {
                while (ns::Var) {
                $(0)}

                while (ns::func()) {
                $(1)}

                while (ns::S{}.Field) {
                $(2)}

                while (ns::S{}.method()) {
                $(3)}
            }
        )c");
    EXPECT_SIZE(4);
    EXPECT_HINT("0", " // while Var");
    EXPECT_HINT("1", " // while func");
    EXPECT_HINT("2", " // while Field");
    EXPECT_HINT("3", " // while method");

    // Print conversions
    run(R"c(
            struct S {
                S(int);
                S(int, int);
                explicit operator bool();
            };
            void foo(int I) {
                while (float(I)) {
                $(0)}

                while (S(I)) {
                $(1)}

                while (S(I, I)) {
                $(2)}
            }
        )c");
    EXPECT_SIZE(3);
    EXPECT_HINT("0", " // while float");
    EXPECT_HINT("1", " // while S");
    EXPECT_HINT("2", " // while S");

    // Print operators
    run(R"c(
            using Integer = int;
            void foo(Integer I) {
                while(++I){
                $(0)}

                while(I++){
                $(1)}

                while(+(I + I)){
                $(2)}

                while(I < 0){
                $(3)}

                while((I + I) < I){
                $(4)}

                while(I < (I + I)){
                $(5)}

                while((I + I) < (I + I)){
                $(6)}
            }
        )c");
    EXPECT_SIZE(7);
    EXPECT_HINT("0", " // while ++I");
    EXPECT_HINT("1", " // while I++");
    EXPECT_HINT("2", " // while");
    EXPECT_HINT("3", " // while I < 0");
    EXPECT_HINT("4", " // while ... < I");
    EXPECT_HINT("5", " // while I < ...");
    EXPECT_HINT("6", " // while");

    // Trailing semicolon
    run(R"c(
            // The hint is placed after the trailing ';'
            struct S1 {
            $(0)}  ;

            // The hint is always placed in the same line with the closing '}'.
            // So in this case where ';' is missing, it is attached to '}'.
            struct S2 {
            $(1)}

            ;

            // No hint because only one trailing ';' is allowed
            struct S3 {
            };;

            // No hint because trailing ';' is only allowed for class/struct/union/enum
            void foo() {
            };

            // Rare case, but yes we'll have a hint here.
            struct {
                int x;
            $(2)}

            s2;
        )c");
    EXPECT_SIZE(3);
    EXPECT_HINT("0", " // struct S1");
    EXPECT_HINT("1", " // struct S2");
    EXPECT_HINT("2", " // struct");

    // Trailing text
    run(R"c(
            struct S1 {
            $(0)}      ;

            // No hint for S2 because of the trailing comment
            struct S2 {
            }; /* Put anything here */

            struct S3 {
                // No hint for S4 because of the trailing source code
                struct S4 {
                };$(1)};

            // No hint for ns because of the trailing comment
            namespace ns {
            } // namespace ns
        )c");
    EXPECT_SIZE(2);
    EXPECT_HINT("0", " // struct S1");
    EXPECT_HINT("1", " // struct S3");

    // Macro
    run(R"c(
            #define DECL_STRUCT(NAME) struct NAME {
            #define RBRACE }

            DECL_STRUCT(S1)
            $(0)};

            // No hint because we require a '}'
            DECL_STRUCT(S2)
            RBRACE;
        )c");
    EXPECT_SIZE(1);
    EXPECT_HINT("0", " // struct S1");

    // Pointer to member function
    run(R"c(
            class A {};
            using Predicate = bool(A::*)();
            void foo(A* a, Predicate p) {
                if ((a->*p)()) {
                $(0)}
            }
        )c");
    EXPECT_SIZE(1);
    EXPECT_HINT("0", " // if");
};

TEST_CASE(DefaultArguments, skip = true) {
    // Smoke test
    run(R"c(
            int foo(int A = 4) { return A; }
            int bar(int A, int B = 1, bool C = foo($(0))) { return A; }
            int A = bar($(1)2$(2));

            void baz(int = 5) { if (false) baz($(3)); };
        )c");
    EXPECT_SIZE(4);
    EXPECT_HINT("0", "A: 4");
    EXPECT_HINT("1", "A: ");
    EXPECT_HINT("2", ", B: 1, C: foo()");
    EXPECT_HINT("3", "5");

    // Without parameter names
    run(R"c(
            struct Baz {
                Baz(float a = 3 //
                            + 2);
            };
            struct Foo {
                Foo(int, Baz baz = //
                        Baz{$(0)}

                    //
                ) {}
            };

            int main() {
                Foo foo1(1$(1));
                Foo foo2{2$(2)};
                Foo foo3 = {3$(3)};
                auto foo4 = Foo{4$(4)};
            }
        )c");
    EXPECT_SIZE(5);
    EXPECT_HINT("0", "...");
    EXPECT_HINT("1", ", Baz{}");
    EXPECT_HINT("2", ", Baz{}");
    EXPECT_HINT("3", ", Baz{}");
    EXPECT_HINT("4", ", Baz{}");
};

// FIXME: flaky on some platforms, skip until root cause is identified.
TEST_CASE(Special, skip = true) {
    // Macros
    run(R"c(
            void foo(int param);
            #define ExpandsToCall() foo(42)
            void bar() {
                ExpandsToCall();
            }
        )c");
    EXPECT_SIZE(0);

    run(R"c(
            #define PI 3.14
            void foo(double param);
            void bar() {
                foo($(0)PI);
            }
        )c");
    EXPECT_SIZE(1);
    EXPECT_HINT("0", "param:");

    run(R"c(
            void abort();
            #define ASSERT(expr) if (!(expr)) abort()
            int foo(int param);
            void bar() {
                ASSERT(foo($(0)42) == 0);
            }
        )c");
    EXPECT_SIZE(1);
    EXPECT_HINT("0", "param:");

    run(R"c(
            void foo(double x, double y);
            #define CONSTANTS 3.14, 2.72
            void bar() {
                foo(CONSTANTS);
            }
        )c");
    EXPECT_SIZE(0);

    // Implicit constructor
    run(R"c(
            struct S {
                S(int param);
            };
            void bar(S);
            S foo() {
                // Do not show hint for implicit constructor call in argument.
                bar(42);
                // Do not show hint for implicit constructor call in return.
                return 42;
            }
        )c");
    EXPECT_SIZE(0);

    // Aggregate init
    run(R"c(
            struct Point {
                int x;
                int y;
            };
            void bar() {
                Point p{41, 42};
            }
        )c");
    EXPECT_SIZE(0);

    // Builtin functions
    run(R"c(
            namespace std { template <typename T> T&& forward(T&); }
            void foo() {
                int i;
                int&& s = std::forward(i);
            }
        )c");
    EXPECT_SIZE(0);

    // Pseudo object expression
    run(R"c(
            struct S {
                __declspec(property(get=GetX, put=PutX)) int x[];
                int GetX(int y, int z) { return 42 + y; }
                void PutX(int) { }

                // This is a PseudoObjectExpression whose syntactic form is a binary
                // operator.
                void Work(int y) { x = y; } // Not `x = y: y`.
            };

            int printf(const char *Format, ...);

            int main() {
                S s;
                __builtin_dump_struct(&s, printf); // Not `Format: __builtin_dump_struct()`
                printf($(0)"Hello, %d", 42); // Normal calls are not affected.
                // This builds a PseudoObjectExpr, but here it's useful for showing the
                // arguments from the semantic form.
                return s.x[ $(1)1 ][ $(2)2 ]; // `x[y: 1][z: 2]`
            }
        )c");
    EXPECT_SIZE(3);
    EXPECT_HINT("0", "Format:");
    EXPECT_HINT("1", "y:");
    EXPECT_HINT("2", "z:");
};

TEST_CASE(VariadicTemplate) {
    // Arg packs and constructors
    run(R"c(
            struct Foo{ Foo(); Foo(int x); };

            void foo(Foo a, int b);

            template <typename... Args>
            void bar(Args... args) {
                foo(args...);
            }

            template <typename... Args>
            void baz(Args... args) { foo($(0)Foo{args...}, $(1)1); }

            template <typename... Args>
            void bax(Args... args) { foo($(2){args...}, args...); }

            void foo() {
                bar($(3)Foo{}, $(4)42);
                bar($(5)42, $(6)42);
                baz($(7)42);
                bax($(8)42);
            }
        )c");
    /// FIXME:
    /// EXPECT_HINT("0", "a:");
    /// EXPECT_HINT("1", "b:");
    /// EXPECT_HINT("2", "a:");
    EXPECT_HINT("3", "a:");
    EXPECT_HINT("4", "b:");
    EXPECT_HINT("5", "a:");
    EXPECT_HINT("6", "b:");
    EXPECT_HINT("7", "x:");
    EXPECT_HINT("8", "b:");

    // Doesn't expand all args
    run(R"c(
            void foo(int x, int y);
            int id(int a, int b, int c);
            template <typename... Args>
            void bar(Args... args) {
                foo(id($(0)args, $(1)1, $(2)args)...);
            }
            void foo() {
                bar(1, 2); // FIXME: We could have `bar(a: 1, a: 2)` here.
            }
        )c");
    /// FIXME:
    /// EXPECT_SIZE(3);
    /// EXPECT_HINT("0", "a:");
    /// EXPECT_HINT("1", "b:");
    /// EXPECT_HINT("2", "c:");
}

TEST_CASE(Dependent, skip = true) {
    // Dependent calls
    run(R"c(
            template <typename T>
            void nonmember(T par1);

            template <typename T>
            struct A {
                void member(T par2);
                static void static_member(T par3);
            };

            void overload(int anInt);
            void overload(double aDouble);

            template <typename T>
            struct S {
                void bar(A<T> a, T t) {
                    nonmember($(0)t);
                    a.member($(1)t);
                    A<T>::static_member($(2)t);
                    // We don't want to arbitrarily pick between
                    // "anInt" or "aDouble", so just show no hint.
                    overload(T{});
                }
            };
        )c");
    EXPECT_SIZE(3);
    EXPECT_HINT("0", "par1:");
    EXPECT_HINT("1", "par2:");
    EXPECT_HINT("2", "par3:");
}

};  // TEST_SUITE(InlayHint)
}  // namespace
}  // namespace clice::testing
