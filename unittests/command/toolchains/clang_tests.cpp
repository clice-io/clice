module;

#include "modules/prelude.h"

module clice;

import :command.table;
import :command.toolchains.clang;
import :unittests.utils;
import :unittests.utils.toolchain_fixture;
import :vfs.file_system;

namespace clice::testing {

namespace {

using namespace std::literals;

ZEST_SUITE(ClangTests, ToolchainFixture) {

ZEST_CASE(ClangJobFirstOnly) {
    auto job = parse_clang_job(R"(clang version 22.0.0
 "/usr/bin/clang-22" "-cc1" "-triple" "x86_64-unknown-linux-gnu" "a.cpp"
 "/usr/bin/clang-22" "-cc1" "b.cpp"
 "/usr/bin/ld" "-o" "a.out")");
    std::vector<std::string> expected = {"/usr/bin/clang-22",
                                         "-cc1",
                                         "-triple",
                                         "x86_64-unknown-linux-gnu",
                                         "a.cpp"};
    ZEXPECT(job == expected);
    ZEXPECT(parse_clang_job(R"( "/usr/bin/ld" "-o" "a.out")").empty());
}

ZEST_CASE(ClangJobMultiCall) {
    /// A multi-call llvm names the tool before -cc1.
    auto job = parse_clang_job(R"( "/opt/xclang/bin/llvm" "clang" "-cc1" "a.cpp")");
    std::vector<std::string> expected = {"/opt/xclang/bin/llvm", "-cc1", "a.cpp"};
    ZEXPECT(job == expected);
}

ZEST_CASE(NamesStandard) {
    /// `--std` takes its value apart as GCC writes it; `--stdlib` names none.
    ZEXPECT(names_standard({"g++", "-std=c++20"}));
    ZEXPECT(names_standard({"g++", "--std", "c++0x"}));
    ZEXPECT(!names_standard({"clang++", "--stdlib", "libc++"}));
}

ZEST_CASE(UnqueriedRunsNothing, skip = Windows) {
    // Neither resolve() nor warm() runs the driver: clang's own builds the
    // job. Queried, the same command runs it.
    auto ran = tmp.path("ran");
    auto driver = fake_driver("g++", std::format("touch '{}'\n", ran));
    auto ref = add("a.cpp", {driver.c_str(), "-std=c++17"});
    auto off = unqueried(ref);
    ZEXPECT(f.key(off) != f.key(ref));
    ZEXPECT(f.commands.command_hash(off.command) != f.commands.command_hash(ref.command));

    llvm::SmallVector<CommandRef> refs = {off};
    f.toolchain.warm(refs);
    auto argv = f.resolve(off);
    ZEXPECT(contains(argv, "-std=c++17"));
    ZEXPECT(adjacent(argv, "-resource-dir", resource_dir()));
    ZEXPECT(!vfs::exists(ran));

    ZEXPECT(llvm::StringRef(f.failure(ref)).contains("runs no compiler job"));
    ZEXPECT(vfs::exists(ran));
}

ZEST_CASE(NoJobSaysWhy) {
    // A command clang's driver builds no job for names the driver's reason.
    auto argv0 = tmp.path("g++");
    auto ref = unqueried(add("a.cpp", {argv0.c_str(), "-x", "bogus"}));
    auto resolved = f.toolchain.resolve(ref.command, ref.input);
    ZASSERT(!resolved);
    ZEXPECT(llvm::StringRef(resolved.error()).contains("language not recognized: 'bogus'"));
}

ZEST_CASE(UnqueriedTargetFromName) {
    // A compiler that is not even here reads as a clang under its name
    // would: the name's target prefix, unless the command names a target.
    auto cross = tmp.path("aarch64-linux-gnu-g++");
    auto argv = f.resolve(unqueried(add("a.cpp", {cross.c_str()})));
    ZEXPECT(adjacent(argv, "-triple", "aarch64-unknown-linux-gnu"));
    argv = f.resolve(unqueried(add("b.cpp", {cross.c_str(), "--target=riscv64-linux-gnu"})));
    ZEXPECT(adjacent(argv, "-triple", "riscv64-unknown-linux-gnu"));
}

ZEST_CASE(UnqueriedRelativeConfig) {
    // Relative paths clang's driver reads, here from a configuration file,
    // are the entry directory's, as they are for a driver clice runs.
    tmp.touch("gcc/lib/gcc/x86_64-linux-gnu/99/crtbegin.o");
    tmp.mkdir("gcc/include/c++/99");
    tmp.touch("toolchain.cfg", "--gcc-install-dir=gcc/lib/gcc/x86_64-linux-gnu/99\n");
    auto config = "--config=" + tmp.path("toolchain.cfg");
    auto argv = f.resolve(
        unqueried(add("a.cpp", {"bin/g++", "--target=x86_64-linux-gnu", config.c_str()})));
    ZEXPECT(std::ranges::any_of(argv, [](llvm::StringRef arg) {
        return arg.ends_with("include/c++/99");
    }));
}

};  // ZEST_SUITE(ClangTests)

}  // namespace

}  // namespace clice::testing
