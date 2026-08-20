#include "syntax/lexer.h"

#include "clang/Lex/Lexer.h"

namespace clice {

static clang::SourceLocation fake_loc = clang::SourceLocation::getFromRawEncoding(1);
static clang::LangOptions default_opts;

Lexer::Lexer(llvm::StringRef content, std::uint32_t start_offset, Options options) :
    content(content), lexer(new clang::Lexer(fake_loc,
                                             options.lang_opts ? *options.lang_opts : default_opts,
                                             content.begin(),
                                             content.begin() + start_offset,
                                             content.end())) {
    // clang's raw lexer reads the terminator as its end sentinel; a prefix
    // slice would make it run past the buffer end.
    assert(content.data() != nullptr && content.data()[content.size()] == '\0' &&
           "Lexer requires a NUL-terminated buffer");
    lexer->SetCommentRetentionState(options.keep_comments);
}

Lexer::Lexer(llvm::StringRef content, Options options) : Lexer(content, 0, options) {}

Lexer Lexer::from_line(llvm::StringRef content, std::uint32_t offset, Options options) {
    std::uint32_t line_start = 0;
    if(auto nl = content.rfind('\n', offset); nl != llvm::StringRef::npos) {
        line_start = static_cast<std::uint32_t>(nl + 1);
    }
    return Lexer(content, line_start, options);
}

Lexer::~Lexer() = default;

void Lexer::lex(Token& token) {
    clang::Token raw_token;

    if(parse_header_name) {
        // One-shot: exactly the next token is a filename argument.
        parse_header_name = false;
        lexer->LexIncludeFilename(raw_token);
    } else {
        lexer->LexFromRawLexer(raw_token);
    }

    token.kind = raw_token.getKind();
    token.is_at_start_of_line = raw_token.isAtStartOfLine();
    token.is_pp_keyword = parse_pp_keyword;

    // The fake location maps the buffer start, so raw locations decode
    // straight to offsets into `content` even when lexing starts mid-buffer.
    auto offset = raw_token.getLocation().getRawEncoding() - fake_loc.getRawEncoding();
    token.range = LocalSourceRange{offset, offset + raw_token.getLength()};

    // Comments are transparent to the directive machinery, matching how the
    // lexer behaves when they are dropped: a retained line-leading comment
    // must neither consume the start-of-line state the next token keys on
    // nor end the module declaration context.
    if(token.kind == clang::tok::comment) {
        pending_start_of_line |= token.is_at_start_of_line;
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
        parse_header_name =
            kw == "include" || kw == "include_next" || kw == "embed" || kw == "import";
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
