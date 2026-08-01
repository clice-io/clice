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

TEST_SUITE(SourceText) {

TEST_CASE(IgnoreComments) {
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
        ASSERT_EQ(tokens.size(), kinds.size());
        for(std::size_t i = 0; i < kinds.size(); i += 1) {
            ASSERT_EQ(tokens[i].kind, kinds[i]);
        }
    }

    kinds.push_back(clang::tok::comment);

    {
        Lexer lexer(content, {.keep_comments = true});
        auto tokens = lex_all(lexer);
        ASSERT_EQ(tokens.size(), kinds.size());
        for(std::size_t i = 0; i < kinds.size(); i += 1) {
            ASSERT_EQ(tokens[i].kind, kinds[i]);
        }
        ASSERT_EQ(tokens.back().text(content), "// comment");
    }
}

TEST_CASE(TokenRanges) {
    llvm::StringRef content = "int foo = 42;";
    Lexer lexer(content);
    auto tokens = lex_all(lexer);

    ASSERT_EQ(tokens.size(), 5U);
    ASSERT_EQ(tokens[0].text(content), "int");
    ASSERT_EQ(tokens[1].text(content), "foo");
    ASSERT_EQ(tokens[1].range.begin, 4U);
    ASSERT_EQ(tokens[1].range.end, 7U);
    ASSERT_EQ(tokens[3].text(content), "42");
    ASSERT_EQ(tokens[4].text(content), ";");
}

TEST_CASE(LexInclude) {
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

    ASSERT_EQ(tokens.size(), kinds.size());
    for(std::size_t i = 0; i < kinds.size(); i += 1) {
        ASSERT_EQ(tokens[i].kind, kinds[i]);
    }

    ASSERT_EQ(tokens[2].text(content), "<iostream>");
    ASSERT_TRUE(tokens[1].is_pp_keyword);
    ASSERT_EQ(tokens[6].text(content), R"("gtest/test.h")");
    ASSERT_TRUE(tokens[8].is_pp_keyword);
}

};  // TEST_SUITE(SourceText)

TEST_SUITE(HeaderNameLexing) {

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

TEST_CASE(HasIncludeArgument) {
    ASSERT_EQ(first_header_name("#if __has_include(<vector>)"), "<vector>");
    ASSERT_EQ(first_header_name(R"(#if __has_include("foo.h"))"), R"("foo.h")");
    ASSERT_EQ(first_header_name("#if __has_include_next(<stdlib.h>)"), "<stdlib.h>");
}

TEST_CASE(HasEmbedArgument) {
    ASSERT_EQ(first_header_name(R"(#if __has_embed("data.bin"))"), R"("data.bin")");
}

TEST_CASE(IncludeNextArgument) {
    ASSERT_EQ(first_header_name("#include_next <stdlib.h>"), "<stdlib.h>");
}

TEST_CASE(EmbedArgument) {
    ASSERT_EQ(first_header_name(R"(#embed "data.bin")"), R"("data.bin")");
}

TEST_CASE(HashImportArgument) {
    ASSERT_EQ(first_header_name("#import <Foundation/Foundation.h>"), "<Foundation/Foundation.h>");
}

TEST_CASE(MacroArgument) {
    // A macro filename argument stays an ordinary identifier.
    llvm::StringRef content = "#include HEADER";
    Lexer lexer(content);
    auto tokens = lex_all(lexer);

    ASSERT_TRUE(tokens.size() >= 3U);
    ASSERT_EQ(first_header_name(content), "");
    ASSERT_TRUE(tokens[2].is_identifier());
    ASSERT_EQ(tokens[2].text(content), "HEADER");
}

TEST_CASE(CommentThenDirective) {
    // A retained leading comment must not consume the start-of-line state
    // the directive machinery keys on.
    llvm::StringRef content = "/* c */ #include <x>\nint y;\n";
    Lexer lexer(content, {.keep_comments = true});

    auto comment = lexer.advance();
    ASSERT_EQ(comment.kind, clang::tok::comment);

    auto hash = lexer.advance();
    ASSERT_EQ(hash.kind, clang::tok::hash);
    ASSERT_TRUE(hash.is_at_start_of_line);

    auto keyword = lexer.advance();
    ASSERT_TRUE(keyword.is_pp_keyword);

    auto name = lexer.advance();
    ASSERT_TRUE(name.is_header_name());
    ASSERT_EQ(name.text(content), "<x>");
}

TEST_CASE(SplicedInclude) {
    // The token spelling legitimately contains the line splice; only the
    // trailing part is the written filename.
    ASSERT_TRUE(first_header_name("#include \\\n<foo.h>\nint x;").ends_with("<foo.h>"));
}

TEST_CASE(EmptyInclude) {
    llvm::StringRef content = "#include \nint x;";
    Lexer lexer(content);
    auto tokens = lex_all(lexer);

    // No filename: the directive just ends; nothing is lexed as a header name.
    ASSERT_EQ(first_header_name(content), "");
    ASSERT_EQ(tokens[2].kind, clang::tok::eod);
}

};  // TEST_SUITE(HeaderNameLexing)

TEST_SUITE(FromLine) {

TEST_CASE(MidFileLine) {
    llvm::StringRef content = "int a;\n#include <foo>\nint b;\n";
    auto offset = static_cast<std::uint32_t>(content.find("include"));

    auto lexer = Lexer::from_line(content, offset);
    auto hash = lexer.advance();
    ASSERT_EQ(hash.kind, clang::tok::hash);
    ASSERT_TRUE(hash.is_at_start_of_line);
    ASSERT_EQ(hash.range.begin, static_cast<std::uint32_t>(content.find('#')));

    auto keyword = lexer.advance();
    ASSERT_TRUE(keyword.is_pp_keyword);
    ASSERT_EQ(keyword.text(content), "include");

    auto name = lexer.advance();
    ASSERT_TRUE(name.is_header_name());
    ASSERT_EQ(name.text(content), "<foo>");
}

TEST_CASE(FirstLine) {
    llvm::StringRef content = "int a = 1;\nint b;\n";
    auto lexer = Lexer::from_line(content, 4);
    ASSERT_EQ(lexer.advance().text(content), "int");
}

TEST_CASE(OffsetAtNewline) {
    // An offset on the terminating newline still lexes the line it ends.
    llvm::StringRef content = "int a;\nint b;\n";
    auto offset = static_cast<std::uint32_t>(content.find('\n', content.find('b')));

    auto lexer = Lexer::from_line(content, offset);
    auto token = lexer.advance();
    ASSERT_EQ(token.text(content), "int");
    ASSERT_EQ(token.range.begin, static_cast<std::uint32_t>(content.find("int b")));
}

TEST_CASE(EmptyContent) {
    auto lexer = Lexer::from_line("", 0);
    ASSERT_TRUE(lexer.advance().is_eof());
}

TEST_CASE(ContinuesToEnd) {
    // from_line picks the starting point; lexing continues past the line.
    llvm::StringRef content = "int a;\nint b;\nint c;\n";
    auto lexer = Lexer::from_line(content, static_cast<std::uint32_t>(content.find('b')));
    auto tokens = lex_all(lexer);
    ASSERT_EQ(tokens.size(), 6U);
    ASSERT_EQ(tokens.back().text(content), ";");
}

};  // TEST_SUITE(FromLine)

TEST_SUITE(IncompleteInput) {

TEST_CASE(UnterminatedHeaderName) {
    llvm::StringRef content = "#include <iost";
    Lexer lexer(content);
    auto tokens = lex_all(lexer);
    ASSERT_TRUE(tokens.size() >= 2U);
}

TEST_CASE(UnterminatedString) {
    llvm::StringRef content = R"(const char* s = "abc)";
    Lexer lexer(content);
    auto tokens = lex_all(lexer);
    ASSERT_TRUE(tokens.size() >= 4U);
}

TEST_CASE(UnterminatedComment) {
    // clang's raw lexer swallows a block comment left unterminated at eof;
    // no comment token is produced for it.
    llvm::StringRef content = "int x; /* abc";
    Lexer lexer(content, {.keep_comments = true});
    auto tokens = lex_all(lexer);
    ASSERT_EQ(tokens.size(), 3U);
    ASSERT_EQ(tokens.back().kind, clang::tok::semi);
}

};  // TEST_SUITE(IncompleteInput)

}  // namespace
}  // namespace clice::testing
