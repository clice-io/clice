
#include "Test/Test.h"
#include "Compiler/Command.h"
#include "Compiler/Compilation.h"

#include "clang/Driver/Driver.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Program.h"
#include "llvm/ADT/ScopeExit.h"

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

suite<"Command"> command = [] {
    test("GetOptionID") = [] {
        using option = clang::driver::options::ID;
        auto expect_id = [](llvm::StringRef command,
                            option opt,
                            std::source_location location = std::source_location::current()) {
            auto id = CompilationDatabase::get_option_id(command);
            fatal / expect(id, location);
            expect(eq(*id, int(opt)), location);
        };

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

    test("DefaultFilters") = [&] {
        auto expect_strip = [](llvm::StringRef argv, llvm::StringRef result) {
            CompilationDatabase database;
            llvm::StringRef file = "main.cpp";
            database.update_command("fake/", file, argv);

            CommandOptions options;
            options.suppress_logging = true;
            expect(eq(result, print_argv(database.get_command(file, options).arguments)));
        };

        /// Filter -c, -o and input file.
        expect_strip("g++ main.cc", "g++ main.cpp");
        expect_strip("clang++ -c main.cpp", "clang++ main.cpp");
        expect_strip("clang++ -o main.o main.cpp", "clang++ main.cpp");
        expect_strip("clang++ -c -o main.o main.cc", "clang++ main.cpp");
        expect_strip("cl.exe /c /Fomain.cpp.o main.cpp", "cl.exe main.cpp");

        /// Filter PCH related.

        /// CMake
        expect_strip(
            "g++ -std=gnu++20 -Winvalid-pch -include cmake_pch.hxx -o main.cpp.o -c main.cpp",
            "g++ -std=gnu++20 -Winvalid-pch -include cmake_pch.hxx main.cpp");
        expect_strip(
            "clang++ -Winvalid-pch -Xclang -include-pch -Xclang cmake_pch.hxx.pch -Xclang -include -Xclang cmake_pch.hxx -o main.cpp.o -c main.cpp",
            "clang++ -Winvalid-pch -Xclang -include -Xclang cmake_pch.hxx main.cpp");
        expect_strip("cl.exe /Yufoo.h /FIfoo.h /Fpfoo.h_v143.pch /c /Fomain.cpp.o main.cpp",
                     "cl.exe -include foo.h main.cpp");

        /// TODO: Test more commands from other build system.
    };

    test("Reuse") = [] {
        using namespace std::literals;

        CompilationDatabase database;
        database.update_command("fake", "test.cpp", "clang++ -std=c++23 test.cpp"sv);
        database.update_command("fake", "test2.cpp", "clang++ -std=c++23 test2.cpp"sv);

        CommandOptions options;
        options.suppress_logging = true;
        auto command1 = database.get_command("test.cpp", options).arguments;
        auto command2 = database.get_command("test2.cpp", options).arguments;
        expect(eq(command1.size(), 3));
        expect(eq(command2.size(), 3));

        expect(eq(command1[0], "clang++"sv));
        expect(eq(command1[1], "-std=c++23"sv));
        expect(eq(command1[2], "test.cpp"sv));

        expect(eq(command1[0], command2[0]));
        expect(eq(command1[1], command2[1]));
        expect(eq(command2[2], "test2.cpp"sv));
    };

    test("RemoveAppend") = [] {
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
        database.update_command("/fake", "main.cpp", args);

        CommandOptions options;

        llvm::SmallVector<std::string> remove;
        llvm::SmallVector<std::string> append;

        remove = {"-DA"};
        options.remove = remove;
        auto result = database.get_command("main.cpp", options).arguments;
        expect(eq(print_argv(result), "clang++ -D B=0 main.cpp"));

        remove = {"-D", "A"};
        options.remove = remove;
        result = database.get_command("main.cpp", options).arguments;
        expect(eq(print_argv(result), "clang++ -D B=0 main.cpp"));

        remove = {"-DA", "-D", "B=0"};
        options.remove = remove;
        result = database.get_command("main.cpp", options).arguments;
        expect(eq(print_argv(result), "clang++ main.cpp"));

        remove = {"-D*"};
        options.remove = remove;
        result = database.get_command("main.cpp", options).arguments;
        expect(eq(print_argv(result), "clang++ main.cpp"));

        remove = {"-D", "*"};
        options.remove = remove;
        result = database.get_command("main.cpp", options).arguments;
        expect(eq(print_argv(result), "clang++ main.cpp"));

        append = {"-D", "C"};
        options.append = append;
        result = database.get_command("main.cpp", options).arguments;
        expect(eq(print_argv(result), "clang++ -D C main.cpp"));
    };

    skip / test("Module") = [] {
        /// TODO:
        CompilationDatabase database;
        database.update_command("/fake",
                                "main.cpp",
                                llvm::StringRef("clang++ @test.txt -std= main.cpp"));
        auto info = database.get_command("main.cpp", {.query_driver = false});
    };

    skip_unless(CIEnvironment) / test("QueryDriver") = [] {
        CompilationDatabase database;
        auto info = database.query_driver("clang++");

        fatal / expect(info);
        expect(!info->target.empty());
        expect(!info->system_includes.empty());

        CompilationParams params;
        params.kind = CompilationUnit::Indexing;
        params.arguments = {
            "clang++",
            "-nostdlibinc",
            "--target",
            info->target.data(),
        };
        for(auto& include: info->system_includes) {
            params.arguments.push_back("-I");
            params.arguments.push_back(include);
        }
        params.arguments.push_back("main.cpp");

        llvm::StringRef hello_world = R"(
            #include <iostream>
            int main() {
                std::cout << "Hello world!" << std::endl;
                return 0;
            }
        )";
        params.add_remapped_file("main.cpp", hello_world);
        expect(compile(params));
    };

    test("ResourceDir") = [] {
        CompilationDatabase database;
        using namespace std::literals;
        database.update_command("/fake", "main.cpp", "clang++ -std=c++23 test.cpp"sv);
        auto arguments = database.get_command("main.cpp", {.resource_dir = true}).arguments;

        fatal / expect(eq(arguments.size(), 4));
        expect(eq(arguments[0], "clang++"sv));
        expect(eq(arguments[1], "-std=c++23"sv));
        expect(eq(arguments[2], std::format("-resource-dir={}", fs::resource_dir)));
        expect(eq(arguments[3], "main.cpp"sv));
    };

    auto expect_load = [](llvm::StringRef content,
                          llvm::StringRef workspace,
                          llvm::StringRef file,
                          llvm::StringRef directory,
                          llvm::ArrayRef<const char*> arguments) {
        CompilationDatabase database;
        auto loaded = database.load_commands(content, workspace);
        expect(loaded.has_value());

        CommandOptions options;
        options.suppress_logging = true;
        auto info = database.get_command(file, options);

        expect(info.directory == directory);
        expect(info.arguments.size() == arguments.size());
        for(size_t i = 0; i < arguments.size(); i++) {
            llvm::StringRef arg = info.arguments[i];
            llvm::StringRef expect_arg = arguments[i];
            expect(eq(arg, expect_arg));
        }
    };

    /// TODO: add windows path testcase
    skip_unless(Linux || MacOS) / test("LoadAbsoluteUnixStyle") = [expect_load] {
        constexpr const char* cmake = R"([
        {
            "directory": "/home/developer/clice/build",
            "command": "/usr/bin/c++ -I/home/developer/clice/include -I/home/developer/clice/build/_deps/libuv-src/include -isystem /home/developer/clice/build/_deps/tomlplusplus-src/include -std=gnu++23 -fno-rtti -fno-exceptions -Wno-deprecated-declarations -Wno-undefined-inline -O3 -o CMakeFiles/clice-core.dir/src/Driver/clice.cpp.o -c /home/developer/clice/src/Driver/clice.cpp",
            "file": "/home/developer/clice/src/Driver/clice.cpp",
            "output": "CMakeFiles/clice-core.dir/src/Driver/clice.cpp.o"
        }
        ])";

        expect_load(cmake,
                    "/home/developer/clice",
                    "/home/developer/clice/src/Driver/clice.cpp",
                    "/home/developer/clice/build",
                    {
                        "/usr/bin/c++",
                        "-I",
                        "/home/developer/clice/include",
                        "-I",
                        "/home/developer/clice/build/_deps/libuv-src/include",
                        "-isystem",
                        "/home/developer/clice/build/_deps/tomlplusplus-src/include",
                        "-std=gnu++23",
                        "-fno-rtti",
                        "-fno-exceptions",
                        "-Wno-deprecated-declarations",
                        "-Wno-undefined-inline",
                        "-O3",
                        "/home/developer/clice/src/Driver/clice.cpp",
                    });
    };

    skip_unless(Linux || MacOS) / test("LoadRelativeUnixStyle") = [expect_load] {
        constexpr const char* xmake = R"([
        {
            "directory": "/home/developer/clice",
            "arguments": ["/usr/bin/clang", "-c", "-Qunused-arguments", "-m64", "-g", "-O0", "-std=c++23", "-Iinclude", "-I/home/developer/clice/include", "-fno-exceptions", "-fno-cxx-exceptions", "-isystem", "/home/developer/.xmake/packages/l/libuv/v1.51.0/3ca1562e6c5d485f9ccafec8e0c50b6f/include", "-isystem", "/home/developer/.xmake/packages/t/toml++/v3.4.0/bde7344d843e41928b1d325fe55450e0/include", "-fsanitize=address", "-fno-rtti", "-o", "build/.objs/clice/linux/x86_64/debug/src/Driver/clice.cc.o", "src/Driver/clice.cc"],
            "file": "src/Driver/clice.cc"
        }
        ])";

        expect_load(
            xmake,
            "/home/developer/clice",
            "/home/developer/clice/src/Driver/clice.cc",
            "/home/developer/clice",
            {
                "/usr/bin/clang",
                "-Qunused-arguments",
                "-m64",
                "-g",
                "-O0",
                "-std=c++23",
                //  parameter "-Iinclude" in CDB, should be convert to absolute path
                "-I",
                "/home/developer/clice/include",
                "-I",
                "/home/developer/clice/include",
                "-fno-exceptions",
                "-fno-cxx-exceptions",
                "-isystem",
                "/home/developer/.xmake/packages/l/libuv/v1.51.0/3ca1562e6c5d485f9ccafec8e0c50b6f/include",
                "-isystem",
                "/home/developer/.xmake/packages/t/toml++/v3.4.0/bde7344d843e41928b1d325fe55450e0/include",
                "-fsanitize=address",
                "-fno-rtti",
                "/home/developer/clice/src/Driver/clice.cc",
            });
    };
};

}  // namespace

}  // namespace clice::testing
