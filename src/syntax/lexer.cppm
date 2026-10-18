module;

#include "modules/prelude.h"

module clice:syntax.lexer;

import :syntax.token;

namespace clice {

/// The options a raw lex of `language` under `standard` runs with: the
/// language defaults plus what the driver turns on by default and they
/// leave off — the alternative operator names (`and`) in C++, `char8_t`
/// and modules from C++20, the GNU keywords (`typeof`) in GNU modes — and
/// line comments in every dialect. One object per dialect, alive for the
/// whole run: the dialect of text no compile reads.
const clang::LangOptions& raw_dialect(clang::Language language, clang::LangStandard::Kind standard);

/// The dialect a raw lex of `path` runs in without a compile command: C
/// for .c files and when `c` says the text is C, C++ otherwise. `standard`
/// is the -std value of a command that compiles the text, when known: a
/// newer standard's extra keywords (`concept` in C++17 code) would shadow
/// identifiers. Absent, unknown or contradicting values fall back to the
/// driver's default standard — the dialect a -std-less command compiles.
/// With nothing known, that is C++'s, which lexes directive lines, braces
/// and statements as any dialect past C89 does.
const clang::LangOptions& dialect_of(llvm::StringRef path = {},
                                     bool c = false,
                                     llvm::StringRef standard = {});

/// The keyword table of the raw dialect of `dialect`'s language and
/// standard, built on first use and alive for the whole run.
const clang::IdentifierTable& keywords(const clang::LangOptions& dialect);

/// What the word `word` lexes as where `keywords` holds the keywords
/// (keywords(), or a compile's own identifier table): a keyword's kind, an
/// alternative operator name's (`and`) the punctuator it spells, else
/// `identifier`.
TokenKind keyword_kind(llvm::StringRef word, const clang::IdentifierTable& keywords);

/// What `token` of `content` lexes as: a raw identifier its keyword_kind,
/// line splices folded away; any other token its kind.
TokenKind lexed_kind(const Token& token,
                     llvm::StringRef content,
                     const clang::IdentifierTable& keywords);

/// A character of an identifier as the lexer reads one: `$` included, and
/// the bytes of multi-byte UTF-8 sequences, as clang accepts extended
/// characters in identifiers.
inline bool is_identifier_char(char c) {
    return clang::isAsciiIdentifierContinue(c, /*AllowDollar=*/true) ||
           static_cast<unsigned char>(c) >= 0x80;
}

struct LexerOptions {
    /// Emit comment tokens instead of dropping them.
    bool keep_comments = false;
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
    Lexer(llvm::StringRef content, const clang::LangOptions& dialect, Options options = {});

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
    /// Where the last token other than eod ended.
    std::uint32_t lexed_end = 0;
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
