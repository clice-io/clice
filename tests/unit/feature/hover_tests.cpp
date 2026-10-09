module;

#include "modules/prelude.h"
/// Ported from clangd's unittests/HoverTests.cpp (llvmorg-21.1.8), part of the LLVM
/// project, licensed under Apache License v2.0 with LLVM Exceptions.
/// See https://llvm.org/LICENSE.txt for license information.

module clice;

import :feature.feature;
import :tests.unit.test.test;
import :tests.unit.test.tester;

namespace clice::testing {

namespace {

namespace protocol = kota::ipc::protocol;

using HoverInfo = feature::HoverInfo;
using PrintedType = display::Type;
using HoverParam = display::Param;
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

ZEST_SUITE(hover, Tester) {

std::optional<protocol::Hover> result;
std::optional<HoverInfo> info;
llvm::StringRef current_code;

void run(llvm::StringRef code) {
    add_main("main.cpp", code);
    ZASSERT(compile());

    auto points = nameless_points();
    ZASSERT(points.size() == 1U);
    auto offset = points[0];
    result = feature::hover(*unit, offset, {}, PositionEncoding::UTF8);
}

void compile_only(llvm::StringRef code) {
    add_main("main.cpp", code);
    ZASSERT(compile());
}

/// Compile the code and compute hover info on the (single) annotated point.
/// The point is either a nameless `§` marker or a named `§(p)` marker, and the
/// expected highlighted token may be wrapped in a `§(sym)⟦...⟧` range.
void run_info(llvm::StringRef code,
              const feature::HoverOptions& options = {},
              llvm::StringRef standard = "-std=c++20") {
    info.reset();
    current_code = code;

    add_main("main.cpp", code);
    ZASSERT(compile(standard));

    auto points = nameless_points();
    std::uint32_t offset = points.size() == 1 ? points[0] : point("p");
    info = feature::hover_info(*unit, offset, options);
}

void expect_hover(const HoverInfo& expected) {
    if(!info) {
        std::println("no hover result for:\n{}", current_code.str());
    }
    ZASSERT(info);

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

    ZEXPECT(dump(info->namespace_scope) == dump(expected.namespace_scope));
    ZEXPECT(info->local_scope == expected.local_scope);
    ZEXPECT(info->name == expected.name);
    ZEXPECT(info->kind.value() == expected.kind.value());
    ZEXPECT(info->documentation == expected.documentation);
    ZEXPECT(info->definition == expected.definition);
    ZEXPECT(info->access_specifier == expected.access_specifier);
    ZEXPECT(dump(info->type) == dump(expected.type));
    ZEXPECT(dump(info->return_type) == dump(expected.return_type));
    ZEXPECT(dump(info->parameters) == dump(expected.parameters));
    ZEXPECT(dump(info->template_parameters) == dump(expected.template_parameters));
    ZEXPECT(dump(info->value) == dump(expected.value));
    ZEXPECT(dump(info->size) == dump(expected.size));
    ZEXPECT(dump(info->offset) == dump(expected.offset));
    ZEXPECT(dump(info->padding) == dump(expected.padding));
    ZEXPECT(dump(info->align) == dump(expected.align));
    ZEXPECT(dump(info->callee_arg_info) == dump(expected.callee_arg_info));
    ZEXPECT(dump(info->call_pass_type) == dump(expected.call_pass_type));
}

void check_sym_range() {
    if(!info || !current_code.contains("§(sym)⟦")) {
        return;
    }

    auto expected = range("sym");
    ZASSERT(info->symbol_range);
    if(*info->symbol_range != expected) {
        std::println("symbol range mismatch for:\n{}", current_code.str());
    }
    ZEXPECT(info->symbol_range->begin == expected.begin);
    ZEXPECT(info->symbol_range->end == expected.end);
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

ZEST_CASE(namespace_decl) {
    run(R"cpp(
namespace §A {
}
)cpp");

    ZASSERT(result);
    auto* content = std::get_if<protocol::MarkupContent>(&result->contents);
    ZASSERT(content != nullptr);
    ZASSERT(content->value.find("namespace") != std::string::npos);
}

ZEST_CASE(function_reference) {
    run(R"cpp(
int foo() { return 0; }
int x = §foo();
)cpp");

    ZASSERT(result);
    auto* content = std::get_if<protocol::MarkupContent>(&result->contents);
    ZASSERT(content != nullptr);
    ZASSERT(content->value.find("foo") != std::string::npos);
}

ZEST_CASE(record_scope) {
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

ZEST_CASE(enum_style) {
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

ZEST_CASE(function_style) {
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

ZEST_CASE(variable_style) {
    compile_only(R"cpp(
void f() {
    constexpr static auto x1 = 1;
}
)cpp");
}

ZEST_CASE(auto_and_decltype) {
    compile_only(R"cpp(
§(a1)aut§(a2)o§(a3) i = -1;

§(d1)dec§(d2)ltype§(d3)(i) j = 2;

struct A { int x; };

aut§(a4)o va§(a5)r = A{};

a§(fa)uto f1() { return 1; }

de§(fn_decltype)cltype(au§(fn_decltype_auto)to) f2() {}

int f3(au§(fn_para_auto)to x) {}
)cpp");
}

ZEST_CASE(expr) {
    compile_only(R"cpp(
int xxxx = 1;
int yyyy = xx§(e1)xx;

struct A {
    int function(int param) {
        return thi§(e2)s§(e3)->§(e4)funct§(e5)ion(para§(e6)m);
    }

    int fn(int param) {
        return static§(e7)_cast<A*>(nul§(e8)lptr)->function(par§(e9)am);
    }
};
)cpp");
}

ZEST_CASE(structured_no_crash) {
    HoverCase cases[] = {
        // Field type initializer.
        {R"cpp(
          struct X { int x = 2; };
          X §(sym)⟦§x⟧;
          )cpp",
         [](HoverInfo& hi) {
             hi.name = "x";
             hi.kind = SymbolKind::Variable;
             hi.namespace_scope = "";
             hi.definition = "X x";
             hi.type = "X";
         }},
        // Don't crash on null types.
        {R"cpp(auto [§(sym)⟦§x⟧] = 1; /*error-ok*/)cpp",
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
          Unknown §(sym)⟦§abc⟧ = invalid;
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
          Bar §(sym)⟦x§x⟧;
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
          int §(sym)⟦y§y⟧;
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
            §(sym)⟦§{⟧1} // FIXME: why the hover range is 1 character?
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

ZEST_CASE(all_no_crash) {
    HoverCase cases[] = {
        {R"cpp(// Should not crash when evaluating the initializer.
            struct Test {};
            void test() { Test && §(sym)⟦te§st⟧ = {}; }
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
            void Foo::foo(Bar §(sym)⟦§x⟧) {})cpp",
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

ZEST_CASE(spaceship_doc_no_crash) {
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
  static_assert(S<void>() =§= S<void>());
    )cpp");

    ZASSERT(info);
    ZEXPECT(info->documentation == "");
}

ZEST_CASE(invalid_default_args) {
    // Function parameter default values are not evaluated on invalid decls.
    run_info(R"cpp(
        // error-ok testing behavior on invalid decl
        class Foo {};
        void foo(Foo p§aram = nullptr);
        )cpp");
    ZASSERT(info);
    ZEXPECT(!info->value.has_value());

    run_info(R"cpp(
        class Foo {};
        void foo(Foo *p§aram = nullptr);
        )cpp");
    ZASSERT(info);
    ZEXPECT(dump(info->value) == "nullptr");
}

ZEST_CASE(disable_show_aka) {
    feature::HoverOptions options;
    options.show_aka = false;

    run_info(R"cpp(
    using m_int = int;
    m_int §(sym)⟦§a⟧;
  )cpp",
             options,
             "-std=c++17");

    ZASSERT(info);
    ZEXPECT(dump(info->type) == dump(std::optional(PrintedType("m_int"))));
    check_sym_range();
}

ZEST_CASE(max_members_option) {
    feature::HoverOptions options;
    options.max_members = 0;

    run_info(R"cpp(
    struct Record { int field; };
    Re§cord record;
  )cpp",
             options);
    ZASSERT(info);
    ZEXPECT(info->definition == "struct Record");

    run_info(R"cpp(
    enum Value { one, two };
    Va§lue value;
  )cpp",
             options);
    ZASSERT(info);
    ZEXPECT(info->definition == "enum Value");
}

ZEST_CASE(tag_members_no_crash) {
    run_info(R"cpp(
    struct Record {
        struct { int x; } method();
        int field;
    };
    Re§cord record;
  )cpp");
    ZASSERT(info);

    run_info(R"cpp(
    struct Record {
        int get() const;
        __declspec(property(get = get)) struct { int x; } property;
    };
    Re§cord record;
  )cpp");
    ZASSERT(info);
}

ZEST_CASE(big_ints_no_crash) {
    // APInt64 wrap around.
    run_info(R"cpp(
    constexpr unsigned long value = -1; // wrap around
    void foo() { va§lue; }
  )cpp");
    ZASSERT(info);

    // __int128_t value printing.
    run_info(R"cpp(
    constexpr __int128_t value = -4;
    void foo() { va§lue; }
  )cpp");
    ZASSERT(info);
    ZEXPECT(dump(info->value) == "-4 (0xfffffffc)");
}

ZEST_CASE(global_casts_no_crash) {
    /// Use `unsigned long long` so the cast does not truncate the pointer on
    /// LLP64 targets (Windows), where `unsigned long` is only 32 bits.
    run_info(R"cpp(
    using uintptr_t = unsigned long long;
    enum Test : uintptr_t {};
    unsigned global_var;
    void foo() {
      Test v§al = static_cast<Test>(reinterpret_cast<uintptr_t>(&global_var));
    }
  )cpp");
    ZASSERT(info);
    ZEXPECT(dump(info->value) == "&global_var");

    run_info(R"cpp(
    using uintptr_t = unsigned long long;
    unsigned global_var;
    void foo() {
      uintptr_t a§ddress = reinterpret_cast<uintptr_t>(&global_var);
    }
  )cpp");
    ZASSERT(info);
    ZEXPECT(dump(info->value) == "&global_var");
}

ZEST_CASE(setter_heuristic_no_crash) {
    run_info(R"cpp(
    /* error-ok */
    template<typename T> T foo(T);

    // Setter variable heuristic might fail if the callexpr is broken.
    struct X { int Y; void §(sym)⟦§setY⟧(float) { Y = foo(undefined); } };)cpp");
    ZASSERT(info);
}

ZEST_CASE(parse_documentation) {
    struct {
        llvm::StringRef documentation;
        llvm::StringRef markdown;
        llvm::StringRef plain_text;
    } cases[] = {
        // Leading/trailing whitespace strips; plain newlines join, and so
        // does markdown's own hard-break spelling (trailing spaces).
        {" \n foo\nbar",                   "foo bar",                 "foo bar"                       },
        {"foo\nbar \n  ",                  "foo bar",                 "foo bar"                       },
        {"foo  \nbar",                     "foo bar",                 "foo bar"                       },
        {"foo    \nbar",                   "foo bar",                 "foo bar"                       },
        {"foo\nbar",                       "foo bar",                 "foo bar"                       },
        // Blank lines, indentation, sentence ends and list stars break.
        {"foo\n\n\nbar",                   "foo  \nbar",              "foo\nbar"                      },
        {"foo\n\n\n\tbar",                 "foo  \nbar",              "foo\nbar"                      },
        {"foo\n\n\n bar",                  "foo  \nbar",              "foo\nbar"                      },
        {"foo.\nbar",                      "foo.  \nbar",             "foo.\nbar"                     },
        {"foo. \nbar",                     "foo.  \nbar",             "foo.\nbar"                     },
        {"foo\n*bar",                      "foo  \n\\*bar",           "foo\n*bar"                     },
        // Inline code spans survive; stray backticks escape in markdown and
        // multiline spans collapse onto one line.
        {"Tests primality of `p`.",        "Tests primality of `p`.", "Tests primality of `p`."       },
        {"'`' should not occur in `Code`",
         "'\\`' should not occur in `Code`",                          "'`' should not occur in `Code`"},
        {"`not\nparsed`",                  "\\`not parsed\\`",        "`not parsed`"                  },
    };

    for(const auto& c: cases) {
        markup::Document output;
        feature::parse_documentation(c.documentation, output);
        ZEXPECT(output.as_markdown() == c.markdown);
        ZEXPECT(output.as_plain_text() == c.plain_text);
    }
}

ZEST_CASE(plaintext_content) {
    add_main("main.cpp", R"cpp(
int §foo = 1;
)cpp");
    ZASSERT(compile());

    feature::HoverOptions options;
    options.parse_comment_as_markdown = false;
    result = feature::hover(*unit, nameless_points()[0], options, PositionEncoding::UTF8);

    ZASSERT(result);
    auto* content = std::get_if<protocol::MarkupContent>(&result->contents);
    ZASSERT(content != nullptr);
    ZASSERT(content->kind == protocol::MarkupKind::PlainText);
    ZASSERT(content->value.find("variable foo") != std::string::npos);
}

ZEST_CASE(protocol_range) {
    run(R"cpp(
int §foo = 1;
)cpp");

    ZASSERT(result);
    ZASSERT(result->range);

    // The highlighted token is `foo` on line 1, columns [4, 7).
    ZASSERT(result->range->start.line == 1U);
    ZASSERT(result->range->start.character == 4U);
    ZASSERT(result->range->end.line == 1U);
    ZASSERT(result->range->end.character == 7U);
}

ZEST_CASE(include_header) {
    add_file("test.h", "#pragma once\n");
    add_main("main.cpp", R"cpp(
#include §(arg)⟦"test.h"§⟧
§(outside)
int x = 0;
)cpp");
    ZASSERT(compile());

    auto arg = range("arg", "main.cpp");
    info = feature::hover_info(*unit, arg.begin + 1);
    ZASSERT(info);
    ZEXPECT(info->kind == SymbolKind::Header);
    ZEXPECT(info->name == "test.h");

    ZEXPECT(info->definition == TestVFS::path("test.h"));
    ZASSERT(info->symbol_range);
    ZEXPECT(info->symbol_range->begin == arg.begin);
    ZEXPECT(info->symbol_range->end == arg.end);

    result = feature::hover(*unit, arg.begin + 1, {}, PositionEncoding::UTF8);
    ZASSERT(result);
    auto* content = std::get_if<protocol::MarkupContent>(&result->contents);
    ZASSERT(content != nullptr);
    ZEXPECT(content->value.contains("test.h"));
    ZEXPECT(content->value.contains(TestVFS::path("test.h")));
    ZASSERT(result->range);
    ZEXPECT(result->range->start.line == 1U);
    ZEXPECT(result->range->start.character == 9U);
    ZEXPECT(result->range->end.line == 1U);
    ZEXPECT(result->range->end.character == 17U);

    ZEXPECT(!feature::hover_info(*unit, point("outside")).has_value());
}

ZEST_CASE(has_include_header) {
    add_file("test.h", "#pragma once\n");
    add_main("main.cpp", R"cpp(
#if __has_include(§(arg)⟦"test.h"§⟧)
#endif
)cpp");
    ZASSERT(compile());

    auto arg = range("arg", "main.cpp");
    auto hover = feature::hover_info(*unit, arg.begin + 1);
    ZASSERT(hover);
    ZEXPECT(hover->kind == SymbolKind::Header);
    ZEXPECT(hover->name == "test.h");

    ZEXPECT(hover->definition == TestVFS::path("test.h"));
    ZEXPECT(hover->symbol_range == arg);
}

ZEST_CASE(embed_file) {
    add_file("data.bin", "0123456789");
    add_main("main.cpp", R"cpp(
const unsigned char data[] = {
#embed §(arg)⟦"data.bin"§⟧
};
)cpp");
    ZASSERT(compile("-std=c++23"));

    auto arg = range("arg", "main.cpp");
    auto hover = feature::hover_info(*unit, arg.begin + 1);
    ZASSERT(hover);
    ZEXPECT(hover->kind == SymbolKind::Header);
    ZEXPECT(hover->name == "data.bin");

    ZEXPECT(hover->definition == TestVFS::path("data.bin"));
    ZEXPECT(hover->symbol_range == arg);
}

ZEST_CASE(has_embed_file) {
    add_file("data.bin", "0123456789");
    add_main("main.cpp", R"cpp(
#if __has_embed(§(arg)⟦"data.bin"§⟧)
#endif
)cpp");
    ZASSERT(compile("-std=c++23"));

    auto arg = range("arg", "main.cpp");
    auto hover = feature::hover_info(*unit, arg.begin + 1);
    ZASSERT(hover);
    ZEXPECT(hover->kind == SymbolKind::Header);
    ZEXPECT(hover->name == "data.bin");

    ZEXPECT(hover->definition == TestVFS::path("data.bin"));
    ZEXPECT(hover->symbol_range == arg);
}

ZEST_CASE(macro_include_header) {
    add_file("test.h", "#pragma once\n");
    add_main("main.cpp", R"cpp(
#define HEADER "test.h"
#include §(arg)⟦HEADER§⟧
)cpp");
    ZASSERT(compile());

    auto arg = range("arg", "main.cpp");
    auto hover = feature::hover_info(*unit, arg.begin + 1);
    ZASSERT(hover);
    ZEXPECT(hover->kind == SymbolKind::Header);
    ZEXPECT(hover->name == "HEADER");

    ZEXPECT(hover->definition == TestVFS::path("test.h"));
    ZEXPECT(hover->symbol_range == arg);
}

ZEST_CASE(missing_include_header) {
    add_main("main.cpp", R"cpp(
/* error-ok */
#include §(arg)⟦"missing.h"§⟧
)cpp");
    ZASSERT(compile());

    auto arg = range("arg", "main.cpp");
    ZEXPECT(!feature::hover_info(*unit, arg.begin + 1).has_value());
}

ZEST_CASE(scoped_attribute) {
    run_info(R"cpp(
[[gnu::no§inline]] void foo();
)cpp");

    ZASSERT(info);
    ZASSERT(info->name == "noinline");
    ZASSERT(info->local_scope == "gnu");
    ZASSERT(info->kind == SymbolKind::Invalid);
}

ZEST_CASE(whitespace_no_hover) {
    add_main("main.cpp", R"cpp(
int x = 1;
§(p)
int y = 2;
)cpp");
    ZASSERT(compile());

    // No spelled token touches the empty line, so there is no hover.
    ZASSERT(!feature::hover_info(*unit, point("p")).has_value());
}

};  // ZEST_SUITE(hover)

}  // namespace

}  // namespace clice::testing
