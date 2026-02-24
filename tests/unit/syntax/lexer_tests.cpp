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
}  // namespace
}  // namespace clice::testing
