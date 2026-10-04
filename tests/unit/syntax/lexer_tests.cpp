#include <cstddef>
#include <vector>

#include "test/test.h"
#include "syntax/lexer.h"

namespace clice::testing {
namespace {

/// Drain the lexer and return every token before eof.
std::vector<Token> lex_all(Lexer& lexer) {
    std::vector<Token> tokens;
    while(true) {
        Token token = lexer.advance();
        if(token.is_eof()) {
            break;
        }
        tokens.push_back(token);
    }
    return tokens;
}

ZEST_SUITE(SourceText) {

ZEST_CASE(IgnoreComments) {
    llvm::StringRef content = "int x = 1; // comment";

    std::vector<TokenKind> kinds = {
        clang::tok::raw_identifier,
        clang::tok::raw_identifier,
        clang::tok::equal,
        clang::tok::numeric_constant,
        clang::tok::semi,
    };

    {
        Lexer lexer(content);
        auto tokens = lex_all(lexer);
        ZASSERT(tokens.size() == kinds.size());
        for(std::size_t i = 0; i < kinds.size(); i += 1) {
            ZASSERT(tokens[i].kind == kinds[i]);
        }
    }

    kinds.push_back(clang::tok::comment);

    {
        Lexer lexer(content, {.keep_comments = true});
        auto tokens = lex_all(lexer);
        ZASSERT(tokens.size() == kinds.size());
        for(std::size_t i = 0; i < kinds.size(); i += 1) {
            ZASSERT(tokens[i].kind == kinds[i]);
        }
        ZASSERT(tokens.back().text(content) == "// comment");
    }
}

ZEST_CASE(TokenRanges) {
    llvm::StringRef content = "int foo = 42;";
    Lexer lexer(content);
    auto tokens = lex_all(lexer);

    ZASSERT(tokens.size() == 5U);
    ZASSERT(tokens[0].text(content) == "int");
    ZASSERT(tokens[1].text(content) == "foo");
    ZASSERT(tokens[1].range.begin == 4U);
    ZASSERT(tokens[1].range.end == 7U);
    ZASSERT(tokens[3].text(content) == "42");
    ZASSERT(tokens[4].text(content) == ";");
}

ZEST_CASE(LexInclude) {
    llvm::StringRef content = R"(
#include <iostream>
#include "gtest/test.h"
module;
int x = 1;
)";
    Lexer lexer(content);
    auto tokens = lex_all(lexer);

    std::vector<TokenKind> kinds = {
        clang::tok::hash,            // #
        clang::tok::raw_identifier,  // include
        clang::tok::header_name,     // <iostream>
        clang::tok::eod,
        clang::tok::hash,            // #
        clang::tok::raw_identifier,  // include
        clang::tok::header_name,     // "gtest/test.h"
        clang::tok::eod,
        clang::tok::raw_identifier,  // module
        clang::tok::semi,            // ;
        clang::tok::eod,
        clang::tok::raw_identifier,  // int
        clang::tok::raw_identifier,  // x
        clang::tok::equal,           // =
        clang::tok::numeric_constant,
        clang::tok::semi,
    };

    ZASSERT(tokens.size() == kinds.size());
    for(std::size_t i = 0; i < kinds.size(); i += 1) {
        ZASSERT(tokens[i].kind == kinds[i]);
    }

    ZASSERT(tokens[2].text(content) == "<iostream>");
    ZASSERT(tokens[1].is_pp_keyword);
    ZASSERT(tokens[6].text(content) == R"("gtest/test.h")");
    ZASSERT(tokens[8].is_pp_keyword);
}

};  // ZEST_SUITE(SourceText)

ZEST_SUITE(HeaderNameLexing) {

/// The text of the first header-name token in `content`, or empty.
llvm::StringRef first_header_name(llvm::StringRef content) {
    Lexer lexer(content);
    while(true) {
        Token token = lexer.advance();
        if(token.is_eof()) {
            return "";
        }
        if(token.is_header_name()) {
            return token.text(content);
        }
    }
}

ZEST_CASE(HasIncludeArgument) {
    ZASSERT(first_header_name("#if __has_include(<vector>)") == "<vector>");
    ZASSERT(first_header_name(R"(#if __has_include("foo.h"))") == R"("foo.h")");
    ZASSERT(first_header_name("#if __has_include_next(<stdlib.h>)") == "<stdlib.h>");
}

ZEST_CASE(HasEmbedArgument) {
    ZASSERT(first_header_name(R"(#if __has_embed("data.bin"))") == R"("data.bin")");
}

ZEST_CASE(IncludeNextArgument) {
    ZASSERT(first_header_name("#include_next <stdlib.h>") == "<stdlib.h>");
}

ZEST_CASE(EmbedArgument) {
    ZASSERT(first_header_name(R"(#embed "data.bin")") == R"("data.bin")");
}

ZEST_CASE(HashImportArgument) {
    ZASSERT(first_header_name("#import <Foundation/Foundation.h>") == "<Foundation/Foundation.h>");
}

ZEST_CASE(MacroArgument) {
    // A macro filename argument stays an ordinary identifier.
    llvm::StringRef content = "#include HEADER";
    Lexer lexer(content);
    auto tokens = lex_all(lexer);

    ZASSERT(tokens.size() >= 3U);
    ZASSERT(first_header_name(content) == "");
    ZASSERT(tokens[2].is_identifier());
    ZASSERT(tokens[2].text(content) == "HEADER");
}

ZEST_CASE(CommentThenDirective) {
    // A retained leading comment must not consume the start-of-line state
    // the directive machinery keys on.
    llvm::StringRef content = "/* c */ #include <x>\nint y;\n";
    Lexer lexer(content, {.keep_comments = true});

    auto comment = lexer.advance();
    ZASSERT(comment.kind == clang::tok::comment);

    auto hash = lexer.advance();
    ZASSERT(hash.kind == clang::tok::hash);
    ZASSERT(hash.is_at_start_of_line);

    auto keyword = lexer.advance();
    ZASSERT(keyword.is_pp_keyword);

    auto name = lexer.advance();
    ZASSERT(name.is_header_name());
    ZASSERT(name.text(content) == "<x>");
}

ZEST_CASE(SplicedInclude) {
    // The token spelling legitimately contains the line splice; only the
    // trailing part is the written filename.
    ZASSERT(first_header_name("#include \\\n<foo.h>\nint x;").ends_with("<foo.h>"));
}

ZEST_CASE(EmptyInclude) {
    llvm::StringRef content = "#include \nint x;";
    Lexer lexer(content);
    auto tokens = lex_all(lexer);

    // No filename: the directive just ends; nothing is lexed as a header name.
    ZASSERT(first_header_name(content) == "");
    ZASSERT(tokens[2].kind == clang::tok::eod);
}

};  // ZEST_SUITE(HeaderNameLexing)

ZEST_SUITE(FromLine) {

ZEST_CASE(MidFileLine) {
    llvm::StringRef content = "int a;\n#include <foo>\nint b;\n";
    auto offset = static_cast<std::uint32_t>(content.find("include"));

    auto lexer = Lexer::from_line(content, offset);
    auto hash = lexer.advance();
    ZASSERT(hash.kind == clang::tok::hash);
    ZASSERT(hash.is_at_start_of_line);
    ZASSERT(hash.range.begin == static_cast<std::uint32_t>(content.find('#')));

    auto keyword = lexer.advance();
    ZASSERT(keyword.is_pp_keyword);
    ZASSERT(keyword.text(content) == "include");

    auto name = lexer.advance();
    ZASSERT(name.is_header_name());
    ZASSERT(name.text(content) == "<foo>");
}

ZEST_CASE(FirstLine) {
    llvm::StringRef content = "int a = 1;\nint b;\n";
    auto lexer = Lexer::from_line(content, 4);
    ZASSERT(lexer.advance().text(content) == "int");
}

ZEST_CASE(OffsetAtNewline) {
    // An offset on the terminating newline still lexes the line it ends.
    llvm::StringRef content = "int a;\nint b;\n";
    auto offset = static_cast<std::uint32_t>(content.find('\n', content.find('b')));

    auto lexer = Lexer::from_line(content, offset);
    auto token = lexer.advance();
    ZASSERT(token.text(content) == "int");
    ZASSERT(token.range.begin == static_cast<std::uint32_t>(content.find("int b")));
}

ZEST_CASE(EmptyContent) {
    auto lexer = Lexer::from_line("", 0);
    ZASSERT(lexer.advance().is_eof());
}

ZEST_CASE(ContinuesToEnd) {
    // from_line picks the starting point; lexing continues past the line.
    llvm::StringRef content = "int a;\nint b;\nint c;\n";
    auto lexer = Lexer::from_line(content, static_cast<std::uint32_t>(content.find('b')));
    auto tokens = lex_all(lexer);
    ZASSERT(tokens.size() == 6U);
    ZASSERT(tokens.back().text(content) == ";");
}

};  // ZEST_SUITE(FromLine)

ZEST_SUITE(IncompleteInput) {

ZEST_CASE(UnterminatedHeaderName) {
    llvm::StringRef content = "#include <iost";
    Lexer lexer(content);
    auto tokens = lex_all(lexer);
    ZASSERT(tokens.size() >= 2U);
}

ZEST_CASE(UnterminatedString) {
    llvm::StringRef content = R"(const char* s = "abc)";
    Lexer lexer(content);
    auto tokens = lex_all(lexer);
    ZASSERT(tokens.size() >= 4U);
}

ZEST_CASE(UnterminatedComment) {
    // clang's raw lexer swallows a block comment left unterminated at eof;
    // no comment token is produced for it.
    llvm::StringRef content = "int x; /* abc";
    Lexer lexer(content, {.keep_comments = true});
    auto tokens = lex_all(lexer);
    ZASSERT(tokens.size() == 3U);
    ZASSERT(tokens.back().kind == clang::tok::semi);
}

};  // ZEST_SUITE(IncompleteInput)

}  // namespace
}  // namespace clice::testing
