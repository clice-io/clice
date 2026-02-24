#include <cassert>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <vector>

#include "test/test.h"
#include "test/tester.h"
#include "feature/feature.h"
#include "semantic/symbol_kind.h"

namespace clice::testing {

namespace {

namespace protocol = eventide::language::protocol;

struct DecodedToken {
    LocalSourceRange range;
    std::uint32_t line = 0;
    std::uint32_t start = 0;
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

    std::uint32_t line = 0;
    std::uint32_t character = 0;
    for(std::size_t i = 0; i < tokens.data.size(); i += 5) {
        auto delta_line = tokens.data[i + 0];
        auto delta_char = tokens.data[i + 1];
        auto length = tokens.data[i + 2];
        auto type = tokens.data[i + 3];
        auto modifiers = tokens.data[i + 4];

        line += delta_line;
        character = delta_line == 0 ? character + delta_char : delta_char;
        assert(line < starts.size() && "line index out of range");

        auto begin = starts[line] + character;
        auto end = begin + length;
        result.push_back({
            .range = LocalSourceRange(begin, end),
            .line = line,
            .start = character,
            .length = length,
            .type = type,
            .modifiers = modifiers,
        });
    }

    return result;
}

auto decode_relative_tokens(const protocol::SemanticTokens& tokens) -> std::vector<DecodedToken> {
    assert(tokens.data.size() % 5 == 0 && "invalid semantic token payload");

    std::vector<DecodedToken> result;
    result.reserve(tokens.data.size() / 5);

    std::uint32_t line = 0;
    std::uint32_t character = 0;
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

TEST_SUITE(SemanticTokens) {

Tester tester;
protocol::SemanticTokens tokens;
std::vector<DecodedToken> decoded;

auto modifier_mask(std::initializer_list<SymbolModifiers::Kind> kinds) -> std::uint32_t {
    std::uint32_t mask = 0;
    for(auto kind: kinds) {
        mask |= static_cast<std::uint32_t>(kind);
    }
    return mask;
}

void run_utf8(llvm::StringRef code) {
    tester.clear();
    tester.add_main("main.cpp", code);
    ASSERT_TRUE(tester.compile_with_pch());
    tokens = feature::semantic_tokens(*tester.unit, feature::PositionEncoding::UTF8);
    decoded = decode_utf8_tokens(tester.unit->interested_content(), tokens);
}

auto find_by_range(llvm::StringRef name) -> const DecodedToken* {
    auto range = tester.range(name);
    for(const auto& token: decoded) {
        if(token.range == range) {
            return &token;
        }
    }
    return nullptr;
}

void expect_token(llvm::StringRef name,
                  SymbolKind::Kind expected_kind,
                  std::uint32_t expected_modifiers = 0) {
    auto* token = find_by_range(name);
    ASSERT_TRUE(token != nullptr);
    ASSERT_EQ(token->type, static_cast<std::uint32_t>(expected_kind));
    ASSERT_EQ(token->modifiers, expected_modifiers);
}

TEST_CASE(BasicLexicalKinds) {
    run_utf8(R"cpp(
@d0[#include] @h0[<stddef.h>]
@d1[#define] @m0[FOO]
@k0[int] main() { @k1[return] 0; }
@c0[// comment]
)cpp");

    expect_token("d0", SymbolKind::Directive);
    expect_token("h0", SymbolKind::Header);
    expect_token("d1", SymbolKind::Directive);
    expect_token("m0", SymbolKind::Macro);
    expect_token("k0", SymbolKind::Keyword);
    expect_token("k1", SymbolKind::Keyword);
    expect_token("c0", SymbolKind::Comment);
}

TEST_CASE(LegacyIncludeForms) {
    run_utf8(R"cpp(
@i0[#include] @h0[<stddef.h>]
@i1[#include] @h1["stddef.h"]
@i2[#] @i3[include] @h2["stddef.h"]
)cpp");

    expect_token("i0", SymbolKind::Directive);
    expect_token("h0", SymbolKind::Header);
    expect_token("i1", SymbolKind::Directive);
    expect_token("h1", SymbolKind::Header);
    expect_token("i2", SymbolKind::Directive);
    expect_token("i3", SymbolKind::Directive);
    expect_token("h2", SymbolKind::Header);
}

TEST_CASE(LegacyComment) {
    run_utf8(R"cpp(
@line[/// line comment]
int x = 1;
)cpp");

    expect_token("line", SymbolKind::Comment);
}

TEST_CASE(LegacyKeyword) {
    run_utf8(R"cpp(
@k0[int] main() {
    @k1[return] 0;
}
)cpp");

    expect_token("k0", SymbolKind::Keyword);
    expect_token("k1", SymbolKind::Keyword);
}

TEST_CASE(LegacyMacro) {
    run_utf8(R"cpp(
@directive[#define] @macro[FOO]
)cpp");

    expect_token("directive", SymbolKind::Directive);
    expect_token("macro", SymbolKind::Macro);
}

TEST_CASE(LegacyFinalAndOverride) {
    run_utf8(R"cpp(
struct A @final[final] {};

struct B {
    virtual void foo();
};

struct C : B {
    void foo() @override[override];
};

struct D : C {
    void foo() @final2[final];
};
)cpp");

    expect_token("final", SymbolKind::Keyword);
    expect_token("override", SymbolKind::Keyword);
    expect_token("final2", SymbolKind::Keyword);
}

TEST_CASE(DeclarationAndTemplateModifiers) {
    run_utf8(R"cpp(
extern int @x1[x];
int @x2[x] = 0;

template <typename T>
extern int @y1[y];

template <typename T>
int @y2[y] = 0;

int main() {
    @x3[x] = 1;
}
)cpp");

    auto declaration = modifier_mask({SymbolModifiers::Declaration});
    auto definition = modifier_mask({SymbolModifiers::Definition});
    auto templated = modifier_mask({SymbolModifiers::Templated});

    expect_token("x1", SymbolKind::Variable, declaration);
    expect_token("x2", SymbolKind::Variable, definition);
    expect_token("y1", SymbolKind::Variable, declaration | templated);
    expect_token("y2", SymbolKind::Variable, definition | templated);
    expect_token("x3", SymbolKind::Variable, 0);
}

TEST_CASE(LegacyVarDeclTemplates) {
    run_utf8(R"cpp(
extern int @x1[x];

int @x2[x] = 1;

template <typename T, typename U>
extern int @y1[y];

template <typename T, typename U>
int @y2[y] = 2;

template<typename T>
extern int @y3[y]<T, int>;

template<typename T>
int @y4[y]<T, int> = 4;

template<>
int @y5[y]<int, int> = 5;

int main() {
    @x3[x] = 6;
}
)cpp");

    auto declaration = modifier_mask({SymbolModifiers::Declaration});
    auto definition = modifier_mask({SymbolModifiers::Definition});
    auto templated = modifier_mask({SymbolModifiers::Templated});

    expect_token("x1", SymbolKind::Variable, declaration);
    expect_token("x2", SymbolKind::Variable, definition);
    expect_token("y1", SymbolKind::Variable, declaration | templated);
    expect_token("y2", SymbolKind::Variable, definition | templated);
    expect_token("y3", SymbolKind::Variable, declaration | templated);
    expect_token("y4", SymbolKind::Variable, definition | templated);
    expect_token("y5", SymbolKind::Variable, definition);
    expect_token("x3", SymbolKind::Variable, 0);
}

TEST_CASE(LegacyFunctionDecl) {
    run_utf8(R"cpp(
extern int @foo1[foo]();

int @foo2[foo]() {
    return 0;
}

template <typename T>
extern int @bar1[bar]();

template <typename T>
int @bar2[bar]() {
    return 1;
}
)cpp");

    auto declaration = modifier_mask({SymbolModifiers::Declaration});
    auto definition = modifier_mask({SymbolModifiers::Definition});
    auto templated = modifier_mask({SymbolModifiers::Templated});

    expect_token("foo1", SymbolKind::Function, declaration);
    expect_token("foo2", SymbolKind::Function, definition);
    expect_token("bar1", SymbolKind::Function, declaration | templated);
    expect_token("bar2", SymbolKind::Function, definition | templated);
}

TEST_CASE(LegacyRecordDecl) {
    run_utf8(R"cpp(
class @a1[A];

class @a2[A] {};

struct @b1[B];

struct @b2[B] {};

union @c1[C];

union @c2[C] {};
)cpp");

    auto declaration = modifier_mask({SymbolModifiers::Declaration});
    auto definition = modifier_mask({SymbolModifiers::Definition});

    expect_token("a1", SymbolKind::Class, declaration);
    expect_token("a2", SymbolKind::Class, definition);
    expect_token("b1", SymbolKind::Struct, declaration);
    expect_token("b2", SymbolKind::Struct, definition);
    expect_token("c1", SymbolKind::Union, declaration);
    expect_token("c2", SymbolKind::Union, definition);
}

TEST_CASE(UTF16LengthDiffersFromUTF8) {
    tester.clear();
    tester.add_main("main.cpp", R"cpp(
int main() {
@lit[u8"你"];
}
)cpp");
    ASSERT_TRUE(tester.compile_with_pch());

    auto utf8_tokens = feature::semantic_tokens(*tester.unit, feature::PositionEncoding::UTF8);
    auto utf16_tokens = feature::semantic_tokens(*tester.unit, feature::PositionEncoding::UTF16);

    auto utf8 = decode_utf8_tokens(tester.unit->interested_content(), utf8_tokens);
    auto utf16 = decode_relative_tokens(utf16_tokens);

    auto string_type = static_cast<std::uint32_t>(SymbolKind::String);
    auto range = tester.range("lit");

    std::optional<DecodedToken> utf8_token;
    for(const auto& token: utf8) {
        if(token.range == range && token.type == string_type) {
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

TEST_CASE(MultiLineCommentSplitMatchesLegacyConverter) {
    tester.clear();
    tester.add_main("main.cpp", R"cpp(
int main() {
/*ab
cd*/
}
)cpp");
    ASSERT_TRUE(tester.compile_with_pch());

    auto utf8_tokens = feature::semantic_tokens(*tester.unit, feature::PositionEncoding::UTF8);
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

};  // TEST_SUITE(SemanticTokens)

}  // namespace

}  // namespace clice::testing
