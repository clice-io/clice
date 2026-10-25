module;

#include "support/prelude.h"

module clice;

import :command.table;
import :command.toolchains.cl;
import :unittests.utils;
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

ZEST_CASE(CLStandardAndVersion, skip = Windows) {
    // `/std:c++latest` means the compiler's newest standard, not the linked
    // clang's: C++26 to cl 19.44, whose version the job takes too. cl hears
    // the standard alone: the command's other flags are spelled for clang.
    auto cl = fake_cl();
    auto argv = f.resolve(add("a.cpp", {cl.c_str(), "/std:c++latest", "/WX-", "/Wall"}));
    ZEXPECT(last_standard(argv) == "-std=c++26");
    ZEXPECT(contains(argv, "-fms-compatibility-version=19.44.35207"));
}

ZEST_CASE(CLKeepsClangSpellings, skip = Windows) {
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

ZEST_CASE(ClangCLOwnVersion, skip = Windows) {
    // clang-cl's version is the toolset its flags select, which the job
    // finds as clang-cl does; its standard is still its own to tell.
    auto clang_cl = fake_cl("clang-cl");
    auto argv = f.resolve(add("a.cpp", {clang_cl.c_str(), "/std:c++latest"}));
    ZEXPECT(last_standard(argv) == "-std=c++26");
    ZEXPECT(!contains(argv, "-fms-compatibility-version=19.44.35207"));
}

ZEST_CASE(ClangCLBuiltinHeaders, skip = Windows) {
    // clang-cl's own builtin headers, in the resource directory its job
    // names, stay right after clice's.
    tmp.touch("llvm/lib/clang/20/include/xray/xray_interface.h");
    auto clang_cl = fake_driver("clang-cl",
                                std::format(R"(case " $* " in *" -v "*)
  echo ' "{0}/bin/clang-cl" "-cc1" "-resource-dir" "{0}/lib/clang/20"' >&2 ;;
esac
echo 'msvc_lang 202002L'
echo 'msc_full_ver 194435207'
)",
                                            tmp.path("llvm")));
    auto argv = f.resolve(add("a.cpp", {clang_cl.c_str()}));
    auto builtin_headers = path::join(resource_dir(), "include");
    auto ours = ranges::find_if(argv, [&](llvm::StringRef arg) { return arg == builtin_headers; });
    ZASSERT(argv.end() - ours > 2);
    ZEXPECT(ours[1] == "-internal-isystem"sv);
    ZEXPECT(ours[2] == tmp.path("llvm/lib/clang/20/include"));
}

ZEST_CASE(UnqueriedCLRunsNothing, skip = Windows) {
    // Nor is cl asked its standard: clang's own cl mode reads the command.
    auto ran = tmp.path("ran");
    auto cl = fake_driver("cl", std::format("touch '{}'\n", ran));
    auto argv = f.resolve(unqueried(add("a.cpp", {cl.c_str(), "/std:c++17"})));
    ZEXPECT(contains(argv, "-std=c++17"));
    ZEXPECT(!fs::exists(ran).value_or(false));
}

ZEST_CASE(UnqueriedCLTargetFromName) {
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
