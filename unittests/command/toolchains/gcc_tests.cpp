module;

#include "support/prelude.h"

module clice;

import :command.edit;
import :command.table;
import :command.toolchains.gcc;
import :compile.compilation;
import :syntax.include_resolver;
import :unittests.utils;
import :unittests.utils.toolchain_fixture;
import :vfs.dir_cache;
import :vfs.file_system;
import :vfs.path;

namespace clice::testing {

namespace {

using namespace std::literals;

ZEST_SUITE(GCCTests, ToolchainFixture) {

/// Ubuntu 24.04's g++ 13, as `g++ -E -dM -v -x c++ query.cpp` prints it.
constexpr static llvm::StringRef ubuntu_gxx = R"(Using built-in specs.
COLLECT_GCC=g++
Target: x86_64-linux-gnu
Thread model: posix
gcc version 13.3.0 (Ubuntu 13.3.0-6ubuntu2~24.04.1)
 /usr/libexec/gcc/x86_64-linux-gnu/13/cc1plus -E -quiet -v -imultiarch x86_64-linux-gnu -D_GNU_SOURCE /tmp/query.cpp -mtune=generic -march=x86-64 -fasynchronous-unwind-tables -fstack-protector-strong -Wformat -Wformat-security -fstack-clash-protection -fcf-protection -dM -dumpbase query.cpp -dumpbase-ext .cpp
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

ZEST_CASE(GCCJob) {
    auto output = parse_gcc_style_output(R"(#define __cplusplus 201703L
#define __GNUC__ 13)",
                                         ubuntu_gxx);
    ZEXPECT(output.clang_job.empty());
    ZEXPECT(output.target == "x86_64-linux-gnu");

    std::vector<std::string> options = {"-D_GNU_SOURCE",
                                        "-mtune=generic",
                                        "-march=x86-64",
                                        "-fasynchronous-unwind-tables",
                                        "-fstack-protector-strong",
                                        "-fstack-clash-protection",
                                        "-fcf-protection"};
    ZEXPECT(output.gcc_options == options);

    ZASSERT(output.search_dirs.size() == std::size_t(6));
    ZEXPECT(output.search_dirs[1].path == "/usr/include/c++/13/backward");
    ZEXPECT(output.private_dir == std::optional<std::size_t>(2));
    ZEXPECT(output.standard == "gnu++17");
}

ZEST_CASE(MultilibPrivateDir) {
    /// A multilib variant's library directory comes first in LIBRARY_PATH;
    /// the private include directory sits beside GCC's own.
    constexpr llvm::StringRef err = R"(Target: arm-none-eabi
 /usr/lib/gcc/arm-none-eabi/13.2.1/cc1 -E -isystem /usr/include/newlib/nano -quiet -v -imultilib thumb/v7e-m+fp/hard -D__USES_INITFINI__ /tmp/query.c -mcpu=cortex-m4 -mthumb -mfloat-abi=hard -mfpu=fpv4-sp-d16 -mlibarch=armv7e-m+fp -march=armv7e-m+fp -dM
#include <...> search starts here:
 /usr/include/newlib/nano
 /usr/lib/gcc/arm-none-eabi/13.2.1/include
 /usr/lib/gcc/arm-none-eabi/13.2.1/include-fixed
 /usr/lib/gcc/arm-none-eabi/13.2.1/../../../arm-none-eabi/include
End of search list.
LIBRARY_PATH=/usr/lib/gcc/arm-none-eabi/13.2.1/thumb/v7e-m+fp/hard/:/usr/lib/gcc/arm-none-eabi/13.2.1/:/usr/lib/gcc/arm-none-eabi/13.2.1/../../../arm-none-eabi/lib/)";
    auto output = parse_gcc_style_output("#define __STDC_VERSION__ 201710L", err);
    ZEXPECT(output.private_dir == std::optional<std::size_t>(1));
    ZEXPECT(ranges::contains(output.gcc_options, "-D__USES_INITFINI__"));
    ZEXPECT(ranges::contains(output.gcc_options, "-mthumb"));
    ZEXPECT(output.standard == "gnu17");
}

/// MinGW's LIBRARY_PATH separates Windows paths with `;`, which reach
/// clice on Windows alone.
ZEST_CASE(MinGWPrivateDir, skip = !Windows) {
    /// Classic MinGW spells one directory two ways: with backslashes and
    /// `bin/..` in the search list, with forward slashes in LIBRARY_PATH.
    constexpr llvm::StringRef err = R"(Target: mingw32
 c:/mingw/bin/../libexec/gcc/mingw32/6.3.0/cc1plus.exe -E -quiet -v -iprefix c:\mingw\bin\../lib/gcc/mingw32/6.3.0/ query.cpp -mtune=generic -march=i586 -dM
#include <...> search starts here:
 c:\mingw\bin\../lib/gcc/mingw32/6.3.0/include/c++
 c:\mingw\bin\../lib/gcc/mingw32/6.3.0/include
 c:\mingw\bin\../lib/gcc/mingw32/6.3.0/../../../../include
End of search list.
LIBRARY_PATH=c:/mingw/bin/../lib/gcc/mingw32/6.3.0/;c:/mingw/bin/../lib/gcc/;c:/mingw/bin/../lib/gcc/mingw32/6.3.0/../../../)";
    auto output = parse_gcc_style_output("#define __cplusplus 201402L", err);
    std::vector<std::string> options = {"-mtune=generic", "-march=i586"};
    ZEXPECT(output.gcc_options == options);
    ZEXPECT(output.private_dir == std::optional<std::size_t>(1));
}

ZEST_CASE(CygwinPrivateDir) {
    /// Cygwin's GCC on Windows lists POSIX paths, separated as on POSIX.
    constexpr llvm::StringRef err = R"(Target: x86_64-pc-cygwin
 /usr/lib/gcc/x86_64-pc-cygwin/14/cc1.exe -E -quiet -v -idirafter /usr/lib/gcc/x86_64-pc-cygwin/14/../../../../include/w32api C:\andes\empty.c -mtune=generic -march=x86-64 -dM
#include <...> search starts here:
 /usr/lib/gcc/x86_64-pc-cygwin/14/include
 /usr/include
 /usr/lib/gcc/x86_64-pc-cygwin/14/../../../../include/w32api
End of search list.
LIBRARY_PATH=/usr/lib/gcc/x86_64-pc-cygwin/14/:/usr/lib/gcc/x86_64-pc-cygwin/14/../../../../x86_64-pc-cygwin/lib/:/usr/lib/gcc/x86_64-pc-cygwin/14/../../../:/lib/:/usr/lib/)";
    auto output = parse_gcc_style_output("#define __STDC_VERSION__ 201710L", err);
    ZEXPECT(output.gcc_job == "/usr/lib/gcc/x86_64-pc-cygwin/14/cc1.exe");
    ZEXPECT(output.target == "x86_64-pc-cygwin");
    ZASSERT(output.search_dirs.size() == std::size_t(3));
    ZEXPECT(output.private_dir == std::optional<std::size_t>(0));
}

ZEST_CASE(FrameworkDir) {
    constexpr llvm::StringRef err = R"(Target: aarch64-apple-darwin24
#include <...> search starts here:
 /opt/homebrew/Cellar/gcc/15.1.0/lib/gcc/current/gcc/aarch64-apple-darwin24/15/include
 /Library/Developer/CommandLineTools/SDKs/MacOSX15.sdk/usr/include
 /Library/Developer/CommandLineTools/SDKs/MacOSX15.sdk/System/Library/Frameworks (framework directory)
End of search list.)";
    auto output = parse_gcc_style_output("", err);
    ZASSERT(output.search_dirs.size() == std::size_t(3));
    ZEXPECT(!output.search_dirs[1].framework);
    ZEXPECT(output.search_dirs[2].framework);
    ZEXPECT(output.search_dirs[2].path ==
            "/Library/Developer/CommandLineTools/SDKs/MacOSX15.sdk/System/Library/Frameworks");
    /// No LIBRARY_PATH names a private directory, and no macros a standard.
    ZEXPECT(!output.private_dir);
    ZEXPECT(output.standard.empty());
}

ZEST_CASE(ClangJob) {
    /// `-v` prints the job unquoted but for the program; the GCC
    /// fields stay empty.
    constexpr llvm::StringRef err = R"(Apple clang version 17.0.0 (clang-1700.0.13.5)
Target: arm64-apple-darwin24.6.0
 "/Library/Developer/CommandLineTools/usr/bin/clang" -cc1 -triple arm64-apple-macosx15.0.0 -E -dM -v -isysroot /Library/Developer/CommandLineTools/SDKs/MacOSX.sdk -o - -x c++ /tmp/query.cpp
clang -cc1 version 17.0.0 (clang-1700.0.13.5) default target arm64-apple-darwin24.6.0
#include <...> search starts here:
 /Library/Developer/CommandLineTools/SDKs/MacOSX.sdk/usr/include/c++/v1
End of search list.)";
    auto output = parse_gcc_style_output("#define __cplusplus 201402L", err);
    std::vector<std::string> job = {"/Library/Developer/CommandLineTools/usr/bin/clang",
                                    "-cc1",
                                    "-triple",
                                    "arm64-apple-macosx15.0.0",
                                    "-E",
                                    "-dM",
                                    "-v",
                                    "-isysroot",
                                    "/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk",
                                    "-o",
                                    "-",
                                    "-x",
                                    "c++",
                                    "/tmp/query.cpp"};
    ZEXPECT(output.clang_job == job);
    ZEXPECT(output.gcc_job.empty());
    ZEXPECT(output.gcc_options.empty());
    ZEXPECT(output.standard == "gnu++14");
}

ZEST_CASE(StandardFromMacros) {
    auto standard = [](llvm::StringRef macros) {
        return parse_gcc_style_output(macros, "").standard;
    };
    /// GCC 15's C default, and a value between two standards: GCC 13's
    /// -std=c++2b reports 202100L.
    ZEXPECT(standard("#define __STDC_VERSION__ 202311L") == "gnu23");
    ZEXPECT(standard("#define __cplusplus 202100L") == "gnu++23");
    ZEXPECT(standard("#define __cplusplus 199711L") == "gnu++98");
    ZEXPECT(standard(R"(#define __STRICT_ANSI__ 1
#define __cplusplus 201703L)") == "c++17");
    ZEXPECT(standard("#define __cplusplus 202400L") == "gnu++26");
    ZEXPECT(standard("#define __cplusplus 202700L") == "gnu++2d");
    ZEXPECT(standard("#define __STDC_VERSION__ 202500L") == "gnu2y");
    /// Neither: an assembler input, which no -std fits.
    ZEXPECT(standard("#define __ASSEMBLER__ 1").empty());
    /// OpenCL C predefines C99's macros, but takes no -std.
    ZEXPECT(standard(R"(#define __OPENCL_C_VERSION__ 120
#define __STDC_VERSION__ 199901L
#define __STRICT_ANSI__ 1)")
                .empty());
}

ZEST_CASE(RISCVTargetWidth) {
    /// xPack's and MounRiver's GCCs name a RISC-V target of no width, which
    /// clang cannot read, nor any -march for it: GCC's macros name the width.
    auto none = parse_gcc_style_output("#define __riscv_xlen 32\n", "Target: riscv-none-elf\n");
    ZEXPECT(none.target == "riscv32-none-elf");
    auto embed = parse_gcc_style_output("#define __riscv_xlen 64", "Target: riscv-none-embed");
    ZEXPECT(embed.target == "riscv64-none-embed");
    auto wch = parse_gcc_style_output("#define __riscv_xlen 32", "Target: riscv32-wch-elf");
    ZEXPECT(wch.target == "riscv32-wch-elf");
}

ZEST_CASE(RISCVTargetFeatures, skip = Windows) {
    auto driver = fake_driver("riscv-none-elf-gcc", R"(echo 'Target: riscv-none-elf' >&2
echo ' /opt/xpack/libexec/gcc/riscv-none-elf/15.2.0/cc1 -E -quiet query.c -march=rv32imac_zicsr -mabi=ilp32 -dM' >&2
echo '#define __riscv_xlen 32'
)");
    auto argv = f.resolve(add("a.c", {driver.c_str(), "-c"}));
    ZEXPECT(adjacent(argv, "-target-feature", "+a"));
    ZEXPECT(adjacent(argv, "-target-abi", "ilp32"));
}

ZEST_CASE(NoCompilerJob, skip = Windows) {
    /// A driver that is neither GCC nor clang (Intel's icc).
    auto ref = add("a.cpp", {fake_driver("icc", "").c_str()});
    ZEXPECT(llvm::StringRef(f.failure(ref)).contains("runs no compiler job"));
}

ZEST_CASE(ClangJobKeepsStandard, skip = Windows) {
    auto driver = fake_clang(clang_job);
    auto argv = f.resolve(add("a.cpp", {driver.c_str(), "-std=c++23"}));
    ZEXPECT(contains(argv, "-std=c++23"));
    ZEXPECT(!contains(argv, "-std=gnu++14"));
}

ZEST_CASE(UniversalBuildFirstArch, skip = Windows) {
    /// clang refuses to preprocess for two architectures at once.
    auto driver = fake_driver("clang++", R"(case " $* " in
*" -arch "*" -arch "*)
  echo "clang++: error: cannot use 'cpp-output' output with multiple -arch options" >&2
  exit 1 ;;
esac
echo ' "/usr/bin/clang" "-cc1" "-triple" "arm64-apple-macosx15.0.0" "-E"' >&2
)");
    auto argv = f.resolve(add("a.cpp", {driver.c_str(), "-arch", "arm64", "-arch", "x86_64"}));
    ZEXPECT(adjacent(argv, "-triple", "arm64-apple-macosx15.0.0"));
}

ZEST_CASE(GCCSetsUpClang, skip = Windows) {
    /// A GCC whose output names its target, a macro of its specs, a CPU
    /// clang does not know, and a search list with its private include
    /// directory and a framework directory.
    auto frameworks = tmp.path("sysroot/Frameworks");
    auto driver = fake_gcc("x86_64-linux-gnu-g++",
                           std::format(R"(Target: x86_64-linux-gnu
 /usr/libexec/gcc/x86_64-linux-gnu/13/cc1plus -E -quiet -v -D_REENTRANT query.cpp -mtune=intel -march=x86-64 -dM
#include <...> search starts here:
 {0}
 {1}
 {2}
 {3} (framework directory)
End of search list.
LIBRARY_PATH={4}/)",
                                       tmp.path("gcc/include/c++"),
                                       tmp.path("gcc/13/include"),
                                       tmp.path("sysroot/usr/include"),
                                       frameworks,
                                       tmp.path("gcc/13")));
    auto argv = f.resolve(add("a.cpp", {driver.c_str(), "-c", "-g"}));

    ZEXPECT(adjacent(argv, "-triple", "x86_64-unknown-linux-gnu"));
    ZEXPECT(adjacent(argv, "-D", "_REENTRANT"));
    ZEXPECT(adjacent(argv, "-target-cpu", "x86-64"));
    ZEXPECT(!contains(argv, "intel"));
    /// The command names no standard: the one GCC compiles in.
    ZEXPECT(contains(argv, "-std=gnu++14"));
    /// Debug info stays out of the parse.
    ZEXPECT(!ranges::any_of(argv, [](llvm::StringRef arg) {
        return arg.starts_with("-debug-info-kind");
    }));

    /// GCC's list in GCC's order, clang's builtin headers right before
    /// GCC's own, and nothing of clang's own idea of the system headers.
    std::vector<std::string> dirs;
    for(std::size_t i = 0; i + 1 < argv.size(); i += 1) {
        if(argv[i] == "-internal-isystem"sv || argv[i] == "-internal-externc-isystem"sv) {
            dirs.push_back(argv[i + 1]);
        }
    }
    std::vector<std::string> expected = {tmp.path("gcc/include/c++"),
                                         path::join(resource_dir(), "include"),
                                         tmp.path("gcc/13/include"),
                                         tmp.path("sysroot/usr/include")};
    ZEXPECT(dirs == expected);
    ZEXPECT(adjacent(argv, "-internal-iframework", frameworks));
}

ZEST_CASE(GCCPrivateHeaders, skip = Windows) {
    /// GCC's private include directory holds headers only GCC has, among
    /// GCC's versions of clang's builtin headers. A header only GCC has is
    /// found there; GCC's stdatomic.h is not, and neither is the C
    /// library's copy behind it, which GCC never reaches; GCC's limits.h
    /// goes on to the C library's, and so does clang's.
    tmp.touch("gcc/13/include/vendor_intrinsics.h", "int vendor_intrinsic(void);\n");
    tmp.touch("gcc/13/include/stdatomic.h", "#error GCC's stdatomic.h\n");
    tmp.touch("gcc/13/include/limits.h", "#include_next <limits.h>\n");
    tmp.touch("libc/include/stdatomic.h", "#error the C library's stdatomic.h\n");
    tmp.touch("libc/include/limits.h", "#define LIBC_LIMITS 1\n");
    tmp.touch("gcc/13/include/module.modulemap");
    tmp.touch("libc/include/module.modulemap");
    tmp.touch("a.c", R"(#include <vendor_intrinsics.h>
#include <stdatomic.h>
#include <limits.h>
atomic_int counter;
int load(void) {
    atomic_store(&counter, 1);
    return atomic_load(&counter) + vendor_intrinsic() + LIBC_LIMITS;
}
)");
    auto driver = fake_driver("riscv32-elf-gcc",
                              std::format(R"(cat >&2 <<'OUTPUT'
Target: riscv32-elf
 /opt/libexec/gcc/riscv32-elf/13/cc1 -E -quiet -v query.c -march=rv32imac -mabi=ilp32 -dM
#include <...> search starts here:
 {0}
 {1}
End of search list.
LIBRARY_PATH={2}/
OUTPUT
echo '#define __STDC_VERSION__ 201710L'
)",
                                          tmp.path("gcc/13/include"),
                                          tmp.path("libc/include"),
                                          tmp.path("gcc/13")));
    auto ref = add("a.c", {driver.c_str(), "-c"});
    ZASSERT(f.toolchain.resolve(ref.command, ref.input));
    auto rendered = f.toolchain.render(ref, f.db.spelling(ref.file));
    /// A module map is no header, which only #include_next reaches.
    ZEXPECT(rendered.hidden.hides(tmp.path("gcc/13/include/module.modulemap")));
    ZEXPECT(!rendered.hidden.hides(tmp.path("libc/include/module.modulemap")));
    auto unit = clice::compile({
        .kind = CompilationKind::Content,
        .input = {.arguments = {rendered.argv.begin(), rendered.argv.end()},
                  .directory = tmp.root.str().str(),
                  .vfs = llvm::makeIntrusiveRefCnt<vfs::View>(std::move(rendered.hidden))},
    });
    ZASSERT(unit->completed());
    ZEXPECT(unit->diagnostics().empty());

    /// clice's own include search agrees with the compile.
    auto search = f.toolchain.search_config(ref);
    vfs::DirCache cache;
    vfs::Scope scope(cache);
    auto vendor = resolve_include("vendor_intrinsics.h", true, "", false, {}, search, scope);
    ZASSERT(vendor);
    ZEXPECT(vendor->path.str() == tmp.path("gcc/13/include/vendor_intrinsics.h"));
    auto atomic = resolve_include("stdatomic.h", true, "", false, {}, search, scope);
    ZASSERT(atomic);
    ZEXPECT(fs::equivalent(atomic->path.str(), path::join(resource_dir(), "include", "stdatomic.h"))
                .value_or(false));
    ZEXPECT(!resolve_include("stdatomic.h", true, "", true, atomic->found_dir_idx, search, scope));
    auto limits = resolve_include("limits.h", true, "", false, {}, search, scope);
    ZASSERT(limits);
    auto next = resolve_include("limits.h", true, "", true, limits->found_dir_idx, search, scope);
    ZASSERT(next);
    ZEXPECT(next->path.str() == tmp.path("libc/include/limits.h"));
}

ZEST_CASE(GCCTargetNames, skip = Windows) {
    auto render = [&](llvm::StringRef target, llvm::StringRef options) {
        auto driver = fake_gcc(target.str() + "-g++",
                               std::format(R"(Target: {0}
 /usr/libexec/gcc/{0}/13/cc1plus -E -quiet -v query.cpp {1} -dM)",
                                           target,
                                           options));
        return f.resolve(add(target.str() + ".cpp", {driver.c_str(), "-c"}));
    };

    /// GCC's two-core tuning keeps its first core; an ABI name only GCC
    /// knows is dropped, one clang knows stays.
    auto arm = render("aarch64-linux-gnu", "-mcpu=cortex-a72.cortex-a53 -mabi=lp64");
    ZEXPECT(adjacent(arm, "-target-cpu", "cortex-a72"));
    ZEXPECT(!contains(arm, "lp64"));
    auto riscv = render("riscv32-unknown-elf", "-march=rv32imafc -mabi=ilp32f");
    ZEXPECT(adjacent(riscv, "-target-abi", "ilp32f"));
    /// A feature's own dot is no second core: single precision stays.
    auto m7 = render("arm-none-eabi", "-mcpu=cortex-m7+nofp.dp -mthumb -mfloat-abi=hard");
    ZEXPECT(adjacent(m7, "-target-feature", "+vfp2sp"));
    /// A CPU newer than clang's target table.
    auto x86 = render("x86_64-linux-gnu", "-march=clice-future-cpu");
    ZEXPECT(!contains(x86, "clice-future-cpu"));
    /// A vendor's RISC-V extension LLVM does not know goes, and the rest
    /// stay: as a command writes them, and as GCC's job versions them.
    auto wch = render("riscv32-wch-elf", "-march=rv32imc_zba_zbb_zbc_zbs_xw -mabi=ilp32");
    ZEXPECT(adjacent(wch, "-target-feature", "+m"));
    ZEXPECT(adjacent(wch, "-target-feature", "+zba"));
    auto andes = render("riscv32-elf",
                        "-march=rv32imc_xandes -march=rv32i2p0_m2p0_c2p0_xandes5p0 -mabi=ilp32");
    ZEXPECT(adjacent(andes, "-target-feature", "+m"));
    ZEXPECT(adjacent(andes, "-target-feature", "+c"));
    /// A vendor's extension glued to the letters before it, and letters
    /// running on after an underscore.
    auto glued = render("riscv32-none-elf", "-march=rv32imacxw -mabi=ilp32");
    ZEXPECT(adjacent(glued, "-target-feature", "+a"));
    auto run = render("riscv32-unknown-linux-gnu", "-march=rv32i_mac -mabi=ilp32");
    ZEXPECT(adjacent(run, "-target-feature", "+a"));
    ZEXPECT(adjacent(run, "-target-feature", "+c"));
    /// A version LLVM does not know, and an extension LLVM has only as
    /// experimental, which clang's driver takes only when asked to.
    auto thead = render("riscv64-unknown-elf", "-march=rv64gcv0p7_zicfilp_xtheadc -mabi=lp64d");
    ZEXPECT(adjacent(thead, "-target-feature", "+v"));
    ZEXPECT(adjacent(thead, "-target-feature", "+d"));
    ZEXPECT(!contains(thead, "+experimental-zicfilp"));
    /// A profile, which LLVM reads as it stands, with a vendor's extension
    /// or without.
    auto profile = render("riscv64-unknown-linux-gnu", "-march=rva22u64 -mabi=lp64d");
    ZEXPECT(adjacent(profile, "-target-feature", "+zba"));
    auto vendor = render("riscv64-linux-gnu", "-march=rva22u64_xw -mabi=lp64d");
    ZEXPECT(adjacent(vendor, "-target-feature", "+zba"));
}

ZEST_CASE(GCCSeesWrittenAliases, skip = Windows) {
    /// clang reads these as aliases of options GCC names otherwise or not
    /// at all (`-G 8`, `-flto=full`, `-ftrigraphs`), which GCC rejects.
    auto args = tmp.path("args");
    auto driver = fake_driver("riscv32-unknown-elf-gcc",
                              std::format(R"(printf '%s\n' "$@" > '{}'
echo 'Target: riscv32-unknown-elf' >&2
echo ' /opt/riscv/libexec/gcc/riscv32-unknown-elf/15/cc1 -E -quiet query.c -dM' >&2
)",
                                          args));
    std::vector<const char*> written = {"-msmall-data-limit=8", "-flto=auto", "-trigraphs"};
    auto command = written;
    command.insert(command.begin(), driver.c_str());
    command.push_back("-c");
    auto ref = add("a.c", command);
    ZEXPECT(f.toolchain.resolve(ref.command, ref.input).has_value());

    auto passed = vfs::read_text(args);
    ZASSERT(passed);
    llvm::SmallVector<llvm::StringRef> lines;
    llvm::StringRef(*passed).split(lines, '\n', -1, false);
    for(llvm::StringRef arg: written) {
        ZEXPECT(contains(lines, arg));
    }
    ZEXPECT(!contains(lines, "-G"));

    /// A rule's edit keeps what the command wrote.
    std::vector<CommandEdit> edits = {
        {.kind = CommandEdit::Kind::Append, .flags = {"-fno-common"}}
    };
    auto edited = ref;
    edited.command = apply_edits(f.commands, f.file_table, ref.command, {.edits = edits});
    ZEXPECT(f.toolchain.resolve(edited.command, edited.input).has_value());
    passed = vfs::read_text(args);
    ZASSERT(passed);
    ZEXPECT(llvm::StringRef(*passed).contains("-msmall-data-limit=8\n"));

    /// Two names of one option are two queries: GCC may take one alone.
    auto clang_name = add("b.c", {driver.c_str(), "-G", "8", "-c"});
    auto gcc_name = add("c.c", {driver.c_str(), "-msmall-data-limit=8", "-c"});
    ZEXPECT(f.key(clang_name) != f.key(gcc_name));
}

ZEST_CASE(CommandISAForClang, skip = Windows) {
    /// The command's own -march, which GCC takes as written and clang
    /// without what LLVM does not know.
    auto driver = fake_driver("riscv32-wch-elf-gcc", R"(echo 'Target: riscv32-wch-elf' >&2
echo ' /opt/wch/libexec/gcc/riscv32-wch-elf/15/cc1 -E -quiet query.c -dM' >&2
)");
    auto argv = f.resolve(
        add("a.c", {driver.c_str(), "-march=rv32imc_zba_zbb_zbc_zbs_xw", "-mabi=ilp32", "-c"}));
    ZEXPECT(adjacent(argv, "-target-feature", "+m"));
    ZEXPECT(adjacent(argv, "-target-feature", "+zbs"));
}

ZEST_CASE(GCCErrorReported, skip = Windows) {
    /// GCC rejects an option before `-v` prints its version, the last line.
    auto driver = fake_driver("riscv32-unknown-elf-gcc", R"(echo 'Using built-in specs.' >&2
echo "riscv32-unknown-elf-gcc: error: unrecognized command-line option '-mclice-future'" >&2
echo 'Target: riscv32-unknown-elf' >&2
echo 'gcc version 15.2.0 (g5115c7e44-dirty)' >&2
exit 1
)");
    auto ref = add("a.c", {driver.c_str(), "-c"});
    auto failure = f.failure(ref);
    llvm::StringRef error = failure;
    ZEXPECT(error.contains("error: unrecognized command-line option '-mclice-future'"));
    ZEXPECT(!error.contains("gcc version"));
}

ZEST_CASE(GCCFailureRejected, skip = Windows) {
    /// GCC prints its job and search list before its compile fails, but
    /// LIBRARY_PATH and the macros only after it succeeds.
    auto driver = fake_driver("g++", R"(echo 'Target: x86_64-linux-gnu' >&2
echo ' /usr/libexec/gcc/x86_64-linux-gnu/13/cc1plus -E -quiet -imacros missing.h query.cpp -dM' >&2
echo 'cc1plus: fatal error: missing.h: No such file or directory' >&2
echo 'compilation terminated.' >&2
exit 1
)");
    auto ref = add("a.cpp", {driver.c_str(), "-c"});
    ZEXPECT(llvm::StringRef(f.failure(ref)).contains("missing.h"));
}

};  // ZEST_SUITE(GCCTests)

}  // namespace

}  // namespace clice::testing
