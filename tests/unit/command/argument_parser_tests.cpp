#include "test/test.h"
#include "command/argument_parser.h"

#include "clang/Options/Options.h"

namespace clice::testing {

namespace {

TEST_SUITE(ArgumentParser) {

using option = clang::options::ID;

void EXPECT_ID(llvm::StringRef command, option opt) {
    auto id = get_option_id(command);
    ASSERT_TRUE(id.has_value());
    ASSERT_EQ(*id, int(opt));
}

TEST_CASE(GetOptionID) {
    /// GroupClass
    EXPECT_ID("-g", option::OPT_g_Flag);

    /// InputClass
    EXPECT_ID("main.cpp", option::OPT_INPUT);

    /// UnknownClass
    EXPECT_ID("--clice", option::OPT_UNKNOWN);

    /// FlagClass
    EXPECT_ID("-v", option::OPT_v);
    EXPECT_ID("-c", option::OPT_c);
    EXPECT_ID("-pedantic", option::OPT_pedantic);
    EXPECT_ID("--pedantic", option::OPT_pedantic);

    /// JoinedClass
    EXPECT_ID("-Wno-unused-variable", option::OPT_W_Joined);
    EXPECT_ID("-W*", option::OPT_W_Joined);
    EXPECT_ID("-W", option::OPT_W_Joined);

    /// ValuesClass

    /// SeparateClass
    EXPECT_ID("-Xclang", option::OPT_Xclang);
    /// EXPECT_ID(GET_ID("-Xclang -ast-dump") , option::OPT_Xclang);

    /// RemainingArgsClass

    /// RemainingArgsJoinedClass

    /// CommaJoinedClass
    EXPECT_ID("-Wl,", option::OPT_Wl_COMMA);

    /// MultiArgClass

    /// JoinedOrSeparateClass
    EXPECT_ID("-o", option::OPT_o);
    EXPECT_ID("-omain.o", option::OPT_o);
    EXPECT_ID("-I", option::OPT_I);
    EXPECT_ID("--include-directory=", option::OPT_I);
    EXPECT_ID("-x", option::OPT_x);
    EXPECT_ID("--language=", option::OPT_x);

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
