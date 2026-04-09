#pragma once

#include <memory>
#include <optional>

#include "syntax/token.h"

#include "llvm/ADT/STLFunctionalExtras.h"
#include "llvm/ADT/StringRef.h"
#include "clang/Basic/LangOptions.h"

namespace clang {

class Lexer;

}

namespace clice {

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

    Token last();
    Token next();
    Token advance();

    std::optional<Token> advance_if(llvm::function_ref<bool(const Token&)> callback);

    std::optional<Token> advance_if(llvm::StringRef spelling) {
        return advance_if([&](const Token& token) {
            return token.is_identifier() && token.text(content) == spelling;
        });
    }

    std::optional<Token> advance_if(TokenKind kind) {
        return advance_if([&](const Token& token) { return token.kind == kind; });
    }

    Token advance_until(TokenKind kind);

    /// Force the lexer into header-name mode so the next token is lexed
    /// via LexIncludeFilename (correctly handling both "..." and <...>).
    /// Use this before lexing filename arguments in contexts like
    /// __has_include() or __has_embed() where the lexer cannot detect
    /// the mode automatically.
    void set_header_name_mode() {
        parse_header_name = true;
    }

private:
    bool ignore_end_of_directive = true;
    bool parse_pp_keyword = false;
    bool parse_header_name = false;
    bool module_declaration_context = true;

    Token last_token;
    Token current_token;
    std::optional<Token> next_token;
    llvm::StringRef content;
    std::unique_ptr<clang::Lexer> lexer;
};

/// Find the range of the filename argument in a preprocessor directive line.
/// `content` is the full source text, `offset` points at or before the directive keyword.
/// Returns the range of the first filename-like token (header name, string literal,
/// or macro identifier) found on the same line, or nullopt if none.
std::optional<LocalSourceRange>
    find_directive_argument(llvm::StringRef content,
                            std::uint32_t offset,
                            const clang::LangOptions* lang_opts = nullptr);

}  // namespace clice
