module;

#include "modules/prelude.h"

module clice;

import :feature.feature;
import :index.tu_index;
import :unittests.utils;
import :unittests.utils.tester;

namespace clice::testing {

namespace {

ZEST_SUITE(index_projection, Tester) {

/// The compiled unit's TU index and the main file's entries, extracted the
/// way the router's index slice does it in production.
index::TUIndex tu;
std::string bytes;
feature::IndexEntries entries;

void extract_entries() {
    bytes = index::build_tu_index(*unit);
    tu = index::TUIndex::from_bytes(bytes);

    auto main_id = tu.path_count() - 1;
    entries = feature::extract_index_entries(tu.shard_of(main_id));
}

std::optional<index::SymbolRef> resolve(index::SymbolHash hash) {
    auto to_ref = [&](const index::SymbolIdentity& identity) {
        return index::SymbolRef{.hash = hash,
                                .name = std::string(identity.name),
                                .args = std::string(identity.args),
                                .parent = identity.parent,
                                .kind = identity.kind,
                                .flags = identity.flags};
    };
    if(auto identity = tu.find_symbol(hash)) {
        return to_ref(*identity);
    }
    auto main_id = tu.path_count() - 1;
    if(auto identity = tu.shard_of(main_id).find_symbol(hash)) {
        return to_ref(*identity);
    }
    return std::nullopt;
}

auto resolver() {
    return [this](index::SymbolHash hash) {
        return resolve(hash);
    };
}

/// The outline the index projects from the unit's own entries.
auto projected_outline() -> std::vector<feature::DocumentSymbol> {
    return feature::index_document_symbols(unit->focus_file().content(),
                                           unit->lang_options(),
                                           entries,
                                           resolver(),
                                           {});
}

/// The projected tokens of the compiled unit equal the AST's, down to the
/// modifiers the index knows; both under the Tester's C++20.
void expect_tokens_match_ast() {
    auto ast = feature::semantic_tokens(*unit);
    auto projected =
        feature::index_semantic_tokens(unit->focus_file().content(),
                                       feature::index_lang_options("main.cpp", false, "c++20"),
                                       entries.occurrences,
                                       entries.decls,
                                       resolver());

    // The index knows Declaration/Definition; every other AST modifier
    // (Readonly, Static, Virtual, ...) is a pinned degradation.
    auto pinned = SymbolModifiers::to_mask(SymbolModifiers::Declaration) |
                  SymbolModifiers::to_mask(SymbolModifiers::Definition);

    ZASSERT(projected.size() == ast.size());
    for(std::size_t i = 0; i < ast.size(); i += 1) {
        ZASSERT(projected[i].range.begin == ast[i].range.begin);
        ZASSERT(projected[i].range.end == ast[i].range.end);
        ZASSERT(projected[i].kind.value_of() == ast[i].kind.value_of());
        ZASSERT(projected[i].modifiers == (ast[i].modifiers & pinned));
    }
}

ZEST_CASE(TokensMatchAst) {
    add_main("main.cpp", R"cpp(
// a line comment
#define VALUE 1

struct Point {
    int x;
    int y;
    int sum();
    Point operator+(Point other);
    ~Point();
};

int Point::sum() {
    return x + y + VALUE;
}

Point::~Point() {}

int total(Point point, int base) {
    const char* label = "sum";
    char letter = 's';
    if(base > 0) {
        return point.sum() + base;
    }
    return 0;
}
)cpp");
    ZASSERT(compile());
    extract_entries();
    expect_tokens_match_ast();
}

ZEST_CASE(OverrideTokensMatchAst) {
    // `override` and `final` lead to the methods they override without
    // spelling them; a class's `final` leads nowhere.
    add_main("main.cpp", R"cpp(
struct Base {
    virtual void draw();
    virtual void paint();
};

struct Derived final : Base {
    void draw() override;
    void paint() final;
};
)cpp");
    ZASSERT(compile());
    extract_entries();
    expect_tokens_match_ast();
}

ZEST_CASE(ModuleTokensMatchAst) {
    add_main("main.cpp", R"cpp(
export module demo.core;

export int exported_value = 1;
)cpp");
    ZASSERT(compile());
    extract_entries();
    expect_tokens_match_ast();
}

ZEST_CASE(ModuleKeywordsMatchAst) {
    // The contextual `module` and `import` are keywords only where they
    // open a declaration or an import; variables of those names stay
    // variables.
    add_files("main.cppm", R"(
#[dep.cppm]
export module dep;
export int value = 1;

#[main.cppm]
module;
export module demo.core;
import dep;
export import dep;
int use() {
    int module = value;
    int import = value;
module = 2;
import;
    return module + import;
}
module :private;
)");
    ZASSERT(compile_with_modules());
    extract_entries();
    expect_tokens_match_ast();
}

ZEST_CASE(ModuleOutlineMatchesAst) {
    add_main("main.cpp", R"cpp(
export module demo.core;

export int entry();
)cpp");
    ZASSERT(compile());
    extract_entries();

    auto ast = feature::document_symbols(*unit, {});
    auto projected = projected_outline();
    ZASSERT(ast.size() == std::size_t(2));
    ZASSERT(ast[0].name == "demo.core");
    ZASSERT(ast[0].kind.value_of() == SymbolKind(SymbolKind::Module).value_of());
    ZASSERT(projected.size() == ast.size());
    for(std::size_t i = 0; i < ast.size(); i += 1) {
        ZASSERT(projected[i].name == ast[i].name);
        ZASSERT(projected[i].kind.value_of() == ast[i].kind.value_of());
        ZASSERT(projected[i].selection_range == ast[i].selection_range);
    }
}

ZEST_CASE(OutlineMatchesAst) {
    add_main("main.cpp", R"cpp(
#define LIMIT 10
#define FIELDS int generated_a; int generated_b;

namespace app {

struct Point {
    int x, y;
    int sum();
    friend void inspect(Point&);
    FIELDS
};

int Point::sum() {
    return x + y;
}

typedef struct {
    int width;
} Size;

template <typename T>
struct Box {
    T value;
};

template struct Box<int>;

namespace alias = app;

enum class Color {
    Red,
    Blue,
};

[[nodiscard]] int scale(int value) {
    int doubled = value * 2;
    return doubled;
}

}
)cpp");
    ZASSERT(compile());
    extract_entries();

    auto ast = feature::document_symbols(*unit, {});
    auto projected = projected_outline();

    auto compare = [](auto& self,
                      const std::vector<feature::DocumentSymbol>& lhs,
                      const std::vector<feature::DocumentSymbol>& rhs) -> void {
        ZASSERT(lhs.size() == rhs.size());
        for(std::size_t i = 0; i < lhs.size(); i += 1) {
            ZASSERT(lhs[i].name == rhs[i].name);
            ZASSERT(lhs[i].kind.value_of() == rhs[i].kind.value_of());
            ZASSERT(lhs[i].range == rhs[i].range);
            ZASSERT(lhs[i].selection_range == rhs[i].selection_range);
            ZASSERT(lhs[i].deprecated == rhs[i].deprecated);
            self(self, lhs[i].children, rhs[i].children);
        }
    };
    compare(compare, projected, ast);
}

ZEST_CASE(FoldsSubsetOfAst) {
    add_main("main.cpp", R"cpp(
namespace app {

struct Point { int x; };

struct Holder {
    int value;

    Holder() : value{0} {
        value += 1;
    }
};

int compute() {
    return Holder().value;
}

}
)cpp");
    ZASSERT(compile());
    extract_entries();

    auto ast = feature::folding_ranges(*unit);
    auto projected = feature::index_folding_ranges(unit->focus_file().content(),
                                                   feature::index_lang_options("main.cpp", false),
                                                   entries.decls,
                                                   resolver());

    ZASSERT(!projected.empty());
    for(auto& fold: projected) {
        bool known = std::ranges::any_of(ast, [&](const feature::FoldingRange& twin) {
            return twin.range == fold.range;
        });
        ZASSERT(known);
    }

    // The constructor's fold anchors at its body, not the member
    // initializer's braces.
    auto body = unit->focus_file().content().find("{\n        value += 1;");
    bool anchored = std::ranges::any_of(projected, [&](const feature::FoldingRange& fold) {
        return fold.range.begin == body;
    });
    ZASSERT(anchored);
}

ZEST_CASE(FoldsMatchAstShape) {
    // A brace below its declaration's head folds from the head, a block
    // folds as its declaration's kind, and braces several decls share (the
    // struct's and the alias's) fold once — as the AST folds them.
    add_main("main.cpp", R"cpp(
struct Allman
{
    int x;
    int y;
};

int compute()
{
    int a = 1;
    return a;
}

typedef struct {
    int x;
    int y;
} Point;
)cpp");
    ZASSERT(compile());
    extract_entries();

    auto ast = feature::folding_ranges(*unit);
    auto projected = feature::index_folding_ranges(unit->focus_file().content(),
                                                   feature::index_lang_options("main.cpp", false),
                                                   entries.decls,
                                                   resolver());

    ZASSERT(projected.size() == std::size_t(3));
    for(auto& fold: projected) {
        auto twin = std::ranges::find_if(ast, [&](const feature::FoldingRange& candidate) {
            return candidate.range == fold.range;
        });
        ZASSERT(twin != ast.end());
        ZASSERT(fold.kind == twin->kind);
        ZASSERT(fold.lines == twin->lines);
    }
    ZASSERT(projected[0].lines);
    ZASSERT(projected[1].lines);
}

ZEST_CASE(InitializerFoldsAtBrace) {
    add_main("main.cpp", R"cpp(
int values[] =
{
    1,
    2,
};
)cpp");
    ZASSERT(compile());
    extract_entries();

    auto projected = feature::index_folding_ranges(unit->focus_file().content(),
                                                   feature::index_lang_options("main.cpp", false),
                                                   entries.decls,
                                                   resolver());
    ZASSERT(projected.size() == std::size_t(1));
    ZASSERT(!projected[0].lines.has_value());
}

ZEST_CASE(ConditionalBracesSuppressFold) {
    // f's branches unbalance braces: any raw pairing ends the fold in a
    // branch the indexed parse never took, so the fold is suppressed. g's
    // conditional keeps every branch balanced and folds normally.
    llvm::StringRef content = R"cpp(void f() {
#if defined(X)
}
#else
}
#endif

void g() {
#if defined(Y)
    int a = 1;
#else
    int a = 2;
#endif
}
)cpp";
    auto f_end = static_cast<std::uint32_t>(content.find("#endif") + 6);
    auto g_begin = static_cast<std::uint32_t>(content.find("void g"));
    auto g_end = static_cast<std::uint32_t>(content.rfind('}') + 1);
    std::vector<feature::IndexDecl> entries = {
        {.range = {5, 6},                     .extent = {0, f_end}, .symbol = 1, .definition = true},
        {.range = {g_begin + 5, g_begin + 6},
         .extent = {g_begin, g_end},
         .symbol = 2,
         .definition = true                                                                        },
    };
    auto resolve_synthetic = [](index::SymbolHash hash) -> std::optional<index::SymbolRef> {
        return index::SymbolRef{.name = hash == 1 ? "f" : "g", .kind = SymbolKind::Function};
    };

    auto folds = feature::index_folding_ranges(content,
                                               feature::index_lang_options("main.cpp", false),
                                               entries,
                                               resolve_synthetic);
    ZASSERT(folds.size() == std::size_t(1));
    ZASSERT(folds[0].range.begin == static_cast<std::uint32_t>(content.find("{", g_begin)));
    ZASSERT(folds[0].range.end == g_end);
}

ZEST_CASE(MergedKindsConflict) {
    llvm::StringRef content = "value;\n";
    std::vector<index::Occurrence> merged = {
        {.range = {0, 5}, .target = 1},
        {.range = {0, 5}, .target = 2},
    };
    auto resolve_synthetic = [](index::SymbolHash hash) -> std::optional<index::SymbolRef> {
        if(hash == 1) {
            return index::SymbolRef{.name = "value", .kind = SymbolKind::Variable};
        }
        return index::SymbolRef{.name = "value", .kind = SymbolKind::Function};
    };

    auto tokens = feature::index_semantic_tokens(content,
                                                 feature::index_lang_options("main.cpp", false),
                                                 merged,
                                                 {},
                                                 resolve_synthetic);
    ZASSERT(tokens.size() == std::size_t(1));
    ZASSERT(tokens[0].kind.value_of() == SymbolKind(SymbolKind::Conflict).value_of());
}

ZEST_CASE(ModuleNameComponents) {
    // The index stores one occurrence spanning the whole written module
    // name; every identifier component must classify, as on the AST path
    // (separators and the contextual `module`/`import` stay unpainted).
    llvm::StringRef content = "export module demo.core;\nimport foo:part;\n";
    std::vector<index::Occurrence> merged = {
        {.range = {14, 23}, .target = 1},
        {.range = {32, 40}, .target = 1},
    };
    auto resolve_synthetic = [](index::SymbolHash) -> std::optional<index::SymbolRef> {
        return index::SymbolRef{.name = "demo.core", .kind = SymbolKind::Module};
    };

    auto tokens = feature::index_semantic_tokens(content,
                                                 feature::index_lang_options("main.cppm", false),
                                                 merged,
                                                 {},
                                                 resolve_synthetic);
    std::vector<std::pair<std::uint32_t, std::uint32_t>> modules;
    for(auto& token: tokens) {
        if(token.kind == SymbolKind::Module) {
            modules.emplace_back(token.range.begin, token.range.end);
        }
    }
    std::vector<std::pair<std::uint32_t, std::uint32_t>> expected = {
        {14, 18},
        {19, 23},
        {32, 35},
        {36, 40},
    };
    ZASSERT(modules == expected);
}

ZEST_CASE(CDialectKeywords) {
    // `class` is a valid C identifier: entries built by C parses must lex
    // under the C keyword table, or the C++ table would take `class` as
    // a keyword and shadow the entry's kind.
    llvm::StringRef content = "int class;\n";
    std::vector<feature::IndexDecl> entries = {
        {.range = {4, 9}, .extent = {0, 10}, .symbol = 1, .definition = true},
    };
    auto resolve_synthetic = [](index::SymbolHash) -> std::optional<index::SymbolRef> {
        return index::SymbolRef{.name = "class", .kind = SymbolKind::Variable};
    };

    auto c_tokens = feature::index_semantic_tokens(content,
                                                   feature::index_lang_options("header.h", true),
                                                   {},
                                                   entries,
                                                   resolve_synthetic);
    ZASSERT(c_tokens.size() == std::size_t(2));
    ZASSERT(c_tokens[1].kind.value_of() == SymbolKind(SymbolKind::Variable).value_of());

    auto cpp_tokens = feature::index_semantic_tokens(content,
                                                     feature::index_lang_options("header.h", false),
                                                     {},
                                                     entries,
                                                     resolve_synthetic);
    ZASSERT(cpp_tokens.size() == std::size_t(2));
    ZASSERT(cpp_tokens[1].kind.value_of() == SymbolKind(SymbolKind::Keyword).value_of());
}

ZEST_CASE(DriverDefaultKeywords) {
    // The driver turns on `char8_t` from C++20 and the GNU keywords in GNU
    // modes; the language defaults alone leave both off.
    llvm::StringRef content = "char8_t c;\ntypeof(c) d;\n";
    auto tokens = feature::index_semantic_tokens(
        content,
        feature::index_lang_options("main.cpp", false, "gnu++20"),
        {},
        {},
        [](index::SymbolHash) -> std::optional<index::SymbolRef> { return std::nullopt; });
    auto kind_at = [&](std::uint32_t begin) {
        auto token = std::ranges::find_if(tokens, [&](const feature::SemanticToken& token) {
            return token.range.begin == begin;
        });
        return token == tokens.end() ? SymbolKind(SymbolKind::Invalid).value_of()
                                     : token->kind.value_of();
    };
    ZASSERT(kind_at(0) == SymbolKind(SymbolKind::Primitive).value_of());
    ZASSERT(kind_at(11) == SymbolKind(SymbolKind::Keyword).value_of());
}

ZEST_CASE(ModuleKeywordsNeedModules) {
    // Before C++20 a line-leading `module` is a plain name.
    llvm::StringRef content = "module m;\n";
    auto tokens = feature::index_semantic_tokens(
        content,
        feature::index_lang_options("main.cpp", false, "c++17"),
        {},
        {},
        [](index::SymbolHash) -> std::optional<index::SymbolRef> { return std::nullopt; });
    ZASSERT(std::ranges::none_of(tokens, [](const feature::SemanticToken& token) {
        return token.kind == SymbolKind::Keyword;
    }));
}

ZEST_CASE(StandardFromCommand) {
    // `concept` is a plain identifier in C++17: entries built under an older
    // -std must lex with that standard's keyword table, or a newer
    // table would take `concept` as a keyword and shadow the entry.
    llvm::StringRef content = "int concept;\n";
    std::vector<feature::IndexDecl> entries = {
        {.range = {4, 11}, .extent = {0, 12}, .symbol = 1, .definition = true},
    };
    auto resolve_synthetic = [](index::SymbolHash) -> std::optional<index::SymbolRef> {
        return index::SymbolRef{.name = "concept", .kind = SymbolKind::Variable};
    };

    auto cxx17_tokens =
        feature::index_semantic_tokens(content,
                                       feature::index_lang_options("main.cpp", false, "c++17"),
                                       {},
                                       entries,
                                       resolve_synthetic);
    ZASSERT(cxx17_tokens.size() == std::size_t(2));
    ZASSERT(cxx17_tokens[1].kind.value_of() == SymbolKind(SymbolKind::Variable).value_of());

    auto cxx20_tokens =
        feature::index_semantic_tokens(content,
                                       feature::index_lang_options("main.cpp", false, "c++20"),
                                       {},
                                       entries,
                                       resolve_synthetic);
    ZASSERT(cxx20_tokens.size() == std::size_t(2));
    ZASSERT(cxx20_tokens[1].kind.value_of() == SymbolKind(SymbolKind::Keyword).value_of());

    // No -std in the command means the entries were indexed under the
    // driver's default dialect (C++17 today); the fallback matches it.
    auto default_tokens =
        feature::index_semantic_tokens(content,
                                       feature::index_lang_options("main.cpp", false),
                                       {},
                                       entries,
                                       resolve_synthetic);
    ZASSERT(default_tokens.size() == std::size_t(2));
    ZASSERT(default_tokens[1].kind.value_of() == SymbolKind(SymbolKind::Variable).value_of());
}

ZEST_CASE(LinksFromEdges) {
    llvm::StringRef content = R"cpp(#include "first.h"
#include "skipped.h"
#include <second>
)cpp";
    std::vector<feature::IndexIncludeEdge> edges = {
        {.line = 1, .target = "/tmp/first.h"       },
        {.line = 3, .target = "/usr/include/second"},
    };

    auto links = feature::index_document_links(content,
                                               feature::index_lang_options("main.cpp", false),
                                               edges);
    ZASSERT(links.size() == std::size_t(2));
    ZASSERT(content.substr(links[0].range.begin, links[0].range.length()) == "\"first.h\"");
    ZASSERT(links[0].target == "/tmp/first.h");
    ZASSERT(content.substr(links[1].range.begin, links[1].range.length()) == "<second>");
    ZASSERT(links[1].target == "/usr/include/second");
}

ZEST_CASE(HoverDefinitionShape) {
    // The stored extent is the whole definition; the card shows what the
    // AST card prints, except that a type's members are left out: the AST
    // card's member summary needs the AST.
    auto card = [](SymbolKind::Kind kind,
                   llvm::StringRef text,
                   index::SymbolFlags flags = index::SymbolFlags::None) {
        index::SymbolRef info{.name = "name", .kind = kind, .flags = flags};
        return feature::index_hover(info, {}, text, {}).definition;
    };
    ZASSERT(card(SymbolKind::Function, "int twice(int x) {\n    return x * 2;\n}") ==
            "int twice(int x)");
    ZASSERT(card(SymbolKind::Function, "int open() { // }\n    return 0;\n}") == "int open()");
    ZASSERT(card(SymbolKind::Method, "Holder() = default") == "Holder() = default");
    ZASSERT(card(SymbolKind::Struct, "struct Point {\n    int x;\n}") == "struct Point { ... }");
    ZASSERT(card(SymbolKind::Namespace, "namespace app {\nint value;\n}") == "namespace app {}");
    ZASSERT(card(SymbolKind::Macro, "LIMIT 10") == "#define LIMIT 10");
    ZASSERT(card(SymbolKind::Variable, "int values[] = {1, 2}") == "int values[] = {1, 2}");
    ZASSERT(card(SymbolKind::Function, "int entry() {}", index::SymbolFlags::Exported) ==
            "export int entry()");
}

};  // ZEST_SUITE(index_projection)

}  // namespace

}  // namespace clice::testing
