module clice;

import :feature.document_symbols;
import :semantic.symbol;
import :syntax.source_text;
import :unittests.utils.index_entries_tester;

namespace clice::testing {

namespace {

ZEST_SUITE(document_symbols, IndexEntriesTester) {

/// The outline the index computes from the unit's own entries.
auto index_outline() -> std::vector<feature::DocumentSymbol> {
    SourceText text(unit->focus_file().content(), unit->lang_options());
    return feature::document_symbols(text, entries, resolver(), {});
}

ZEST_CASE(ModuleOutlineMatchesAst) {
    add_focus("main.cpp", R"cpp(
export module demo.core;

export int entry();
)cpp");
    ZASSERT(compile());
    extract_entries();

    auto ast = feature::document_symbols(*unit, {});
    auto from_index = index_outline();
    ZASSERT(ast.size() == std::size_t(2));
    ZASSERT(ast[0].name == "demo.core");
    ZASSERT(ast[0].kind.value_of() == SymbolKind(SymbolKind::Module).value_of());
    ZASSERT(from_index.size() == ast.size());
    for(std::size_t i = 0; i < ast.size(); i += 1) {
        ZASSERT(from_index[i].name == ast[i].name);
        ZASSERT(from_index[i].kind.value_of() == ast[i].kind.value_of());
        ZASSERT(from_index[i].selection_range == ast[i].selection_range);
    }
}

ZEST_CASE(OutlineMatchesAst) {
    add_focus("main.cpp", R"cpp(
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
    auto from_index = index_outline();

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
    compare(compare, from_index, ast);
}

};  // ZEST_SUITE(document_symbols)

}  // namespace

}  // namespace clice::testing
