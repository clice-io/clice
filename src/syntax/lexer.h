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

struct LexerOptions {
    /// Emit comment tokens instead of dropping them.
    bool keep_comments = false;

    const clang::LangOptions* lang_opts = nullptr;
};

/// A pull-style wrapper over clang's raw lexer with just enough
/// preprocessor awareness for scanning: directive lines are terminated by
/// eod tokens, directive keywords are flagged via Token::is_pp_keyword, and
/// header-name arguments (after #include/#embed keywords and inside the
/// parentheses of the __has_include family) are lexed as single
/// header_name tokens.
class Lexer {
public:
    using Options = LexerOptions;

    /// `content` must end at a NUL terminator (clang's raw lexer reads it
    /// as its end sentinel): full buffers and suffix slices are fine, but
    /// never pass a prefix slice — bound the lexing logically instead.
    explicit Lexer(llvm::StringRef content, Options options = {});

    /// Lex in place: start at the beginning of the line containing `offset`
    /// rather than at the buffer start. Token ranges are still offsets into
    /// the full `content`, so they compose directly with file coordinates
    /// and token.text(content).
    static Lexer from_line(llvm::StringRef content, std::uint32_t offset, Options options = {});

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

private:
    Lexer(llvm::StringRef content, std::uint32_t start_offset, Options options);

    bool parse_pp_keyword = false;
    bool parse_header_name = false;
    bool after_has_include = false;
    bool module_declaration_context = true;
    bool pending_start_of_line = false;

    Token last_token;
    Token current_token;
    std::optional<Token> next_token;
    llvm::StringRef content;
    std::unique_ptr<clang::Lexer> lexer;
};

}  // namespace clice
