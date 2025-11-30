#include "Test/Test.h"
#include "Compiler/Command.h"
#include "Compiler/Compilation.h"

#include "llvm/ADT/ScopeExit.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Program.h"
#include "clang/Driver/Driver.h"

namespace clice::testing {

namespace {

std::string print_argv(llvm::ArrayRef<const char*> args) {
    std::string buf;
    llvm::raw_string_ostream os(buf);
    bool Sep = false;
    for(llvm::StringRef arg: args) {
        if(Sep)
            os << ' ';
        Sep = true;
        if(llvm::all_of(arg, llvm::isPrint) &&
           arg.find_first_of(" \t\n\"\\") == llvm::StringRef::npos) {
            os << arg;
            continue;
        }
        os << '"';
        os.write_escaped(arg, /*UseHexEscapes=*/true);
        os << '"';
    }
    return std::move(os.str());
}

TEST_SUITE(Command) {

using option = clang::driver::options::ID;

void expect_id(llvm::StringRef command, option opt) {
    auto id = CompilationDatabase::get_option_id(command);
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
    ASSERT_EQ(result, print_argv(database.lookup(file, options).arguments));
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
    auto command1 = database.lookup("test.cpp", options).arguments;
    auto command2 = database.lookup("test2.cpp", options).arguments;
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
    auto result = database.lookup("main.cpp", options).arguments;
    ASSERT_EQ(print_argv(result), "clang++ -D B=0 main.cpp");

    remove = {"-D", "A"};
    options.remove = remove;
    result = database.lookup("main.cpp", options).arguments;
    ASSERT_EQ(print_argv(result), "clang++ -D B=0 main.cpp");

    remove = {"-DA", "-D", "B=0"};
    options.remove = remove;
    result = database.lookup("main.cpp", options).arguments;
    ASSERT_EQ(print_argv(result), "clang++ main.cpp");

    remove = {"-D*"};
    options.remove = remove;
    result = database.lookup("main.cpp", options).arguments;
    ASSERT_EQ(print_argv(result), "clang++ main.cpp");

    remove = {"-D", "*"};
    options.remove = remove;
    result = database.lookup("main.cpp", options).arguments;
    ASSERT_EQ(print_argv(result), "clang++ main.cpp");

    append = {"-D", "C"};
    options.append = append;
    result = database.lookup("main.cpp", options).arguments;
    ASSERT_EQ(print_argv(result), "clang++ -D C main.cpp");
};

TEST_CASE(Module) {
    // TODO: revisit module command handling.
}

TEST_CASE(ResourceDir) {
    CompilationDatabase database;
    using namespace std::literals;
    database.add_command("/fake", "main.cpp", "clang++ -std=c++23 test.cpp"sv);
    auto arguments = database.lookup("main.cpp", {.resource_dir = true}).arguments;

    ASSERT_EQ(arguments.size(), 5U);
    ASSERT_EQ(arguments[0], "clang++"sv);
    ASSERT_EQ(arguments[1], "-std=c++23"sv);
    ASSERT_EQ(arguments[2], "-resource-dir"sv);
    ASSERT_EQ(arguments[3], fs::resource_dir);
    ASSERT_EQ(arguments[4], "main.cpp"sv);
};

void expect_load(llvm::StringRef content,
                 llvm::StringRef workspace,
                 llvm::StringRef file,
                 llvm::StringRef directory,
                 llvm::ArrayRef<const char*> arguments) {
    CompilationDatabase database;
    auto loaded = database.load_commands(content, workspace);
    ASSERT_TRUE(loaded.has_value());

    CommandOptions options;
    options.suppress_logging = true;
    auto info = database.lookup(file, options);

    ASSERT_EQ(info.directory, directory);
    ASSERT_EQ(info.arguments.size(), arguments.size());
    for(size_t i = 0; i < arguments.size(); i++) {
        llvm::StringRef arg = info.arguments[i];
        llvm::StringRef expect_arg = arguments[i];
        ASSERT_EQ(arg, expect_arg);
    }
};

/// TODO: add windows path testcase
// skip_unless(Linux || MacOS) / test("LoadAbsoluteUnixStyle") = [expect_load] {
//     constexpr const char* cmake = R"([
//     {
//         "directory": "/home/developer/clice/build",
//         "command": "/usr/bin/c++ -I/home/developer/clice/include
//         -I/home/developer/clice/build/_deps/libuv-src/include -isystem
//         /home/developer/clice/build/_deps/tomlplusplus-src/include -std=gnu++23 -fno-rtti
//         -fno-exceptions -Wno-deprecated-declarations -Wno-undefined-inline -O3 -o
//         CMakeFiles/clice-core.dir/src/Driver/clice.cpp.o -c
//         /home/developer/clice/src/Driver/clice.cpp", "file":
//         "/home/developer/clice/src/Driver/clice.cpp", "output":
//         "CMakeFiles/clice-core.dir/src/Driver/clice.cpp.o"
//     }
//     ])";
//
//     expect_load(cmake,
//                 "/home/developer/clice",
//                 "/home/developer/clice/src/Driver/clice.cpp",
//                 "/home/developer/clice/build",
//                 {
//                     "/usr/bin/c++",
//                     "-I",
//                     "/home/developer/clice/include",
//                     "-I",
//                     "/home/developer/clice/build/_deps/libuv-src/include",
//                     "-isystem",
//                     "/home/developer/clice/build/_deps/tomlplusplus-src/include",
//                     "-std=gnu++23",
//                     "-fno-rtti",
//                     "-fno-exceptions",
//                     "-Wno-deprecated-declarations",
//                     "-Wno-undefined-inline",
//                     "-O3",
//                     "/home/developer/clice/src/Driver/clice.cpp",
//                 });
// };

// skip_unless(Linux || MacOS) / test("LoadRelativeUnixStyle") = [expect_load] {
//     constexpr const char* xmake = R"([
//     {
//         "directory": "/home/developer/clice",
//         "arguments": ["/usr/bin/clang", "-c", "-Qunused-arguments", "-m64", "-g", "-O0",
//         "-std=c++23", "-Iinclude", "-I/home/developer/clice/include", "-fno-exceptions",
//         "-fno-cxx-exceptions", "-isystem",
//         "/home/developer/.xmake/packages/l/libuv/v1.51.0/3ca1562e6c5d485f9ccafec8e0c50b6f/include",
//         "-isystem",
//         "/home/developer/.xmake/packages/t/toml++/v3.4.0/bde7344d843e41928b1d325fe55450e0/include",
//         "-fsanitize=address", "-fno-rtti", "-o",
//         "build/.objs/clice/linux/x86_64/debug/src/Driver/clice.cc.o", "src/Driver/clice.cc"],
//         "file": "src/Driver/clice.cc"
//     }
//     ])";
//
//    expect_load(
//        xmake,
//        "/home/developer/clice",
//        "/home/developer/clice/src/Driver/clice.cc",
//        "/home/developer/clice",
//        {
//            "/usr/bin/clang",
//            "-Qunused-arguments",
//            "-m64",
//            "-g",
//            "-O0",
//            "-std=c++23",
//            //  parameter "-Iinclude" in CDB, should be convert to absolute path
//            "-I",
//            "/home/developer/clice/include",
//            "-I",
//            "/home/developer/clice/include",
//            "-fno-exceptions",
//            "-fno-cxx-exceptions",
//            "-isystem",
//            "/home/developer/.xmake/packages/l/libuv/v1.51.0/3ca1562e6c5d485f9ccafec8e0c50b6f/include",
//            "-isystem",
//            "/home/developer/.xmake/packages/t/toml++/v3.4.0/bde7344d843e41928b1d325fe55450e0/include",
//            "-fsanitize=address",
//            "-fno-rtti",
//            "/home/developer/clice/src/Driver/clice.cc",
//        });
//};

};  // TEST_SUITE(Command)

}  // namespace

}  // namespace clice::testing
