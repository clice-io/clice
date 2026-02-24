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
        parse_header_name = token.text(content) == "include";
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

}  // namespace clice
