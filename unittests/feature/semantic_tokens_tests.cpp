module;

#include "modules/prelude.h"
/// Primary semantic-tokens coverage lives in the cases
/// (tests/semantic_tokens/), which pin both the inspect and the
/// server path. This file drives the feature layer directly: preamble
/// state under a real PCH split, module imports (which need dependency
/// modules), and the encoder math the snapshots decode away.

module clice;

import :feature.feature;
import :semantic.token_class;
import :unittests.utils;
import :unittests.utils.tester;

namespace clice::testing {

namespace {

namespace lsp = kota::ipc::lsp;
namespace protocol = kota::ipc::protocol;

struct DecodedToken {
    /// Absolute positions are accumulated in 64 bits so that a broken delta
    /// stream (e.g. an underflowed deltaStart) stays visible instead of
    /// wrapping back to a plausible value.
    std::uint64_t line = 0;
    std::uint64_t start = 0;
    std::uint32_t length = 0;
    std::uint32_t type = 0;
    std::uint32_t modifiers = 0;
};

auto decode_relative_tokens(const protocol::SemanticTokens& tokens) -> std::vector<DecodedToken> {
    assert(tokens.data.size() % 5 == 0 && "invalid semantic token payload");

    std::vector<DecodedToken> result;
    result.reserve(tokens.data.size() / 5);

    std::uint64_t line = 0;
    std::uint64_t character = 0;
    for(std::size_t i = 0; i < tokens.data.size(); i += 5) {
        auto delta_line = tokens.data[i + 0];
        auto delta_char = tokens.data[i + 1];
        auto length = tokens.data[i + 2];
        auto type = tokens.data[i + 3];
        auto modifiers = tokens.data[i + 4];

        line += delta_line;
        character = delta_line == 0 ? character + delta_char : delta_char;
        result.push_back({
            .line = line,
            .start = character,
            .length = length,
            .type = type,
            .modifiers = modifiers,
        });
    }

    return result;
}

ZEST_SUITE(semantic_tokens, Tester) {

std::vector<feature::SemanticToken> tokens;
feature::SemanticTokenLegend legend = feature::semantic_token_legend({});

void run(llvm::StringRef code) {
    add_main("main.cpp", code);
    ZASSERT(compile_with_pch());
    tokens = feature::semantic_tokens(*unit, {});
}

protocol::SemanticTokens encode(PositionEncoding encoding) {
    return feature::semantic_tokens_to_protocol(tokens,
                                                unit->focus_file().positions(),
                                                encoding,
                                                legend);
}

void EXPECT_TOKEN(llvm::StringRef name,
                  SemanticTokenKind expected_kind,
                  std::uint32_t expected_modifiers = 0) {
    auto expected = range(name);
    auto it = llvm::find_if(tokens, [&](const feature::SemanticToken& token) {
        return token.range == expected;
    });
    ZASSERT(it != tokens.end());
    ZASSERT(it->kind == expected_kind);
    ZASSERT(it->modifiers == expected_modifiers);
}

ZEST_CASE(PreambleDefineUnderPch) {
    // The leading `#define` sits in the preamble, so under the PCH split no
    // MacroDefine record exists in the main compile; the macro name must
    // still classify as a definition through the lexical directive fallback.
    run(R"cpp(
§(d1)⟦#define⟧ §(m0)⟦FOO⟧
§(k0)⟦int⟧ main() { §(k1)⟦return⟧ 0; }
§(c0)⟦// comment⟧
)cpp");

    EXPECT_TOKEN("d1", SemanticTokenKind::Directive);
    EXPECT_TOKEN("m0",
                 SemanticTokenKind::Macro,
                 mask(SemanticTokenModifier::Definition) | mask(SemanticTokenModifier::Empty));
    EXPECT_TOKEN("k0", SemanticTokenKind::Primitive);
    EXPECT_TOKEN("k1", SemanticTokenKind::Keyword, mask(SemanticTokenModifier::ControlFlow));
    EXPECT_TOKEN("c0", SemanticTokenKind::Comment);
}

ZEST_CASE(ModuleDeclarationUnderPch) {
    // The whole module preamble (global fragment included) sits under the
    // PCH split; every written module token must still classify — with a
    // leading comment block ahead of the global fragment, like a license
    // header.
    run(R"cpp(// leading comment block
// above the global module fragment

§(g0)⟦module⟧;

export §(k0)⟦module⟧ §(n0)⟦demo⟧.§(n1)⟦core⟧;

export int exported_value = 1;

§(p0)⟦module⟧ :private;

int private_value = 2;
)cpp");

    auto definition = mask(SemanticTokenModifier::Definition);
    EXPECT_TOKEN("g0", SemanticTokenKind::Keyword);
    EXPECT_TOKEN("k0", SemanticTokenKind::Keyword);
    EXPECT_TOKEN("n0", SemanticTokenKind::Module, definition);
    EXPECT_TOKEN("n1", SemanticTokenKind::Module, definition);
    EXPECT_TOKEN("p0", SemanticTokenKind::Keyword);
}

ZEST_CASE(UsingFromDependentBase) {
    // A dependent name that a template base brings in with a
    // using-declaration names the member itself.
    run(R"cpp(
struct Base { void f(); };
template <class T> struct B : Base { using Base::f; };
template <class T> struct D : B<T> {
    void g() {
        this->§(member)⟦f⟧();
        B<T>::§(qualified)⟦f⟧();
    }
};
)cpp");

    auto member = mask(SemanticTokenModifier::Member) | mask(SemanticTokenModifier::Dependent);
    EXPECT_TOKEN("member", SemanticTokenKind::Method, member);
    EXPECT_TOKEN("qualified", SemanticTokenKind::Method, member);
}

ZEST_CASE(UTF16LengthDiffersFromUTF8) {
    run(R"cpp(
int main() {
§(lit)⟦u8"你"⟧;
}
)cpp");

    // The `u8` prefix is a piece of its own; the quoted text follows it.
    auto string_type = *legend.type_of[static_cast<std::size_t>(SemanticTokenKind::String)];
    auto lit = range("lit");
    auto body = encode(PositionEncoding::UTF8);
    auto wide = encode(PositionEncoding::UTF16);
    auto length = [&](const protocol::SemanticTokens& encoded) {
        auto decoded = decode_relative_tokens(encoded);
        auto it = llvm::find_if(decoded, [&](const DecodedToken& token) {
            return token.type == string_type && token.length > 2;
        });
        return it == decoded.end() ? 0u : it->length;
    };
    ZASSERT(lit.length() == 7U);
    ZASSERT(length(body) == 5U);
    ZASSERT(length(wide) == 3U);
}

/// A block comment over two lines, `/*<head>` and `cd*/`, splits into one
/// piece per line, each ending before its line's terminator.
void check_comment_split(llvm::StringRef head,
                         llvm::StringRef newline,
                         PositionEncoding encoding,
                         std::uint32_t head_length) {
    run(std::format(R"cpp(int main() {{
/*{}{}cd*/
}}
)cpp",
                    head,
                    newline));

    auto relative = decode_relative_tokens(encode(encoding));
    auto comment_type = *legend.type_of[static_cast<std::size_t>(SemanticTokenKind::Comment)];
    std::vector<DecodedToken> comments;
    for(const auto& token: relative) {
        if(token.type == comment_type) {
            comments.push_back(token);
        }
    }

    ZASSERT(comments.size() == 2);
    ZASSERT(comments[0].length == head_length);
    ZASSERT(comments[1].line == comments[0].line + 1);
    ZASSERT(comments[1].start == 0);
    ZASSERT(comments[1].length == 4);
}

ZEST_CASE(MultiLineCommentSplit) {
    check_comment_split("ab", "\n", PositionEncoding::UTF8, 4);
}

ZEST_CASE(CRLFCommentSplit) {
    check_comment_split("ab", "\r\n", PositionEncoding::UTF8, 4);
}

ZEST_CASE(UTF16CommentSplit) {
    check_comment_split("\xc3\xa9", "\r\n", PositionEncoding::UTF16, 3);
}

ZEST_CASE(ModuleImport) {
    add_files("main.cpp", R"(
#[mod.cppm]
export module foo;
export int x = 42;

#[main.cpp]
§(kw)⟦import⟧ §(mod)⟦foo⟧;
int y = x;
)");
    ZASSERT(compile_with_modules());
    tokens = feature::semantic_tokens(*unit, {});

    EXPECT_TOKEN("kw", SemanticTokenKind::Keyword);
    EXPECT_TOKEN("mod", SemanticTokenKind::Module);
}

ZEST_CASE(DottedModuleImport) {
    add_files("main.cpp", R"(
#[mod.cppm]
export module app.core;
export int x = 42;

#[main.cpp]
import §(m0)⟦app⟧.§(m1)⟦core⟧;
int y = x;
)");
    ZASSERT(compile_with_modules());
    tokens = feature::semantic_tokens(*unit, {});

    EXPECT_TOKEN("m0", SemanticTokenKind::Module);
    EXPECT_TOKEN("m1", SemanticTokenKind::Module);
}

ZEST_CASE(ImportChannelAudit) {
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

    // The preprocessor callback channel must record every import form the
    // AST records: a named import, an export-import and a partition import
    // (whose full name resolves through the owning module).
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

ZEST_CASE(ModulePartitionImport) {
    add_files("main.cppm", R"(
#[part.cppm]
export module foo:part;
export int x = 42;

#[main.cppm]
export module foo;
export §(kw)⟦import⟧ :§(part)⟦part⟧;
)");
    ZASSERT(compile_with_modules());
    tokens = feature::semantic_tokens(*unit, {});

    EXPECT_TOKEN("kw", SemanticTokenKind::Keyword);
    EXPECT_TOKEN("part", SemanticTokenKind::Module);
}

ZEST_CASE(ModuleImplementationUnit) {
    add_files("main.cpp", R"(
#[mod.cppm]
export module foo;
export int x = 42;

#[main.cpp]
§(kw)⟦module⟧ §(mod)⟦foo⟧;
int y = §(ref)⟦x⟧;
)");
    ZASSERT(compile_with_modules());
    tokens = feature::semantic_tokens(*unit, {});

    EXPECT_TOKEN("kw", SemanticTokenKind::Keyword);
    // An implementation unit declares the module its interface defines.
    EXPECT_TOKEN("mod", SemanticTokenKind::Module, mask(SemanticTokenModifier::Declaration));
    EXPECT_TOKEN("ref", SemanticTokenKind::Variable, mask(SemanticTokenModifier::Global));
}

ZEST_CASE(ModuleReexport) {
    add_files("main.cppm", R"(
#[mod.cppm]
export module foo;
export int x = 42;

#[main.cppm]
export module bar;
export §(kw)⟦import⟧ §(mod)⟦foo⟧;
)");
    ZASSERT(compile_with_modules());
    tokens = feature::semantic_tokens(*unit, {});

    EXPECT_TOKEN("kw", SemanticTokenKind::Keyword);
    EXPECT_TOKEN("mod", SemanticTokenKind::Module);
}

};  // ZEST_SUITE(semantic_tokens)

}  // namespace

}  // namespace clice::testing
