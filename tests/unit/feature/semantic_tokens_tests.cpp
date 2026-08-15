/// Primary semantic-tokens coverage lives in the snapshot corpus
/// (tests/snap/semantic_tokens/), which pins both the standalone and the
/// server path. This file keeps only what that corpus cannot express:
/// preamble state under a real PCH split, module imports (which need
/// dependency modules), and the encoder math the snapshots decode away.

#include <cassert>
#include <cstdint>
#include <optional>
#include <vector>

#include "test/test.h"
#include "test/tester.h"
#include "feature/feature.h"
#include "semantic/symbol.h"

#include "kota/meta/enum.h"

namespace clice::testing {

namespace {

namespace lsp = kota::ipc::lsp;
namespace protocol = kota::ipc::protocol;

struct DecodedToken {
    LocalSourceRange range;
    /// Absolute positions are accumulated in 64 bits so that a broken delta
    /// stream (e.g. an underflowed deltaStart) stays visible instead of
    /// wrapping back to a plausible value.
    std::uint64_t line = 0;
    std::uint64_t start = 0;
    std::uint32_t length = 0;
    std::uint32_t type = 0;
    std::uint32_t modifiers = 0;
};

auto compute_line_starts(llvm::StringRef content) -> std::vector<std::uint32_t> {
    std::vector<std::uint32_t> starts = {0};
    for(std::uint32_t i = 0; i < content.size(); ++i) {
        if(content[i] == '\n') {
            starts.push_back(i + 1);
        }
    }
    return starts;
}

auto decode_utf8_tokens(llvm::StringRef content, const protocol::SemanticTokens& tokens)
    -> std::vector<DecodedToken> {
    assert(tokens.data.size() % 5 == 0 && "invalid semantic token payload");

    auto starts = compute_line_starts(content);
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

        DecodedToken token{
            .line = line,
            .start = character,
            .length = length,
            .type = type,
            .modifiers = modifiers,
        };

        // Only map in-bounds positions back to a byte range; out-of-range
        // tokens keep an invalid range so lookups by annotation fail loudly.
        if(line < starts.size()) {
            auto begin = starts[line] + character;
            auto end = begin + length;
            if(end <= content.size()) {
                token.range = LocalSourceRange(static_cast<std::uint32_t>(begin),
                                               static_cast<std::uint32_t>(end));
            }
        }

        result.push_back(token);
    }

    return result;
}

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

TEST_SUITE(semantic_tokens, Tester) {

protocol::SemanticTokens tokens;
std::vector<DecodedToken> decoded;

void run_utf8(llvm::StringRef code) {
    add_main("main.cpp", code);
    ASSERT_TRUE(compile_with_pch());
    tokens = feature::semantic_tokens(*unit, feature::PositionEncoding::UTF8);
    decoded = decode_utf8_tokens(unit->interested_content(), tokens);
}

auto find_by_range(llvm::StringRef name) -> const DecodedToken* {
    auto expected = range(name);
    for(const auto& token: decoded) {
        if(token.range == expected) {
            return &token;
        }
    }
    return nullptr;
}

void EXPECT_TOKEN(llvm::StringRef name,
                  SymbolKind::Kind expected_kind,
                  std::uint32_t expected_modifiers = 0) {
    auto* token = find_by_range(name);
    ASSERT_TRUE(token != nullptr);
    ASSERT_EQ(token->type, static_cast<std::uint32_t>(expected_kind));
    ASSERT_EQ(token->modifiers, expected_modifiers);
}

TEST_CASE(PreambleDefineUnderPch) {
    // The leading `#define` sits in the preamble, so under the PCH split no
    // MacroDefine record exists in the main compile; the macro name must
    // still classify through the lexical directive fallback.
    run_utf8(R"cpp(
§(d1)⟦#define⟧ §(m0)⟦FOO⟧
§(k0)⟦int⟧ main() { §(k1)⟦return⟧ 0; }
§(c0)⟦// comment⟧
)cpp");

    EXPECT_TOKEN("d1", SymbolKind::Directive);
    EXPECT_TOKEN("m0", SymbolKind::Macro);
    EXPECT_TOKEN("k0", SymbolKind::Primitive);
    EXPECT_TOKEN("k1", SymbolKind::Keyword);
    EXPECT_TOKEN("c0", SymbolKind::Comment);
}

TEST_CASE(ModuleDeclarationUnderPch) {
    // The whole module preamble (global fragment included) sits under the
    // PCH split; every written module token must still classify — with a
    // leading comment block ahead of the global fragment, like a license
    // header.
    run_utf8(R"cpp(// leading comment block
// above the global module fragment

§(g0)⟦module⟧;

export §(k0)⟦module⟧ §(n0)⟦demo⟧.§(n1)⟦core⟧;

export int exported_value = 1;

§(p0)⟦module⟧ :private;

int private_value = 2;
)cpp");

    EXPECT_TOKEN("g0", SymbolKind::Keyword);
    EXPECT_TOKEN("k0", SymbolKind::Keyword);
    EXPECT_TOKEN("n0", SymbolKind::Module);
    EXPECT_TOKEN("n1", SymbolKind::Module);
    EXPECT_TOKEN("p0", SymbolKind::Keyword);
}

TEST_CASE(UTF16LengthDiffersFromUTF8) {
    add_main("main.cpp", R"cpp(
int main() {
§(lit)⟦u8"你"⟧;
}
)cpp");
    ASSERT_TRUE(compile_with_pch());

    auto utf8_tokens = feature::semantic_tokens(*unit, feature::PositionEncoding::UTF8);
    auto utf16_tokens = feature::semantic_tokens(*unit, feature::PositionEncoding::UTF16);

    auto utf8 = decode_utf8_tokens(unit->interested_content(), utf8_tokens);
    auto utf16 = decode_relative_tokens(utf16_tokens);

    auto string_type = static_cast<std::uint32_t>(SymbolKind::String);
    auto lit_range = range("lit");

    std::optional<DecodedToken> utf8_token;
    for(const auto& token: utf8) {
        if(token.range == lit_range && token.type == string_type) {
            utf8_token = token;
            break;
        }
    }
    ASSERT_TRUE(utf8_token.has_value());

    std::optional<DecodedToken> utf16_token;
    for(const auto& token: utf16) {
        if(token.line == utf8_token->line && token.start == utf8_token->start &&
           token.type == string_type) {
            utf16_token = token;
            break;
        }
    }
    ASSERT_TRUE(utf16_token.has_value());

    ASSERT_TRUE(utf8_token->length > utf16_token->length);
}

TEST_CASE(MultiLineCommentSplit) {
    add_main("main.cpp", R"cpp(
int main() {
/*ab
cd*/
}
)cpp");
    ASSERT_TRUE(compile_with_pch());

    auto utf8_tokens = feature::semantic_tokens(*unit, feature::PositionEncoding::UTF8);
    auto relative = decode_relative_tokens(utf8_tokens);

    auto comment_type = static_cast<std::uint32_t>(SymbolKind::Comment);
    std::vector<DecodedToken> comments;
    for(const auto& token: relative) {
        if(token.type == comment_type) {
            comments.push_back(token);
        }
    }

    ASSERT_EQ(comments.size(), 2);
    ASSERT_EQ(comments[0].length, 5);
    ASSERT_EQ(comments[1].line, comments[0].line + 1);
    ASSERT_EQ(comments[1].start, 0);
    ASSERT_EQ(comments[1].length, 4);
}

TEST_CASE(ModuleImport) {
    add_files("main.cpp", R"(
#[mod.cppm]
export module foo;
export int x = 42;

#[main.cpp]
§(kw)⟦import⟧ §(mod)⟦foo⟧;
int y = x;
)");
    ASSERT_TRUE(compile_with_modules());
    tokens = feature::semantic_tokens(*unit, feature::PositionEncoding::UTF8);
    decoded = decode_utf8_tokens(unit->interested_content(), tokens);

    EXPECT_TOKEN("kw", SymbolKind::Keyword);
    EXPECT_TOKEN("mod", SymbolKind::Module);
}

TEST_CASE(ImportChannelAudit) {
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
    ASSERT_TRUE(compile_with_modules());

    // The preprocessor callback channel must record every import form the
    // AST records: a named import, an export-import and a partition import
    // (whose full name resolves through the owning module).
    auto& imports = unit->directives()[unit->interested_file()].imports;
    ASSERT_EQ(imports.size(), 3U);
    ASSERT_EQ(imports[0].name, "a");
    ASSERT_EQ(imports[1].name, "b");
    ASSERT_EQ(imports[2].name, "foo:part");

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
    ASSERT_EQ(ast_imports, 3U);
}

TEST_CASE(ModulePartitionImport) {
    add_files("main.cppm", R"(
#[part.cppm]
export module foo:part;
export int x = 42;

#[main.cppm]
export module foo;
export §(kw)⟦import⟧ :§(part)⟦part⟧;
)");
    ASSERT_TRUE(compile_with_modules());
    tokens = feature::semantic_tokens(*unit, feature::PositionEncoding::UTF8);
    decoded = decode_utf8_tokens(unit->interested_content(), tokens);

    EXPECT_TOKEN("kw", SymbolKind::Keyword);
    EXPECT_TOKEN("part", SymbolKind::Module);
}

TEST_CASE(ModuleImplementationUnit) {
    add_files("main.cpp", R"(
#[mod.cppm]
export module foo;
export int x = 42;

#[main.cpp]
§(kw)⟦module⟧ §(mod)⟦foo⟧;
int y = §(ref)⟦x⟧;
)");
    ASSERT_TRUE(compile_with_modules());
    tokens = feature::semantic_tokens(*unit, feature::PositionEncoding::UTF8);
    decoded = decode_utf8_tokens(unit->interested_content(), tokens);

    EXPECT_TOKEN("kw", SymbolKind::Keyword);
    EXPECT_TOKEN("mod", SymbolKind::Module);
    EXPECT_TOKEN("ref", SymbolKind::Variable);
}

TEST_CASE(ModuleReexport) {
    add_files("main.cppm", R"(
#[mod.cppm]
export module foo;
export int x = 42;

#[main.cppm]
export module bar;
export §(kw)⟦import⟧ §(mod)⟦foo⟧;
)");
    ASSERT_TRUE(compile_with_modules());
    tokens = feature::semantic_tokens(*unit, feature::PositionEncoding::UTF8);
    decoded = decode_utf8_tokens(unit->interested_content(), tokens);

    EXPECT_TOKEN("kw", SymbolKind::Keyword);
    EXPECT_TOKEN("mod", SymbolKind::Module);
}

};  // TEST_SUITE(semantic_tokens)

}  // namespace

}  // namespace clice::testing
