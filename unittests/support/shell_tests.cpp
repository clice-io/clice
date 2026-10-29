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

ZEST_CASE(PrintArgv) {
    std::vector<const char*> args = {"clang++", "-std=c++20", "main.cpp"};
    ZASSERT(print_argv(args) == "clang++ -std=c++20 main.cpp");

    std::vector<const char*> empty = {};
    ZASSERT(print_argv(empty) == "");

    std::vector<const char*> spaced = {"clang++", "-DFOO=hello world"};
    auto result = print_argv(spaced);
    ZEXPECT(llvm::StringRef(result).contains("\""));

    std::vector<const char*> escaped = {"clang++", "-DPATH=C:\\foo"};
    auto result2 = print_argv(escaped);
    ZEXPECT(llvm::StringRef(result2).contains("\""));

    std::vector<const char*> unicode = {"clang++", "-I/home/张三/头文件", "-DNAME=a b\tç"};
    ZEXPECT(print_argv(unicode) == R"(clang++ -I/home/张三/头文件 "-DNAME=a b\x09ç")");
}

};  // ZEST_SUITE(Shell)

}  // namespace
}  // namespace clice::testing
