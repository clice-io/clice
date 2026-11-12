module clice;

import :command.argument;
import :command.normalize;
import :command.table;
import :vfs.file_table;
import :unittests.utils;

namespace clice::testing {
namespace {

ZEST_SUITE(SplitCommand) {

ZEST_CASE(SingleQuotesLiteral) {
    // sh keeps every backslash inside single quotes.
    ZEXPECT(split_command(R"(cc '-DS="a\\b"' x.c)", sys::cmd::Quoting::POSIX) ==
            (std::vector<std::string>{"cc", R"(-DS="a\\b")", "x.c"}));
}

ZEST_CASE(EscapesOutsideSingleQuotes) {
    ZEXPECT(split_command(R"(cc -DS=\"a\\b\" "-I a\"b"	x.c '')", sys::cmd::Quoting::POSIX) ==
            (std::vector<std::string>{"cc", R"(-DS="a\b")", R"(-I a"b)", "x.c", ""}));
}

ZEST_CASE(BlanksBeforeProgram) {
    std::vector<std::string> expected = {"cc", "-c", "x.c"};
    ZEXPECT(split_command("  cc -c x.c", sys::cmd::Quoting::Windows) == expected);
    ZEXPECT(split_command("\tcc -c x.c", sys::cmd::Quoting::POSIX) == expected);
}

ZEST_CASE(DatabaseViews) {
    /// A database command with nothing to unquote is read as views of its
    /// bytes, splitting as unquoting it would; any other is unquoted.
    FileTable files;
    CommandTable commands;
    Normalizer normalizer{.commands = commands, .files = files};
    llvm::SmallVector<std::string_view> views;
    std::vector<std::string> tokens;
    for(llvm::StringRef line: {"  cc -c  x.c ", "/usr/bin/c++ -DA=1 -I/x -o a.o -c a.cpp"}) {
        auto read = normalizer.split(line, views, tokens);
        ZEXPECT(read.size() == views.size());
        ZEXPECT(ranges::equal(read, split_command(line)));
    }
    llvm::StringRef quoted = R"(cc "-DA=1" x.c)";
    auto read = normalizer.split(quoted, views, tokens);
    ZEXPECT(read.size() == tokens.size());
    ZEXPECT(tokens == split_command(quoted));
};

ZEST_CASE(JoinedReadsBack) {
    std::vector<std::string> arguments = {"cc", "-DS=\"a b\"", R"(-I C:\x)", "", "it's"};
    ZEXPECT(split_command(join_command(arguments)) == arguments);
}

ZEST_CASE(JoinedPlain) {
    std::vector<const char*> args = {"clang++", "-std=c++20", "main.cpp"};
    ZEXPECT(join_command(args) == "clang++ -std=c++20 main.cpp");
    ZEXPECT(join_command(std::vector<const char*>{}) == "");
    std::vector<std::string> unicode = {"clang++", "-I/home/张三/头文件", "-DNAME=a b\tç"};
    ZEXPECT(split_command(join_command(unicode)) == unicode);
}

};  // ZEST_SUITE(SplitCommand)

}  // namespace
}  // namespace clice::testing
