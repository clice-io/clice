module;

#include "modules/prelude.h"

module clice;

import :feature.folding_ranges;
import :feature.index_entries;
import :index.symbol;
import :semantic.symbol;
import :syntax.lexer;
import :syntax.source_text;
import :unittests.utils.index_entries_tester;

namespace clice::testing {

namespace {

ZEST_SUITE(folding_range, IndexEntriesTester) {

ZEST_CASE(FoldsSubsetOfAst) {
    add_focus("main.cpp", R"cpp(
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
    SourceText text(unit->focus_file().content(), dialect_of("main.cpp", false));
    auto from_index = feature::folding_ranges(text, entries.decls, resolver());

    ZASSERT(!from_index.empty());
    for(auto& fold: from_index) {
        bool known = std::ranges::any_of(ast, [&](const feature::FoldingRange& twin) {
            return twin.range == fold.range;
        });
        ZASSERT(known);
    }

    // The constructor's fold anchors at its body, not the member
    // initializer's braces.
    auto body = unit->focus_file().content().find("{\n        value += 1;");
    bool anchored = std::ranges::any_of(from_index, [&](const feature::FoldingRange& fold) {
        return fold.range.begin == body;
    });
    ZASSERT(anchored);
}

ZEST_CASE(FoldsMatchAstShape) {
    // A brace below its declaration's head folds from the head, a block
    // folds as its declaration's kind, and braces several decls share (the
    // struct's and the alias's) fold once — as the AST folds them.
    add_focus("main.cpp", R"cpp(
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
    SourceText text(unit->focus_file().content(), dialect_of("main.cpp", false));
    auto from_index = feature::folding_ranges(text, entries.decls, resolver());

    ZASSERT(from_index.size() == std::size_t(3));
    for(auto& fold: from_index) {
        auto twin = std::ranges::find_if(ast, [&](const feature::FoldingRange& candidate) {
            return candidate.range == fold.range;
        });
        ZASSERT(twin != ast.end());
        ZASSERT(fold.kind == twin->kind);
        ZASSERT(fold.lines == twin->lines);
        ZASSERT(fold.closer == twin->closer);
    }
    ZASSERT(from_index[0].lines);
    ZASSERT(from_index[1].lines);
}

ZEST_CASE(InitializerFoldsAtBrace) {
    add_focus("main.cpp", R"cpp(
int values[] =
{
    1,
    2,
};
)cpp");
    ZASSERT(compile());
    extract_entries();

    SourceText text(unit->focus_file().content(), dialect_of("main.cpp", false));
    auto from_index = feature::folding_ranges(text, entries.decls, resolver());
    ZASSERT(from_index.size() == std::size_t(1));
    ZASSERT(!from_index[0].lines.has_value());
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
    auto resolve_synthetic = [](index::SymbolHash hash) -> std::optional<index::SymbolInfo> {
        return index::SymbolInfo{.name = hash == 1 ? "f" : "g", .kind = SymbolKind::Function};
    };

    SourceText text(content, dialect_of("main.cpp", false));
    auto folds = feature::folding_ranges(text, entries, resolve_synthetic);
    ZASSERT(folds.size() == std::size_t(1));
    ZASSERT(folds[0].range.begin == static_cast<std::uint32_t>(content.find("{", g_begin)));
    ZASSERT(folds[0].range.end == g_end);
}

};  // ZEST_SUITE(folding_range)

}  // namespace

}  // namespace clice::testing
