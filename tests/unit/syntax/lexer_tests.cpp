#include <cstddef>
#include <vector>

#include "test/test.h"
#include "syntax/lexer.h"

namespace clice::testing {
namespace {
TEST_SUITE(SourceText) {

TEST_CASE(IgnoreComments) {
    std::size_t count = 0;

    std::vector<clang::tok::TokenKind> kinds = {
        clang::tok::raw_identifier,
        clang::tok::raw_identifier,
        clang::tok::equal,
        clang::tok::numeric_constant,
        clang::tok::semi,
    };

    {
        Lexer lexer("int x = 1; // comment", true);

        while(true) {
            Token token = lexer.advance();
            if(token.is_eof()) {
                break;
            }

            ASSERT_EQ(token.kind, kinds[count]);
            count += 1;
        }

        ASSERT_EQ(count, 5);
    }

    count = 0;

    kinds = {
        clang::tok::raw_identifier,
        clang::tok::raw_identifier,
        clang::tok::equal,
        clang::tok::numeric_constant,
        clang::tok::semi,
        clang::tok::comment,
    };

    {
        Lexer lexer("int x = 1; // comment", false);

        while(true) {
            Token token = lexer.advance();
            if(token.is_eof()) {
                break;
            }

            ASSERT_EQ(token.kind, kinds[count]);
            count += 1;
        }

        ASSERT_EQ(count, 6);
    }
}

TEST_CASE(LexInclude) {
    Lexer lexer(R"(
#include <iostream>
#include "gtest/test.h"
module;
int x = 1;
)",
                true,
                nullptr,
                false);

    while(true) {
        Token token = lexer.advance();
        if(token.is_eof()) {
            break;
        }
    }
}

};  // TEST_SUITE(SourceText)

TEST_SUITE(DirectiveArgument) {

void EXPECT_RANGE(llvm::StringRef content, std::uint32_t offset, llvm::StringRef expected) {
    auto result = find_directive_argument(content, offset);
    ASSERT_TRUE(result.has_value());
    ASSERT_EQ(content.substr(result->begin, result->length()), expected);
}

void EXPECT_NONE(llvm::StringRef content, std::uint32_t offset) {
    auto result = find_directive_argument(content, offset);
    ASSERT_FALSE(result.has_value());
}

TEST_CASE(IncludeQuoted) {
    llvm::StringRef src = R"(#include "foo.h")";
    EXPECT_RANGE(src, 0, R"("foo.h")");
}

TEST_CASE(IncludeAngled) {
    llvm::StringRef src = "#include <iostream>";
    EXPECT_RANGE(src, 0, "<iostream>");
}

TEST_CASE(IncludeMacro) {
    llvm::StringRef src = "#include HEADER";
    EXPECT_RANGE(src, 0, "HEADER");
}

TEST_CASE(HasIncludeQuoted) {
    llvm::StringRef src = R"(#if __has_include("foo.h"))";
    // offset at __has_include
    auto pos = src.find("__has_include");
    EXPECT_RANGE(src, static_cast<std::uint32_t>(pos), R"("foo.h")");
}

TEST_CASE(HasIncludeAngled) {
    llvm::StringRef src = "#if __has_include(<vector>)";
    auto pos = src.find("__has_include");
    EXPECT_RANGE(src, static_cast<std::uint32_t>(pos), "<vector>");
}

TEST_CASE(EmbedQuoted) {
    llvm::StringRef src = R"(#embed "data.bin")";
    EXPECT_RANGE(src, 0, R"("data.bin")");
}

TEST_CASE(HasEmbedQuoted) {
    llvm::StringRef src = R"(#if __has_embed("data.bin"))";
    auto pos = src.find("__has_embed");
    EXPECT_RANGE(src, static_cast<std::uint32_t>(pos), R"("data.bin")");
}

TEST_CASE(MultilineOffset) {
    llvm::StringRef src = "#include \"a.h\"\n#include \"b.h\"";
    // offset pointing into the second line
    auto pos = src.find("#include \"b.h\"");
    EXPECT_RANGE(src, static_cast<std::uint32_t>(pos), R"("b.h")");
}

TEST_CASE(EmptyDirective) {
    llvm::StringRef src = "#include \n";
    EXPECT_NONE(src, 0);
}

TEST_CASE(HasIncludeFromLineStart) {
    llvm::StringRef src = "#if __has_include(<vector>)";
    EXPECT_RANGE(src, 0, "<vector>");
}

TEST_CASE(HasEmbedFromLineStart) {
    llvm::StringRef src = R"(#if __has_embed("data.bin"))";
    EXPECT_RANGE(src, 0, R"("data.bin")");
}

TEST_CASE(IncludeNext) {
    llvm::StringRef src = "#include_next <stdlib.h>";
    EXPECT_RANGE(src, 0, "<stdlib.h>");
}

};  // TEST_SUITE(DirectiveArgument)

}  // namespace
}  // namespace clice::testing
