#include "test/test.h"
#include "command/argument_parser.h"
#include "command/command.h"

#include "clang/Driver/Options.h"

namespace clice::testing {

namespace {

TEST_SUITE(Command) {

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

void expect_strip(llvm::StringRef argv, llvm::StringRef result) {
    CompilationDatabase database;
    llvm::StringRef file = "main.cpp";
    database.add_command("fake/", file, argv);

    CommandOptions options;
    options.suppress_logging = true;
    ASSERT_EQ(result, print_argv(database.lookup(file, options).front().arguments));
};

TEST_CASE(DefaultFilters) {
    /// Filter -c, -o and input file.
    expect_strip("g++ main.cpp", "g++ main.cpp");
    expect_strip("clang++ -c main.cpp", "clang++ main.cpp");
    expect_strip("clang++ -o main.o main.cpp", "clang++ main.cpp");
    expect_strip("clang++ -c -o main.o main.cpp", "clang++ main.cpp");
    expect_strip("cl.exe /c /Fomain.cpp.o main.cpp", "cl.exe main.cpp");

    /// Filter PCH related.

    /// CMake
    expect_strip("g++ -std=gnu++20 -Winvalid-pch -include cmake_pch.hxx -o main.cpp.o -c main.cpp",
                 "g++ -std=gnu++20 -Winvalid-pch -include cmake_pch.hxx main.cpp");
    expect_strip(
        "clang++ -Winvalid-pch -Xclang -include-pch -Xclang cmake_pch.hxx.pch -Xclang -include -Xclang cmake_pch.hxx -o main.cpp.o -c main.cpp",
        "clang++ -Winvalid-pch -Xclang -include -Xclang cmake_pch.hxx main.cpp");
    expect_strip("cl.exe /Yufoo.h /FIfoo.h /Fpfoo.h_v143.pch /c /Fomain.cpp.o main.cpp",
                 "cl.exe -include foo.h main.cpp");

    /// TODO: Test more commands from other build system.
};

TEST_CASE(Reuse) {
    using namespace std::literals;

    CompilationDatabase database;
    database.add_command("fake", "test.cpp", "clang++ -std=c++23 test.cpp"sv);
    database.add_command("fake", "test2.cpp", "clang++ -std=c++23 test2.cpp"sv);

    CommandOptions options;
    options.suppress_logging = true;
    auto command1 = database.lookup("test.cpp", options).front().arguments;
    auto command2 = database.lookup("test2.cpp", options).front().arguments;
    ASSERT_EQ(command1.size(), 3U);
    ASSERT_EQ(command2.size(), 3U);

    ASSERT_EQ(command1[0], "clang++"sv);
    ASSERT_EQ(command1[1], "-std=c++23"sv);
    ASSERT_EQ(command1[2], "test.cpp"sv);

    ASSERT_EQ(command1[0], command2[0]);
    ASSERT_EQ(command1[1], command2[1]);
    ASSERT_EQ(command2[2], "test2.cpp"sv);
};

TEST_CASE(RemoveAppend) {
    llvm::SmallVector args = {
        "clang++",
        "--output=main.o",
        "-D",
        "A",
        "-D",
        "B=0",
        "main.cpp",
    };

    CompilationDatabase database;
    database.add_command("/fake", "main.cpp", args);

    CommandOptions options;

    llvm::SmallVector<std::string> remove;
    llvm::SmallVector<std::string> append;

    remove = {"-DA"};
    options.remove = remove;
    auto result = database.lookup("main.cpp", options).front().arguments;
    ASSERT_EQ(print_argv(result), "clang++ -D B=0 main.cpp");

    remove = {"-D", "A"};
    options.remove = remove;
    result = database.lookup("main.cpp", options).front().arguments;
    ASSERT_EQ(print_argv(result), "clang++ -D B=0 main.cpp");

    remove = {"-DA", "-D", "B=0"};
    options.remove = remove;
    result = database.lookup("main.cpp", options).front().arguments;
    ASSERT_EQ(print_argv(result), "clang++ main.cpp");

    remove = {"-D*"};
    options.remove = remove;
    result = database.lookup("main.cpp", options).front().arguments;
    ASSERT_EQ(print_argv(result), "clang++ main.cpp");

    remove = {"-D", "*"};
    options.remove = remove;
    result = database.lookup("main.cpp", options).front().arguments;
    ASSERT_EQ(print_argv(result), "clang++ main.cpp");

    append = {"-D", "C"};
    options.append = append;
    result = database.lookup("main.cpp", options).front().arguments;
    ASSERT_EQ(print_argv(result), "clang++ -D C main.cpp");
};

TEST_CASE(Module) {
    // TODO: revisit module command handling.
}

TEST_CASE(ResourceDir) {
    // When query_toolchain is enabled, resource dir is injected automatically.
    CompilationDatabase database;
    using namespace std::literals;
    database.add_command("/fake", "main.cpp", "clang++ -std=c++23 test.cpp"sv);

    // Without query_toolchain, no resource dir injection.
    auto args_no_tc = database.lookup("main.cpp").front().arguments;
    ASSERT_EQ(args_no_tc.size(), 3U);
    ASSERT_EQ(args_no_tc[0], "clang++"sv);
    ASSERT_EQ(args_no_tc[1], "-std=c++23"sv);
    ASSERT_EQ(args_no_tc[2], "main.cpp"sv);

    // With query_toolchain, resource dir is present in the result.
    auto args_tc = database.lookup("main.cpp", {.query_toolchain = true}).front().arguments;
    bool has_resource_dir = false;
    for(size_t i = 0; i + 1 < args_tc.size(); ++i) {
        if(args_tc[i] == llvm::StringRef("-resource-dir")) {
            EXPECT_EQ(llvm::StringRef(args_tc[i + 1]), resource_dir());
            has_resource_dir = true;
            break;
        }
    }
    EXPECT_TRUE(has_resource_dir);
};

};  // TEST_SUITE(Command)

}  // namespace

}  // namespace clice::testing
