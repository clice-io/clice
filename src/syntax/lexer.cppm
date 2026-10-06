module;

#include "modules/prelude.h"

module clice:syntax.lexer;

import :syntax.token;

namespace clang {}

namespace clice {

/// The options a raw lex of `language` under `standard` runs with: the
/// language defaults plus what the driver turns on by default and they
/// leave off — `char8_t` and modules from C++20, the GNU keywords
/// (`typeof`) in GNU modes — and line comments in every dialect.
clang::LangOptions raw_dialect(clang::Language language, clang::LangStandard::Kind standard);

struct LexerOptions {
    /// Emit comment tokens instead of dropping them.
    bool keep_comments = false;

    const clang::LangOptions* lang_opts = nullptr;
};

/// Whether a directive keyword takes a header-name argument: `#include`,
/// `#include_next`, `#import` and `#embed`. The lexer switches to
/// header-name mode after one, and every consumer hunting the filename
/// token keys on the same set.
inline bool takes_header_name(llvm::StringRef keyword) {
    return keyword == "include" || keyword == "include_next" || keyword == "import" ||
           keyword == "embed";
}

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
