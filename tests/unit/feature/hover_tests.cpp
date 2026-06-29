/// Ported from clangd's unittests/HoverTests.cpp (llvmorg-21.1.8), part of the LLVM
/// project, licensed under Apache License v2.0 with LLVM Exceptions.
/// See https://llvm.org/LICENSE.txt for license information.

#include <functional>
#include <optional>

#include "test/test.h"
#include "test/tester.h"
#include "feature/feature.h"

#include "kota/meta/enum.h"

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

std::string dump_param(const HoverParam& param) {
    std::string result;
    llvm::raw_string_ostream os(result);
    os << param;
    return result;
}

std::string dump_pass_type(const PassType& pass) {
    std::string result;
    switch(pass.pass_by) {
        case PassMode::Ref: result = "by reference"; break;
        case PassMode::ConstRef: result = "by const reference"; break;
        case PassMode::Value: result = "by value"; break;
    }
    if(pass.converted) {
        result += " (converted)";
    }
    return result;
}

/// Render \p text as a YAML block scalar under \p key, indented by
/// \p indent spaces, so that snapshot files stay valid YAML documents.
std::string yaml_block(llvm::StringRef key, llvm::StringRef text, std::size_t indent = 0) {
    std::string pad(indent, ' ');
    if(text.empty()) {
        return std::format("{}{}: \"\"\n", pad, key.str());
    }

    std::string out = std::format("{}{}: |\n", pad, key.str());
    llvm::SmallVector<llvm::StringRef> lines;
    text.split(lines, '\n', -1, true);
    for(auto line: lines) {
        out += line.empty() ? "\n" : std::format("{}  {}\n", pad, line.str());
    }
    return out;
}

/// Render every field of the hover info as a deterministic, human readable
/// YAML mapping (indented by two spaces, to be nested under the point name).
/// Absent or empty fields are omitted, except for `name` and `kind` which
/// are always printed.
std::string dump(const HoverInfo& info) {
    std::string out;

    auto add = [&](std::string_view key, llvm::StringRef value) {
        out += std::format("  {}: {}\n", key, yaml_str(value));
    };

    auto add_params = [&](std::string_view key, const std::vector<HoverParam>& params) {
        if(params.empty()) {
            out += std::format("  {}: []\n", key);
            return;
        }
        out += std::format("  {}:\n", key);
        for(const auto& param: params) {
            out += std::format("    - {}\n", yaml_str(dump_param(param)));
        }
    };

    add("name", info.name);
    out += std::format(
        "  kind: {}\n",
        kota::meta::enum_name(static_cast<SymbolKind::Kind>(info.kind.value()), "Unknown"));
    if(info.namespace_scope) {
        add("namespace_scope", *info.namespace_scope);
    }
    if(!info.local_scope.empty()) {
        add("local_scope", info.local_scope);
    }
    if(!info.documentation.empty()) {
        add("documentation", info.documentation);
    }
    if(!info.definition.empty()) {
        add("definition", info.definition);
    }
    if(!info.access_specifier.empty()) {
        add("access_specifier", info.access_specifier);
    }
    if(info.type) {
        add("type", dump(info.type));
    }
    if(info.return_type) {
        add("return_type", dump(info.return_type));
    }
    if(info.parameters) {
        add_params("parameters", *info.parameters);
    }
    if(info.template_parameters) {
        add_params("template_parameters", *info.template_parameters);
    }
    if(info.value) {
        add("value", *info.value);
    }
    if(info.size) {
        out += std::format("  size: {}\n", *info.size);
    }
    if(info.offset) {
        out += std::format("  offset: {}\n", *info.offset);
    }
    if(info.padding) {
        out += std::format("  padding: {}\n", *info.padding);
    }
    if(info.align) {
        out += std::format("  align: {}\n", *info.align);
    }
    if(info.callee_arg_info) {
        add("callee_arg_info", dump_param(*info.callee_arg_info));
    }
    if(info.call_pass_type) {
        out += std::format("  passed: {}\n", dump_pass_type(*info.call_pass_type));
    }
    if(info.symbol_range) {
        out += std::format("  symbol_range: \"[{}, {})\"\n",
                           info.symbol_range->begin,
                           info.symbol_range->end);
    }

    out += yaml_block("markdown", info.present().as_markdown(), 2);
    return out;
}

TEST_SUITE(hover, Tester) {

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

TEST_CASE(namespace_decl) {
    run(R"cpp(
namespace $A {
}
)cpp");

    ASSERT_TRUE(result.has_value());
    auto* content = std::get_if<protocol::MarkupContent>(&result->contents);
    ASSERT_TRUE(content != nullptr);
    ASSERT_TRUE(content->value.find("namespace") != std::string::npos);
}

TEST_CASE(function_reference) {
    run(R"cpp(
int foo() { return 0; }
int x = $foo();
)cpp");

    ASSERT_TRUE(result.has_value());
    auto* content = std::get_if<protocol::MarkupContent>(&result->contents);
    ASSERT_TRUE(content != nullptr);
    ASSERT_TRUE(content->value.find("foo") != std::string::npos);
}

TEST_CASE(record_scope) {
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

TEST_CASE(enum_style) {
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

TEST_CASE(function_style) {
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

TEST_CASE(variable_style) {
    compile_only(R"cpp(
void f() {
    constexpr static auto x1 = 1;
}
)cpp");
}

TEST_CASE(auto_and_decltype) {
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

TEST_CASE(expr) {
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

TEST_CASE(structured_no_crash) {
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
             hi.type = "struct X";
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

TEST_CASE(all_no_crash) {
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
             hi.type = "struct Bar";
             hi.definition = "Bar x = <recovery - expr>()";
         }},
    };
    check_cases(cases);
}

TEST_CASE(spaceship_doc_no_crash) {
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

TEST_CASE(invalid_default_args) {
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

TEST_CASE(disable_show_aka) {
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

TEST_CASE(big_ints_no_crash) {
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

TEST_CASE(global_casts_no_crash) {
    /// Use `unsigned long long` so the cast does not truncate the pointer on
    /// LLP64 targets (Windows), where `unsigned long` is only 32 bits.
    run_info(R"cpp(
    using uintptr_t = unsigned long long;
    enum Test : uintptr_t {};
    unsigned global_var;
    void foo() {
      Test v$al = static_cast<Test>(reinterpret_cast<uintptr_t>(&global_var));
    }
  )cpp");
    ASSERT_TRUE(info.has_value());
    EXPECT_EQ(dump(info->value), "&global_var");

    run_info(R"cpp(
    using uintptr_t = unsigned long long;
    unsigned global_var;
    void foo() {
      uintptr_t a$ddress = reinterpret_cast<uintptr_t>(&global_var);
    }
  )cpp");
    ASSERT_TRUE(info.has_value());
    EXPECT_EQ(dump(info->value), "&global_var");
}

TEST_CASE(setter_heuristic_no_crash) {
    run_info(R"cpp(
    /* error-ok */
    template<typename T> T foo(T);

    // Setter variable heuristic might fail if the callexpr is broken.
    struct X { int Y; void @sym[$setY](float) { Y = foo(undefined); } };)cpp");
    ASSERT_TRUE(info.has_value());
}

TEST_CASE(snapshot) {
    /// Pin the target triple so type widths, sizes and layout are identical
    /// on every host platform; otherwise e.g. `sizeof` has type
    /// `unsigned long` on LP64 but `unsigned long long` on LLP64 (Windows)
    /// and the snapshots would diverge.
    triple = "x86_64-unknown-linux-gnu";

    auto transform = [&](std::string_view path) -> std::string {
        if(!compile_file(path)) {
            return "COMPILE_ERROR";
        }

        auto& source = sources.all_files[src_path];
        std::vector<std::pair<std::string, std::uint32_t>> points;
        for(const auto& entry: source.offsets) {
            points.emplace_back(entry.getKey().str(), entry.getValue());
        }
        std::ranges::sort(points);
        for(std::size_t i = 0; i < source.nameless_offsets.size(); ++i) {
            points.emplace_back(std::format("nameless_{}", i), source.nameless_offsets[i]);
        }

        std::string out;
        for(const auto& [name, offset]: points) {
            auto hover = feature::hover_info(*unit, offset);
            if(!hover) {
                out += std::format("{}: NO HOVER\n\n", name);
                continue;
            }
            out += std::format("{}:\n", name);
            out += dump(*hover);
            out += "\n";
        }
        return out;
    };

    ASSERT_SNAPSHOT_GLOB(test_dir + "/hover", "**/*.cpp", transform);
}

TEST_CASE(present) {
    struct {
        std::string_view name;
        std::function<void(HoverInfo&)> builder;
    } cases[] = {
        {"invalid-kind",
         [](HoverInfo& hi) {
             hi.kind = SymbolKind::Invalid;
             hi.name = "X";
         }          },
        {"namespace",
         [](HoverInfo& hi) {
             hi.kind = SymbolKind::Namespace;
             hi.name = "foo";
         }          },
        {"class-template",
         [](HoverInfo& hi) {
             hi.kind = SymbolKind::Class;
             hi.size = 80;
             hi.template_parameters = {
                 {                              {"typename"},                              std::string("T"),         std::nullopt},
                 {           {"typename"},            std::string("C"),      std::string("bool")},
             };
             hi.documentation = "documentation";
             hi.definition = "template <typename T, typename C = bool> class Foo {}";
             hi.name = "foo";
             hi.namespace_scope.emplace();
         }                    },
        {"function-parameters",
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
         }          },
        {"field-byte-layout",
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
         }},
        {"field-bit-layout",
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
         }          },
        {"field-access-specifier",
         [](HoverInfo& hi) {
             hi.kind = SymbolKind::Field;
             hi.access_specifier = "public";
             hi.name = "foo";
             hi.local_scope = "test::Bar::";
             hi.definition = "def";
         }          },
        {"method-protected",
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
         }          },
        {"constructor-parameters",
         [](HoverInfo& hi) {
             hi.definition = "cls(int a, int b = 5)";
             hi.access_specifier = "public";
             hi.kind = SymbolKind::Method;
             hi.namespace_scope = "";
             hi.local_scope = "cls";
             hi.name = "cls";
             hi.parameters = {
                 { {"int"},  std::string("a"), std::nullopt},
                 {{"int"}, std::string("b"), std::string("5")},
             };
         }          },
        {"union-access-specifier",
         [](HoverInfo& hi) {
             hi.kind = SymbolKind::Union;
             hi.access_specifier = "private";
             hi.name = "foo";
             hi.namespace_scope = "ns1::";
             hi.definition = "union foo {}";
         }          },
        {"passed-by-value-as-arg",
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
         }                    },
        {"passed-by-value-unnamed-arg",
         [](HoverInfo& hi) {
             hi.kind = SymbolKind::Variable;
             hi.name = "foo";
             hi.callee_arg_info.emplace();
             hi.callee_arg_info->type = PrintedType("int");
             hi.call_pass_type = PassType{PassMode::Value, false};
         }          },
        {"passed-by-reference-as-arg",
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
         }},
        {"passed-converted-to-alias",
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
         }          },
        {"macro-expansion",
         [](HoverInfo& hi) {
             hi.kind = SymbolKind::Macro;
             hi.name = "PLUS_ONE";
             hi.definition = "#define PLUS_ONE(X) (X+1)\n\n" "// Expands to\n" "(1 + 1)";
         }          },
        {"passed-by-const-ref-converted",
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
         }          },
        {"header-file",
         [](HoverInfo& hi) {
             hi.name = "stdio.h";
             hi.definition = "/usr/include/stdio.h";
         }          },
    };

    for(const auto& c: cases) {
        HoverInfo hi;
        c.builder(hi);
        ASSERT_SNAPSHOT(yaml_block("plaintext", hi.present().as_plain_text()), c.name);
    }
}

TEST_CASE(present_headings) {
    // Headings don't create any differences in plaintext mode.
    HoverInfo hi;
    hi.kind = SymbolKind::Variable;
    hi.name = "foo";

    ASSERT_SNAPSHOT(yaml_block("markdown", hi.present().as_markdown()), "variable-heading-md");
}

TEST_CASE(present_rulers) {
    // Rulers behave differently in markdown vs plaintext.
    HoverInfo hi;
    hi.kind = SymbolKind::Variable;
    hi.name = "foo";
    hi.value = "val";
    hi.definition = "def";

    ASSERT_SNAPSHOT(yaml_block("markdown", hi.present().as_markdown()), "rulers-md");
    ASSERT_SNAPSHOT(yaml_block("plaintext", hi.present().as_plain_text()), "rulers-plaintext");
}

TEST_CASE(parse_documentation) {
    struct {
        std::string_view name;
        llvm::StringRef documentation;
    } cases[] = {
        {"strip-leading-whitespace",  " \n foo\nbar"                  },
        {"strip-trailing-whitespace", "foo\nbar \n  "                 },
        {"join-two-trailing-spaces",  "foo  \nbar"                    },
        {"join-four-trailing-spaces", "foo    \nbar"                  },
        {"blank-lines-break",         "foo\n\n\nbar"                  },
        {"blank-lines-tab-indent",    "foo\n\n\n\tbar"                },
        {"blank-lines-space-indent",  "foo\n\n\n bar"                 },
        {"period-breaks-line",        "foo.\nbar"                     },
        {"period-space-breaks-line",  "foo. \nbar"                    },
        {"star-breaks-line",          "foo\n*bar"                     },
        {"simple-join",               "foo\nbar"                      },
        {"inline-code-span",          "Tests primality of `p`."       },
        {"backtick-in-text",          "'`' should not occur in `Code`"},
        {"multiline-code-span",       "`not\nparsed`"                 },
    };

    for(const auto& c: cases) {
        markup::Document output;
        feature::parse_documentation(c.documentation, output);

        ASSERT_SNAPSHOT(yaml_block("markdown", output.as_markdown()), std::format("{}-md", c.name));
        ASSERT_SNAPSHOT(yaml_block("plaintext", output.as_plain_text()),
                        std::format("{}-plaintext", c.name));
    }
}

TEST_CASE(plaintext_content) {
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
    ASSERT_EQ(content->kind, protocol::MarkupKind::plain_text);
    ASSERT_TRUE(content->value.find("variable foo") != std::string::npos);
}

TEST_CASE(protocol_range) {
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

TEST_CASE(scoped_attribute) {
    run_info(R"cpp(
[[gnu::no$inline]] void foo();
)cpp");

    ASSERT_TRUE(info.has_value());
    ASSERT_EQ(info->name, "noinline");
    ASSERT_EQ(info->local_scope, "gnu");
    ASSERT_EQ(info->kind, SymbolKind::Invalid);
}

TEST_CASE(whitespace_no_hover) {
    add_main("main.cpp", R"cpp(
int x = 1;
$(p)
int y = 2;
)cpp");
    ASSERT_TRUE(compile());

    // No spelled token touches the empty line, so there is no hover.
    ASSERT_TRUE(!feature::hover_info(*unit, point("p")).has_value());
}

};  // TEST_SUITE(hover)

}  // namespace

}  // namespace clice::testing
