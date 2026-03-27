#include "test/test.h"
#include "command/argument_parser.h"

#include "clang/Driver/Options.h"

namespace clice::testing {

namespace {

TEST_SUITE(ArgumentParser) {

using option = clang::driver::options::ID;

void expect_id(llvm::StringRef command, option opt) {
    auto id = get_option_id(command);
    ASSERT_TRUE(id.has_value());
    ASSERT_EQ(*id, int(opt));
}

TEST_CASE(GetOptionID) {
    /// GroupClass
    expect_id("-g", option::OPT_g_Flag);

    /// InputClass
    expect_id("main.cpp", option::OPT_INPUT);

    /// UnknownClass
    expect_id("--clice", option::OPT_UNKNOWN);

    /// FlagClass
    expect_id("-v", option::OPT_v);
    expect_id("-c", option::OPT_c);
    expect_id("-pedantic", option::OPT_pedantic);
    expect_id("--pedantic", option::OPT_pedantic);

    /// JoinedClass
    expect_id("-Wno-unused-variable", option::OPT_W_Joined);
    expect_id("-W*", option::OPT_W_Joined);
    expect_id("-W", option::OPT_W_Joined);

    /// ValuesClass

    /// SeparateClass
    expect_id("-Xclang", option::OPT_Xclang);
    /// expect_id(GET_ID("-Xclang -ast-dump") , option::OPT_Xclang);

    /// RemainingArgsClass

    /// RemainingArgsJoinedClass

    /// CommaJoinedClass
    expect_id("-Wl,", option::OPT_Wl_COMMA);

    /// MultiArgClass

    /// JoinedOrSeparateClass
    expect_id("-o", option::OPT_o);
    expect_id("-omain.o", option::OPT_o);
    expect_id("-I", option::OPT_I);
    expect_id("--include-directory=", option::OPT_I);
    expect_id("-x", option::OPT_x);
    expect_id("--language=", option::OPT_x);

    /// JoinedAndSeparateClass
};

TEST_CASE(PrintArgv) {
    /// Normal args.
    std::vector<const char*> args = {"clang++", "-std=c++20", "main.cpp"};
    ASSERT_EQ(print_argv(args), "clang++ -std=c++20 main.cpp");

    /// Empty args.
    std::vector<const char*> empty = {};
    ASSERT_EQ(print_argv(empty), "");

    /// Args with spaces get quoted.
    std::vector<const char*> spaced = {"clang++", "-DFOO=hello world"};
    auto result = print_argv(spaced);
    EXPECT_TRUE(llvm::StringRef(result).contains("\""));

    /// Args with backslash get quoted/escaped.
    std::vector<const char*> escaped = {"clang++", "-DPATH=C:\\foo"};
    auto result2 = print_argv(escaped);
    EXPECT_TRUE(llvm::StringRef(result2).contains("\""));
};

};  // TEST_SUITE(ArgumentParser)

}  // namespace

}  // namespace clice::testing
