module clice;

import :support.shell;
import :unittests.utils;

namespace clice::testing {
namespace {

ZEST_SUITE(Shell) {

ZEST_CASE(SingleQuotesLiteral, skip = Windows) {
    // sh keeps every backslash inside single quotes.
    ZEXPECT(split_command(R"(cc '-DS="a\\b"' x.c)") ==
            (std::vector<std::string>{"cc", R"(-DS="a\\b")", "x.c"}));
}

ZEST_CASE(EscapesOutsideSingleQuotes, skip = Windows) {
    ZEXPECT(split_command(R"(cc -DS=\"a\\b\" "-I a\"b"	x.c '')") ==
            (std::vector<std::string>{"cc", R"(-DS="a\b")", R"(-I a"b)", "x.c", ""}));
}

ZEST_CASE(BlanksBeforeProgram) {
    std::vector<std::string> expected = {"cc", "-c", "x.c"};
    ZEXPECT(split_command("  cc -c x.c", sys::cmd::Quoting::Windows) == expected);
    ZEXPECT(split_command("\tcc -c x.c", sys::cmd::Quoting::POSIX) == expected);
}

ZEST_CASE(PlainSplitsAlike) {
    /// A line quoting and escaping nothing splits at its spaces under every
    /// quoting; any other is no plain line.
    llvm::SmallVector<std::string_view> plain;
    for(auto line: {"  cc -c  x.c ", "/usr/bin/c++ -DA=1 -I/x -o a.o -c a.cpp"}) {
        ZASSERT(split_plain(line, plain));
        ZEXPECT(ranges::equal(plain, split_command(line, sys::cmd::Quoting::POSIX)));
        ZEXPECT(ranges::equal(plain, split_command(line, sys::cmd::Quoting::Windows)));
    }
    for(auto line: {R"(cc "-DA=1" x.c)", R"(cc -DA='x' x.c)", R"(cc C:\x.c)", "cc\t-c x.c"}) {
        ZEXPECT(!split_plain(line, plain));
    }
};

ZEST_CASE(JoinedReadsBack) {
    std::vector<std::string> arguments = {"cc", "-DS=\"a b\"", R"(-I C:\x)", "", "it's"};
    ZEXPECT(split_command(join_command(arguments)) == arguments);
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
