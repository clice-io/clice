module;

#include "modules/prelude.h"

module clice;

import :command.driver_report;
import :unittests.utils;

namespace clice::testing {
namespace {

ZEST_SUITE(DriverReportTests) {

/// Ubuntu 24.04's g++ 13, as `g++ -E -dM -v -x c++ probe.cpp` prints it.
constexpr static llvm::StringRef ubuntu_gxx = R"(Using built-in specs.
COLLECT_GCC=g++
Target: x86_64-linux-gnu
Thread model: posix
gcc version 13.3.0 (Ubuntu 13.3.0-6ubuntu2~24.04.1)
 /usr/libexec/gcc/x86_64-linux-gnu/13/cc1plus -E -quiet -v -imultiarch x86_64-linux-gnu -D_GNU_SOURCE /tmp/probe.cpp -mtune=generic -march=x86-64 -fasynchronous-unwind-tables -fstack-protector-strong -Wformat -Wformat-security -fstack-clash-protection -fcf-protection -dM -dumpbase probe.cpp -dumpbase-ext .cpp
ignoring nonexistent directory "/usr/include/x86_64-linux-gnu/c++/13"
#include "..." search starts here:
#include <...> search starts here:
 /usr/include/c++/13
 /usr/include/c++/13/backward
 /usr/lib/gcc/x86_64-linux-gnu/13/include
 /usr/local/include
 /usr/include/x86_64-linux-gnu
 /usr/include
End of search list.
COMPILER_PATH=/usr/libexec/gcc/x86_64-linux-gnu/13/:/usr/libexec/gcc/x86_64-linux-gnu/
LIBRARY_PATH=/usr/lib/gcc/x86_64-linux-gnu/13/:/usr/lib/gcc/x86_64-linux-gnu/13/../../../x86_64-linux-gnu/:/lib/x86_64-linux-gnu/
COLLECT_GCC_OPTIONS='-E' '-dM' '-v' '-shared-libgcc' '-mtune=generic' '-march=x86-64')";

/// GCC's LIBRARY_PATH is a list in the host's spelling: a Linux GCC's
/// report never reaches clice on Windows, nor MinGW's elsewhere.
ZEST_CASE(GCCJob, skip = Windows) {
    auto report = parse_driver_report(R"(#define __cplusplus 201703L
#define __GNUC__ 13)",
                                      ubuntu_gxx);
    ZEXPECT(report.clang_job.empty());
    ZEXPECT(report.target == "x86_64-linux-gnu");

    std::vector<std::string> options = {"-D_GNU_SOURCE",
                                        "-mtune=generic",
                                        "-march=x86-64",
                                        "-fasynchronous-unwind-tables",
                                        "-fstack-protector-strong",
                                        "-fstack-clash-protection",
                                        "-fcf-protection"};
    ZEXPECT(report.gcc_options == options);

    ZASSERT(report.search_dirs.size() == std::size_t(6));
    ZEXPECT(report.search_dirs[1].path == "/usr/include/c++/13/backward");
    ZEXPECT(report.private_dir == std::optional<std::size_t>(2));
    ZEXPECT(report.standard == "gnu++17");
}

ZEST_CASE(MultilibPrivateDir, skip = Windows) {
    /// A multilib variant's library directory comes first in LIBRARY_PATH;
    /// the private include directory sits beside GCC's own.
    constexpr llvm::StringRef err = R"(Target: arm-none-eabi
 /usr/lib/gcc/arm-none-eabi/13.2.1/cc1 -E -isystem /usr/include/newlib/nano -quiet -v -imultilib thumb/v7e-m+fp/hard -D__USES_INITFINI__ /tmp/probe.c -mcpu=cortex-m4 -mthumb -mfloat-abi=hard -mfpu=fpv4-sp-d16 -mlibarch=armv7e-m+fp -march=armv7e-m+fp -dM
#include <...> search starts here:
 /usr/include/newlib/nano
 /usr/lib/gcc/arm-none-eabi/13.2.1/include
 /usr/lib/gcc/arm-none-eabi/13.2.1/include-fixed
 /usr/lib/gcc/arm-none-eabi/13.2.1/../../../arm-none-eabi/include
End of search list.
LIBRARY_PATH=/usr/lib/gcc/arm-none-eabi/13.2.1/thumb/v7e-m+fp/hard/:/usr/lib/gcc/arm-none-eabi/13.2.1/:/usr/lib/gcc/arm-none-eabi/13.2.1/../../../arm-none-eabi/lib/)";
    auto report = parse_driver_report("#define __STDC_VERSION__ 201710L", err);
    ZEXPECT(report.private_dir == std::optional<std::size_t>(1));
    ZEXPECT(llvm::is_contained(report.gcc_options, "-D__USES_INITFINI__"));
    ZEXPECT(llvm::is_contained(report.gcc_options, "-mthumb"));
    ZEXPECT(report.standard == "gnu17");
}

ZEST_CASE(MinGWPrivateDir, skip = !Windows) {
    /// Classic MinGW spells one directory two ways: with backslashes and
    /// `bin/..` in the search list, with forward slashes in LIBRARY_PATH.
    constexpr llvm::StringRef err = R"(Target: mingw32
 c:/mingw/bin/../libexec/gcc/mingw32/6.3.0/cc1plus.exe -E -quiet -v -iprefix c:\mingw\bin\../lib/gcc/mingw32/6.3.0/ probe.cpp -mtune=generic -march=i586 -dM
#include <...> search starts here:
 c:\mingw\bin\../lib/gcc/mingw32/6.3.0/include/c++
 c:\mingw\bin\../lib/gcc/mingw32/6.3.0/include
 c:\mingw\bin\../lib/gcc/mingw32/6.3.0/../../../../include
End of search list.
LIBRARY_PATH=c:/mingw/bin/../lib/gcc/mingw32/6.3.0/;c:/mingw/bin/../lib/gcc/;c:/mingw/bin/../lib/gcc/mingw32/6.3.0/../../../)";
    auto report = parse_driver_report("#define __cplusplus 201402L", err);
    std::vector<std::string> options = {"-mtune=generic", "-march=i586"};
    ZEXPECT(report.gcc_options == options);
    ZEXPECT(report.private_dir == std::optional<std::size_t>(1));
}

ZEST_CASE(FrameworkDir) {
    constexpr llvm::StringRef err = R"(Target: aarch64-apple-darwin24
#include <...> search starts here:
 /opt/homebrew/Cellar/gcc/15.1.0/lib/gcc/current/gcc/aarch64-apple-darwin24/15/include
 /Library/Developer/CommandLineTools/SDKs/MacOSX15.sdk/usr/include
 /Library/Developer/CommandLineTools/SDKs/MacOSX15.sdk/System/Library/Frameworks (framework directory)
End of search list.)";
    auto report = parse_driver_report("", err);
    ZASSERT(report.search_dirs.size() == std::size_t(3));
    ZEXPECT(!report.search_dirs[1].framework);
    ZEXPECT(report.search_dirs[2].framework);
    ZEXPECT(report.search_dirs[2].path ==
            "/Library/Developer/CommandLineTools/SDKs/MacOSX15.sdk/System/Library/Frameworks");
    /// No LIBRARY_PATH names a private directory, and no macros a standard.
    ZEXPECT(!report.private_dir);
    ZEXPECT(report.standard.empty());
}

ZEST_CASE(ClangJob) {
    /// `-v` prints the job unquoted but for the program; the probe's own
    /// preprocessing request does not survive into the job.
    constexpr llvm::StringRef err = R"(Apple clang version 17.0.0 (clang-1700.0.13.5)
Target: arm64-apple-darwin24.6.0
 "/Library/Developer/CommandLineTools/usr/bin/clang" -cc1 -triple arm64-apple-macosx15.0.0 -E -dM -v -isysroot /Library/Developer/CommandLineTools/SDKs/MacOSX.sdk -o - -x c++ /tmp/probe.cpp
clang -cc1 version 17.0.0 (clang-1700.0.13.5) default target arm64-apple-darwin24.6.0
#include <...> search starts here:
 /Library/Developer/CommandLineTools/SDKs/MacOSX.sdk/usr/include/c++/v1
End of search list.)";
    auto report = parse_driver_report("#define __cplusplus 201402L", err);
    std::vector<std::string> job = {"/Library/Developer/CommandLineTools/usr/bin/clang",
                                    "-cc1",
                                    "-triple",
                                    "arm64-apple-macosx15.0.0",
                                    "-fsyntax-only",
                                    "-isysroot",
                                    "/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk",
                                    "-x",
                                    "c++",
                                    "/tmp/probe.cpp"};
    ZEXPECT(report.clang_job == job);
    ZEXPECT(report.gcc_options.empty());
    ZEXPECT(report.standard == "gnu++14");
}

ZEST_CASE(StandardFromMacros) {
    auto standard = [](llvm::StringRef macros) {
        return parse_driver_report(macros, "").standard;
    };
    /// GCC 15's C default, and a value between two standards: GCC 13's
    /// -std=c++2b reports 202100L.
    ZEXPECT(standard("#define __STDC_VERSION__ 202311L") == "gnu23");
    ZEXPECT(standard("#define __cplusplus 202100L") == "gnu++23");
    ZEXPECT(standard("#define __cplusplus 199711L") == "gnu++98");
    ZEXPECT(standard(R"(#define __STRICT_ANSI__ 1
#define __cplusplus 201703L)") == "c++17");
    ZEXPECT(standard("#define __cplusplus 202400L") == "gnu++26");
    ZEXPECT(standard("#define __STDC_VERSION__ 202500L") == "gnu2y");
    /// Neither: an assembler input, which no -std fits.
    ZEXPECT(standard("#define __ASSEMBLER__ 1").empty());
    /// OpenCL C predefines C99's macros, but takes no -std.
    ZEXPECT(standard(R"(#define __OPENCL_C_VERSION__ 120
#define __STDC_VERSION__ 199901L
#define __STRICT_ANSI__ 1)")
                .empty());
}

};  // ZEST_SUITE(DriverReportTests)

}  // namespace
}  // namespace clice::testing
