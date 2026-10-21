module;

#include "modules/prelude.h"

module clice;

import :support.shell;
import :unittests.utils;

namespace clice::testing {
namespace {

ZEST_SUITE(Shell) {

std::vector<std::string> tokenize(llvm::StringRef command) {
    llvm::BumpPtrAllocator allocator;
    llvm::StringSaver saver(allocator);
    std::vector<std::string> arguments;
    for(const char* argument: tokenize_command(command, saver)) {
        arguments.emplace_back(argument);
    }
    return arguments;
}

ZEST_CASE(SingleQuotesLiteral, skip = Windows) {
    // sh keeps every backslash inside single quotes.
    ZEXPECT(tokenize(R"(cc '-DS="a\\b"' x.c)") ==
            (std::vector<std::string>{"cc", R"(-DS="a\\b")", "x.c"}));
}

ZEST_CASE(EscapesOutsideSingleQuotes, skip = Windows) {
    ZEXPECT(tokenize(R"(cc -DS=\"a\\b\" "-I a\"b"	x.c '')") ==
            (std::vector<std::string>{"cc", R"(-DS="a\b")", R"(-I a"b)", "x.c", ""}));
}

ZEST_CASE(JoinedReadsBack) {
    std::vector<std::string> arguments = {"cc", "-DS=\"a b\"", R"(-I C:\x)", "", "it's"};
    ZEXPECT(tokenize(join_command(arguments)) == arguments);
}

};  // ZEST_SUITE(Shell)

}  // namespace
}  // namespace clice::testing
