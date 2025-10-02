#include "Test/Test.h"
#include "Compiler/Command.h"
#include "clang/Driver/Driver.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Program.h"
#include "llvm/ADT/ScopeExit.h"

namespace clice::testing {

inline static auto& option_table = clang::driver::getDriverOptTable();

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
    auto expect_strip = [](llvm::StringRef argv, llvm::StringRef result) {
        CompilationDatabase database;
        llvm::StringRef file = "main.cpp";
        database.update_command("fake/", file, argv);

        CommandOptions options;
        options.suppress_logging = true;
        expect(eq(result, print_argv(database.get_command(file, options).arguments)));
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

    test("GetOptionID") = [] {
        auto GET_OPTION_ID = CompilationDatabase::get_option_id;
        namespace option = clang::driver::options;

        /// GroupClass
        expect(that % GET_OPTION_ID("-g") == option::OPT_g_Flag);

        /// InputClass
        expect(that % GET_OPTION_ID("main.cpp") == option::OPT_INPUT);

        /// UnknownClass
        expect(that % GET_OPTION_ID("--clice") == option::OPT_UNKNOWN);

        /// FlagClass
        expect(that % GET_OPTION_ID("-v") == option::OPT_v);
        expect(that % GET_OPTION_ID("-c") == option::OPT_c);
        expect(that % GET_OPTION_ID("-pedantic") == option::OPT_pedantic);
        expect(that % GET_OPTION_ID("--pedantic") == option::OPT_pedantic);

        /// JoinedClass
        expect(that % GET_OPTION_ID("-Wno-unused-variable") == option::OPT_W_Joined);
        expect(that % GET_OPTION_ID("-W*") == option::OPT_W_Joined);
        expect(that % GET_OPTION_ID("-W") == option::OPT_W_Joined);

        /// ValuesClass

        /// SeparateClass
        expect(that % GET_OPTION_ID("-Xclang") == option::OPT_Xclang);
        /// expect(that % GET_ID("-Xclang -ast-dump") == option::OPT_Xclang);

        /// RemainingArgsClass

        /// RemainingArgsJoinedClass

        /// CommaJoinedClass
        expect(that % GET_OPTION_ID("-Wl,") == option::OPT_Wl_COMMA);

        /// MultiArgClass

        /// JoinedOrSeparateClass
        expect(that % GET_OPTION_ID("-o") == option::OPT_o);
        expect(that % GET_OPTION_ID("-omain.o") == option::OPT_o);
        expect(that % GET_OPTION_ID("-I") == option::OPT_I);
        expect(that % GET_OPTION_ID("--include-directory=") == option::OPT_I);
        expect(that % GET_OPTION_ID("-x") == option::OPT_x);
        expect(that % GET_OPTION_ID("--language=") == option::OPT_x);

        /// JoinedAndSeparateClass
    };

    test("DefaultFilters") = [&] {
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
        expect(that % command1.size() == 3);
        expect(that % command2.size() == 3);

        expect(that % command1[0] == "clang++"sv);
        expect(that % command1[1] == "-std=c++23"sv);
        expect(that % command1[2] == "test.cpp"sv);

        expect(that % command1[0] == command2[0]);
        expect(that % command1[1] == command2[1]);
        expect(that % command2[2] == "test2.cpp"sv);
    };

    skip / test("Module") = [] {
        // Empty test
        CompilationDatabase database;
        database.update_command("/fake",
                                "main.cpp",
                                llvm::StringRef("clang++ @test.txt -std= main.cpp"));
        auto info = database.get_command("main.cpp", {.query_driver = false});
    };

    test("QueryDriver") = [] {
#ifdef _GLIBCXX_RELEASE
        using namespace std::literals;
        using ErrorKind = CompilationDatabase::QueryDriverError::ErrorKind;

        CompilationDatabase database;
        auto info = database.query_driver("g++");
        if(!info) {
            auto& err = info.error();
            /// If driver not installed or not found in PATH, skip the following test to avoid
            /// failures on developer's machine, but never skip the test in CI.
            if(err.kind == ErrorKind::NotFoundInPATH && !std::getenv("CI")) {
                return;
            }
        }

        expect(that % info);

#if defined CLICE_CI_ENVIRONMENT
        expect(that % info->target == llvm::StringRef("x86_64-linux-gnu"));
        expect(that % info->system_includes.size() == 6);

        if(_GLIBCXX_RELEASE == 13) {
            expect(that % info->system_includes[0] == "/usr/include/c++/13"sv);
            expect(that % info->system_includes[1] == "/usr/include/x86_64-linux-gnu/c++/13"sv);
            expect(that % info->system_includes[2] == "/usr/include/c++/13/backward"sv);
        } else if(_GLIBCXX_RELEASE == 14) {
            expect(that % info->system_includes[0] == "/usr/include/c++/14"sv);
            expect(that % info->system_includes[1] == "/usr/include/x86_64-linux-gnu/c++/14"sv);
            expect(that % info->system_includes[2] == "/usr/include/c++/14/backward"sv);
        }

        expect(that % info->system_includes[3] == "/usr/local/include"sv);
        expect(that % info->system_includes[4] == "/usr/include/x86_64-linux-gnu"sv);
        expect(that % info->system_includes[5] == "/usr/include"sv);

        info = database.query_driver("clang++");
        expect(that % info);
#endif

#endif
    };

    test("ResourceDir") = [] {
        /// CompilationDatabase database;
        /// database.update_command("test.cpp", "clang++ -std=c++23 test.cpp");
        /// auto command = database.get_command("test.cpp", false, true);
        /// expect(that % command.size() == 4);
        ///
        /// using namespace std::literals;
        /// expect(that % command[0] == "clang++"sv);
        /// expect(that % command[1] == "-std=c++23"sv);
        /// expect(that % command[2] == "test.cpp"sv);
        /// expect(that % command[3] == std::format("-resource-dir={}", fs::resource_dir));
    };

    auto expect_load = [](llvm::StringRef content,
                          llvm::StringRef workspace,
                          llvm::StringRef file,
                          llvm::StringRef directory,
                          llvm::ArrayRef<const char*> arguments) {
        CompilationDatabase database;
        auto loaded = database.load_commands(content, workspace);
        expect(that % loaded.has_value());

        CommandOptions options;
        options.suppress_logging = true;
        auto info = database.get_command(file, options);

        expect(that % info.directory == directory);
        expect(that % info.arguments.size() == arguments.size());
        for(size_t i = 0; i < arguments.size(); i++) {
            llvm::StringRef arg = info.arguments[i];
            llvm::StringRef expect_arg = arguments[i];
            // llvm::outs() << "arg: " << arg << ", expect: " << expect_arg << "\n";
            expect(that % arg == expect_arg);
        }
    };

#if defined(__unix__) || defined(__APPLE__)
    /// TODO: add windows path testcase
    test("LoadAbsoluteUnixStyle") = [expect_load] {
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

    test("LoadRelativeUnixStyle") = [expect_load] {
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
#endif
};

}  // namespace

}  // namespace clice::testing
