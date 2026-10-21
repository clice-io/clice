module;

#include "modules/prelude.h"

module clice;

import :support.text;
import :syntax.lexer;

namespace clice {

namespace {

/// Every dialect raw_dialect() made and the keyword tables keywords()
/// built of them. Stable addresses: callers hold both by reference.
struct RawDialects {
    std::mutex mutex;
    std::map<std::pair<clang::Language, clang::LangStandard::Kind>, clang::LangOptions> dialects;
    std::map<const clang::LangOptions*, clang::IdentifierTable> keywords;

    static RawDialects& get() {
        static RawDialects instance;
        return instance;
    }
};

}  // namespace

const clang::LangOptions& raw_dialect(clang::Language language,
                                      clang::LangStandard::Kind standard) {
    auto& raw = RawDialects::get();
    std::lock_guard lock(raw.mutex);
    auto [it, inserted] = raw.dialects.try_emplace({language, standard});
    if(inserted) {
        auto& options = it->second;
        std::vector<std::string> includes;
        clang::LangOptions::setLangDefaults(options, language, llvm::Triple(), includes, standard);
        // FIXME: only the language and -std reach a raw dialect, never
        // -fno-operator-names, -fno-char8_t or -fno-gnu-keywords: until the
        // file's compile finishes, the index route of semantic tokens paints
        // `int and;` as an operator and a variable named `char8_t` as a type.
        options.CXXOperatorNames = options.CPlusPlus;
        options.Char8 = options.CPlusPlus20;
        options.CPlusPlusModules = options.CPlusPlus20;
        options.GNUKeywords = options.GNUMode;
        options.LineComment = true;
    }
    return it->second;
}

const clang::LangOptions& dialect_of(llvm::StringRef path, bool c, llvm::StringRef standard) {
    auto language = c || path.ends_with(".c") ? clang::Language::C : clang::Language::CXX;
    auto kind = clang::getDefaultLanguageStandard(language, llvm::Triple());
    if(!standard.empty()) {
        auto parsed = clang::LangStandard::getLangKind(standard);
        if(parsed != clang::LangStandard::lang_unspecified &&
           clang::LangStandard::getLangStandardForKind(parsed).getLanguage() == language) {
            kind = parsed;
        }
    }
    return raw_dialect(language, kind);
}

const clang::IdentifierTable& keywords(const clang::LangOptions& dialect) {
    const auto& raw =
        raw_dialect(dialect.CPlusPlus ? clang::Language::CXX : clang::Language::C, dialect.LangStd);
    auto& dialects = RawDialects::get();
    std::lock_guard lock(dialects.mutex);
    return dialects.keywords.try_emplace(&raw, raw).first->second;
}

TokenKind keyword_kind(llvm::StringRef word, const clang::IdentifierTable& keywords) {
    auto it = keywords.find(word);
    return it == keywords.end() ? clang::tok::identifier : it->second->getTokenID();
}

TokenKind lexed_kind(const Token& token,
                     llvm::StringRef content,
                     const clang::IdentifierTable& keywords) {
    if(!token.is_identifier()) {
        return token.kind;
    }
    auto text = token.text(content);
    return text.contains('\\') ? keyword_kind(unspliced(text), keywords)
                               : keyword_kind(text, keywords);
}

static clang::SourceLocation fake_loc = clang::SourceLocation::getFromRawEncoding(1);

Lexer::Lexer(llvm::StringRef content, const clang::LangOptions& dialect, Options options) :
    content(content),
    lexer(new clang::Lexer(fake_loc, dialect, content.begin(), content.begin(), content.end())) {
    // clang's raw lexer reads the terminator as its end sentinel; a prefix
    // slice would make it run past the buffer end.
    assert(content.data() != nullptr && content.data()[content.size()] == '\0' &&
           "Lexer requires a NUL-terminated buffer");
    lexer->SetCommentRetentionState(options.keep_comments);
}

Lexer::~Lexer() = default;

void Lexer::lex(Token& token) {
    clang::Token raw_token;

    // One-shot: exactly the next token is a filename argument.
    bool header_name = std::exchange(parse_header_name, false);
    if(header_name) {
        lexer->LexIncludeFilename(raw_token);
    } else {
        lexer->LexFromRawLexer(raw_token);
    }

    token.kind = raw_token.getKind();
    token.is_at_start_of_line = raw_token.isAtStartOfLine();
    token.is_pp_keyword = parse_pp_keyword;

    // The fake location maps the buffer start, so raw locations decode
    // straight to offsets into `content`.
    auto offset = raw_token.getLocation().getRawEncoding() - fake_loc.getRawEncoding();
    token.range = LocalSourceRange{offset, offset + raw_token.getLength()};

    // clang's raw lexer drops a block comment left open (one being typed)
    // and goes on to the end of the file: it is a comment to the end.
    if(token.is_eof() && lexer->inKeepCommentMode()) {
        auto rest = content.substr(lexed_end);
        auto at = rest.find_first_not_of(" \t\r\n\f\v");
        if(at != llvm::StringRef::npos && rest.substr(at).starts_with("/*")) {
            token.kind = clang::tok::comment;
            token.range = {lexed_end + static_cast<std::uint32_t>(at),
                           static_cast<std::uint32_t>(content.size())};
            lexed_end = token.range.end;
            return;
        }
    }
    if(!token.is_eod() && !token.is_eof()) {
        lexed_end = token.range.end;
    }

    // Comments are transparent to the directive machinery, matching how the
    // lexer behaves when they are dropped: a retained comment must neither
    // consume the start-of-line state the next token keys on, nor end the
    // module declaration context, nor take the place of the filename a
    // directive keyword awaits.
    if(token.kind == clang::tok::comment) {
        token.is_pp_keyword = false;
        pending_start_of_line |= token.is_at_start_of_line;
        parse_header_name = header_name;
        return;
    }

    if(pending_start_of_line) {
        token.is_at_start_of_line = true;
        pending_start_of_line = false;
    }

    if(token.is_at_start_of_line) {
        if(token.kind == clang::tok::hash ||
           (module_declaration_context && token.text(content) == "export")) {
            parse_pp_keyword = true;
            lexer->setParsingPreprocessorDirective(true);
        } else if(module_declaration_context && token.text(content) == "module") {
            token.is_pp_keyword = true;
            lexer->setParsingPreprocessorDirective(true);
        } else {
            module_declaration_context = false;
        }
    } else if(parse_pp_keyword) {
        parse_pp_keyword = false;
        auto kw = token.text(content);
        // `import` here is the directive form (`#import`), which takes a
        // filename; a module import never sets parse_pp_keyword.
        parse_header_name = takes_header_name(kw);
    }

    // The __has_include family takes a parenthesized filename argument the
    // raw lexer cannot detect on its own (LexIncludeFilename handles both
    // "..." and <...>): switch to header-name mode right after the opening
    // paren.
    if(after_has_include && token.kind == clang::tok::l_paren) {
        parse_header_name = true;
    }

    after_has_include = false;
    if(token.is_identifier()) {
        auto text = token.text(content);
        after_has_include =
            text == "__has_include" || text == "__has_include_next" || text == "__has_embed";
    }
}

Token Lexer::last() {
    return last_token;
}

Token Lexer::next() {
    if(!next_token) {
        Token token;
        lex(token);
        next_token.emplace(token);
    }

    return *next_token;
}

Token Lexer::advance() {
    last_token = current_token;

    if(next_token) {
        current_token = *next_token;
        next_token.reset();
    } else {
        Token token;
        lex(token);
        current_token = token;
    }

    return current_token;
}

Token Lexer::advance_until(TokenKind kind) {
    while(true) {
        auto token = advance();
        if(token.kind == kind || token.is_eof()) {
            return token;
        }
    }
}

}  // namespace clice
