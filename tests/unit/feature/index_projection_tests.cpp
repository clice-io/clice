#include <algorithm>
#include <string>
#include <vector>

#include "test/test.h"
#include "test/tester.h"
#include "feature/feature.h"
#include "index/tu_index.h"

namespace clice::testing {

namespace {

TEST_SUITE(index_projection, Tester) {

/// The compiled unit's envelope and the main file's rows, extracted the
/// way the router's index slice does it in production.
index::TUIndex tu;
std::string envelope;
std::vector<index::Occurrence> occurrences;
std::vector<feature::IndexDeclRow> decls;

void extract_rows() {
    envelope = index::build_tu_index(*unit);
    tu = index::TUIndex::from_bytes(envelope);

    auto main_id = tu.path_count() - 1;
    auto& shard = tu.shard_of(main_id);

    occurrences.clear();
    shard.for_each_occurrence([&](const index::Occurrence& occurrence) {
        occurrences.push_back(occurrence);
        return true;
    });

    decls.clear();
    shard.for_each_relation([&](index::SymbolHash hash, const index::Relation& relation) {
        RelationKind kind(relation.kind);
        if(kind.isDeclOrDef()) {
            auto copy = relation;
            decls.push_back({.range = relation.range,
                             .extent = copy.definition_range(),
                             .symbol = hash,
                             .definition = kind.is_one_of(RelationKind::Definition)});
        }
        return true;
    });
}

std::optional<feature::IndexSymbolInfo> resolve(index::SymbolHash hash) {
    if(auto identity = tu.find_symbol(hash)) {
        return feature::IndexSymbolInfo{std::string(identity->name), identity->kind};
    }
    auto main_id = tu.path_count() - 1;
    std::string name;
    SymbolKind kind;
    if(tu.shard_of(main_id).find_symbol(hash, name, kind)) {
        return feature::IndexSymbolInfo{std::move(name), kind};
    }
    return std::nullopt;
}

auto resolver() {
    return [this](index::SymbolHash hash) {
        return resolve(hash);
    };
}

TEST_CASE(TokensMatchAst) {
    add_main("main.cpp", R"cpp(
// a line comment
#define VALUE 1

struct Point {
    int x;
    int y;
    int sum();
    Point operator+(Point other);
};

int Point::sum() {
    return x + y + VALUE;
}

int total(Point point, int base) {
    const char* label = "sum";
    char letter = 's';
    if(base > 0) {
        return point.sum() + base;
    }
    return 0;
}
)cpp");
    ASSERT_TRUE(compile());
    extract_rows();

    auto ast = feature::semantic_tokens(*unit);
    auto projected = feature::index_semantic_tokens(unit->interested_content(),
                                                    feature::index_lang_options("main.cpp", false),
                                                    occurrences,
                                                    decls,
                                                    resolver());

    // The index knows Declaration/Definition; every other AST modifier
    // (Readonly, Static, Virtual, ...) is a pinned degradation.
    auto pinned = SymbolModifiers::to_mask(SymbolModifiers::Declaration) |
                  SymbolModifiers::to_mask(SymbolModifiers::Definition);

    ASSERT_EQ(projected.size(), ast.size());
    for(std::size_t i = 0; i < ast.size(); i += 1) {
        ASSERT_EQ(projected[i].range.begin, ast[i].range.begin);
        ASSERT_EQ(projected[i].range.end, ast[i].range.end);
        ASSERT_EQ(projected[i].kind.value_of(), ast[i].kind.value_of());
        ASSERT_EQ(projected[i].modifiers, ast[i].modifiers & pinned);
    }
}

TEST_CASE(OutlineMatchesAst) {
    add_main("main.cpp", R"cpp(
#define LIMIT 10

namespace app {

struct Point {
    int x;
    int sum();
};

enum class Color {
    Red,
    Blue,
};

int scale(int value) {
    return value * 2;
}

}
)cpp");
    ASSERT_TRUE(compile());
    extract_rows();

    auto ast = feature::document_symbols(*unit);
    auto projected = feature::index_document_symbols(decls, resolver());

    auto compare = [](auto& self,
                      const std::vector<feature::DocumentSymbol>& lhs,
                      const std::vector<feature::DocumentSymbol>& rhs) -> void {
        ASSERT_EQ(lhs.size(), rhs.size());
        for(std::size_t i = 0; i < lhs.size(); i += 1) {
            ASSERT_EQ(lhs[i].name, rhs[i].name);
            ASSERT_EQ(lhs[i].kind.value_of(), rhs[i].kind.value_of());
            self(self, lhs[i].children, rhs[i].children);
        }
    };
    compare(compare, projected, ast);
}

TEST_CASE(FoldsSubsetOfAst) {
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
    ASSERT_TRUE(compile());
    extract_rows();

    auto ast = feature::folding_ranges(*unit);
    auto projected = feature::index_folding_ranges(unit->interested_content(),
                                                   feature::index_lang_options("main.cpp", false),
                                                   decls,
                                                   resolver());

    ASSERT_TRUE(!projected.empty());
    for(auto& fold: projected) {
        bool known = std::ranges::any_of(ast, [&](const feature::FoldingRange& twin) {
            return twin.range == fold.range;
        });
        ASSERT_TRUE(known);
    }

    // The constructor's fold anchors at its body, not the member
    // initializer's braces.
    auto body = unit->interested_content().find("{\n        value += 1;");
    bool anchored = std::ranges::any_of(projected, [&](const feature::FoldingRange& fold) {
        return fold.range.begin == body;
    });
    ASSERT_TRUE(anchored);
}

TEST_CASE(ConditionalBracesSuppressFold) {
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
    std::vector<feature::IndexDeclRow> rows = {
        {.range = {5, 6},                     .extent = {0, f_end}, .symbol = 1, .definition = true},
        {.range = {g_begin + 5, g_begin + 6},
         .extent = {g_begin, g_end},
         .symbol = 2,
         .definition = true                                                                        },
    };
    auto resolve_synthetic = [](index::SymbolHash hash) -> std::optional<feature::IndexSymbolInfo> {
        return feature::IndexSymbolInfo{hash == 1 ? "f" : "g", SymbolKind::Function};
    };

    auto folds = feature::index_folding_ranges(content,
                                               feature::index_lang_options("main.cpp", false),
                                               rows,
                                               resolve_synthetic);
    ASSERT_EQ(folds.size(), std::size_t(1));
    ASSERT_EQ(folds[0].range.begin, static_cast<std::uint32_t>(content.find("{", g_begin)));
    ASSERT_EQ(folds[0].range.end, g_end);
}

TEST_CASE(CollapsedRowsBecomeSiblings) {
    std::vector<feature::IndexDeclRow> rows = {
        {.range = {8, 9},   .extent = {0, 100}, .symbol = 1, .definition = true},
        {.range = {20, 25}, .extent = {20, 50}, .symbol = 2, .definition = true},
        {.range = {20, 25}, .extent = {20, 50}, .symbol = 3, .definition = true},
    };
    auto resolve_synthetic = [](index::SymbolHash hash) -> std::optional<feature::IndexSymbolInfo> {
        switch(hash) {
            case 1: return feature::IndexSymbolInfo{"outer", SymbolKind::Struct};
            case 2: return feature::IndexSymbolInfo{"first", SymbolKind::Field};
            case 3: return feature::IndexSymbolInfo{"second", SymbolKind::Field};
            default: return std::nullopt;
        }
    };

    auto symbols = feature::index_document_symbols(rows, resolve_synthetic);
    ASSERT_EQ(symbols.size(), std::size_t(1));
    ASSERT_EQ(symbols[0].name, "outer");
    ASSERT_EQ(symbols[0].children.size(), std::size_t(2));
    ASSERT_EQ(symbols[0].children[0].children.size(), std::size_t(0));
    ASSERT_EQ(symbols[0].children[1].children.size(), std::size_t(0));
}

TEST_CASE(MergedKindsConflict) {
    llvm::StringRef content = "value;\n";
    std::vector<index::Occurrence> merged = {
        {.range = {0, 5}, .target = 1},
        {.range = {0, 5}, .target = 2},
    };
    auto resolve_synthetic = [](index::SymbolHash hash) -> std::optional<feature::IndexSymbolInfo> {
        if(hash == 1) {
            return feature::IndexSymbolInfo{"value", SymbolKind::Variable};
        }
        return feature::IndexSymbolInfo{"value", SymbolKind::Function};
    };

    auto tokens = feature::index_semantic_tokens(content,
                                                 feature::index_lang_options("main.cpp", false),
                                                 merged,
                                                 {},
                                                 resolve_synthetic);
    ASSERT_EQ(tokens.size(), std::size_t(1));
    ASSERT_EQ(tokens[0].kind.value_of(), SymbolKind(SymbolKind::Conflict).value_of());
}

TEST_CASE(ModuleNameComponents) {
    // The index stores one occurrence spanning the whole written module
    // name; every identifier component must classify, as on the AST path
    // (separators and the contextual `module`/`import` stay unpainted).
    llvm::StringRef content = "export module demo.core;\nimport foo:part;\n";
    std::vector<index::Occurrence> merged = {
        {.range = {14, 23}, .target = 1},
        {.range = {32, 40}, .target = 1},
    };
    auto resolve_synthetic = [](index::SymbolHash) -> std::optional<feature::IndexSymbolInfo> {
        return feature::IndexSymbolInfo{"demo.core", SymbolKind::Module};
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
    ASSERT_EQ(modules, expected);
}

TEST_CASE(CDialectKeywords) {
    // `class` is a valid C identifier: rows built by C parses must lex
    // under the C keyword table, or the C++ table would take `class` as
    // a keyword and shadow the row's kind.
    llvm::StringRef content = "int class;\n";
    std::vector<feature::IndexDeclRow> rows = {
        {.range = {4, 9}, .extent = {0, 10}, .symbol = 1, .definition = true},
    };
    auto resolve_synthetic = [](index::SymbolHash) -> std::optional<feature::IndexSymbolInfo> {
        return feature::IndexSymbolInfo{"class", SymbolKind::Variable};
    };

    auto c_tokens = feature::index_semantic_tokens(content,
                                                   feature::index_lang_options("header.h", true),
                                                   {},
                                                   rows,
                                                   resolve_synthetic);
    ASSERT_EQ(c_tokens.size(), std::size_t(2));
    ASSERT_EQ(c_tokens[1].kind.value_of(), SymbolKind(SymbolKind::Variable).value_of());

    auto cpp_tokens = feature::index_semantic_tokens(content,
                                                     feature::index_lang_options("header.h", false),
                                                     {},
                                                     rows,
                                                     resolve_synthetic);
    ASSERT_EQ(cpp_tokens.size(), std::size_t(2));
    ASSERT_EQ(cpp_tokens[1].kind.value_of(), SymbolKind(SymbolKind::Keyword).value_of());
}

TEST_CASE(StandardFromCommand) {
    // `concept` is a plain identifier in C++17: rows built under an older
    // -std must lex with that standard's keyword table, or a newer
    // table would take `concept` as a keyword and shadow the row.
    llvm::StringRef content = "int concept;\n";
    std::vector<feature::IndexDeclRow> rows = {
        {.range = {4, 11}, .extent = {0, 12}, .symbol = 1, .definition = true},
    };
    auto resolve_synthetic = [](index::SymbolHash) -> std::optional<feature::IndexSymbolInfo> {
        return feature::IndexSymbolInfo{"concept", SymbolKind::Variable};
    };

    auto cxx17_tokens =
        feature::index_semantic_tokens(content,
                                       feature::index_lang_options("main.cpp", false, "c++17"),
                                       {},
                                       rows,
                                       resolve_synthetic);
    ASSERT_EQ(cxx17_tokens.size(), std::size_t(2));
    ASSERT_EQ(cxx17_tokens[1].kind.value_of(), SymbolKind(SymbolKind::Variable).value_of());

    auto cxx20_tokens =
        feature::index_semantic_tokens(content,
                                       feature::index_lang_options("main.cpp", false, "c++20"),
                                       {},
                                       rows,
                                       resolve_synthetic);
    ASSERT_EQ(cxx20_tokens.size(), std::size_t(2));
    ASSERT_EQ(cxx20_tokens[1].kind.value_of(), SymbolKind(SymbolKind::Keyword).value_of());

    // No -std in the command means the rows were indexed under the
    // driver's default dialect (C++17 today); the fallback matches it.
    auto default_tokens =
        feature::index_semantic_tokens(content,
                                       feature::index_lang_options("main.cpp", false),
                                       {},
                                       rows,
                                       resolve_synthetic);
    ASSERT_EQ(default_tokens.size(), std::size_t(2));
    ASSERT_EQ(default_tokens[1].kind.value_of(), SymbolKind(SymbolKind::Variable).value_of());
}

TEST_CASE(LinksFromEdges) {
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
    ASSERT_EQ(links.size(), std::size_t(2));
    ASSERT_EQ(content.substr(links[0].range.begin, links[0].range.length()), "\"first.h\"");
    ASSERT_EQ(links[0].target, "/tmp/first.h");
    ASSERT_EQ(content.substr(links[1].range.begin, links[1].range.length()), "<second>");
    ASSERT_EQ(links[1].target, "/usr/include/second");
}

TEST_CASE(CommentBlockExtraction) {
    llvm::StringRef content = R"cpp(int unrelated;

/// Adds two numbers.
/// Returns their sum.
int add(int a, int b);

// stale note

int gap();

/* Scales the
   given input. */
int scale(int value);

int base = 1; /* setup */
int next();

/*
Frees the buffer.
Then clears it.
*/
int release();

int done(); /* trailing block
still trailing
*/
int after();
)cpp";

    auto add_offset = static_cast<std::uint32_t>(content.find("int add"));
    ASSERT_EQ(feature::preceding_comment(content, add_offset),
              "Adds two numbers.\nReturns their sum.");

    // A blank line between the comment and the declaration breaks the
    // attachment.
    auto gap_offset = static_cast<std::uint32_t>(content.find("int gap"));
    ASSERT_EQ(feature::preceding_comment(content, gap_offset), "");

    // A block comment whose closing line only ends with the marker still
    // attaches whole.
    auto scale_offset = static_cast<std::uint32_t>(content.find("int scale"));
    ASSERT_EQ(feature::preceding_comment(content, scale_offset), "Scales the\ngiven input.");

    // A code line trailing a self-contained block comment is code, not
    // documentation.
    auto next_offset = static_cast<std::uint32_t>(content.find("int next"));
    ASSERT_EQ(feature::preceding_comment(content, next_offset), "");

    // Interior lines of a block comment need no marker of their own.
    auto release_offset = static_cast<std::uint32_t>(content.find("int release"));
    ASSERT_EQ(feature::preceding_comment(content, release_offset),
              "Frees the buffer.\nThen clears it.");

    // A block comment opened behind code trails that code, even when it
    // closes directly above the declaration.
    auto after_offset = static_cast<std::uint32_t>(content.find("int after"));
    ASSERT_EQ(feature::preceding_comment(content, after_offset), "");
}

};  // TEST_SUITE(index_projection)

}  // namespace

}  // namespace clice::testing
