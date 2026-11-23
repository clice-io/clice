module clice;

import :command.table;
import :command.toolchains.cl;
import :unittests.utils;
import :unittests.utils.platform;
import :unittests.utils.toolchain_fixture;
import :vfs.file_system;
import :vfs.path;

namespace clice::testing {

namespace {

using namespace std::literals;

ZEST_SUITE(CLTests, ToolchainFixture) {

ZEST_CASE(CLOutput) {
    auto output = parse_cl_output("msvc_lang 202400L\r\nmsc_full_ver 194435207\r\n");
    ZEXPECT(output.standard == "c++26");
    ZEXPECT(output.version == "19.44.35207");
    ZEXPECT(parse_cl_output("msvc_lang 202700L").standard == "c++2d");

    /// A C input has no `_MSVC_LANG`: the name stays as written.
    auto c = parse_cl_output("msvc_lang _MSVC_LANG\nmsc_full_ver 192930133\n");
    ZEXPECT(c.standard.empty());
    ZEXPECT(c.version == "19.29.30133");
}

/// A cl answering the macro query as cl 19.44 does, and rejecting a flag
/// spelled for clang's cl mode as cl does (`/WX` renders as -Werror: D8021).
std::string fake_cl(llvm::StringRef name = "cl") {
    return fake_driver(name, R"(case " $* " in *" -W"*|*" /clang:"*)
  echo "cl : Command line error D8021 : invalid numeric argument '/Werror'"
  exit 2 ;;
esac
case " $* " in
*" /std:c++17 "*) echo 'msvc_lang 201703L' ;;
*) echo 'msvc_lang 202400L' ;;
esac
echo 'msc_full_ver 194435207'
)");
}

/// The standard the cc1 command compiles in: its last -std.
llvm::StringRef last_standard(llvm::ArrayRef<const char*> argv) {
    for(llvm::StringRef arg: views::reverse(argv)) {
        if(arg.starts_with("-std=")) {
            return arg;
        }
    }
    return {};
}

ZEST_CASE(CLStandardAndVersion) {
    // `/std:c++latest` means the compiler's newest standard, not the linked
    // clang's: C++26 to cl 19.44, whose version the job takes too. cl hears
    // the standard alone: the command's other flags are spelled for clang.
    auto cl = fake_cl();
    auto argv = f.resolve(add("a.cpp", {cl.c_str(), "/std:c++latest", "/WX-", "/Wall"}));
    ZEXPECT(last_standard(argv) == "-std=c++26");
    ZEXPECT(contains(argv, "-fms-compatibility-version=19.44.35207"));
}

ZEST_CASE(CLKeepsClangSpellings) {
    // A standard and a version set in clang's spelling stay clang's reading.
    auto cl = fake_cl();
    auto argv = f.resolve(add("a.cpp",
                              {cl.c_str(),
                               "/std:c++17",
                               "/clang:-std=c++23",
                               "/clang:-fms-compatibility-version=19.29"}));
    ZEXPECT(last_standard(argv) == "-std=c++23");
    ZEXPECT(!contains(argv, "-fms-compatibility-version=19.44.35207"));
}

ZEST_CASE(CLWarningsStayWarnings) {
    // clang warns of what cl does not: cl's /WX makes no errors of those.
    // clang-cl's stays.
    auto cl = fake_cl();
    ZEXPECT(!contains(f.resolve(add("a.cpp", {cl.c_str(), "/WX"})), "-Werror"));
    auto clang_cl = fake_cl("clang-cl");
    ZEXPECT(contains(f.resolve(add("b.cpp", {clang_cl.c_str(), "/WX"})), "-Werror"));
}

ZEST_CASE(CLRuntimeChecks) {
    // cl defines __MSVC_RUNTIME_CHECKS for its runtime checks; clang-cl
    // ignores /RTC and defines nothing.
    auto cl = fake_cl();
    auto argv = f.resolve(add("a.cpp", {cl.c_str(), "/RTC1"}));
    ZEXPECT(adjacent(argv, "-D", "__MSVC_RUNTIME_CHECKS"));
    auto clang_cl = fake_cl("clang-cl");
    argv = f.resolve(add("b.cpp", {clang_cl.c_str(), "/RTC1"}));
    ZEXPECT(!adjacent(argv, "-D", "__MSVC_RUNTIME_CHECKS"));
}

ZEST_CASE(CLToolsetHeaders) {
    // A cl.exe compiles with the headers of its own toolset, not the
    // newest one installed, outside a developer prompt.
    ScopedEnv outside_prompt("INCLUDE", std::nullopt);
    auto toolset = tmp.path("MSVC/14.29.30133");
    auto cl = path::join(toolset, "bin", "Hostx64", "x64", "cl.exe");
    auto argv = f.resolve(unqueried(add("a.cpp", {cl.c_str(), "/c"})));
    ZEXPECT(adjacent(argv, "-internal-isystem", path::join(toolset, "include")));
}

ZEST_CASE(CLPromptHeaders) {
    // Inside a developer prompt, the INCLUDE it sets lists the build's
    // headers.
    auto headers = tmp.path("prompt/include");
    ScopedEnv inside_prompt("INCLUDE", headers);
    auto toolset = tmp.path("MSVC/14.29.30133");
    auto cl = path::join(toolset, "bin", "Hostx64", "x64", "cl.exe");
    auto argv = f.resolve(unqueried(add("a.cpp", {cl.c_str(), "/c"})));
    ZEXPECT(adjacent(argv, "-internal-isystem", headers));
    ZEXPECT(!adjacent(argv, "-internal-isystem", path::join(toolset, "include")));
}

ZEST_CASE(CLSkipsClangConfig) {
    // The configuration file of a clang-cl installed beside a cl configures
    // that clang-cl alone: clang's driver reading cl's command takes none
    // of it.
    tmp.touch("clang-cl.cfg", "-DFROM_CLANG_CFG\n");
    auto configured = [](llvm::ArrayRef<const char*> argv) {
        return ranges::any_of(argv,
                              [](llvm::StringRef arg) { return arg.contains("FROM_CLANG_CFG"); });
    };
    auto cl = fake_cl();
    ZEXPECT(!configured(f.resolve(add("a.cpp", {cl.c_str(), "/c"}))));
    auto clang_cl = fake_cl("clang-cl");
    ZEXPECT(configured(f.resolve(add("b.cpp", {clang_cl.c_str(), "/c"}))));
}

ZEST_CASE(ClangCLOwnVersion) {
    // clang-cl's version is the toolset its flags select, which the job
    // finds as clang-cl does; its standard is still its own to tell.
    auto clang_cl = fake_cl("clang-cl");
    auto argv = f.resolve(add("a.cpp", {clang_cl.c_str(), "/std:c++latest"}));
    ZEXPECT(last_standard(argv) == "-std=c++26");
    ZEXPECT(!contains(argv, "-fms-compatibility-version=19.44.35207"));
}

ZEST_CASE(ClangCLBuiltinHeaders) {
    // clang-cl's own builtin headers, in the resource directory its job
    // names, stay right after clice's.
    tmp.touch("llvm/lib/clang/20/include/xray/xray_interface.h");
    auto job =
        clang_job_of(std::format(R"("-cc1" "-resource-dir" "{}")", tmp.path("llvm/lib/clang/20")),
                     tmp.path("llvm/bin/clang-cl"));
    auto clang_cl = fake_driver("clang-cl",
                                std::format(R"(case " $* " in *" -v "*)
  echo '{}' >&2 ;;
esac
echo 'msvc_lang 202002L'
echo 'msc_full_ver 194435207'
)",
                                            job));
    auto argv = f.resolve(add("a.cpp", {clang_cl.c_str()}));
    auto builtin_headers = path::join(resource_dir(), "include");
    auto ours = ranges::find_if(argv, [&](llvm::StringRef arg) { return arg == builtin_headers; });
    ZASSERT(argv.end() - ours > 2);
    ZEXPECT(ours[1] == "-internal-isystem"sv);
    ZEXPECT(ours[2] == tmp.path("llvm/lib/clang/20/include"));
}

ZEST_CASE(ClangCLAskedOncePerQuestion) {
    // clang-cl is asked a command's standard alone: commands that differ in
    // anything else, the directories each names for its headers, share one
    // run of it, and each one's job takes its own directories.
    auto clang_cl = fake_driver("clang-cl",
                                std::format(R"(echo run >> '{}'
case " $* " in
*" /std:c++17 "*) echo 'msvc_lang 201703L' ;;
*) echo 'msvc_lang 202400L' ;;
esac
)",
                                            tmp.path("runs")));
    auto a = add("a.cpp", {clang_cl.c_str(), "/imsvc", tmp.c_path("a")});
    auto b = add("b.cpp", {clang_cl.c_str(), "/imsvc", tmp.c_path("b")});
    auto c = add("c.cpp", {clang_cl.c_str(), "/std:c++17", "/imsvc", tmp.c_path("a")});
    llvm::SmallVector<CommandRef> refs = {a, b, c};
    f.toolchain.warm(refs);
    auto runs = vfs::read_text(tmp.path("runs"));
    ZASSERT(runs);
    ZEXPECT(*runs == "run\nrun\n");

    auto dir_a = Spelling::absolute(tmp.path("a")).str();
    auto dir_b = Spelling::absolute(tmp.path("b")).str();
    auto argv = f.resolve(a);
    ZEXPECT(adjacent(argv, "-internal-isystem", dir_a));
    ZEXPECT(!adjacent(argv, "-internal-isystem", dir_b));
    ZEXPECT(last_standard(argv) == "-std=c++26");
    argv = f.resolve(b);
    ZEXPECT(adjacent(argv, "-internal-isystem", dir_b));
    ZEXPECT(!adjacent(argv, "-internal-isystem", dir_a));
    ZEXPECT(last_standard(f.resolve(c)) == "-std=c++17");
}

ZEST_CASE(HungClangCLStopsItsQuestion) {
    // The bound that stops a clang-cl that never exits fails every query
    // asking what it was asked at once: none waits out a bound of its own.
    ScopedEnv bound("CLICE_TEST_QUERY_BOUND_MS", "1000");
    auto clang_cl = fake_driver("clang-cl", "while :; do :; done\n");
    llvm::SmallVector<CommandRef> refs;
    for(unsigned i = 0; i < 8 * sys::parallelism(); i += 1) {
        auto headers = tmp.path(std::to_string(i));
        refs.push_back(
            add(std::format("{}.cpp", i), {clang_cl.c_str(), "/imsvc", headers.c_str()}));
    }
    auto start = chrono::steady_clock::now();
    f.toolchain.warm(refs);
    ZEXPECT(chrono::steady_clock::now() - start < chrono::seconds(5));
    for(auto& ref: refs) {
        ZEXPECT(llvm::StringRef(f.failure(ref))
                    .ends_with(" did not finish within 1000 ms and was stopped"));
    }
}

ZEST_CASE(IntelCLHostJob) {
    // icx-cl takes cl's options, and its SYCL compilation reads as the
    // host job, built after the device's.
    auto icx = fake_cl("icx-cl");
    auto argv = f.resolve(add("a.cpp", {icx.c_str(), "/DWIN32", "/EHsc", "-fsycl", "/c"}));
    ZEXPECT(contains(argv, "-fsycl-is-host"));
    ZEXPECT(!contains(argv, "-fsycl-is-device"));
    ZEXPECT(adjacent(argv, "-D", "WIN32"));
    ZEXPECT(contains(argv, "-fcxx-exceptions"));
}

ZEST_CASE(UnqueriedCLRunsNothing) {
    // Nor is cl asked its standard: clang's own cl mode reads the command.
    auto ran = tmp.path("ran");
    auto cl = fake_driver("cl", std::format("touch '{}'\n", ran));
    auto argv = f.resolve(unqueried(add("a.cpp", {cl.c_str(), "/std:c++17"})));
    ZEXPECT(contains(argv, "-std=c++17"));
    ZEXPECT(!fs::exists(ran));
}

ZEST_CASE(UnqueriedTargetFromName) {
    // A clang-cl that is not here reads for the target its name sets, as
    // any other unqueried driver does.
    auto cl = tmp.path("aarch64-pc-windows-msvc-clang-cl");
    auto argv = f.resolve(unqueried(add("a.cpp", {cl.c_str(), "/c"})));
    auto triple = ranges::find_if(argv, [](llvm::StringRef arg) { return arg == "-triple"; });
    ZASSERT(triple != argv.end());
    ZASSERT(triple + 1 != argv.end());
    ZEXPECT(llvm::StringRef(*(triple + 1)).starts_with("aarch64-pc-windows-msvc"));
}

};  // ZEST_SUITE(CLTests)

}  // namespace

}  // namespace clice::testing
