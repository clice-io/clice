/// Ported from clangd's unittests/HoverTests.cpp (llvmorg-21.1.8), part of the LLVM
/// project, licensed under Apache License v2.0 with LLVM Exceptions.
/// See https://llvm.org/LICENSE.txt for license information.

#include <functional>
#include <optional>

#include "test/test.h"
#include "test/tester.h"
#include "feature/feature.h"

#include "clang/AST/Attr.h"

namespace clice::testing {

namespace {

namespace protocol = kota::ipc::protocol;

using HoverInfo = feature::HoverInfo;
using PrintedType = feature::HoverInfo::PrintedType;
using HoverParam = feature::HoverInfo::Param;
using PassType = feature::HoverInfo::PassType;
using PassMode = feature::HoverInfo::PassMode;

std::string dump(const std::optional<std::string>& value) {
    return value ? *value : "<none>";
}

std::string dump(const std::optional<std::uint64_t>& value) {
    return value ? std::to_string(*value) : "<none>";
}

std::string dump(const std::optional<PrintedType>& type) {
    if(!type) {
        return "<none>";
    }
    std::string result;
    llvm::raw_string_ostream os(result);
    os << *type;
    return result;
}

std::string dump(const std::optional<HoverParam>& param) {
    if(!param) {
        return "<none>";
    }
    std::string result;
    llvm::raw_string_ostream os(result);
    os << *param;
    return result;
}

std::string dump(const std::optional<std::vector<HoverParam>>& params) {
    if(!params) {
        return "<none>";
    }
    std::string result = "[";
    for(const auto& param: *params) {
        if(result.size() > 1) {
            result += "; ";
        }
        llvm::raw_string_ostream os(result);
        os << param;
    }
    result += "]";
    return result;
}

std::string dump(const std::optional<PassType>& pass) {
    if(!pass) {
        return "<none>";
    }
    return std::format("{{pass_by: {}, converted: {}}}",
                       static_cast<int>(pass->pass_by),
                       pass->converted);
}

TEST_SUITE(Hover, Tester) {

std::optional<protocol::Hover> result;
std::optional<HoverInfo> info;
llvm::StringRef current_code;

void run(llvm::StringRef code) {
    add_main("main.cpp", code);
    ASSERT_TRUE(compile());

    auto points = nameless_points();
    ASSERT_EQ(points.size(), 1U);
    auto offset = points[0];
    result = feature::hover(*unit, offset, {}, feature::PositionEncoding::UTF8);
}

void compile_only(llvm::StringRef code) {
    add_main("main.cpp", code);
    ASSERT_TRUE(compile());
}

/// Compile the code and compute hover info on the (single) annotated point.
/// The point is either a nameless `$` marker or a named `$(p)` marker, and the
/// expected highlighted token may be wrapped in a `@sym[...]` range.
void run_info(llvm::StringRef code,
              const feature::HoverOptions& options = {},
              llvm::StringRef standard = "-std=c++20") {
    info.reset();
    current_code = code;

    add_main("main.cpp", code);
    ASSERT_TRUE(compile(standard));

    auto points = nameless_points();
    std::uint32_t offset = points.size() == 1 ? points[0] : point("p");
    info = feature::hover_info(*unit, offset, options);
}

void expect_hover(const HoverInfo& expected) {
    if(!info) {
        std::println("no hover result for:\n{}", current_code.str());
    }
    ASSERT_TRUE(info.has_value());

    bool same =
        info->namespace_scope == expected.namespace_scope &&
        info->local_scope == expected.local_scope && info->name == expected.name &&
        info->kind == expected.kind && info->documentation == expected.documentation &&
        info->definition == expected.definition &&
        info->access_specifier == expected.access_specifier && info->type == expected.type &&
        info->return_type == expected.return_type && info->parameters == expected.parameters &&
        info->template_parameters == expected.template_parameters &&
        info->value == expected.value && info->size == expected.size &&
        info->offset == expected.offset && info->padding == expected.padding &&
        info->align == expected.align && info->callee_arg_info == expected.callee_arg_info &&
        info->call_pass_type == expected.call_pass_type;
    if(!same) {
        std::println("hover mismatch for:\n{}", current_code.str());
    }

    EXPECT_EQ(dump(info->namespace_scope), dump(expected.namespace_scope));
    EXPECT_EQ(info->local_scope, expected.local_scope);
    EXPECT_EQ(info->name, expected.name);
    EXPECT_EQ(info->kind.value(), expected.kind.value());
    EXPECT_EQ(info->documentation, expected.documentation);
    EXPECT_EQ(info->definition, expected.definition);
    EXPECT_EQ(info->access_specifier, expected.access_specifier);
    EXPECT_EQ(dump(info->type), dump(expected.type));
    EXPECT_EQ(dump(info->return_type), dump(expected.return_type));
    EXPECT_EQ(dump(info->parameters), dump(expected.parameters));
    EXPECT_EQ(dump(info->template_parameters), dump(expected.template_parameters));
    EXPECT_EQ(dump(info->value), dump(expected.value));
    EXPECT_EQ(dump(info->size), dump(expected.size));
    EXPECT_EQ(dump(info->offset), dump(expected.offset));
    EXPECT_EQ(dump(info->padding), dump(expected.padding));
    EXPECT_EQ(dump(info->align), dump(expected.align));
    EXPECT_EQ(dump(info->callee_arg_info), dump(expected.callee_arg_info));
    EXPECT_EQ(dump(info->call_pass_type), dump(expected.call_pass_type));
}

void check_sym_range() {
    if(!info || !current_code.contains("@sym[")) {
        return;
    }

    auto expected = range("sym");
    ASSERT_TRUE(info->symbol_range.has_value());
    if(*info->symbol_range != expected) {
        std::println("symbol range mismatch for:\n{}", current_code.str());
    }
    EXPECT_EQ(info->symbol_range->begin, expected.begin);
    EXPECT_EQ(info->symbol_range->end, expected.end);
}

struct HoverCase {
    llvm::StringRef code;
    std::function<void(HoverInfo&)> expected;
};

void check_cases(llvm::ArrayRef<HoverCase> cases, llvm::StringRef standard = "-std=c++20") {
    for(const auto& c: cases) {
        run_info(c.code, {}, standard);
        HoverInfo expected;
        c.expected(expected);
        expect_hover(expected);
        check_sym_range();
    }
}

TEST_CASE(Namespace) {
    run(R"cpp(
namespace $A {
}
)cpp");

    ASSERT_TRUE(result.has_value());
    auto* content = std::get_if<protocol::MarkupContent>(&result->contents);
    ASSERT_TRUE(content != nullptr);
    ASSERT_TRUE(content->value.find("namespace") != std::string::npos);
}

TEST_CASE(FunctionReference) {
    run(R"cpp(
int foo() { return 0; }
int x = $foo();
)cpp");

    ASSERT_TRUE(result.has_value());
    auto* content = std::get_if<protocol::MarkupContent>(&result->contents);
    ASSERT_TRUE(content != nullptr);
    ASSERT_TRUE(content->value.find("foo") != std::string::npos);
}

TEST_CASE(RecordScope) {
    compile_only(R"cpp(
typedef struct A {
    struct B {
        struct C {};
    };

    struct {
        struct D {};
    } _;
} T;

struct FORWARD_STRUCT;
struct FORWARD_CLASS;

void f() {
    struct X {};
    class Y {};

    struct {
        struct Z {};
    } _;
}

namespace n1 {
    namespace n2 {
        struct NA {
            struct NB {};
        };
    }

    namespace {
        struct NC {};
    }
}

namespace out {
    namespace in {
        struct M {
            int x;
            double y;
            char z;
            T a, b;
        };
    }
}
)cpp");
}

TEST_CASE(EnumStyle) {
    compile_only(R"cpp(
enum Free {
    A = 1,
    B = 2,
    C = 999,
};

enum class Scope: long {
    A = -8,
    B = 2,
    C = 100,
};
)cpp");
}

TEST_CASE(FunctionStyle) {
    compile_only(R"cpp(
typedef long long ll;

ll f(int x, int y, ll z = 1) { return 0; }

template<typename T, typename S>
T t(T a, T b, int c, ll d, S s) { return a; }

namespace {
    constexpr static const char* g() { return "hello"; }
}

namespace test {
    namespace {
        [[deprecated("test deprecate message")]] consteval int h() { return 1; }
    }
}

struct A {
    constexpr static A m(int left, double right) { return A(); }
};
)cpp");
}

TEST_CASE(VariableStyle) {
    compile_only(R"cpp(
void f() {
    constexpr static auto x1 = 1;
}
)cpp");
}

TEST_CASE(AutoAndDecltype) {
    compile_only(R"cpp(
$(a1)aut$(a2)o$(a3) i = -1;

$(d1)dec$(d2)ltype$(d3)(i) j = 2;

struct A { int x; };

aut$(a4)o va$(a5)r = A{};

a$(fa)uto f1() { return 1; }

de$(fn_decltype)cltype(au$(fn_decltype_auto)to) f2() {}

int f3(au$(fn_para_auto)to x) {}
)cpp");
}

TEST_CASE(Expr) {
    compile_only(R"cpp(
int xxxx = 1;
int yyyy = xx$(e1)xx;

struct A {
    int function(int param) {
        return thi$(e2)s$(e3)->$(e4)funct$(e5)ion(para$(e6)m);
    }

    int fn(int param) {
        return static$(e7)_cast<A*>(nul$(e8)lptr)->function(par$(e9)am);
    }
};
)cpp");
}

TEST_CASE(StructuredBasics) {
    HoverCase cases[] = {
        // Global scope.
        {R"cpp(
          // Best foo ever.
          void @sym[fo$o]() {}
          )cpp",
         [](HoverInfo& hi) {
             hi.namespace_scope = "";
             hi.name = "foo";
             hi.kind = SymbolKind::Function;
             hi.documentation = "Best foo ever.";
             hi.definition = "void foo()";
             hi.return_type = "void";
             hi.type = "void ()";
             hi.parameters.emplace();
         }},
        // Inside namespace
        {R"cpp(
          namespace ns1 { namespace ns2 {
            /// Best foo ever.
            void @sym[fo$o]() {}
          }}
          )cpp",
         [](HoverInfo& hi) {
             hi.namespace_scope = "ns1::ns2::";
             hi.name = "foo";
             hi.kind = SymbolKind::Function;
             hi.documentation = "Best foo ever.";
             hi.definition = "void foo()";
             hi.return_type = "void";
             hi.type = "void ()";
             hi.parameters.emplace();
         }},
        // Field
        {R"cpp(
          namespace ns1 { namespace ns2 {
            class Foo {
              char @sym[b$ar];
              double y[2];
            };
          }}
          )cpp",
         [](HoverInfo& hi) {
             hi.namespace_scope = "ns1::ns2::";
             hi.local_scope = "Foo::";
             hi.name = "bar";
             hi.kind = SymbolKind::Field;
             hi.definition = "char bar";
             hi.type = "char";
             hi.offset = 0;
             hi.size = 8;
             hi.padding = 56;
             hi.align = 8;
             hi.access_specifier = "private";
         }},
        // Union field
        {R"cpp(
            union Foo {
              char @sym[b$ar];
              double y[2];
            };
          )cpp",
         [](HoverInfo& hi) {
             hi.namespace_scope = "";
             hi.local_scope = "Foo::";
             hi.name = "bar";
             hi.kind = SymbolKind::Field;
             hi.definition = "char bar";
             hi.type = "char";
             hi.size = 8;
             hi.padding = 120;
             hi.align = 8;
             hi.access_specifier = "public";
         }},
        // Bitfield
        {R"cpp(
            struct Foo {
              int @sym[$x] : 1;
              int y : 1;
            };
          )cpp",
         [](HoverInfo& hi) {
             hi.namespace_scope = "";
             hi.local_scope = "Foo::";
             hi.name = "x";
             hi.kind = SymbolKind::Field;
             hi.definition = "int x : 1";
             hi.type = "int";
             hi.offset = 0;
             hi.size = 1;
             hi.padding = 0;
             hi.align = 32;
             hi.access_specifier = "public";
         }},
        // Bitfield offset, size and padding
        {R"cpp(
            struct Foo {
              char x;
              char @sym[$y] : 1;
              int z;
            };
          )cpp",
         [](HoverInfo& hi) {
             hi.namespace_scope = "";
             hi.local_scope = "Foo::";
             hi.name = "y";
             hi.kind = SymbolKind::Field;
             hi.definition = "char y : 1";
             hi.type = "char";
             hi.offset = 8;
             hi.size = 1;
             hi.padding = 23;
             hi.align = 8;
             hi.access_specifier = "public";
         }},
        // Local to class method.
        {R"cpp(
          namespace ns1 { namespace ns2 {
            struct Foo {
              void foo() {
                int @sym[b$ar];
              }
            };
          }}
          )cpp",
         [](HoverInfo& hi) {
             hi.namespace_scope = "ns1::ns2::";
             hi.local_scope = "Foo::foo::";
             hi.name = "bar";
             hi.kind = SymbolKind::Variable;
             hi.definition = "int bar";
             hi.type = "int";
         }},
        // Anon namespace and local scope.
        {R"cpp(
          namespace ns1 { namespace {
            struct {
              char @sym[b$ar];
            } T;
          }}
          )cpp",
         [](HoverInfo& hi) {
             hi.namespace_scope = "ns1::";
             hi.local_scope = "(anonymous struct)::";
             hi.name = "bar";
             hi.kind = SymbolKind::Field;
             hi.definition = "char bar";
             hi.type = "char";
             hi.offset = 0;
             hi.size = 8;
             hi.padding = 0;
             hi.align = 8;
             hi.access_specifier = "public";
         }},
        // Struct definition shows size.
        {R"cpp(
          struct @sym[$X]{};
          )cpp",
         [](HoverInfo& hi) {
             hi.namespace_scope = "";
             hi.name = "X";
             hi.kind = SymbolKind::Struct;
             hi.definition = "struct X {}";
             hi.size = 8;
             hi.align = 8;
         }},
    };
    check_cases(cases);
}

TEST_CASE(PredefinedVariable) {
    HoverCase cases[] = {
        // Predefined variable
        {R"cpp(
          void foo() {
            @sym[__f$unc__];
          }
          )cpp",
         [](HoverInfo& hi) {
             hi.name = "__func__";
             hi.kind = SymbolKind::Variable;
             hi.documentation = "Name of the current function (predefined variable)";
             hi.value = "\"foo\"";
             hi.type = "const char[4]";
         }},
        // Predefined variable (dependent)
        {R"cpp(
          template<int> void foo() {
            @sym[__f$unc__];
          }
          )cpp",
         [](HoverInfo& hi) {
             hi.name = "__func__";
             hi.kind = SymbolKind::Variable;
             hi.documentation = "Name of the current function (predefined variable)";
             hi.type = "const char[]";
         }},
    };
    check_cases(cases);
}

TEST_CASE(StructuredTemplates) {
    HoverCase cases[] = {
        // Variable with template type
        {R"cpp(
          template <typename T, class... Ts> class Foo { public: Foo(int); };
          Foo<int, char, bool> @sym[fo$o] = Foo<int, char, bool>(5);
          )cpp",
         [](HoverInfo& hi) {
             hi.namespace_scope = "";
             hi.name = "foo";
             hi.kind = SymbolKind::Variable;
             hi.definition = "Foo<int, char, bool> foo = Foo<int, char, bool>(5)";
             hi.type = "Foo<int, char, bool>";
         }          },
        // Implicit template instantiation
        {R"cpp(
          template <typename T> class vector{};
          @sym[vec$tor]<int> foo;
          )cpp",
         [](HoverInfo& hi) {
             hi.namespace_scope = "";
             hi.name = "vector<int>";
             hi.kind = SymbolKind::Class;
             hi.definition = "template <> class vector<int> {}";
         }          },
        // Class template
        {R"cpp(
          template <template<typename, bool...> class C,
                    typename = char,
                    int = 0,
                    bool Q = false,
                    class... Ts> class Foo final {};
          template <template<typename, bool...> class T>
          @sym[F$oo]<T> foo;
          )cpp",
         [](HoverInfo& hi) {
             hi.namespace_scope = "";
             hi.name = "Foo";
             hi.kind = SymbolKind::Class;
             hi.definition =
                 R"cpp(template <template <typename, bool...> class C, typename = char, int = 0,
          bool Q = false, class... Ts>
class Foo final {})cpp";
             hi.template_parameters = {
                 {                                      {"template <typename, bool...> class"},                                      std::string("C"),                                      std::nullopt},
                 {                                      {"typename"},                                      std::nullopt,                                std::string("char")},
                 {                                {"int"},                                      std::nullopt,                                      std::string("0")},
                 {                                {"bool"},                                std::string("Q"),           std::string("false")},
                 { {"class..."},   std::string("Ts"),                                std::nullopt},
             };
         }                    },
        // Function template
        {R"cpp(
          template <template<typename, bool...> class C,
                    typename = char,
                    int = 0,
                    bool Q = false,
                    class... Ts> void foo();
          template<typename, bool...> class Foo;

          void bar() {
            @sym[fo$o]<Foo>();
          }
          )cpp",
         [](HoverInfo& hi) {
             hi.namespace_scope = "";
             hi.name = "foo";
             hi.kind = SymbolKind::Function;
             hi.definition = "template <> void foo<Foo, char, 0, false, <>>()";
             hi.return_type = "void";
             hi.type = "void ()";
             hi.parameters.emplace();
         }          },
        // Function decl
        {R"cpp(
          template<typename, bool...> class Foo {};
          Foo<bool, true, false> foo(int, bool T = false);

          void bar() {
            @sym[fo$o](3);
          }
          )cpp",
         [](HoverInfo& hi) {
             hi.namespace_scope = "";
             hi.name = "foo";
             hi.kind = SymbolKind::Function;
             hi.definition = "Foo<bool, true, false> foo(int, bool T = false)";
             hi.return_type = "Foo<bool, true, false>";
             hi.type = "Foo<bool, true, false> (int, bool)";
             hi.parameters = {
                 {{"int"}, std::nullopt, std::nullopt},
                 {{"bool"}, std::string("T"), std::string("false")},
             };
         }          },
        // Partially-specialized class template. (formerly type-parameter-0-0)
        {R"cpp(
        template <typename T> class X;
        template <typename T> class @sym[$X]<T*> {};
        )cpp",
         [](HoverInfo& hi) {
             hi.name = "X<T *>";
             hi.namespace_scope = "";
             hi.kind = SymbolKind::Class;
             hi.definition = "template <typename T> class X<T *> {}";
         }          },
        // Constructor of partially-specialized class template
        {R"cpp(
          template<typename, typename=void> struct X;
          template<typename T> struct X<T*>{ @sym[$X](); };
          )cpp",
         [](HoverInfo& hi) {
             hi.namespace_scope = "";
             hi.name = "X";
             hi.local_scope = "X<T *>::";
             hi.kind = SymbolKind::Method;
             hi.definition = "X()";
             hi.parameters.emplace();
             hi.access_specifier = "public";
         }          },
        {"class X { @sym[$~]X(); };",
         [](HoverInfo& hi) {
             hi.namespace_scope = "";
             hi.name = "~X";
             hi.local_scope = "X::";
             hi.kind = SymbolKind::Method;
             hi.definition = "~X()";
             hi.parameters.emplace();
             hi.access_specifier = "private";
         }},
        {"class X { @sym[op$erator] int(); };",
         [](HoverInfo& hi) {
             hi.namespace_scope = "";
             hi.name = "operator int";
             hi.local_scope = "X::";
             hi.kind = SymbolKind::Method;
             hi.definition = "operator int()";
             hi.parameters.emplace();
             hi.access_specifier = "private";
         }          },
        {"class X { operator @sym[$X](); };",
         [](HoverInfo& hi) {
             hi.namespace_scope = "";
             hi.name = "X";
             hi.kind = SymbolKind::Class;
             hi.definition = "class X {}";
         }                    },
        // Falls back to primary template, when the type is not instantiated.
        {R"cpp(
          // comment from primary
          template <typename T> class Foo {};
          // comment from specialization
          template <typename T> class Foo<T*> {};
          void foo() {
            @sym[Fo$o]<int*> *x = nullptr;
          }
          )cpp",
         [](HoverInfo& hi) {
             hi.name = "Foo<int *>";
             hi.kind = SymbolKind::Class;
             hi.namespace_scope = "";
             hi.definition = "template <> class Foo<int *>";
             hi.documentation = "comment from primary";
         }          },
        // Var template decl
        {R"cpp(
          using m_int = int;

          template <int Size> m_int @sym[$arr][Size];
         )cpp",
         [](HoverInfo& hi) {
             hi.name = "arr";
             hi.kind = SymbolKind::Variable;
             hi.type = {"m_int[Size]", "int[Size]"};
             hi.namespace_scope = "";
             hi.definition = "template <int Size> m_int arr[Size]";
             hi.template_parameters = {
                 {{"int"}, {"Size"}, std::nullopt}};
         }},
        // Var template decl specialization
        {R"cpp(
          using m_int = int;

          template <int Size> m_int arr[Size];

          template <> m_int @sym[$arr]<4>[4];
         )cpp",
         [](HoverInfo& hi) {
             hi.name = "arr<4>";
             hi.kind = SymbolKind::Variable;
             hi.type = {"m_int[4]", "int[4]"};
             hi.namespace_scope = "";
             hi.definition = "m_int arr[4]";
         }          },
        // Canonical type
        {R"cpp(
          template<typename T>
          struct TestHover {
            using Type = T;
          };

          void code() {
            TestHover<int>::Type @sym[$a];
          }
         )cpp",
         [](HoverInfo& hi) {
             hi.name = "a";
             hi.namespace_scope = "";
             hi.local_scope = "code::";
             hi.definition = "TestHover<int>::Type a";
             hi.kind = SymbolKind::Variable;
             hi.type = {"TestHover<int>::Type", "int"};
         }          },
        // Canonical template type
        {R"cpp(
          template<typename T>
          void @sym[$foo](T arg) {}
         )cpp",
         [](HoverInfo& hi) {
             hi.name = "foo";
             hi.kind = SymbolKind::Function;
             hi.namespace_scope = "";
             hi.definition = "template <typename T> void foo(T arg)";
             hi.type = "void (T)";
             hi.return_type = "void";
             hi.parameters = {
                 {{"T"}, std::string("arg"), std::nullopt}};
             hi.template_parameters = {
                 {{"typename"}, std::string("T"), std::nullopt}};
         }          },
        // TypeAlias Template
        {R"cpp(
          template<typename T>
          using @sym[$alias] = T;
         )cpp",
         [](HoverInfo& hi) {
             hi.name = "alias";
             hi.namespace_scope = "";
             hi.local_scope = "";
             hi.kind = SymbolKind::Type;
             hi.definition = "template <typename T> using alias = T";
             hi.type = "T";
             hi.template_parameters = {
                 {{"typename"}, std::string("T"), std::nullopt}};
         }          },
        // TypeAlias Template
        {R"cpp(
          template<typename T>
          using A = T;

          template<typename T>
          using @sym[$AA] = A<T>;
         )cpp",
         [](HoverInfo& hi) {
             hi.name = "AA";
             hi.namespace_scope = "";
             hi.local_scope = "";
             hi.kind = SymbolKind::Type;
             hi.definition = "template <typename T> using AA = A<T>";
             hi.type = {"A<T>", "T"};
             hi.template_parameters = {
                 {{"typename"}, std::string("T"), std::nullopt}};
         }          },
        // Constant array
        {R"cpp(
          using m_int = int;

          m_int @sym[$arr][10];
         )cpp",
         [](HoverInfo& hi) {
             hi.name = "arr";
             hi.namespace_scope = "";
             hi.local_scope = "";
             hi.kind = SymbolKind::Variable;
             hi.definition = "m_int arr[10]";
             hi.type = {"m_int[10]", "int[10]"};
         }          },
        // Incomplete array
        {R"cpp(
          using m_int = int;

          extern m_int @sym[$arr][];
         )cpp",
         [](HoverInfo& hi) {
             hi.name = "arr";
             hi.namespace_scope = "";
             hi.local_scope = "";
             hi.kind = SymbolKind::Variable;
             hi.definition = "extern m_int arr[]";
             hi.type = {"m_int[]", "int[]"};
         }                    },
        // Dependent size array
        {R"cpp(
          using m_int = int;

          template<int Size>
          struct Test {
            m_int @sym[$arr][Size];
          };
         )cpp",
         [](HoverInfo& hi) {
             hi.name = "arr";
             hi.namespace_scope = "";
             hi.local_scope = "Test<Size>::";
             hi.access_specifier = "public";
             hi.kind = SymbolKind::Field;
             hi.definition = "m_int arr[Size]";
             hi.type = {"m_int[Size]", "int[Size]"};
         }},
    };
    check_cases(cases);
}

TEST_CASE(StructuredLambdas) {
    HoverCase cases[] = {
        // Pointers to lambdas
        {R"cpp(
        void foo() {
          auto lamb = [](int T, bool B) -> bool { return T && B; };
          auto *b = &lamb;
          auto *@sym[$c] = &b;
        }
        )cpp",
         [](HoverInfo& hi) {
             hi.namespace_scope = "";
             hi.local_scope = "foo::";
             hi.name = "c";
             hi.kind = SymbolKind::Variable;
             hi.definition = "auto *c = &b";
             hi.type = "(lambda) **";
             hi.return_type = "bool";
             hi.parameters = {
                 {                     {"int"},           std::string("T"), std::nullopt},
                 {                     {"bool"},           std::string("B"), std::nullopt},
             };
         }},
        // Lambda parameter with decltype reference
        {R"cpp(
        auto lamb = [](int T, bool B) -> bool { return T && B; };
        void foo(decltype(lamb)& bar) {
          @sym[ba$r](0, false);
        }
        )cpp",
         [](HoverInfo& hi) {
             hi.namespace_scope = "";
             hi.local_scope = "foo::";
             hi.name = "bar";
             hi.kind = SymbolKind::Parameter;
             hi.definition = "decltype(lamb) &bar";
             hi.type = {"decltype(lamb) &", "(lambda) &"};
             hi.return_type = "bool";
             hi.parameters = {
                 {{"int"}, std::string("T"), std::nullopt},
                 {{"bool"}, std::string("B"), std::nullopt},
             };
         }},
        // Lambda parameter with decltype
        {R"cpp(
        auto lamb = [](int T, bool B) -> bool { return T && B; };
        void foo(decltype(lamb) bar) {
          @sym[ba$r](0, false);
        }
        )cpp",
         [](HoverInfo& hi) {
             hi.namespace_scope = "";
             hi.local_scope = "foo::";
             hi.name = "bar";
             hi.kind = SymbolKind::Parameter;
             hi.definition = "decltype(lamb) bar";
             hi.type = "class (lambda)";
             hi.return_type = "bool";
             hi.parameters = {
                 {{"int"}, std::string("T"), std::nullopt},
                 {{"bool"}, std::string("B"), std::nullopt},
             };
             hi.value = "false";
         }},
        // Lambda variable
        {R"cpp(
        void foo() {
          int bar = 5;
          auto lamb = [&bar](int T, bool B) -> bool { return T && B && bar; };
          bool res = @sym[lam$b](bar, false);
        }
        )cpp",
         [](HoverInfo& hi) {
             hi.namespace_scope = "";
             hi.local_scope = "foo::";
             hi.name = "lamb";
             hi.kind = SymbolKind::Variable;
             hi.definition = "auto lamb = [&bar](int T, bool B) -> bool {}";
             hi.type = "class (lambda)";
             hi.return_type = "bool";
             hi.parameters = {
                 {{"int"}, std::string("T"), std::nullopt},
                 {{"bool"}, std::string("B"), std::nullopt},
             };
         }},
        // Local variable in lambda
        {R"cpp(
        void foo() {
          auto lamb = []{int @sym[te$st];};
        }
        )cpp",
         [](HoverInfo& hi) {
             hi.namespace_scope = "";
             hi.local_scope = "foo::(anonymous class)::operator()::";
             hi.name = "test";
             hi.kind = SymbolKind::Variable;
             hi.definition = "int test";
             hi.type = "int";
         }},
    };
    check_cases(cases);
}

TEST_CASE(StructuredAutoDeduction) {
    HoverCase cases[] = {
        // auto on structured bindings
        {R"cpp(
        void foo() {
          struct S { int x; float y; };
          @sym[au$to] [x, y] = S();
        }
        )cpp",
         [](HoverInfo& hi) {
             hi.name = "auto";
             hi.kind = SymbolKind::Type;
             hi.definition = "S";
         }},
        // undeduced auto
        {R"cpp(
        template<typename T>
        void foo() {
          @sym[au$to] x = T{};
        }
        )cpp",
         [](HoverInfo& hi) {
             hi.name = "auto";
             hi.kind = SymbolKind::Type;
             hi.definition = "/* not deduced */";
         }},
        // constrained auto
        {R"cpp(
        template <class T> concept F = true;
        F @sym[au$to] x = 1;
        )cpp",
         [](HoverInfo& hi) {
             hi.name = "auto";
             hi.kind = SymbolKind::Type;
             hi.definition = "int";
         }},
        // auto on lambda
        {R"cpp(
        void foo() {
          @sym[au$to] lamb = []{};
        }
        )cpp",
         [](HoverInfo& hi) {
             hi.name = "auto";
             hi.kind = SymbolKind::Type;
             hi.definition = "class(lambda)";
         }},
        // auto on template instantiation
        {R"cpp(
        template<typename T> class Foo{};
        void foo() {
          @sym[au$to] x = Foo<int>();
        }
        )cpp",
         [](HoverInfo& hi) {
             hi.name = "auto";
             hi.kind = SymbolKind::Type;
             hi.definition = "Foo<int>";
         }},
        // auto on specialized template
        {R"cpp(
        template<typename T> class Foo{};
        template<> class Foo<int>{};
        void foo() {
          @sym[au$to] x = Foo<int>();
        }
        )cpp",
         [](HoverInfo& hi) {
             hi.name = "auto";
             hi.kind = SymbolKind::Type;
             hi.definition = "Foo<int>";
         }},
    };
    check_cases(cases);
}

TEST_CASE(StructuredConcepts) {
    HoverCase cases[] = {
        {R"cpp(
        template <class T> concept F = true;
        @sym[$F] auto x = 1;
        )cpp",
         [](HoverInfo& hi) {
             hi.namespace_scope = "";
             hi.name = "F";
             hi.kind = SymbolKind::Concept;
             hi.definition = "template <class T>\nconcept F = true";
         }},
        // constrained template parameter
        {R"cpp(
        template<class T> concept Fooable = true;
        template<@sym[Foo$able] T>
        void bar(T t) {}
        )cpp",
         [](HoverInfo& hi) {
             hi.namespace_scope = "";
             hi.name = "Fooable";
             hi.kind = SymbolKind::Concept;
             hi.definition = "template <class T>\nconcept Fooable = true";
         }},
        {R"cpp(
        template<class T> concept Fooable = true;
        template<Fooable @sym[T$T]>
        void bar(TT t) {}
        )cpp",
         [](HoverInfo& hi) {
             hi.name = "TT";
             hi.type = "class";
             hi.access_specifier = "public";
             hi.namespace_scope = "";
             hi.local_scope = "bar::";
             hi.kind = SymbolKind::Type;
             hi.definition = "Fooable TT";
         }},
        {R"cpp(
        template<class T> concept Fooable = true;
        void bar(@sym[Foo$able] auto t) {}
        )cpp",
         [](HoverInfo& hi) {
             hi.namespace_scope = "";
             hi.name = "Fooable";
             hi.kind = SymbolKind::Concept;
             hi.definition = "template <class T>\nconcept Fooable = true";
         }},
        // concept reference
        {R"cpp(
        template<class T> concept Fooable = true;
        auto X = @sym[Fooa$ble]<int>;
        )cpp",
         [](HoverInfo& hi) {
             hi.namespace_scope = "";
             hi.name = "Fooable";
             hi.kind = SymbolKind::Concept;
             hi.definition = "template <class T>\nconcept Fooable = true";
             hi.value = "true";
         }},
    };
    check_cases(cases);
}

TEST_CASE(StructuredValues) {
    HoverCase cases[] = {
        // constexprs
        {R"cpp(
        constexpr int add(int a, int b) { return a + b; }
        int @sym[b$ar] = add(1, 2);
        )cpp",
         [](HoverInfo& hi) {
             hi.name = "bar";
             hi.definition = "int bar = add(1, 2)";
             hi.kind = SymbolKind::Variable;
             hi.type = "int";
             hi.namespace_scope = "";
             hi.value = "3";
         }},
        {R"cpp(
        int @sym[b$ar] = sizeof(char);
        )cpp",
         [](HoverInfo& hi) {
             hi.name = "bar";
             hi.definition = "int bar = sizeof(char)";
             hi.kind = SymbolKind::Variable;
             hi.type = "int";
             hi.namespace_scope = "";
             hi.value = "1";
         }},
        {R"cpp(
        template<int a, int b> struct Add {
          static constexpr int result = a + b;
        };
        int @sym[ba$r] = Add<1, 2>::result;
        )cpp",
         [](HoverInfo& hi) {
             hi.name = "bar";
             hi.definition = "int bar = Add<1, 2>::result";
             hi.kind = SymbolKind::Variable;
             hi.type = "int";
             hi.namespace_scope = "";
             hi.value = "3";
         }},
        {R"cpp(
        enum Color { RED = -123, GREEN = 5, };
        Color x = @sym[GR$EEN];
       )cpp",
         [](HoverInfo& hi) {
             hi.name = "GREEN";
             hi.namespace_scope = "";
             hi.local_scope = "Color::";
             hi.definition = "GREEN = 5";
             hi.kind = SymbolKind::EnumMember;
             hi.type = "enum Color";
             hi.value = "5";  // Numeric on the enumerator name, no hex as small.
         }},
        {R"cpp(
        enum Color { RED = -123, GREEN = 5, };
        Color x = RED;
        Color y = @sym[$x];
       )cpp",
         [](HoverInfo& hi) {
             hi.name = "x";
             hi.namespace_scope = "";
             hi.definition = "Color x = RED";
             hi.kind = SymbolKind::Variable;
             hi.type = "Color";
             hi.value = "RED (0xffffff85)";  // Symbolic on an expression.
         }},
        {R"cpp(
        template<int a, int b> struct Add {
          static constexpr int result = a + b;
        };
        int bar = Add<1, 2>::@sym[resu$lt];
        )cpp",
         [](HoverInfo& hi) {
             hi.name = "result";
             hi.definition = "static constexpr int result = a + b";
             hi.kind = SymbolKind::Variable;
             hi.type = "const int";
             hi.namespace_scope = "";
             hi.local_scope = "Add<1, 2>::";
             hi.value = "3";
             hi.access_specifier = "public";
         }},
        {R"cpp(
        using my_int = int;
        constexpr my_int answer() { return 40 + 2; }
        int x = @sym[ans$wer]();
        )cpp",
         [](HoverInfo& hi) {
             hi.name = "answer";
             hi.definition = "constexpr my_int answer()";
             hi.kind = SymbolKind::Function;
             hi.type = {"my_int ()", "int ()"};
             hi.return_type = {"my_int", "int"};
             hi.parameters.emplace();
             hi.namespace_scope = "";
             hi.value = "42 (0x2a)";
         }},
        {R"cpp(
        const char *@sym[ba$r] = "1234";
        )cpp",
         [](HoverInfo& hi) {
             hi.name = "bar";
             hi.definition = "const char *bar = \"1234\"";
             hi.kind = SymbolKind::Variable;
             hi.type = "const char *";
             hi.namespace_scope = "";
             hi.value = "&\"1234\"[0]";
         }},
        {R"cpp(// Should not crash
        template <typename T>
        struct Tmpl {
          Tmpl(int name);
        };

        template <typename A>
        void boom(int name) {
          new Tmpl<A>(@sym[na$me]);
        })cpp",
         [](HoverInfo& hi) {
             hi.name = "name";
             hi.definition = "int name";
             hi.kind = SymbolKind::Parameter;
             hi.type = "int";
             hi.namespace_scope = "";
             hi.local_scope = "boom::";
         }},
        {R"cpp(// Should not print inline or anon namespaces.
          namespace ns {
            inline namespace in_ns {
              namespace a {
                namespace {
                  namespace b {
                    inline namespace in_ns2 {
                      class Foo {};
                    } // in_ns2
                  } // b
                } // anon
              } // a
            } // in_ns
          } // ns
          void foo() {
            ns::a::b::@sym[F$oo] x;
            (void)x;
          }
          )cpp",
         [](HoverInfo& hi) {
             hi.name = "Foo";
             hi.kind = SymbolKind::Class;
             hi.namespace_scope = "ns::a::b::";
             hi.definition = "class Foo {}";
         }},
        {R"cpp(
          template <typename T> class Foo {};
          class X;
          void foo() {
            @sym[$auto] x = Foo<X>();
          }
          )cpp",
         [](HoverInfo& hi) {
             hi.name = "auto";
             hi.kind = SymbolKind::Type;
             hi.definition = "Foo<X>";
         }},
    };
    check_cases(cases);
}

TEST_CASE(TemplateParamHovers) {
    HoverCase cases[] = {
        // Template Type Parameter
        {R"cpp(
          template <typename @sym[$T] = int> void foo();
          )cpp",
         [](HoverInfo& hi) {
             hi.name = "T";
             hi.kind = SymbolKind::Type;
             hi.namespace_scope = "";
             hi.definition = "typename T = int";
             hi.local_scope = "foo::";
             hi.type = "typename";
             hi.access_specifier = "public";
         }},
        // TemplateTemplate Type Parameter
        {R"cpp(
          template <template<typename> class @sym[$T]> void foo();
          )cpp",
         [](HoverInfo& hi) {
             hi.name = "T";
             hi.kind = SymbolKind::Type;
             hi.namespace_scope = "";
             hi.definition = "template <typename> class T";
             hi.local_scope = "foo::";
             hi.type = "template <typename> class";
             hi.access_specifier = "public";
         }},
        // NonType Template Parameter
        {R"cpp(
          template <int @sym[$T] = 5> void foo();
          )cpp",
         [](HoverInfo& hi) {
             hi.name = "T";
             hi.kind = SymbolKind::Variable;
             hi.namespace_scope = "";
             hi.definition = "int T = 5";
             hi.local_scope = "foo::";
             hi.type = "int";
             hi.access_specifier = "public";
         }},
    };
    check_cases(cases);
}

TEST_CASE(GetterSetterDocs) {
    HoverCase cases[] = {
        // Getter
        {R"cpp(
          struct X { int Y; float @sym[$y]() { return Y; } };
          )cpp",
         [](HoverInfo& hi) {
             hi.name = "y";
             hi.kind = SymbolKind::Method;
             hi.namespace_scope = "";
             hi.definition = "float y()";
             hi.local_scope = "X::";
             hi.documentation = "Trivial accessor for `Y`.";
             hi.type = "float ()";
             hi.return_type = "float";
             hi.parameters.emplace();
             hi.access_specifier = "public";
         }          },
        // Setter
        {R"cpp(
          struct X { int Y; void @sym[$setY](float v) { Y = v; } };
          )cpp",
         [](HoverInfo& hi) {
             hi.name = "setY";
             hi.kind = SymbolKind::Method;
             hi.namespace_scope = "";
             hi.definition = "void setY(float v)";
             hi.local_scope = "X::";
             hi.documentation = "Trivial setter for `Y`.";
             hi.type = "void (float)";
             hi.return_type = "void";
             hi.parameters = {
                 {       {"float"}, std::string("v"), std::nullopt}};
             hi.access_specifier = "public";
         }                    },
        // Setter (builder)
        {R"cpp(
          struct X { int Y; X& @sym[$setY](float v) { Y = v; return *this; } };
          )cpp",
         [](HoverInfo& hi) {
             hi.name = "setY";
             hi.kind = SymbolKind::Method;
             hi.namespace_scope = "";
             hi.definition = "X &setY(float v)";
             hi.local_scope = "X::";
             hi.documentation = "Trivial setter for `Y`.";
             hi.type = "X &(float)";
             hi.return_type = "X &";
             hi.parameters = {
                 {{"float"}, std::string("v"), std::nullopt}};
             hi.access_specifier = "public";
         }},
        // Setter (move)
        {R"cpp(
          namespace std { template<typename T> T&& move(T&& t); }
          struct X { int Y; void @sym[$setY](float v) { Y = std::move(v); } };
          )cpp",
         [](HoverInfo& hi) {
             hi.name = "setY";
             hi.kind = SymbolKind::Method;
             hi.namespace_scope = "";
             hi.definition = "void setY(float v)";
             hi.local_scope = "X::";
             hi.documentation = "Trivial setter for `Y`.";
             hi.type = "void (float)";
             hi.return_type = "void";
             hi.parameters = {
                 {{"float"}, std::string("v"), std::nullopt}};
             hi.access_specifier = "public";
         }                    },
    };
    check_cases(cases);
}

TEST_CASE(StructuredNoCrash) {
    HoverCase cases[] = {
        // Field type initializer.
        {R"cpp(
          struct X { int x = 2; };
          X @sym[$x];
          )cpp",
         [](HoverInfo& hi) {
             hi.name = "x";
             hi.kind = SymbolKind::Variable;
             hi.namespace_scope = "";
             hi.definition = "X x";
             hi.type = "X";
         }},
        // Don't crash on null types.
        {R"cpp(auto [@sym[$x]] = 1; /*error-ok*/)cpp",
         [](HoverInfo& hi) {
             hi.name = "x";
             hi.kind = SymbolKind::Variable;
             hi.namespace_scope = "";
             hi.definition = "";
             hi.type = "NULL TYPE";
             // Bindings are in theory public members of an anonymous struct.
             hi.access_specifier = "public";
         }},
        // Don't crash on invalid decl with invalid init expr.
        {R"cpp(
          Unknown @sym[$abc] = invalid;
          // error-ok
          )cpp",
         [](HoverInfo& hi) {
             hi.name = "abc";
             hi.kind = SymbolKind::Variable;
             hi.namespace_scope = "";
             hi.definition = "int abc";
             hi.type = "int";
             hi.access_specifier = "public";
         }},
        // Don't crash on invalid decl
        {R"cpp(
        // error-ok
        struct Foo {
          Bar @sym[x$x];
        };)cpp",
         [](HoverInfo& hi) {
             hi.name = "xx";
             hi.kind = SymbolKind::Field;
             hi.namespace_scope = "";
             hi.definition = "int xx";
             hi.local_scope = "Foo::";
             hi.type = "int";
             hi.access_specifier = "public";
         }},
        {R"cpp(
        // error-ok
        struct Foo {
          Bar xx;
          int @sym[y$y];
        };)cpp",
         [](HoverInfo& hi) {
             hi.name = "yy";
             hi.kind = SymbolKind::Field;
             hi.namespace_scope = "";
             hi.definition = "int yy";
             hi.local_scope = "Foo::";
             hi.type = "int";
             hi.access_specifier = "public";
         }},
        // No crash on InitListExpr.
        {R"cpp(
          struct Foo {
            int a[10];
          };
          constexpr Foo k2 = {
            @sym[${]1} // FIXME: why the hover range is 1 character?
          };
         )cpp",
         [](HoverInfo& hi) {
             hi.name = "expression";
             hi.kind = SymbolKind::Invalid;
             hi.type = "int[10]";
             hi.value = "{1}";
         }},
    };
    check_cases(cases);
}

TEST_CASE(StructuredCalleeArgs) {
    HoverCase cases[] = {
        // Extra info for function call.
        {R"cpp(
          void fun(int arg_a, int &arg_b) {};
          void code() {
            int a = 1, b = 2;
            fun(a, @sym[$b]);
          }
          )cpp",
         [](HoverInfo& hi) {
             hi.name = "b";
             hi.kind = SymbolKind::Variable;
             hi.namespace_scope = "";
             hi.definition = "int b = 2";
             hi.local_scope = "code::";
             hi.value = "2";
             hi.type = "int";
             hi.callee_arg_info.emplace();
             hi.callee_arg_info->name = "arg_b";
             hi.callee_arg_info->type = PrintedType("int &");
             hi.call_pass_type = PassType{PassMode::Ref, false};
         }},
        // make_unique-like function call
        {R"cpp(
          struct Foo {
            explicit Foo(int arg_a) {}
          };
          template<class T, class... Args>
          T make(Args&&... args)
          {
              return T(args...);
          }

          void code() {
            int a = 1;
            auto foo = make<Foo>(@sym[$a]);
          }
          )cpp",
         [](HoverInfo& hi) {
             hi.name = "a";
             hi.kind = SymbolKind::Variable;
             hi.namespace_scope = "";
             hi.definition = "int a = 1";
             hi.local_scope = "code::";
             hi.value = "1";
             hi.type = "int";
             hi.callee_arg_info.emplace();
             hi.callee_arg_info->name = "arg_a";
             hi.callee_arg_info->type = PrintedType("int");
             hi.call_pass_type = PassType{PassMode::Value, false};
         }},
        {R"cpp(
          void foobar(const float &arg);
          int main() {
            int a = 0;
            foobar(@sym[$a]);
          }
          )cpp",
         [](HoverInfo& hi) {
             hi.name = "a";
             hi.kind = SymbolKind::Variable;
             hi.namespace_scope = "";
             hi.definition = "int a = 0";
             hi.local_scope = "main::";
             hi.value = "0";
             hi.type = "int";
             hi.callee_arg_info.emplace();
             hi.callee_arg_info->name = "arg";
             hi.callee_arg_info->type = PrintedType("const float &");
             hi.call_pass_type = PassType{PassMode::Value, true};
         }},
        {R"cpp(
          struct Foo {
            explicit Foo(const float& arg) {}
          };
          int main() {
            int a = 0;
            Foo foo(@sym[$a]);
          }
          )cpp",
         [](HoverInfo& hi) {
             hi.name = "a";
             hi.kind = SymbolKind::Variable;
             hi.namespace_scope = "";
             hi.definition = "int a = 0";
             hi.local_scope = "main::";
             hi.value = "0";
             hi.type = "int";
             hi.callee_arg_info.emplace();
             hi.callee_arg_info->name = "arg";
             hi.callee_arg_info->type = PrintedType("const float &");
             hi.call_pass_type = PassType{PassMode::Value, true};
         }},
        // Literal passed to function call
        {R"cpp(
          void fun(int arg_a, const int &arg_b) {};
          void code() {
            int a = 1;
            fun(a, @sym[$2]);
          }
          )cpp",
         [](HoverInfo& hi) {
             hi.name = "literal";
             hi.kind = SymbolKind::Invalid;
             hi.callee_arg_info.emplace();
             hi.callee_arg_info->name = "arg_b";
             hi.callee_arg_info->type = PrintedType("const int &");
             hi.call_pass_type = PassType{PassMode::ConstRef, false};
         }},
        // Expression passed to function call
        {R"cpp(
          void fun(int arg_a, const int &arg_b) {};
          void code() {
            int a = 1;
            fun(a, 1 @sym[$+] 2);
          }
          )cpp",
         [](HoverInfo& hi) {
             hi.name = "expression";
             hi.kind = SymbolKind::Invalid;
             hi.type = "int";
             hi.value = "3";
             hi.callee_arg_info.emplace();
             hi.callee_arg_info->name = "arg_b";
             hi.callee_arg_info->type = PrintedType("const int &");
             hi.call_pass_type = PassType{PassMode::ConstRef, false};
         }},
        {R"cpp(
        int add(int lhs, int rhs);
        int main() {
          add(1 @sym[$+] 2, 3);
        }
        )cpp",
         [](HoverInfo& hi) {
             hi.name = "expression";
             hi.kind = SymbolKind::Invalid;
             hi.type = "int";
             hi.value = "3";
             hi.callee_arg_info.emplace();
             hi.callee_arg_info->name = "lhs";
             hi.callee_arg_info->type = PrintedType("int");
             hi.call_pass_type = PassType{PassMode::Value, false};
         }},
        {R"cpp(
        void foobar(const float &arg);
        int main() {
          foobar(@sym[$0]);
        }
        )cpp",
         [](HoverInfo& hi) {
             hi.name = "literal";
             hi.kind = SymbolKind::Invalid;
             hi.callee_arg_info.emplace();
             hi.callee_arg_info->name = "arg";
             hi.callee_arg_info->type = PrintedType("const float &");
             hi.call_pass_type = PassType{PassMode::Value, true};
         }},
        // Extra info for method call.
        {R"cpp(
          class C {
           public:
            void fun(int arg_a = 3, int arg_b = 4) {}
          };
          void code() {
            int a = 1, b = 2;
            C c;
            c.fun(@sym[$a], b);
          }
          )cpp",
         [](HoverInfo& hi) {
             hi.name = "a";
             hi.kind = SymbolKind::Variable;
             hi.namespace_scope = "";
             hi.definition = "int a = 1";
             hi.local_scope = "code::";
             hi.value = "1";
             hi.type = "int";
             hi.callee_arg_info.emplace();
             hi.callee_arg_info->name = "arg_a";
             hi.callee_arg_info->type = PrintedType("int");
             hi.callee_arg_info->default_value = "3";
             hi.call_pass_type = PassType{PassMode::Value, false};
         }},
        {R"cpp(
          struct Foo {
            Foo(const int &);
          };
          void foo(Foo);
          void bar() {
            const int x = 0;
            foo(@sym[$x]);
          }
       )cpp",
         [](HoverInfo& hi) {
             hi.name = "x";
             hi.kind = SymbolKind::Variable;
             hi.namespace_scope = "";
             hi.definition = "const int x = 0";
             hi.local_scope = "bar::";
             hi.value = "0";
             hi.type = "const int";
             hi.callee_arg_info.emplace();
             hi.callee_arg_info->type = PrintedType("Foo");
             hi.call_pass_type = PassType{PassMode::ConstRef, true};
         }},
    };
    check_cases(cases);
}

TEST_CASE(ExprAndLiterals) {
    HoverCase cases[] = {
        {"auto x = @sym['$A']; // character literal",
         [](HoverInfo& hi) {
             hi.name = "expression";
             hi.kind = SymbolKind::Invalid;
             hi.type = "char";
             hi.value = "65 (0x41)";
         }},
        {R"cpp(auto s = @sym[$"Hello, world!"]; // string literal)cpp",
         [](HoverInfo& hi) {
             hi.name = "string-literal";
             hi.kind = SymbolKind::Invalid;
             hi.size = 112;
             hi.type = "const char[14]";
         }},
        {R"cpp(// sizeof expr
          void foo() {
            (void)@sym[size$of](char);
          })cpp",
         [](HoverInfo& hi) {
             hi.name = "expression";
             hi.kind = SymbolKind::Invalid;
             hi.type = "unsigned long";
             hi.value = "1";
         }},
        {R"cpp(// alignof expr
          void foo() {
            (void)@sym[align$of](char);
          })cpp",
         [](HoverInfo& hi) {
             hi.name = "expression";
             hi.kind = SymbolKind::Invalid;
             hi.type = "unsigned long";
             hi.value = "1";
         }},
    };
    check_cases(cases);
}

TEST_CASE(AllVariables) {
    HoverCase cases[] = {
        {R"cpp(// Local variable
            int main() {
              int bonjour;
              @sym[$bonjour] = 2;
              int test1 = bonjour;
            }
          )cpp",
         [](HoverInfo& hi) {
             hi.name = "bonjour";
             hi.kind = SymbolKind::Variable;
             hi.namespace_scope = "";
             hi.local_scope = "main::";
             hi.type = "int";
             hi.definition = "int bonjour";
         }},
        {R"cpp(// Local variable in method
            struct s {
              void method() {
                int bonjour;
                @sym[$bonjour] = 2;
              }
            };
          )cpp",
         [](HoverInfo& hi) {
             hi.name = "bonjour";
             hi.kind = SymbolKind::Variable;
             hi.namespace_scope = "";
             hi.local_scope = "s::method::";
             hi.type = "int";
             hi.definition = "int bonjour";
         }},
        {R"cpp(// Global variable
            static int hey = 10;
            void foo() {
              @sym[he$y]++;
            }
          )cpp",
         [](HoverInfo& hi) {
             hi.name = "hey";
             hi.kind = SymbolKind::Variable;
             hi.namespace_scope = "";
             hi.type = "int";
             hi.definition = "static int hey = 10";
             hi.documentation = "Global variable";
             // FIXME: Value shouldn't be set in this case
             hi.value = "10 (0xa)";
         }},
        {R"cpp(// Global variable in namespace
            namespace ns1 {
              static long long hey = -36637162602497;
            }
            void foo() {
              ns1::@sym[he$y]++;
            }
          )cpp",
         [](HoverInfo& hi) {
             hi.name = "hey";
             hi.kind = SymbolKind::Variable;
             hi.namespace_scope = "ns1::";
             hi.type = "long long";
             hi.definition = "static long long hey = -36637162602497";
             hi.value = "-36637162602497 (0xffffdeadbeefffff)";  // needs 64 bits
         }},
        {R"cpp(// Anonymous namespace
            namespace ns {
              namespace {
                int foo;
              } // anonymous namespace
            } // namespace ns
            int main() { ns::@sym[f$oo]++; }
          )cpp",
         [](HoverInfo& hi) {
             hi.name = "foo";
             hi.kind = SymbolKind::Variable;
             hi.namespace_scope = "ns::";
             hi.type = "int";
             hi.definition = "int foo";
         }},
    };
    check_cases(cases);
}

TEST_CASE(AllTagDecls) {
    HoverCase cases[] = {
        {R"cpp(// Struct
            namespace ns1 {
              struct MyClass {};
            } // namespace ns1
            int main() {
              ns1::@sym[My$Class]* Params;
            }
          )cpp",
         [](HoverInfo& hi) {
             hi.name = "MyClass";
             hi.kind = SymbolKind::Struct;
             hi.namespace_scope = "ns1::";
             hi.definition = "struct MyClass {}";
         }},
        {R"cpp(// Class
            namespace ns1 {
              class MyClass {};
            } // namespace ns1
            int main() {
              ns1::@sym[My$Class]* Params;
            }
          )cpp",
         [](HoverInfo& hi) {
             hi.name = "MyClass";
             hi.kind = SymbolKind::Class;
             hi.namespace_scope = "ns1::";
             hi.definition = "class MyClass {}";
         }},
        {R"cpp(// Union
            namespace ns1 {
              union MyUnion { int x; int y; };
            } // namespace ns1
            int main() {
              ns1::@sym[My$Union] Params;
            }
          )cpp",
         [](HoverInfo& hi) {
             hi.name = "MyUnion";
             hi.kind = SymbolKind::Union;
             hi.namespace_scope = "ns1::";
             hi.definition = "union MyUnion {}";
         }},
        {R"cpp(// Forward class declaration
            class Foo;
            class Foo {};
            @sym[F$oo]* foo();
          )cpp",
         [](HoverInfo& hi) {
             hi.name = "Foo";
             hi.kind = SymbolKind::Class;
             hi.namespace_scope = "";
             hi.definition = "class Foo {}";
             hi.documentation = "Forward class declaration";
         }},
        {R"cpp(// Enum declaration
            enum Hello {
              ONE, TWO, THREE,
            };
            void foo() {
              @sym[Hel$lo] hello = ONE;
            }
          )cpp",
         [](HoverInfo& hi) {
             hi.name = "Hello";
             hi.kind = SymbolKind::Enum;
             hi.namespace_scope = "";
             hi.definition = "enum Hello {}";
             hi.documentation = "Enum declaration";
         }},
        {R"cpp(// Enumerator
            enum Hello {
              ONE, TWO, THREE,
            };
            void foo() {
              Hello hello = @sym[O$NE];
            }
          )cpp",
         [](HoverInfo& hi) {
             hi.name = "ONE";
             hi.kind = SymbolKind::EnumMember;
             hi.namespace_scope = "";
             hi.local_scope = "Hello::";
             hi.type = "enum Hello";
             hi.definition = "ONE";
             hi.value = "0";
         }},
        {R"cpp(// C++20's using enum
            enum class Hello {
              ONE, TWO, THREE,
            };
            void foo() {
              using enum Hello;
              Hello hello = @sym[O$NE];
            }
          )cpp",
         [](HoverInfo& hi) {
             hi.name = "ONE";
             hi.kind = SymbolKind::EnumMember;
             hi.namespace_scope = "";
             hi.local_scope = "Hello::";
             hi.type = "enum Hello";
             hi.definition = "ONE";
             hi.value = "0";
         }},
        {R"cpp(// Enumerator in anonymous enum
            enum {
              ONE, TWO, THREE,
            };
            void foo() {
              int hello = @sym[O$NE];
            }
          )cpp",
         [](HoverInfo& hi) {
             hi.name = "ONE";
             hi.kind = SymbolKind::EnumMember;
             hi.namespace_scope = "";
             // FIXME: This should be `(anon enum)::`
             hi.local_scope = "";
             hi.type = "enum (unnamed)";
             hi.definition = "ONE";
             hi.value = "0";
         }},
        {R"cpp(// Typedef
            typedef int Foo;
            int main() {
              @sym[$Foo] bar;
            }
          )cpp",
         [](HoverInfo& hi) {
             hi.name = "Foo";
             hi.kind = SymbolKind::Type;
             hi.namespace_scope = "";
             hi.definition = "typedef int Foo";
             hi.type = "int";
             hi.documentation = "Typedef";
         }},
        {R"cpp(// Typedef with embedded definition
            typedef struct Bar {} Foo;
            int main() {
              @sym[$Foo] bar;
            }
          )cpp",
         [](HoverInfo& hi) {
             hi.name = "Foo";
             hi.kind = SymbolKind::Type;
             hi.namespace_scope = "";
             hi.definition = "typedef struct Bar Foo";
             hi.type = "struct Bar";
             hi.documentation = "Typedef with embedded definition";
         }},
        {R"cpp(// Namespace
            namespace ns {
            struct Foo { static void bar(); };
            } // namespace ns
            int main() { @sym[$ns]::Foo::bar(); }
          )cpp",
         [](HoverInfo& hi) {
             hi.name = "ns";
             hi.kind = SymbolKind::Namespace;
             hi.namespace_scope = "";
             hi.definition = "namespace ns {}";
         }},
        {R"cpp(// Field in anonymous struct
            static struct {
              int hello;
            } s;
            void foo() {
              s.@sym[he$llo]++;
            }
          )cpp",
         [](HoverInfo& hi) {
             hi.name = "hello";
             hi.kind = SymbolKind::Field;
             hi.namespace_scope = "";
             hi.local_scope = "(anonymous struct)::";
             hi.type = "int";
             hi.definition = "int hello";
             hi.access_specifier = "public";
         }},
        {R"cpp(// Anonymous union
            struct outer {
              union {
                int abc, def;
              } v;
            };
            void g() { struct outer o; o.v.@sym[d$ef]++; }
          )cpp",
         [](HoverInfo& hi) {
             hi.name = "def";
             hi.kind = SymbolKind::Field;
             hi.namespace_scope = "";
             hi.local_scope = "outer::(anonymous union)::";
             hi.type = "int";
             hi.definition = "int def";
             hi.access_specifier = "public";
         }},
        {R"cpp(// Templated function
            template <typename T>
            T foo() {
              return 17;
            }
            void g() { auto x = @sym[f$oo]<int>(); }
          )cpp",
         [](HoverInfo& hi) {
             hi.name = "foo";
             hi.kind = SymbolKind::Function;
             hi.namespace_scope = "";
             hi.type = "int ()";
             hi.definition = "template <> int foo<int>()";
             hi.documentation = "Templated function";
             hi.return_type = "int";
             hi.parameters.emplace();
         }},
        {R"cpp(// should not crash.
          template <class T> struct cls {
            int method();
          };

          auto test = cls<int>().@sym[m$ethod]();
          )cpp",
         [](HoverInfo& hi) {
             hi.definition = "int method()";
             hi.kind = SymbolKind::Method;
             hi.namespace_scope = "";
             hi.local_scope = "cls<int>::";
             hi.name = "method";
             hi.parameters.emplace();
             hi.return_type = "int";
             hi.type = "int ()";
             hi.access_specifier = "public";
         }},
        {R"cpp(// type of nested templates.
          template <class T> struct cls {};
          cls<cls<cls<int>>> @sym[fo$o];
          )cpp",
         [](HoverInfo& hi) {
             hi.definition = "cls<cls<cls<int>>> foo";
             hi.kind = SymbolKind::Variable;
             hi.namespace_scope = "";
             hi.name = "foo";
             hi.type = "cls<cls<cls<int>>>";
         }},
        {R"cpp(// type of nested templates.
          template <class T> struct cls {};
          @sym[cl$s]<cls<cls<int>>> foo;
          )cpp",
         [](HoverInfo& hi) {
             hi.definition = "template <> struct cls<cls<cls<int>>> {}";
             hi.kind = SymbolKind::Struct;
             hi.namespace_scope = "";
             hi.name = "cls<cls<cls<int>>>";
             hi.documentation = "type of nested templates.";
         }},
    };
    check_cases(cases);
}

TEST_CASE(AllFunctions) {
    HoverCase cases[] = {
        {R"cpp(// Function definition via pointer
            void foo(int) {}
            int main() {
              auto *X = &@sym[$foo];
            }
          )cpp",
         [](HoverInfo& hi) {
             hi.name = "foo";
             hi.kind = SymbolKind::Function;
             hi.namespace_scope = "";
             hi.type = "void (int)";
             hi.definition = "void foo(int)";
             hi.documentation = "Function definition via pointer";
             hi.return_type = "void";
             hi.parameters = {
                 {                     {"int"}, std::nullopt,                     std::nullopt},
             };
         }},
        {R"cpp(// Function declaration via call
            int foo(int);
            int main() {
              return @sym[$foo](42);
            }
          )cpp",
         [](HoverInfo& hi) {
             hi.name = "foo";
             hi.kind = SymbolKind::Function;
             hi.namespace_scope = "";
             hi.type = "int (int)";
             hi.definition = "int foo(int)";
             hi.documentation = "Function declaration via call";
             hi.return_type = "int";
             hi.parameters = {
                 {{"int"}, std::nullopt, std::nullopt},
             };
         }},
        {R"cpp(// Function declaration
            void foo();
            void g() { @sym[f$oo](); }
            void foo() {}
          )cpp",
         [](HoverInfo& hi) {
             hi.name = "foo";
             hi.kind = SymbolKind::Function;
             hi.namespace_scope = "";
             hi.type = "void ()";
             hi.definition = "void foo()";
             hi.documentation = "Function declaration";
             hi.return_type = "void";
             hi.parameters.emplace();
         }},
        {R"cpp(
          template <typename T = int>
          void foo(const T& = T()) {
            @sym[f$oo]<>(3);
          })cpp",
         [](HoverInfo& hi) {
             hi.name = "foo";
             hi.kind = SymbolKind::Function;
             hi.type = "void (const int &)";
             hi.return_type = "void";
             hi.parameters = {
                 {{"const int &"}, std::nullopt, std::string("T()")}};
             hi.definition = "template <> void foo<int>(const int &)";
             hi.namespace_scope = "";
         }},
    };
    check_cases(cases);
}

TEST_CASE(AllFields) {
    HoverCase cases[] = {
        {R"cpp(// Field
            struct Foo { int x; };
            int main() {
              Foo bar;
              (void)bar.@sym[$x];
            }
          )cpp",
         [](HoverInfo& hi) {
             hi.name = "x";
             hi.kind = SymbolKind::Field;
             hi.namespace_scope = "";
             hi.local_scope = "Foo::";
             hi.type = "int";
             hi.definition = "int x";
             hi.access_specifier = "public";
         }},
        {R"cpp(// Field with initialization
            struct Foo { int x = 5; };
            int main() {
              Foo bar;
              (void)bar.@sym[$x];
            }
          )cpp",
         [](HoverInfo& hi) {
             hi.name = "x";
             hi.kind = SymbolKind::Field;
             hi.namespace_scope = "";
             hi.local_scope = "Foo::";
             hi.type = "int";
             hi.definition = "int x = 5";
             hi.access_specifier = "public";
         }},
        {R"cpp(// Static field
            struct Foo { static int x; };
            int main() {
              (void)Foo::@sym[$x];
            }
          )cpp",
         [](HoverInfo& hi) {
             hi.name = "x";
             hi.kind = SymbolKind::Variable;
             hi.namespace_scope = "";
             hi.local_scope = "Foo::";
             hi.type = "int";
             hi.definition = "static int x";
             hi.access_specifier = "public";
         }},
        {R"cpp(// Field, member initializer
            struct Foo {
              int x;
              Foo() : @sym[$x](0) {}
            };
          )cpp",
         [](HoverInfo& hi) {
             hi.name = "x";
             hi.kind = SymbolKind::Field;
             hi.namespace_scope = "";
             hi.local_scope = "Foo::";
             hi.type = "int";
             hi.definition = "int x";
             hi.access_specifier = "public";
         }},
        {R"cpp(// Field, GNU old-style field designator
            struct Foo { int x; };
            int main() {
              Foo bar = { @sym[$x] : 1 };
            }
          )cpp",
         [](HoverInfo& hi) {
             hi.name = "x";
             hi.kind = SymbolKind::Field;
             hi.namespace_scope = "";
             hi.local_scope = "Foo::";
             hi.type = "int";
             hi.definition = "int x";
             hi.access_specifier = "public";
             // FIXME: Initializer for x is a DesignatedInitListExpr, hence it is
             // of struct type and omitted.
         }},
        {R"cpp(// Field, field designator
            struct Foo { int x; int y; };
            int main() {
              Foo bar = { .@sym[$x] = 2, .y = 2 };
            }
          )cpp",
         [](HoverInfo& hi) {
             hi.name = "x";
             hi.kind = SymbolKind::Field;
             hi.namespace_scope = "";
             hi.local_scope = "Foo::";
             hi.type = "int";
             hi.definition = "int x";
             hi.access_specifier = "public";
         }},
        {R"cpp(// Method call
            struct Foo { int x(); };
            int main() {
              Foo bar;
              bar.@sym[$x]();
            }
          )cpp",
         [](HoverInfo& hi) {
             hi.name = "x";
             hi.kind = SymbolKind::Method;
             hi.namespace_scope = "";
             hi.local_scope = "Foo::";
             hi.type = "int ()";
             hi.definition = "int x()";
             hi.return_type = "int";
             hi.parameters.emplace();
             hi.access_specifier = "public";
         }},
        {R"cpp(// Static method call
            struct Foo { static int x(); };
            int main() {
              Foo::@sym[$x]();
            }
          )cpp",
         [](HoverInfo& hi) {
             hi.name = "x";
             hi.kind = SymbolKind::Method;
             hi.namespace_scope = "";
             hi.local_scope = "Foo::";
             hi.type = "int ()";
             hi.definition = "static int x()";
             hi.return_type = "int";
             hi.parameters.emplace();
             hi.access_specifier = "public";
         }},
    };
    check_cases(cases);
}

TEST_CASE(UsingDeclarations) {
    HoverCase cases[] = {
        {R"cpp(// Function definition via using declaration
            namespace ns {
              void foo();
            }
            int main() {
              using ns::foo;
              @sym[$foo]();
            }
          )cpp",
         [](HoverInfo& hi) {
             hi.name = "foo";
             hi.kind = SymbolKind::Function;
             hi.namespace_scope = "ns::";
             hi.type = "void ()";
             hi.definition = "void foo()";
             hi.documentation = "";
             hi.return_type = "void";
             hi.parameters.emplace();
         }},
        {R"cpp( // using declaration and two possible function declarations
            namespace ns { void foo(int); void foo(char); }
            using ns::foo;
            template <typename T> void bar() { @sym[f$oo](T{}); }
          )cpp",
         [](HoverInfo& hi) {
             hi.name = "foo";
             hi.kind = SymbolKind::Invalid;
             hi.namespace_scope = "";
             hi.definition = "using ns::foo";
         }},
    };
    check_cases(cases);
}

TEST_CASE(AllAuto) {
    HoverCase cases[] = {
        {R"cpp(// Simple initialization with auto
            void foo() {
              @sym[$auto] i = 1;
            }
          )cpp",
         [](HoverInfo& hi) {
             hi.name = "auto";
             hi.kind = SymbolKind::Type;
             hi.definition = "int";
         }},
        {R"cpp(// Simple initialization with const auto
            void foo() {
              const @sym[$auto] i = 1;
            }
          )cpp",
         [](HoverInfo& hi) {
             hi.name = "auto";
             hi.kind = SymbolKind::Type;
             hi.definition = "int";
         }},
        {R"cpp(// Simple initialization with const auto&
            void foo() {
              const @sym[$auto]& i = 1;
            }
          )cpp",
         [](HoverInfo& hi) {
             hi.name = "auto";
             hi.kind = SymbolKind::Type;
             hi.definition = "int";
         }},
        {R"cpp(// Simple initialization with auto&
            void foo() {
              int x;
              @sym[$auto]& i = x;
            }
          )cpp",
         [](HoverInfo& hi) {
             hi.name = "auto";
             hi.kind = SymbolKind::Type;
             hi.definition = "int";
         }},
        {R"cpp(// Simple initialization with auto*
            void foo() {
              int a = 1;
              @sym[$auto]* i = &a;
            }
          )cpp",
         [](HoverInfo& hi) {
             hi.name = "auto";
             hi.kind = SymbolKind::Type;
             hi.definition = "int";
         }},
        {R"cpp(// Simple initialization with auto from pointer
            void foo() {
              int a = 1;
              @sym[$auto] i = &a;
            }
          )cpp",
         [](HoverInfo& hi) {
             hi.name = "auto";
             hi.kind = SymbolKind::Type;
             hi.definition = "int *";
         }},
        {R"cpp(// Auto with initializer list.
            namespace std
            {
              template<class _E>
              class initializer_list { const _E *a, *b; };
            }
            void foo() {
              @sym[$auto] i = {1,2};
            }
          )cpp",
         [](HoverInfo& hi) {
             hi.name = "auto";
             hi.kind = SymbolKind::Type;
             hi.definition = "std::initializer_list<int>";
         }},
        {R"cpp(// User defined conversion to auto
            struct Bar {
              operator @sym[$auto]() const { return 10; }
            };
          )cpp",
         [](HoverInfo& hi) {
             hi.name = "auto";
             hi.kind = SymbolKind::Type;
             hi.definition = "int";
         }},
        {R"cpp(// simple trailing return type
            @sym[$auto] main() -> int {
              return 0;
            }
          )cpp",
         [](HoverInfo& hi) {
             hi.name = "auto";
             hi.kind = SymbolKind::Type;
             hi.definition = "int";
         }},
        {R"cpp(// auto function return with trailing type
            struct Bar {};
            @sym[$auto] test() -> decltype(Bar()) {
              return Bar();
            }
          )cpp",
         [](HoverInfo& hi) {
             hi.name = "auto";
             hi.kind = SymbolKind::Type;
             hi.definition = "Bar";
             hi.documentation = "auto function return with trailing type";
         }},
        {R"cpp(// auto in function return
            struct Bar {};
            @sym[$auto] test() {
              return Bar();
            }
          )cpp",
         [](HoverInfo& hi) {
             hi.name = "auto";
             hi.kind = SymbolKind::Type;
             hi.definition = "Bar";
             hi.documentation = "auto in function return";
         }},
        {R"cpp(// auto& in function return
            struct Bar {};
            @sym[$auto]& test() {
              static Bar x;
              return x;
            }
          )cpp",
         [](HoverInfo& hi) {
             hi.name = "auto";
             hi.kind = SymbolKind::Type;
             hi.definition = "Bar";
             hi.documentation = "auto& in function return";
         }},
        {R"cpp(// auto* in function return
            struct Bar {};
            @sym[$auto]* test() {
              Bar* bar;
              return bar;
            }
          )cpp",
         [](HoverInfo& hi) {
             hi.name = "auto";
             hi.kind = SymbolKind::Type;
             hi.definition = "Bar";
             hi.documentation = "auto* in function return";
         }},
        {R"cpp(// const auto& in function return
            struct Bar {};
            const @sym[$auto]& test() {
              static Bar x;
              return x;
            }
          )cpp",
         [](HoverInfo& hi) {
             hi.name = "auto";
             hi.kind = SymbolKind::Type;
             hi.definition = "Bar";
             hi.documentation = "const auto& in function return";
         }},
        {R"cpp(// More complicated structured types.
            int bar();
            @sym[$auto] (*foo)() = bar;
          )cpp",
         [](HoverInfo& hi) {
             hi.name = "auto";
             hi.kind = SymbolKind::Type;
             hi.definition = "int";
         }},
        {R"cpp(// auto on alias
          typedef int int_type;
          @sym[$auto] x = int_type();
          )cpp",
         [](HoverInfo& hi) {
             hi.name = "auto";
             hi.kind = SymbolKind::Type;
             hi.definition = "int_type // aka: int";
         }},
        {R"cpp(// auto on alias
          struct cls {};
          typedef cls cls_type;
          @sym[$auto] y = cls_type();
          )cpp",
         [](HoverInfo& hi) {
             hi.name = "auto";
             hi.kind = SymbolKind::Type;
             hi.definition = "cls_type // aka: cls";
             hi.documentation = "auto on alias";
         }},
        {R"cpp(// auto on alias
          template <class>
          struct templ {};
          @sym[$auto] z = templ<int>();
          )cpp",
         [](HoverInfo& hi) {
             hi.name = "auto";
             hi.kind = SymbolKind::Type;
             hi.definition = "templ<int>";
             hi.documentation = "auto on alias";
         }},
        {R"cpp(// Undeduced auto declaration
            template<typename T>
            void foo() {
              @sym[$auto] x = T();
            }
          )cpp",
         [](HoverInfo& hi) {
             hi.name = "auto";
             hi.kind = SymbolKind::Type;
             hi.definition = "/* not deduced */";
         }},
        {R"cpp(// Undeduced auto return type
            template<typename T>
            @sym[$auto] foo() {
              return T();
            }
          )cpp",
         [](HoverInfo& hi) {
             hi.name = "auto";
             hi.kind = SymbolKind::Type;
             hi.definition = "/* not deduced */";
         }},
        {R"cpp(// Template auto parameter
            template<@sym[a$uto] T>
              void func() {
            }
          )cpp",
         [](HoverInfo& hi) {
             // FIXME: not sure this is what we want, but this is what we
             // currently get with getDeducedType.
             hi.name = "auto";
             hi.kind = SymbolKind::Type;
             hi.definition = "/* not deduced */";
         }},
    };
    check_cases(cases);
}

TEST_CASE(AllDecltype) {
    HoverCase cases[] = {
        {R"cpp(// Simple initialization with decltype(auto)
            void foo() {
              @sym[decltype]$(p)(auto) i = 1;
            }
          )cpp",
         [](HoverInfo& hi) {
             hi.name = "decltype";
             hi.kind = SymbolKind::Type;
             hi.definition = "int";
         }},
        {R"cpp(// Simple initialization with const decltype(auto)
            void foo() {
              const int j = 0;
              @sym[decltype]$(p)(auto) i = j;
            }
          )cpp",
         [](HoverInfo& hi) {
             hi.name = "decltype";
             hi.kind = SymbolKind::Type;
             hi.definition = "const int";
         }},
        {R"cpp(// Simple initialization with const& decltype(auto)
            void foo() {
              int k = 0;
              const int& j = k;
              @sym[decltype]$(p)(auto) i = j;
            }
          )cpp",
         [](HoverInfo& hi) {
             hi.name = "decltype";
             hi.kind = SymbolKind::Type;
             hi.definition = "const int &";
         }},
        {R"cpp(// Simple initialization with & decltype(auto)
            void foo() {
              int k = 0;
              int& j = k;
              @sym[decltype]$(p)(auto) i = j;
            }
          )cpp",
         [](HoverInfo& hi) {
             hi.name = "decltype";
             hi.kind = SymbolKind::Type;
             hi.definition = "int &";
         }},
        {R"cpp(// decltype(auto) in function return
            struct Bar {};
            @sym[decltype]$(p)(auto) test() {
              return Bar();
            }
          )cpp",
         [](HoverInfo& hi) {
             hi.name = "decltype";
             hi.kind = SymbolKind::Type;
             hi.definition = "Bar";
             hi.documentation = "decltype(auto) in function return";
         }},
        {R"cpp(// decltype(auto) reference in function return
            @sym[decltype]$(p)(auto) test() {
              static int a;
              return (a);
            }
          )cpp",
         [](HoverInfo& hi) {
             hi.name = "decltype";
             hi.kind = SymbolKind::Type;
             hi.definition = "int &";
         }},
        {R"cpp(// decltype lvalue reference
            void foo() {
              int I = 0;
              @sym[decltype]$(p)(I) J = I;
            }
          )cpp",
         [](HoverInfo& hi) {
             hi.name = "decltype";
             hi.kind = SymbolKind::Type;
             hi.definition = "int";
         }},
        {R"cpp(// decltype lvalue reference
            void foo() {
              int I= 0;
              int &K = I;
              @sym[decltype]$(p)(K) J = I;
            }
          )cpp",
         [](HoverInfo& hi) {
             hi.name = "decltype";
             hi.kind = SymbolKind::Type;
             hi.definition = "int &";
         }},
        {R"cpp(// decltype lvalue reference parenthesis
            void foo() {
              int I = 0;
              @sym[decltype]$(p)((I)) J = I;
            }
          )cpp",
         [](HoverInfo& hi) {
             hi.name = "decltype";
             hi.kind = SymbolKind::Type;
             hi.definition = "int &";
         }},
        {R"cpp(// decltype rvalue reference
            void foo() {
              int I = 0;
              @sym[decltype]$(p)(static_cast<int&&>(I)) J = static_cast<int&&>(I);
            }
          )cpp",
         [](HoverInfo& hi) {
             hi.name = "decltype";
             hi.kind = SymbolKind::Type;
             hi.definition = "int &&";
         }},
        {R"cpp(// decltype rvalue reference function call
            int && bar();
            void foo() {
              int I = 0;
              @sym[decltype]$(p)(bar()) J = bar();
            }
          )cpp",
         [](HoverInfo& hi) {
             hi.name = "decltype";
             hi.kind = SymbolKind::Type;
             hi.definition = "int &&";
         }},
        {R"cpp(// decltype of function with trailing return type.
            struct Bar {};
            auto test() -> decltype(Bar()) {
              return Bar();
            }
            void foo() {
              @sym[decltype]$(p)(test()) i = test();
            }
          )cpp",
         [](HoverInfo& hi) {
             hi.name = "decltype";
             hi.kind = SymbolKind::Type;
             hi.definition = "Bar";
             hi.documentation = "decltype of function with trailing return type.";
         }},
        {R"cpp(// decltype of var with decltype.
            void foo() {
              int I = 0;
              decltype(I) J = I;
              @sym[decltype]$(p)(J) K = J;
            }
          )cpp",
         [](HoverInfo& hi) {
             hi.name = "decltype";
             hi.kind = SymbolKind::Type;
             hi.definition = "int";
         }},
        {R"cpp(// decltype of dependent type
            template <typename T>
            struct X {
              using Y = @sym[decltype]$(p)(T::Z);
            };
          )cpp",
         [](HoverInfo& hi) {
             hi.name = "decltype";
             hi.kind = SymbolKind::Type;
             hi.definition = "<dependent type>";
         }},
        {R"cpp(// Undeduced decltype(auto) return type
            template<typename T>
            @sym[decltype]$(p)(auto) foo() {
              return T();
            }
          )cpp",
         [](HoverInfo& hi) {
             hi.name = "decltype";
             hi.kind = SymbolKind::Type;
             hi.definition = "/* not deduced */";
         }},
        {R"cpp(// type with decltype
          int a;
          decltype(a) @sym[b$] = a;)cpp",
         [](HoverInfo& hi) {
             hi.definition = "decltype(a) b = a";
             hi.kind = SymbolKind::Variable;
             hi.namespace_scope = "";
             hi.name = "b";
             hi.type = "int";
         }},
        {R"cpp(// type with decltype
          int a;
          decltype(a) c;
          decltype(c) @sym[b$] = a;)cpp",
         [](HoverInfo& hi) {
             hi.definition = "decltype(c) b = a";
             hi.kind = SymbolKind::Variable;
             hi.namespace_scope = "";
             hi.name = "b";
             hi.type = "int";
         }},
        {R"cpp(// type with decltype
          int a;
          const decltype(a) @sym[b$] = a;)cpp",
         [](HoverInfo& hi) {
             hi.definition = "const decltype(a) b = a";
             hi.kind = SymbolKind::Variable;
             hi.namespace_scope = "";
             hi.name = "b";
             hi.type = "int";
         }},
        {R"cpp(// type with decltype
          int a;
          auto @sym[f$oo](decltype(a) x) -> decltype(a) { return 0; })cpp",
         [](HoverInfo& hi) {
             hi.definition = "auto foo(decltype(a) x) -> decltype(a)";
             hi.kind = SymbolKind::Function;
             hi.namespace_scope = "";
             hi.name = "foo";
             // FIXME: Handle composite types with decltype with a printing
             // policy.
             hi.type = {"auto (decltype(a)) -> decltype(a)", "auto (int) -> int"};
             hi.return_type = "int";
             hi.parameters = {
                 {{"int"}, std::string("x"), std::nullopt}};
         }          },
    };
    check_cases(cases);
}

TEST_CASE(ThisExpr) {
    HoverCase cases[] = {
        {R"cpp(// this expr
          // comment
          namespace ns {
            class Foo {
              Foo* bar() {
                return @sym[t$his];
              }
            };
          }
          )cpp",
         [](HoverInfo& hi) {
             hi.name = "this";
             hi.definition = "ns::Foo *";
         }},
        {R"cpp(// this expr for template class
          namespace ns {
            template <typename T>
            class Foo {
              Foo* bar() const {
                return @sym[t$his];
              }
            };
          }
          )cpp",
         [](HoverInfo& hi) {
             hi.name = "this";
             hi.definition = "const Foo<T> *";
         }},
        {R"cpp(// this expr for specialization class
          namespace ns {
            template <typename T> class Foo {};
            template <>
            struct Foo<int> {
              Foo* bar() {
                return @sym[thi$s];
              }
            };
          }
          )cpp",
         [](HoverInfo& hi) {
             hi.name = "this";
             hi.definition = "Foo<int> *";
         }},
        {R"cpp(// this expr for partial specialization struct
          namespace ns {
            template <typename T, typename F> struct Foo {};
            template <typename F>
            struct Foo<int, F> {
              Foo* bar() const {
                return @sym[thi$s];
              }
            };
          }
          )cpp",
         [](HoverInfo& hi) {
             hi.name = "this";
             hi.definition = "const Foo<int, F> *";
         }},
    };
    check_cases(cases);
}

TEST_CASE(SpaceshipOperator) {
    run_info(R"cpp(
          namespace std {
          struct strong_ordering {
            int n;
            constexpr operator int() const { return n; }
            static const strong_ordering equal, greater, less;
          };
          constexpr strong_ordering strong_ordering::equal = {0};
          constexpr strong_ordering strong_ordering::greater = {1};
          constexpr strong_ordering strong_ordering::less = {-1};
          }

          struct Foo
          {
            int x;
            // Foo spaceship
            auto operator<=>(const Foo&) const = default;
          };

          bool x = Foo(1) @sym[!$=] Foo(2);
         )cpp");

    HoverInfo expected;
    expected.type = "bool (const Foo &) const noexcept";
    expected.value = "true";
    expected.name = "operator==";
    expected.parameters = {
        {{"const Foo &"}, std::nullopt, std::nullopt}
    };
    expected.return_type = "bool";
    expected.kind = SymbolKind::Operator;
    expected.local_scope = "Foo::";
    expected.namespace_scope = "";
    expected.definition = "bool operator==(const Foo &) const noexcept = default";
    expected.documentation = "";
    expected.access_specifier = "public";
    expect_hover(expected);
    check_sym_range();
}

TEST_CASE(AttributeHover) {
    run_info(R"cpp(
         void foo(int * __attribute__((@sym[non$null], noescape)) );
         )cpp");

    HoverInfo expected;
    expected.name = "nonnull";
    expected.kind = SymbolKind::Invalid;
    expected.definition = "__attribute__((nonnull))";
    expected.documentation = clang::Attr::getDocumentation(clang::attr::NonNull).str();
    expect_hover(expected);
    check_sym_range();
}

TEST_CASE(AllNoCrash) {
    HoverCase cases[] = {
        {R"cpp(// Should not crash when evaluating the initializer.
            struct Test {};
            void test() { Test && @sym[te$st] = {}; }
          )cpp",
         [](HoverInfo& hi) {
             hi.name = "test";
             hi.kind = SymbolKind::Variable;
             hi.namespace_scope = "";
             hi.local_scope = "test::";
             hi.type = "Test &&";
             hi.definition = "Test &&test = {}";
         }},
        {R"cpp(// Shouldn't crash when evaluating the initializer.
            struct Bar {}; // error-ok
            struct Foo { void foo(Bar x = y); }
            void Foo::foo(Bar @sym[$x]) {})cpp",
         [](HoverInfo& hi) {
             hi.name = "x";
             hi.kind = SymbolKind::Parameter;
             hi.namespace_scope = "";
             hi.local_scope = "Foo::foo::";
             hi.type = "Bar";
             hi.definition = "Bar x = <recovery - expr>()";
         }},
    };
    check_cases(cases);
}

TEST_CASE(CallPassType) {
    llvm::StringRef prefix = R"cpp(
class Base {};
class Derived : public Base {};
class CustomClass {
 public:
  CustomClass() {}
  CustomClass(const Base &x) {}
  CustomClass(int &x) {}
  CustomClass(float x) {}
  CustomClass(int x, int y) {}
};

void int_by_ref(int &x) {}
void int_by_const_ref(const int &x) {}
void int_by_value(int x) {}
void base_by_ref(Base &x) {}
void base_by_const_ref(const Base &x) {}
void base_by_value(Base x) {}
void float_by_value(float x) {}
void custom_by_value(CustomClass x) {}

void fun() {
  int int_x;
  int &int_ref = int_x;
  const int &int_const_ref = int_x;
  Base base;
  const Base &base_const_ref = base;
  Derived derived;
  float float_x;
)cpp";
    llvm::StringRef suffix = "}";

    struct {
        llvm::StringRef code;
        PassMode pass_by;
        bool converted;
    } cases[] = {
        // Integer tests
        {"int_by_value($int_x);",               PassMode::Value,    false},
        {"int_by_value($123);",                 PassMode::Value,    false},
        {"int_by_ref($int_x);",                 PassMode::Ref,      false},
        {"int_by_const_ref($int_x);",           PassMode::ConstRef, false},
        {"int_by_const_ref($123);",             PassMode::ConstRef, false},
        {"int_by_value($int_ref);",             PassMode::Value,    false},
        {"int_by_const_ref($int_ref);",         PassMode::ConstRef, false},
        {"int_by_const_ref($int_const_ref);",   PassMode::ConstRef, false},
        // Custom class tests
        {"base_by_ref($base);",                 PassMode::Ref,      false},
        {"base_by_const_ref($base);",           PassMode::ConstRef, false},
        {"base_by_const_ref($base_const_ref);", PassMode::ConstRef, false},
        {"base_by_value($base);",               PassMode::Value,    false},
        {"base_by_value($base_const_ref);",     PassMode::Value,    false},
        {"base_by_ref($derived);",              PassMode::Ref,      false},
        {"base_by_const_ref($derived);",        PassMode::ConstRef, false},
        {"base_by_value($derived);",            PassMode::Value,    false},
        // Custom class constructor tests
        {"CustomClass c1($base);",              PassMode::ConstRef, false},
        {"auto c2 = new CustomClass($base);",   PassMode::ConstRef, false},
        {"CustomClass c3($int_x);",             PassMode::Ref,      false},
        {"CustomClass c3(int_x, $int_x);",      PassMode::Value,    false},
        // Converted tests
        {"float_by_value($int_x);",             PassMode::Value,    true },
        {"float_by_value($int_ref);",           PassMode::Value,    true },
        {"float_by_value($int_const_ref);",     PassMode::Value,    true },
        {"float_by_value($123.0f);",            PassMode::Value,    false},
        {"float_by_value($123);",               PassMode::Value,    true },
        {"custom_by_value($int_x);",            PassMode::Ref,      true },
        {"custom_by_value($float_x);",          PassMode::Value,    true },
        {"custom_by_value($base);",             PassMode::ConstRef, true },
    };

    for(const auto& c: cases) {
        std::string code = (prefix + c.code + suffix).str();
        run_info(code, {}, "-std=c++17");
        if(!info) {
            std::println("no hover result for: {}", c.code.str());
        }
        ASSERT_TRUE(info.has_value());
        ASSERT_TRUE(info->call_pass_type.has_value());
        bool same = info->call_pass_type->pass_by == c.pass_by &&
                    info->call_pass_type->converted == c.converted;
        if(!same) {
            std::println("call pass type mismatch for: {}", c.code.str());
        }
        EXPECT_EQ(static_cast<int>(info->call_pass_type->pass_by), static_cast<int>(c.pass_by));
        EXPECT_EQ(info->call_pass_type->converted, c.converted);
    }
}

TEST_CASE(NoHover) {
    llvm::StringRef cases[] = {
        "$int main() {}",
        "void foo() {$}",
        // FIXME: "decltype(auto)" should be a single hover
        "decltype(au$to) x = 0;",
        // FIXME: not supported yet
        R"cpp(// Lambda auto parameter
            auto lamb = [](a$uto){};
          )cpp",
        R"cpp(// non-named decls don't get hover. Don't crash!
            $static_assert(1, "");
          )cpp",
        R"cpp(// non-evaluatable expr
          template <typename T> void foo() {
            (void)size$of(T);
          })cpp",
        R"cpp(// should not crash on invalid semantic form of init-list-expr.
            /*error-ok*/
            struct Foo {
              int xyz = 0;
            };
            class Bar {};
            constexpr Foo s = $(p){
              .xyz = Bar(),
            };
          )cpp",
        // literals
        "auto x = t$rue;",
        "auto x = $(p)(int){42};",
        "auto x = $42.;",
        "auto x = $42.0i;",
        "auto x = $42;",
        "auto x = $nullptr;",
    };

    for(llvm::StringRef code: cases) {
        run_info(code, {}, "-std=c++17");
        if(info) {
            std::println("unexpected hover for:\n{}", code.str());
        }
        EXPECT_FALSE(info.has_value());
    }
}

TEST_CASE(SpaceshipDocNoCrash) {
    run_info(R"cpp(
  namespace std {
  struct strong_ordering {
    int n;
    constexpr operator int() const { return n; }
    static const strong_ordering equal, greater, less;
  };
  constexpr strong_ordering strong_ordering::equal = {0};
  constexpr strong_ordering strong_ordering::greater = {1};
  constexpr strong_ordering strong_ordering::less = {-1};
  }

  template <typename T>
  struct S {
    // Foo bar baz
    friend auto operator<=>(S, S) = default;
  };
  static_assert(S<void>() =$= S<void>());
    )cpp");

    ASSERT_TRUE(info.has_value());
    EXPECT_EQ(info->documentation, "");
}

TEST_CASE(ForwardStructValue) {
    run_info(R"cpp(
  struct Foo;
  int bar;
  auto baz = (Fo$o*)&bar;
    )cpp");

    ASSERT_TRUE(info.has_value());
    EXPECT_EQ(dump(info->value), "&bar");
}

TEST_CASE(InvalidDefaultArgs) {
    // Function parameter default values are not evaluated on invalid decls.
    run_info(R"cpp(
        // error-ok testing behavior on invalid decl
        class Foo {};
        void foo(Foo p$aram = nullptr);
        )cpp");
    ASSERT_TRUE(info.has_value());
    EXPECT_FALSE(info->value.has_value());

    run_info(R"cpp(
        class Foo {};
        void foo(Foo *p$aram = nullptr);
        )cpp");
    ASSERT_TRUE(info.has_value());
    EXPECT_EQ(dump(info->value), "nullptr");
}

TEST_CASE(DisableShowAka) {
    feature::HoverOptions options;
    options.show_aka = false;

    run_info(R"cpp(
    using m_int = int;
    m_int @sym[$a];
  )cpp",
             options,
             "-std=c++17");

    ASSERT_TRUE(info.has_value());
    EXPECT_EQ(dump(info->type), dump(std::optional(PrintedType("m_int"))));
    check_sym_range();
}

TEST_CASE(HideBigInitializers) {
    run_info(R"cpp(
  #define A(x) x, x, x, x
  #define B(x) A(A(A(A(x))))
  int a$rr[] = {B(0)};
  )cpp");

    ASSERT_TRUE(info.has_value());
    EXPECT_EQ(info->definition, "int arr[]");
}

TEST_CASE(TypedefChain) {
    run_info(R"cpp(
  template <bool X, typename T, typename F>
  struct cond { using type = T; };
  template <typename T, typename F>
  struct cond<false, T, F> { using type = F; };

  template <bool X, typename T, typename F>
  using type = typename cond<X, T, F>::type;

  void foo() {
    using f$oo = type<true, int, double>;
  }
  )cpp");

    ASSERT_TRUE(info.has_value());
    ASSERT_TRUE(info->type.has_value());
    EXPECT_EQ(info->type->type, "int");
    EXPECT_EQ(info->definition, "using foo = type<true, int, double>");
}

TEST_CASE(BigIntsNoCrash) {
    // APInt64 wrap around.
    run_info(R"cpp(
    constexpr unsigned long value = -1; // wrap around
    void foo() { va$lue; }
  )cpp");
    ASSERT_TRUE(info.has_value());

    // __int128_t value printing.
    run_info(R"cpp(
    constexpr __int128_t value = -4;
    void foo() { va$lue; }
  )cpp");
    ASSERT_TRUE(info.has_value());
    EXPECT_EQ(dump(info->value), "-4 (0xfffffffc)");
}

TEST_CASE(GlobalCastsNoCrash) {
    run_info(R"cpp(
    using uintptr_t = unsigned long;
    enum Test : uintptr_t {};
    unsigned global_var;
    void foo() {
      Test v$al = static_cast<Test>(reinterpret_cast<uintptr_t>(&global_var));
    }
  )cpp");
    ASSERT_TRUE(info.has_value());
    EXPECT_EQ(dump(info->value), "&global_var");

    run_info(R"cpp(
    using uintptr_t = unsigned long;
    unsigned global_var;
    void foo() {
      uintptr_t a$ddress = reinterpret_cast<uintptr_t>(&global_var);
    }
  )cpp");
    ASSERT_TRUE(info.has_value());
    EXPECT_EQ(dump(info->value), "&global_var");
}

TEST_CASE(DocsFromAst) {
    add_main("main.cpp", R"cpp(
  // doc
  template <typename T> class X {};
  // doc
  template <typename T> void bar() {}
  // doc
  template <typename T> T baz;
  void foo() {
    au$to t = X<int>();
    X$<int>();
    b$ar<int>();
    au$to T = ba$z<X<int>>;
    ba$z<int> = 0;
  })cpp");
    ASSERT_TRUE(compile());

    for(auto offset: nameless_points()) {
        auto hover = feature::hover_info(*unit, offset, {});
        ASSERT_TRUE(hover.has_value());
        EXPECT_EQ(hover->documentation, "doc");
    }
}

TEST_CASE(DocsMostSpecial) {
    add_main("main.cpp", R"cpp(
  // doc1
  template <typename T> class $(doc1a)X {};
  // doc2
  template <> class $(doc2a)X<int> {};
  // doc3
  template <typename T> class $(doc3a)X<T*> {};
  void foo() {
    X$(doc1b)<char>();
    X$(doc2b)<int>();
    X$(doc3b)<int*>();
  })cpp");
    ASSERT_TRUE(compile());

    struct {
        llvm::StringRef point_name;
        llvm::StringRef doc;
    } cases[] = {
        {"doc1a", "doc1"},
        {"doc1b", "doc1"},
        {"doc2a", "doc2"},
        {"doc2b", "doc2"},
        {"doc3a", "doc3"},
        {"doc3b", "doc3"},
    };

    for(const auto& c: cases) {
        auto hover = feature::hover_info(*unit, point(c.point_name), {});
        ASSERT_TRUE(hover.has_value());
        EXPECT_EQ(hover->documentation, c.doc.str());
    }
}

TEST_CASE(SetterHeuristicNoCrash) {
    run_info(R"cpp(
    /* error-ok */
    template<typename T> T foo(T);

    // Setter variable heuristic might fail if the callexpr is broken.
    struct X { int Y; void @sym[$setY](float) { Y = foo(undefined); } };)cpp");
    ASSERT_TRUE(info.has_value());
}

TEST_CASE(Present) {
    struct {
        std::function<void(HoverInfo&)> builder;
        llvm::StringRef expected;
    } cases[] = {
        {
         [](HoverInfo& hi) {
                hi.kind = SymbolKind::Invalid;
                hi.name = "X";
            },          R"(X)",
         },
        {
         [](HoverInfo& hi) {
                hi.kind = SymbolKind::Namespace;
                hi.name = "foo";
            },             R"(namespace foo)",
         },
        {
         [](HoverInfo& hi) {
                hi.kind = SymbolKind::Class;
                hi.size = 80;
                hi.template_parameters = {
                    {{"typename"},std::string("T"),std::nullopt},
                    {{"typename"},std::string("C"),std::string("bool")},
                };
                hi.documentation = "documentation";
                hi.definition = "template <typename T, typename C = bool> class Foo {}";
                hi.name = "foo";
                hi.namespace_scope.emplace();
            },          R"(class foo

Size: 10 bytes
documentation

template <typename T, typename C = bool> class Foo {})",
         },
        {
         [](HoverInfo& hi) {
                hi.kind = SymbolKind::Function;
                hi.name = "foo";
                hi.type = {"type", "c_type"};
                hi.return_type = {"ret_type", "can_ret_type"};
                hi.parameters.emplace();
                HoverParam p;
                hi.parameters->push_back(p);
                p.type = PrintedType("type", "can_type");
                hi.parameters->push_back(p);
                p.name = "foo";
                hi.parameters->push_back(p);
                p.default_value = "default";
                hi.parameters->push_back(p);
                hi.namespace_scope = "ns::";
                hi.definition = "ret_type foo(params) {}";
            },             "function foo\n" "\n" "→ ret_type (aka can_ret_type)\n" "Parameters:\n" "- \n" "- type (aka can_type)\n" "- type foo (aka can_type)\n" "- type foo = default (aka can_type)\n" "\n" "// In namespace ns\n" "ret_type foo(params) {}",
         },
        {
         [](HoverInfo& hi) {
                hi.kind = SymbolKind::Field;
                hi.local_scope = "test::Bar::";
                hi.value = "value";
                hi.name = "foo";
                hi.type = {"type", "can_type"};
                hi.definition = "def";
                hi.size = 32;
                hi.offset = 96;
                hi.padding = 32;
                hi.align = 32;
            },                       R"(field foo

Type: type (aka can_type)
Value = value
Offset: 12 bytes
Size: 4 bytes (+4 bytes padding), alignment 4 bytes

// In test::Bar
def)",
         },
        {
         [](HoverInfo& hi) {
                hi.kind = SymbolKind::Field;
                hi.local_scope = "test::Bar::";
                hi.value = "value";
                hi.name = "foo";
                hi.type = {"type", "can_type"};
                hi.definition = "def";
                hi.size = 25;
                hi.offset = 35;
                hi.padding = 4;
                hi.align = 64;
            },              R"(field foo

Type: type (aka can_type)
Value = value
Offset: 4 bytes and 3 bits
Size: 25 bits (+4 bits padding), alignment 8 bytes

// In test::Bar
def)",
         },
        {
         [](HoverInfo& hi) {
                hi.kind = SymbolKind::Field;
                hi.access_specifier = "public";
                hi.name = "foo";
                hi.local_scope = "test::Bar::";
                hi.definition = "def";
            },              R"(field foo

// In test::Bar
public: def)",
         },
        {
         [](HoverInfo& hi) {
                hi.definition = "size_t method()";
                hi.access_specifier = "protected";
                hi.kind = SymbolKind::Method;
                hi.namespace_scope = "";
                hi.local_scope = "cls<int>::";
                hi.name = "method";
                hi.parameters.emplace();
                hi.return_type = {"size_t", "unsigned long"};
                hi.type = {"size_t ()", "unsigned long ()"};
            }, R"(method method

→ size_t (aka unsigned long)

// In cls<int>
protected: size_t method())",
         },
        {
         [](HoverInfo& hi) {
                hi.definition = "cls(int a, int b = 5)";
                hi.access_specifier = "public";
                hi.kind = SymbolKind::Method;
                hi.namespace_scope = "";
                hi.local_scope = "cls";
                hi.name = "cls";
                hi.parameters = {
                    {{"int"},std::string("a"),std::nullopt},
                    {{"int"},std::string("b"),std::string("5")},
                };
            },             R"(method cls

Parameters:
- int a
- int b = 5

// In cls
public: cls(int a, int b = 5))",
         },
        {
         [](HoverInfo& hi) {
                hi.kind = SymbolKind::Union;
                hi.access_specifier = "private";
                hi.name = "foo";
                hi.namespace_scope = "ns1::";
                hi.definition = "union foo {}";
            },          R"(union foo

// In namespace ns1
private: union foo {})",
         },
        {
         [](HoverInfo& hi) {
                hi.kind = SymbolKind::Variable;
                hi.name = "foo";
                hi.definition = "int foo = 3";
                hi.local_scope = "test::Bar::";
                hi.value = "3";
                hi.type = "int";
                hi.callee_arg_info.emplace();
                hi.callee_arg_info->name = "arg_a";
                hi.callee_arg_info->type = PrintedType("int");
                hi.callee_arg_info->default_value = "7";
                hi.call_pass_type = PassType{PassMode::Value, false};
            },             R"(variable foo

Type: int
Value = 3
Passed as arg_a

// In test::Bar
int foo = 3)",
         },
        {
         [](HoverInfo& hi) {
                hi.kind = SymbolKind::Variable;
                hi.name = "foo";
                hi.callee_arg_info.emplace();
                hi.callee_arg_info->type = PrintedType("int");
                hi.call_pass_type = PassType{PassMode::Value, false};
            },          R"(variable foo

Passed by value)",
         },
        {
         [](HoverInfo& hi) {
                hi.kind = SymbolKind::Variable;
                hi.name = "foo";
                hi.definition = "int foo = 3";
                hi.local_scope = "test::Bar::";
                hi.value = "3";
                hi.type = "int";
                hi.callee_arg_info.emplace();
                hi.callee_arg_info->name = "arg_a";
                hi.callee_arg_info->type = PrintedType("int");
                hi.callee_arg_info->default_value = "7";
                hi.call_pass_type = PassType{PassMode::Ref, false};
            },             R"(variable foo

Type: int
Value = 3
Passed by reference as arg_a

// In test::Bar
int foo = 3)",
         },
        {
         [](HoverInfo& hi) {
                hi.kind = SymbolKind::Variable;
                hi.name = "foo";
                hi.definition = "int foo = 3";
                hi.local_scope = "test::Bar::";
                hi.value = "3";
                hi.type = "int";
                hi.callee_arg_info.emplace();
                hi.callee_arg_info->name = "arg_a";
                hi.callee_arg_info->type = PrintedType("alias_int", "int");
                hi.callee_arg_info->default_value = "7";
                hi.call_pass_type = PassType{PassMode::Value, true};
            },          R"(variable foo

Type: int
Value = 3
Passed as arg_a (converted to alias_int)

// In test::Bar
int foo = 3)",
         },
        {
         [](HoverInfo& hi) {
                hi.kind = SymbolKind::Macro;
                hi.name = "PLUS_ONE";
                hi.definition = "#define PLUS_ONE(X) (X+1)\n\n" "// Expands to\n" "(1 + 1)";
            },             R"(macro PLUS_ONE

#define PLUS_ONE(X) (X+1)

// Expands to
(1 + 1))",
         },
        {
         [](HoverInfo& hi) {
                hi.kind = SymbolKind::Variable;
                hi.name = "foo";
                hi.definition = "int foo = 3";
                hi.local_scope = "test::Bar::";
                hi.value = "3";
                hi.type = "int";
                hi.callee_arg_info.emplace();
                hi.callee_arg_info->name = "arg_a";
                hi.callee_arg_info->type = PrintedType("int");
                hi.callee_arg_info->default_value = "7";
                hi.call_pass_type = PassType{PassMode::ConstRef, true};
            },          R"(variable foo

Type: int
Value = 3
Passed by const reference as arg_a (converted to int)

// In test::Bar
int foo = 3)",
         },
        {
         [](HoverInfo& hi) {
                hi.name = "stdio.h";
                hi.definition = "/usr/include/stdio.h";
            },             R"(stdio.h

/usr/include/stdio.h)",
         },
    };

    for(const auto& c: cases) {
        HoverInfo hi;
        c.builder(hi);
        EXPECT_EQ(hi.present().as_plain_text(), c.expected.str());
    }
}

TEST_CASE(PresentHeadings) {
    // Headings don't create any differences in plaintext mode.
    HoverInfo hi;
    hi.kind = SymbolKind::Variable;
    hi.name = "foo";

    EXPECT_EQ(hi.present().as_markdown(), "### variable `foo`");
}

TEST_CASE(PresentRulers) {
    // Rulers behave differently in markdown vs plaintext.
    HoverInfo hi;
    hi.kind = SymbolKind::Variable;
    hi.name = "foo";
    hi.value = "val";
    hi.definition = "def";

    llvm::StringRef expected_markdown =  //
        "### variable `foo`  \n"
        "\n"
        "---\n"
        "Value = `val`  \n"
        "\n"
        "---\n"
        "```cpp\n"
        "def\n"
        "```";
    EXPECT_EQ(hi.present().as_markdown(), expected_markdown.str());

    llvm::StringRef expected_plaintext = R"pt(variable foo

Value = val

def)pt";
    EXPECT_EQ(hi.present().as_plain_text(), expected_plaintext.str());
}

TEST_CASE(ParseDocumentation) {
    struct {
        llvm::StringRef documentation;
        llvm::StringRef markdown;
        llvm::StringRef plaintext;
    } cases[] = {
        {
         " \n foo\nbar",  "foo bar",
         "foo bar", },
        {
         "foo\nbar \n  ",              "foo bar",
         "foo bar", },
        {
         "foo  \nbar","foo bar",
         "foo bar", },
        {
         "foo    \nbar",                "foo bar",
         "foo bar", },
        {
         "foo\n\n\nbar",      "foo  \nbar",
         "foo\nbar", },
        {
         "foo\n\n\n\tbar",  "foo  \nbar",
         "foo\nbar", },
        {
         "foo\n\n\n bar",              "foo  \nbar",
         "foo\nbar", },
        {
         "foo.\nbar","foo.  \nbar",
         "foo.\nbar", },
        {
         "foo. \nbar",                "foo.  \nbar",
         "foo.\nbar", },
        {
         "foo\n*bar",     "foo  \n\\*bar",
         "foo\n*bar", },
        {
         "foo\nbar", "foo bar",
         "foo bar", },
        {
         "Tests primality of `p`.",              "Tests primality of `p`.",
         "Tests primality of `p`.", },
        {
         "'`' should not occur in `Code`",     "'\\`' should not occur in `Code`",
         "'`' should not occur in `Code`", },
        {
         "`not\nparsed`",                "\\`not parsed\\`",
         "`not parsed`", },
    };

    for(const auto& c: cases) {
        markup::Document output;
        feature::parse_documentation(c.documentation, output);

        EXPECT_EQ(output.as_markdown(), c.markdown.str());
        EXPECT_EQ(output.as_plain_text(), c.plaintext.str());
    }
}

TEST_CASE(PlaintextContent) {
    add_main("main.cpp", R"cpp(
int $foo = 1;
)cpp");
    ASSERT_TRUE(compile());

    feature::HoverOptions options;
    options.parse_comment_as_markdown = false;
    result = feature::hover(*unit, nameless_points()[0], options, feature::PositionEncoding::UTF8);

    ASSERT_TRUE(result.has_value());
    auto* content = std::get_if<protocol::MarkupContent>(&result->contents);
    ASSERT_TRUE(content != nullptr);
    ASSERT_EQ(content->kind, protocol::MarkupKind::Plaintext);
    ASSERT_TRUE(content->value.find("variable foo") != std::string::npos);
}

TEST_CASE(ProtocolRange) {
    run(R"cpp(
int $foo = 1;
)cpp");

    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(result->range.has_value());

    // The highlighted token is `foo` on line 1, columns [4, 7).
    ASSERT_EQ(result->range->start.line, 1U);
    ASSERT_EQ(result->range->start.character, 4U);
    ASSERT_EQ(result->range->end.line, 1U);
    ASSERT_EQ(result->range->end.character, 7U);
}

TEST_CASE(ScopedAttribute) {
    run_info(R"cpp(
[[gnu::no$inline]] void foo();
)cpp");

    ASSERT_TRUE(info.has_value());
    ASSERT_EQ(info->name, "noinline");
    ASSERT_EQ(info->local_scope, "gnu");
    ASSERT_EQ(info->kind, SymbolKind::Invalid);
}

TEST_CASE(WhitespaceNoHover) {
    add_main("main.cpp", R"cpp(
int x = 1;
$(p)
int y = 2;
)cpp");
    ASSERT_TRUE(compile());

    // No spelled token touches the empty line, so there is no hover.
    ASSERT_TRUE(!feature::hover_info(*unit, point("p")).has_value());
}

};  // TEST_SUITE(Hover)

}  // namespace

}  // namespace clice::testing
