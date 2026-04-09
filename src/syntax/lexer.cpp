#include "syntax/lexer.h"

#include "clang/Lex/Lexer.h"

namespace clice {

static clang::SourceLocation fake_loc = clang::SourceLocation::getFromRawEncoding(1);
static clang::LangOptions default_opts;

Lexer::Lexer(llvm::StringRef content,
             bool ignore_comments,
             const clang::LangOptions* lang_opts,
             bool ignore_end_of_directive) :
    content(content), ignore_end_of_directive(ignore_end_of_directive),
    lexer(new clang::Lexer(fake_loc,
                           lang_opts ? *lang_opts : default_opts,
                           content.begin(),
                           content.begin(),
                           content.end())) {
    lexer->SetCommentRetentionState(!ignore_comments);
}

Lexer::~Lexer() = default;

void Lexer::lex(Token& token) {
    clang::Token raw_token;

    if(parse_header_name) {
        lexer->LexIncludeFilename(raw_token);
    } else {
        lexer->LexFromRawLexer(raw_token);
    }

    token.kind = raw_token.getKind();
    token.is_at_start_of_line = raw_token.isAtStartOfLine();
    token.is_pp_keyword = parse_pp_keyword;

    auto offset = raw_token.getLocation().getRawEncoding() - fake_loc.getRawEncoding();
    token.range = LocalSourceRange{offset, offset + raw_token.getLength()};

    if(token.is_at_start_of_line) {
        parse_header_name = false;

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
        parse_header_name = kw == "include" || kw == "include_next" || kw == "embed";
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

std::optional<Token> Lexer::advance_if(llvm::function_ref<bool(const Token&)> callback) {
    auto token = next();

    if(callback(token)) {
        return advance();
    }

    return std::nullopt;
}

Token Lexer::advance_until(TokenKind kind) {
    while(true) {
        auto token = advance();
        if(token.kind == kind || token.is_eof()) {
            return token;
        }
    }
}

static bool is_directive_keyword(llvm::StringRef word) {
    return word == "include" || word == "include_next" || word == "import" || word == "embed" ||
           word == "__has_include" || word == "__has_include_next" || word == "__has_embed";
}

std::optional<LocalSourceRange> find_directive_argument(llvm::StringRef content,
                                                        std::uint32_t offset,
                                                        const clang::LangOptions* lang_opts) {
    std::uint32_t line_start = 0;
    if(auto nl = content.rfind('\n', offset); nl != llvm::StringRef::npos)
        line_start = static_cast<std::uint32_t>(nl + 1);

    auto line = content.substr(line_start);
    Lexer lexer(line, true, lang_opts);
    bool after_has_keyword = false;
    bool ready = false;

    while(true) {
        auto tok = lexer.advance();
        if(tok.is_eof() || tok.is_eod())
            break;

        auto abs_begin = line_start + tok.range.begin;
        auto abs_end = line_start + tok.range.end;

        if(tok.is_identifier()) {
            auto text = tok.text(line);
            if(text == "__has_include" || text == "__has_include_next" || text == "__has_embed") {
                after_has_keyword = true;
                continue;
            }
            if(text == "include" || text == "include_next" || text == "embed") {
                ready = true;
                continue;
            }
        }

        if(tok.kind == clang::tok::l_paren && after_has_keyword) {
            after_has_keyword = false;
            ready = true;
            lexer.set_header_name_mode();
            continue;
        }

        if(abs_begin < offset || !ready)
            continue;

        if(tok.is_header_name() || tok.kind == clang::tok::string_literal)
            return LocalSourceRange(abs_begin, abs_end);

        if(tok.is_identifier())
            return LocalSourceRange(abs_begin, abs_end);
    }
    return std::nullopt;
}

}  // namespace clice
