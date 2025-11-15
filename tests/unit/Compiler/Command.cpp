
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
            expect(eq(result, print_argv(database.lookup(file, options).arguments)));
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
        auto command1 = database.lookup("test.cpp", options).arguments;
        auto command2 = database.lookup("test2.cpp", options).arguments;
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
        auto result = database.lookup("main.cpp", options).arguments;
        expect(eq(print_argv(result), "clang++ -D B=0 main.cpp"));

        remove = {"-D", "A"};
        options.remove = remove;
        result = database.lookup("main.cpp", options).arguments;
        expect(eq(print_argv(result), "clang++ -D B=0 main.cpp"));

        remove = {"-DA", "-D", "B=0"};
        options.remove = remove;
        result = database.lookup("main.cpp", options).arguments;
        expect(eq(print_argv(result), "clang++ main.cpp"));

        remove = {"-D*"};
        options.remove = remove;
        result = database.lookup("main.cpp", options).arguments;
        expect(eq(print_argv(result), "clang++ main.cpp"));

        remove = {"-D", "*"};
        options.remove = remove;
        result = database.lookup("main.cpp", options).arguments;
        expect(eq(print_argv(result), "clang++ main.cpp"));

        append = {"-D", "C"};
        options.append = append;
        result = database.lookup("main.cpp", options).arguments;
        expect(eq(print_argv(result), "clang++ -D C main.cpp"));
    };

    skip / test("Module") = [] {
        /// TODO:
        CompilationDatabase database;
        database.update_command("/fake",
                                "main.cpp",
                                llvm::StringRef("clang++ @test.txt -std= main.cpp"));
        auto info = database.lookup("main.cpp", {.query_driver = false});
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
        auto arguments = database.lookup("main.cpp", {.resource_dir = true}).arguments;

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
        auto info = database.lookup(file, options);

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

    test("Local") = [] {
        llvm::StringRef args =
            "/opt/zig/zig -cc1 -triple x86_64-unknown-linux5.10.0-gnu2.39.0 -O0 -E -disable-free -clear-ast-before-backend -disable-llvm-verifier -discard-value-names -main-file-name null -mrelocation-model pic -pic-level 2 -fhalf-no-semantic-interposition -mframe-pointer=all -fmath-errno -ffp-contract=on -fno-rounding-math -mconstructor-aliases -funwind-tables=2 -target-cpu x86-64 -tune-cpu generic -debug-info-kind=constructor -dwarf-version=4 -debugger-tuning=gdb -gdwarf32 -fdebug-compilation-dir=/home/ykiko/C++/clice -v -fcoverage-compilation-dir=/home/ykiko/C++/clice -nostdsysteminc -nobuiltininc -resource-dir /opt/lib/clang/21 -dependency-file /home/ykiko/.cache/zig/tmp/91495aeec7c7f40-null.o.d -MT null.o -sys-header-deps -MV -isystem /opt/zig/lib/libcxx/include -isystem /opt/zig/lib/libcxxabi/include -isystem /opt/zig/lib/include -isystem /usr/include -isystem /usr/include/x86_64-linux-gnu -isystem /opt/zig/lib/libunwind/include -D __GLIBC_MINOR__=39 -D _LIBCPP_ABI_VERSION=1 -D _LIBCPP_ABI_NAMESPACE=__1 -D _LIBCPP_HAS_THREADS=1 -D _LIBCPP_HAS_MONOTONIC_CLOCK -D _LIBCPP_HAS_TERMINAL -D _LIBCPP_HAS_MUSL_LIBC=0 -D _LIBCXXABI_DISABLE_VISIBILITY_ANNOTATIONS -D _LIBCPP_DISABLE_VISIBILITY_ANNOTATIONS -D _LIBCPP_HAS_VENDOR_AVAILABILITY_ANNOTATIONS=0 -D _LIBCPP_HAS_FILESYSTEM=1 -D _LIBCPP_HAS_RANDOM_DEVICE -D _LIBCPP_HAS_LOCALIZATION -D _LIBCPP_HAS_UNICODE -D _LIBCPP_HAS_WIDE_CHARACTERS -D _LIBCPP_HAS_NO_STD_MODULES -D _LIBCPP_HAS_TIME_ZONE_DATABASE -D _LIBCPP_PSTL_BACKEND_SERIAL -D _LIBCPP_HARDENING_MODE=_LIBCPP_HARDENING_MODE_NONE -fdeprecated-macro -ferror-limit 19 -stack-protector 2 -stack-protector-buffer-size 4 -fgnuc-version=4.2.1 -fskip-odr-check-in-gmf -fcxx-exceptions -fexceptions -fno-spell-checking -target-cpu alderlake -target-feature -16bit-mode -target-feature -32bit-mode -target-feature +64bit -target-feature +adx -target-feature +aes -target-feature +allow-light-256-bit -target-feature -amx-avx512 -target-feature -amx-bf16 -target-feature -amx-complex -target-feature -amx-fp16 -target-feature -amx-fp8 -target-feature -amx-int8 -target-feature -amx-movrs -target-feature -amx-tf32 -target-feature -amx-tile -target-feature -amx-transpose -target-feature +avx -target-feature -avx10.1-512 -target-feature -avx10.2-512 -target-feature +avx2 -target-feature -avx512bf16 -target-feature -avx512bitalg -target-feature -avx512bw -target-feature -avx512cd -target-feature -avx512dq -target-feature -avx512f -target-feature -avx512fp16 -target-feature -avx512ifma -target-feature -avx512vbmi -target-feature -avx512vbmi2 -target-feature -avx512vl -target-feature -avx512vnni -target-feature -avx512vp2intersect -target-feature -avx512vpopcntdq -target-feature -avxifma -target-feature -avxneconvert -target-feature +avxvnni -target-feature -avxvnniint16 -target-feature -avxvnniint8 -target-feature +bmi -target-feature +bmi2 -target-feature -branch-hint -target-feature -branchfusion -target-feature -ccmp -target-feature -cf -target-feature -cldemote -target-feature +clflushopt -target-feature +clwb -target-feature -clzero -target-feature +cmov -target-feature -cmpccxadd -target-feature +crc32 -target-feature +cx16 -target-feature +cx8 -target-feature -egpr -target-feature -enqcmd -target-feature -ermsb -target-feature -evex512 -target-feature +f16c -target-feature -false-deps-getmant -target-feature -false-deps-lzcnt-tzcnt -target-feature -false-deps-mulc -target-feature -false-deps-mullq -target-feature +false-deps-perm -target-feature +false-deps-popcnt -target-feature -false-deps-range -target-feature -fast-11bytenop -target-feature +fast-15bytenop -target-feature -fast-7bytenop -target-feature -fast-bextr -target-feature -fast-dpwssd -target-feature +fast-gather -target-feature -fast-hops -target-feature -fast-imm16 -target-feature -fast-lzcnt -target-feature -fast-movbe -target-feature +fast-scalar-fsqrt -target-feature -fast-scalar-shift-masks -target-feature +fast-shld-rotate -target-feature +fast-variable-crosslane-shuffle -target-feature +fast-variable-perlane-shuffle -target-feature +fast-vector-fsqrt -target-feature -fast-vector-shift-masks -target-feature -faster-shift-than-shuffle -target-feature +fma -target-feature -fma4 -target-feature +fsgsbase -target-feature -fsrm -target-feature +fxsr -target-feature +gfni -target-feature -harden-sls-ijmp -target-feature -harden-sls-ret -target-feature -hreset -target-feature -idivl-to-divb -target-feature +idivq-to-divl -target-feature -inline-asm-use-gpr32 -target-feature +invpcid -target-feature -kl -target-feature -lea-sp -target-feature -lea-uses-ag -target-feature -lvi-cfi -target-feature -lvi-load-hardening -target-feature -lwp -target-feature +lzcnt -target-feature +macrofusion -target-feature +mmx -target-feature +movbe -target-feature +movdir64b -target-feature +movdiri -target-feature -movrs -target-feature -mwaitx -target-feature -ndd -target-feature -nf -target-feature -no-bypass-delay -target-feature +no-bypass-delay-blend -target-feature +no-bypass-delay-mov -target-feature +no-bypass-delay-shuffle -target-feature +nopl -target-feature -pad-short-functions -target-feature +pclmul -target-feature -pconfig -target-feature -pku -target-feature +popcnt -target-feature -ppx -target-feature -prefer-128-bit -target-feature -prefer-256-bit -target-feature -prefer-mask-registers -target-feature +prefer-movmsk-over-vtest -target-feature -prefer-no-gather -target-feature -prefer-no-scatter -target-feature -prefetchi -target-feature +prfchw -target-feature -ptwrite -target-feature -push2pop2 -target-feature -raoint -target-feature +rdpid -target-feature -rdpru -target-feature +rdrnd -target-feature +rdseed -target-feature -retpoline -target-feature -retpoline-external-thunk -target-feature -retpoline-indirect-branches -target-feature -retpoline-indirect-calls -target-feature -rtm -target-feature +sahf -target-feature -sbb-dep-breaking -target-feature +serialize -target-feature -seses -target-feature -sgx -target-feature +sha -target-feature -sha512 -target-feature +shstk -target-feature +slow-3ops-lea -target-feature -slow-incdec -target-feature -slow-lea -target-feature -slow-pmaddwd -target-feature -slow-pmulld -target-feature -slow-shld -target-feature -slow-two-mem-ops -target-feature -slow-unaligned-mem-16 -target-feature -slow-unaligned-mem-32 -target-feature -sm3 -target-feature -sm4 -target-feature +sse -target-feature +sse2 -target-feature +sse3 -target-feature +sse4.1 -target-feature +sse4.2 -target-feature -sse4a -target-feature -sse-unaligned-mem -target-feature +ssse3 -target-feature -tagged-globals -target-feature -tbm -target-feature -tsxldtrk -target-feature +tuning-fast-imm-vector-shift -target-feature -uintr -target-feature -use-glm-div-sqrt-costs -target-feature -use-slm-arith-costs -target-feature -usermsr -target-feature +vaes -target-feature +vpclmulqdq -target-feature +vzeroupper -target-feature +waitpkg -target-feature -wbnoinvd -target-feature -widekl -target-feature +x87 -target-feature -xop -target-feature +xsave -target-feature +xsavec -target-feature +xsaveopt -target-feature +xsaves -target-feature -zu -fsanitize=alignment,array-bounds,bool,builtin,enum,float-cast-overflow,integer-divide-by-zero,nonnull-attribute,null,pointer-overflow,return,returns-nonnull-attribute,shift-base,shift-exponent,signed-integer-overflow,unreachable,vla-bound -fsanitize-recover=alignment,array-bounds,bool,builtin,enum,float-cast-overflow,integer-divide-by-zero,nonnull-attribute,null,pointer-overflow,returns-nonnull-attribute,shift-base,shift-exponent,signed-integer-overflow,vla-bound -fsanitize-merge=alignment,array-bounds,bool,builtin,enum,float-cast-overflow,integer-divide-by-zero,nonnull-attribute,null,pointer-overflow,return,returns-nonnull-attribute,shift-base,shift-exponent,signed-integer-overflow,unreachable,vla-bound -fno-sanitize-memory-param-retval -fno-sanitize-address-use-odr-indicator -faddrsig -D__GCC_HAVE_DWARF2_CFI_ASM=1 -o - -x c++ /dev/null";

        CompilationDatabase database;
        database.update_command("/fake", "main.cpp", args);
        auto info = database.lookup("main.cpp");
    };
};

}  // namespace

}  // namespace clice::testing
