#pragma once

#include <tuple>

#include "clang/Basic/SourceLocation.h"
#include "clang/Lex/Token.h"

namespace std {

template <>
struct tuple_size<clang::SourceRange> : std::integral_constant<std::size_t, 2> {};

template <>
struct tuple_element<0, clang::SourceRange> {
    using type = clang::SourceLocation;
};

template <>
struct tuple_element<1, clang::SourceRange> {
    using type = clang::SourceLocation;
};

}  // namespace std

namespace clang {

/// Through ADL, make `clang::SourceRange` could be destructured.
template <std::size_t I>
clang::SourceLocation get(clang::SourceRange range) {
    if constexpr(I == 0) {
        return range.getBegin();
    } else {
        return range.getEnd();
    }
}

class Lexer;

}  // namespace clang

namespace clice {

struct LocalSourceRange {
    /// The begin position offset to the source file.
    uint32_t begin = static_cast<uint32_t>(-1);

    /// The end position offset to the source file.
    uint32_t end = static_cast<uint32_t>(-1);

    constexpr bool operator==(const LocalSourceRange& other) const = default;

    constexpr auto length() {
        return end - begin;
    }

    constexpr bool contains(uint32_t offset) const {
        return offset >= begin && offset <= end;
    }

    constexpr bool intersects(const LocalSourceRange& other) const {
        return begin <= other.end && end >= other.begin;
    }

    constexpr bool valid() const {
        return begin != -1 && end != -1;
    }
};

using TokenKind = clang::tok::TokenKind;

struct Token {
    /// Whether this token is at the start of line.
    bool is_at_start_of_line = false;

    /// Whether this token is a preprocessor directive.
    bool is_pp_keyword = false;

    /// The kind of this token.
    TokenKind kind;

    /// The source range of this token.
    LocalSourceRange range;

    bool valid() {
        return range.valid();
    }

    llvm::StringRef name() const {
        return clang::tok::getTokenName(kind);
    }

    llvm::StringRef text(llvm::StringRef content) const {
        assert(range.valid() && "Invalid source range");
        return content.substr(range.begin, range.end - range.begin);
    }

    bool is_eod() const {
        return kind == clang::tok::eod;
    }

    bool is_eof() const {
        return kind == clang::tok::eof;
    }

    bool is_identifier() const {
        return kind == clang::tok::raw_identifier;
    }

    bool is_directive_hash() const {
        return is_at_start_of_line && kind == clang::tok::hash;
    }

    /// The tokens after the include diretive are regarded as
    /// a whole token, whose kind is `header_name`. For example
    /// `<iostream>` and `"test.h"` are both header name.
    bool is_header_name() const {
        return kind == clang::tok::header_name;
    }
};

class Lexer {
public:
    Lexer(llvm::StringRef content,
          bool ignore_comments = true,
          const clang::LangOptions* lang_opts = nullptr,
          bool ignore_end_of_directive = true);

    Lexer(const Lexer&) = delete;

    Lexer(Lexer&&) = delete;

    Lexer& operator=(const Lexer&) = delete;

    Lexer& operator=(Lexer&&) = delete;

    ~Lexer();

    void lex(Token& token);

    /// Get the token before this token without moving the lexer.
    Token last();

    /// Get the token after this token without moving the lexer.
    Token next();

    /// Advance the lexer and return the next token.
    Token advance();

    /// Advance the lexer if the next token kind is the param.
    std::optional<Token> advance_if(llvm::function_ref<bool(const Token&)> callback);

    std::optional<Token> advance_if(llvm::StringRef spelling) {
        return advance_if([&](const Token& token) {
            return token.is_identifier() && token.text(content) == spelling;
        });
    }

    std::optional<Token> advance_if(TokenKind kind) {
        return advance_if([&](const Token& token) { return token.kind == kind; });
    }

    /// Advance the lexer until meet the specific kind token.
    Token advance_until(TokenKind kind);

private:
    /// If this is set to false, the lexer will emit tok::eod at the end
    /// of directive.
    bool ignore_end_of_directive = true;

    /// Whether we are lexing the preprocessor directive.
    bool parse_pp_keyword = false;

    /// Whether we are lexing the header name.
    bool parse_header_name = false;

    bool module_declaration_context = true;

    /// The cache of last token.
    Token last_token;

    /// The cache of current token.
    Token current_token;

    /// The cache of next token.
    std::optional<Token> next_token;

    /// The lexed content.
    llvm::StringRef content;

    std::unique_ptr<clang::Lexer> lexer;
};

}  // namespace clice
