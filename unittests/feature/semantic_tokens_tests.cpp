module clice;

import :feature.semantic_tokens.stream;
import :feature.semantic_tokens;
import :syntax.lexer;
import :syntax.source_text;
import :unittests.utils.index_entries_tester;
import :unittests.utils;

namespace clice::testing {

namespace {

ZEST_SUITE(semantic_tokens, IndexEntriesTester) {

/// The preprocessor records every import form the AST holds, a named import, an
/// export-import and a partition import named through its module; no answer
/// lists them.
ZEST_CASE(ImportsMatchAST) {
    add_files("main.cppm", R"(
#[a.cppm]
export module a;
export int va = 1;

#[b.cppm]
export module b;
export int vb = 1;

#[part.cppm]
export module foo:part;
export int vp = 1;

#[main.cppm]
export module foo;
import a;
export import b;
import :part;
)");
    ZASSERT(compile_with_modules());

    auto& imports = unit->directives()[unit->focus_file().file()].imports;
    ZASSERT(imports.size() == 3U);
    ZASSERT(imports[0].name == "a");
    ZASSERT(imports[1].name == "b");
    ZASSERT(imports[2].name == "foo:part");

    std::size_t ast_imports = 0;
    auto count = [&](const clang::Decl* decl, auto& self) -> void {
        if(llvm::isa<clang::ImportDecl>(decl)) {
            ast_imports += 1;
        }
        if(auto* context = llvm::dyn_cast<clang::DeclContext>(decl)) {
            for(auto* child: context->decls()) {
                self(child, self);
            }
        }
    };
    for(auto* decl: unit->context().getTranslationUnitDecl()->decls()) {
        count(decl, count);
    }
    ZASSERT(ast_imports == 3U);
}

/// The tokens the index computes for the compiled unit equal, modifiers included,
/// the AST's restricted to what the index records, a stream no answer shows;
/// both under the Tester's C++20.
void expect_tokens_match_ast() {
    auto& source = unit->focus_file().text();
    auto ast =
        feature::semantic_token_stream(source, feature::semantic_token_facts(*unit, {}, true), {});
    SourceText served(source.text(), dialect_of("main.cpp", std::nullopt, "c++20"));
    auto from_index = feature::semantic_tokens(served, entries, resolver(), {});

    ZASSERT(from_index.size() == ast.size());
    for(std::size_t i = 0; i < ast.size(); i += 1) {
        ZASSERT(from_index[i].range.begin == ast[i].range.begin);
        ZASSERT(from_index[i].range.end == ast[i].range.end);
        ZASSERT(from_index[i].kind == ast[i].kind);
        ZASSERT(from_index[i].modifiers == ast[i].modifiers);
    }
}

ZEST_CASE(TokensMatchAST) {
    add_focus("main.cpp", R"cpp(
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

ZEST_CASE(OverrideTokensMatchAST) {
    // `override` and `final` lead to the methods they override without
    // spelling them; a class's `final` leads nowhere.
    add_focus("main.cpp", R"cpp(
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

ZEST_CASE(ModuleTokensMatchAST) {
    add_focus("main.cpp", R"cpp(
export module demo.core;

export int exported_value = 1;
)cpp");
    ZASSERT(compile());
    extract_entries();
    expect_tokens_match_ast();
}

ZEST_CASE(ModuleKeywordsMatchAST) {
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

};  // ZEST_SUITE(semantic_tokens)

}  // namespace

}  // namespace clice::testing
