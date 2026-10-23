module;

#include "support/prelude.h"

module clice;

import :command.table;
import :command.toolchain;
import :unittests.utils;
import :unittests.utils.toolchain_fixture;
import :vfs.file_system;
import :vfs.path;

namespace clice::testing {

namespace {

using namespace std::literals;

ZEST_SUITE(ToolchainTests, ToolchainFixture) {

ZEST_CASE(InitiallyEmpty) {
    ZEXPECT(!f.toolchain.has_cache());
}

ZEST_CASE(KeyIgnoresUserContent) {
    auto base = f.add("/fake", "/tmp/a.cpp", {"clang++", "-std=c++23", "/tmp/a.cpp"});
    auto user = f.add("/fake",
                      "/tmp/b.cpp",
                      {"clang++",
                       "-std=c++23",
                       "-I/usr/include",
                       "-DFOO=1",
                       "-include",
                       "foo.h",
                       "-imacros",
                       "macros.h",
                       "-isystem",
                       "/opt/include",
                       "/tmp/b.cpp"});
    ZEXPECT(f.key(base) == f.key(user));
}

ZEST_CASE(KeyIgnoresDriverIgnored) {
    /// Driver-ignored flags carry per-file values in the wild (bazel's GCC
    /// toolchain stamps -frandom-seed=<output> on every command); keying on
    /// them would spawn one query per file instead of one per toolchain.
    auto base = f.add("/fake", "/tmp/a.cpp", {"g++", "-std=c++23", "/tmp/a.cpp"});
    auto seed_a = f.add("/fake",
                        "/tmp/b.cpp",
                        {"g++", "-std=c++23", "-frandom-seed=bazel-out/b.o", "/tmp/b.cpp"});
    auto seed_b = f.add("/fake",
                        "/tmp/c.cpp",
                        {"g++", "-std=c++23", "-frandom-seed=bazel-out/c.o", "/tmp/c.cpp"});
    ZEXPECT(f.key(base) == f.key(seed_a));
    ZEXPECT(f.key(base) == f.key(seed_b));
}

ZEST_CASE(KeyIgnoresPreprocessorDepfile) {
    /// Kbuild writes `-Wp,-MMD,<depfile>` into every command: keying on the
    /// file would spawn one query per unit of the kernel.
    auto base = f.add("/fake", "/tmp/a.c", {"gcc", "-std=gnu11", "/tmp/a.c"});
    auto b = f.add("/fake", "/tmp/b.c", {"gcc", "-std=gnu11", "-Wp,-MMD,.b.o.d", "/tmp/b.c"});
    auto c = f.add("/fake", "/tmp/c.c", {"gcc", "-std=gnu11", "-Wp,-MMD,.c.o.d", "/tmp/c.c"});
    ZEXPECT(f.key(base) == f.key(b));
    ZEXPECT(f.key(base) == f.key(c));

    auto fortified =
        f.add("/fake", "/tmp/d.c", {"gcc", "-std=gnu11", "-Wp,-D_FORTIFY_SOURCE=2", "/tmp/d.c"});
    ZEXPECT(f.key(base) != f.key(fortified));
}

ZEST_CASE(KeyIgnoresCodegen) {
    /// Debug info, section layout and colored diagnostics never change how
    /// a driver sets up the parse; stack protection does, through its macros.
    auto base = f.add("/fake", "/tmp/a.cpp", {"g++", "-std=c++23", "/tmp/a.cpp"});
    auto codegen = f.add("/fake",
                         "/tmp/b.cpp",
                         {"g++",
                          "-std=c++23",
                          "-g",
                          "-ffunction-sections",
                          "-fdiagnostics-color=always",
                          "/tmp/b.cpp"});
    ZEXPECT(f.key(base) == f.key(codegen));
    auto off =
        f.add("/fake", "/tmp/c.cpp", {"g++", "-std=c++23", "-fno-stack-protector", "/tmp/c.cpp"});
    ZEXPECT(f.key(base) != f.key(off));
}

ZEST_CASE(KeyTracksSemantics) {
    auto base = f.add("/fake", "/tmp/a.cpp", {"clang++", "-std=c++23", "/tmp/a.cpp"});

    auto driver = f.add("/fake", "/tmp/b.cpp", {"g++", "-std=c++23", "/tmp/b.cpp"});
    ZEXPECT(f.key(base) != f.key(driver));

    auto target = f.add("/fake",
                        "/tmp/c.cpp",
                        {"clang++", "-std=c++23", "--target=aarch64-linux-gnu", "/tmp/c.cpp"});
    ZEXPECT(f.key(base) != f.key(target));

    /// The language dimension: an -x selector and an extension of another
    /// language both change the key; a C++ driver compiles a C file as
    /// C++ and shares it.
    auto lang = f.add("/fake", "/tmp/d.cpp", {"clang++", "-std=c++23", "-x", "c", "/tmp/d.cpp"});
    ZEXPECT(f.key(base) != f.key(lang));

    auto ext = f.add("/fake", "/tmp/e.m", {"clang++", "-std=c++23", "/tmp/e.m"});
    ZEXPECT(f.key(base) != f.key(ext));

    auto c_file = f.add("/fake", "/tmp/f.c", {"clang++", "-std=c++23", "/tmp/f.c"});
    ZEXPECT(f.key(base) == f.key(c_file));

    // Any non-user-content flag affects the key, not just toolchain options.
    auto semantic =
        f.add("/fake", "/tmp/g.cpp", {"clang++", "-std=c++23", "-fno-exceptions", "/tmp/g.cpp"});
    ZEXPECT(f.key(base) != f.key(semantic));
}

ZEST_CASE(KeyTracksWrittenAlias) {
    /// A GCC may know an option by one name alone; nvcc's query reads
    /// clang's names, whichever the command wrote.
    auto g = f.add("/fake", "/tmp/a.c", {"gcc", "-G", "8", "/tmp/a.c"});
    auto limit = f.add("/fake", "/tmp/b.c", {"gcc", "-msmall-data-limit=8", "/tmp/b.c"});
    ZEXPECT(f.key(g) != f.key(limit));
    auto offload = f.add("/fake", "/tmp/c.cu", {"nvcc", "--offload-host-only", "/tmp/c.cu"});
    auto cuda = f.add("/fake", "/tmp/d.cu", {"nvcc", "--cuda-host-only", "/tmp/d.cu"});
    ZEXPECT(f.key(offload) == f.key(cuda));
}

ZEST_CASE(KeyTracksConfigFile) {
    /// A --config path resolves against the compilation directory, so
    /// identical commands in different directories must not share a query.
    auto a = f.add("/fake/a", "/tmp/a.cpp", {"clang++", "--config", "sub/clang.cfg", "/tmp/a.cpp"});
    auto b = f.add("/fake/b", "/tmp/b.cpp", {"clang++", "--config", "sub/clang.cfg", "/tmp/b.cpp"});
    ZEXPECT(f.key(a) != f.key(b));

    /// An absolute config file and a bare name, which clang searches in its
    /// configuration directories, are directory-independent. Paths are
    /// platform-native absolute (POSIX spellings are not absolute on
    /// Windows), and the driver is absolute too: a bare driver name is
    /// never cwd-exempt on Windows and would tie the key to the directory
    /// on its own.
    auto driver = tmp.path("clang++");
    for(auto cfg: {"--config=" + tmp.path("clang.cfg"), std::string("--config=clang.cfg")}) {
        auto c = f.add("/fake/a", "/tmp/c.cpp", {driver.c_str(), cfg.c_str(), "/tmp/c.cpp"});
        auto d = f.add("/fake/b", "/tmp/d.cpp", {driver.c_str(), cfg.c_str(), "/tmp/d.cpp"});
        ZEXPECT(f.key(c) == f.key(d));
    }
}

ZEST_CASE(FailedQueryCooldown, skip = Windows) {
    // A failed query is cached: within the cooldown the driver is not
    // spawned again, so removing it changes nothing.
    auto driver = fake_driver("clang", "exit 1\n");
    auto ref = add("a.cpp", {driver.c_str()});

    auto first = f.failure(ref);
    ZEXPECT(f.toolchain.failed_count() == std::size_t(1));

    ZASSERT(!vfs::remove(driver));
    ZEXPECT(f.failure(ref) == first);
}

ZEST_CASE(FailedQueryRetries, skip = Windows) {
    // A transient driver failure must not poison the key for the session:
    // past the cooldown, resolve() and warm() query the driver again.
    f.toolchain.set_failed_retry(std::chrono::seconds(0));
    auto a = add("a.cpp", {tmp.c_path("clang"), "-std=c++17"});
    auto b = add("b.cpp", {tmp.c_path("clang"), "-std=c++20"});
    llvm::SmallVector<CommandRef> refs = {a, b};
    f.toolchain.warm(refs);
    ZEXPECT(f.toolchain.failed_count() == std::size_t(2));

    fake_clang(clang_job);
    ZEXPECT(f.toolchain.resolve(a.command, a.input).has_value());
    ZEXPECT(f.toolchain.failed_count() == std::size_t(1));
    f.toolchain.warm(refs);
    ZEXPECT(f.toolchain.failed_count() == std::size_t(0));
    ZEXPECT(f.toolchain.query_count() == std::size_t(2));
}

ZEST_CASE(FailedQueryUnqueried) {
    // A compiler that cannot be queried is read as an unqueried one is: by
    // clang's own driver, for the target its name sets, without the CPU
    // and ABI names clang's target does not know (arm64 Linux builds GCC
    // with -mabi=lp64).
    auto ref = add("a.c", {tmp.c_path("aarch64-linux-gnu-gcc"), "-mabi=lp64", "-c"});
    ZEXPECT(llvm::StringRef(f.failure(ref)).contains("not found"));
    auto rendered = f.toolchain.render(ref, f.db.spelling(ref.file));
    ZEXPECT(llvm::StringRef(rendered.query_failure).contains("not found"));
    ZEXPECT(adjacent(rendered.argv, "-triple", "aarch64-unknown-linux-gnu"));
    ZEXPECT(!contains(rendered.argv, "lp64"));
}

ZEST_CASE(WarmPartialFailure, skip = Windows) {
    auto driver = fake_clang(clang_job);
    auto good = add("a.cpp", {driver.c_str()});
    auto bad = add("b.cpp", {"clice-nonexistent-driver"});

    llvm::SmallVector<CommandRef> refs = {good, bad};
    f.toolchain.warm(refs);

    // The successful query is cached; the failed one is negatively cached
    // so later resolve() calls read the command unqueried without
    // re-querying the driver.
    ZEXPECT(f.toolchain.query_count() == std::size_t(1));
    ZEXPECT(f.toolchain.failed_count() == std::size_t(1));
    ZEXPECT(f.toolchain.resolve(good.command, good.input).has_value());
    ZEXPECT(llvm::StringRef(f.failure(bad)).contains("clice-nonexistent-driver"));
    ZEXPECT(f.toolchain.failed_count() == std::size_t(1));
}

ZEST_CASE(WarmDedupesQueries, skip = Windows) {
    auto driver = fake_clang(clang_job);
    auto a = add("a.cpp", {driver.c_str(), "-std=c++23"});
    auto b = add("b.cpp", {driver.c_str(), "-std=c++23"});
    auto c = add("c.cpp", {driver.c_str(), "-std=c++17"});

    llvm::SmallVector<CommandRef> refs = {a, b, c};
    f.toolchain.warm(refs);
    ZEXPECT(f.toolchain.query_count() == std::size_t(2));

    // With the driver gone, only the query cache can still resolve.
    ZASSERT(!vfs::remove(driver));
    ZEXPECT(f.toolchain.resolve(a.command, a.input).has_value());
}

ZEST_CASE(WarmBoundsDrivers, skip = Windows) {
    // Each driver records how many drivers run beside it: twice as many
    // queries as the process has CPUs never run more drivers than CPUs.
    tmp.mkdir("running");
    auto driver = fake_driver("clang",
                              std::format(R"(touch '{0}/'$$
ls '{0}' | wc -l >> '{1}'
sleep 0.2
rm '{0}/'$$
echo '{2}' >&2
)",
                                          tmp.path("running"),
                                          tmp.path("counts"),
                                          clang_job));

    auto cores = kota::sys::parallelism();
    llvm::SmallVector<CommandRef> refs;
    for(unsigned i = 0; i < 2 * cores; i += 1) {
        auto depth = std::format("-ftemplate-depth={}", 100 + i);
        refs.push_back(add(std::format("{}.cpp", i), {driver.c_str(), depth.c_str()}));
    }
    f.toolchain.warm(refs);
    ZEXPECT(f.toolchain.query_count() == std::size_t(2 * cores));

    auto counts = vfs::read(tmp.path("counts"));
    ZASSERT(counts);
    llvm::SmallVector<llvm::StringRef> lines;
    (*counts)->getBuffer().split(lines, '\n', -1, false);
    ZEXPECT(lines.size() == std::size_t(2 * cores));
    for(auto line: lines) {
        unsigned running = 0;
        ZASSERT(!line.trim().getAsInteger(10, running));
        ZEXPECT(running <= cores);
    }
}

ZEST_CASE(ClangJobCleaned, skip = Windows) {
    /// What only the query asked for goes, and so does what the linked cc1
    /// cannot take: a newer clang's flag with its value, a codegen flag's
    /// newer value, a CPU name the target does not know.
    auto driver = fake_clang(
        R"( "/usr/bin/clang-22" "-cc1" "-triple" "x86_64-unknown-linux-gnu" "-E" "-dM" "-v" "-main-file-name" "query.cpp" "-clice-future-flag" "val" "-mframe-pointer=non-leaf-no-reserve" "-target-cpu" "clice-future-cpu" "-fmodules-reduced-bmi" "-fmodule-output=/tmp/query.pcm" "-o" "-" "-x" "c++" "/tmp/query.cpp")");
    auto argv = f.resolve(add("dir/a.cpp", {driver.c_str()}));

    ZEXPECT(contains(argv, "-fmodules-reduced-bmi"));
    ZEXPECT(ranges::count(argv, "-fsyntax-only"sv) == 1);
    ZEXPECT(ranges::count(argv, "-main-file-name"sv) == 1);
    ZEXPECT(adjacent(argv, "-main-file-name", "a.cpp"));
    for(llvm::StringRef arg: argv) {
        ZEXPECT(arg != "-E");
        ZEXPECT(arg != "-dM");
        ZEXPECT(arg != "-v");
        ZEXPECT(arg != "-o");
        ZEXPECT(arg != "-clice-future-flag");
        ZEXPECT(arg != "val");
        ZEXPECT(arg != "clice-future-cpu");
        ZEXPECT(!arg.starts_with("-mframe-pointer"));
        ZEXPECT(!arg.starts_with("-fmodule-output"));
        ZEXPECT(!arg.ends_with("query.cpp"));
    }
    /// The command names no standard: the one the driver compiles in.
    ZEXPECT(contains(argv, "-std=gnu++14"));
}

ZEST_CASE(UserContentAttached, skip = Windows) {
    // The query never sees user content; the render puts it back on the
    // driver's cc1 line.
    auto driver = fake_clang(clang_job);
    auto include = "-I" + tmp.path("inc");
    auto argv = f.resolve(add("a.cpp", {driver.c_str(), include.c_str(), "-DFOO=1"}));
    ZEXPECT(adjacent(argv, "-I", Spelling::absolute(tmp.path("inc")).str()));
    ZEXPECT(adjacent(argv, "-D", "FOO=1"));
}

ZEST_CASE(PrecompileFullBMI, skip = Windows) {
    auto driver = fake_clang(
        R"( "/usr/bin/clang-22" "-cc1" "-triple" "x86_64-unknown-linux-gnu" "-fmodules-reduced-bmi")");

    // Whether the module unit's cc1 asks for a reduced BMI.
    auto reduced = [&](std::vector<const char*> flags) {
        flags.insert(flags.begin(), driver.c_str());
        flags.push_back(tmp.c_path("a.cppm"));
        ToolchainEntries fresh;
        auto ref = fresh.add(tmp.root.str(), tmp.path("a.cppm"), flags);
        return contains(fresh.resolve(ref), "-fmodules-reduced-bmi");
    };

    // --precompile writes the full BMI whatever the driver asks for, and the
    // reduced one only to a module output the command names.
    ZEXPECT(reduced({}));
    ZEXPECT(!reduced({"--precompile"}));
    ZEXPECT(reduced({"--precompile", "-fmodule-output"}));
    ZEXPECT(reduced({"--precompile", "-fmodule-output=/tmp/a.pcm"}));
    ZEXPECT(reduced({"--precompile-reduced-bmi"}));
}

ZEST_CASE(ResourceDirBecomesOurs, skip = Windows) {
    // The external driver's resource dir and its builtin headers become
    // ours, a trailing separator or not, and so does a builtin header the
    // command forces; the ignorelist under its share/ has no counterpart in
    // our tree and stays.
    auto driver = fake_clang(
        R"( "/usr/bin/clang-22" "-cc1" "-triple" "x86_64-unknown-linux-gnu" "-resource-dir" "/clice-fake/lib/clang/22//" "-internal-isystem" "/clice-fake/lib/clang/22//include" "-fsanitize=address" "-fsanitize-system-ignorelist=/clice-fake/lib/clang/22//share/asan_ignorelist.txt")");
    auto argv = f.resolve(
        add("a.cpp", {driver.c_str(), "-include", "/clice-fake/lib/clang/22/include/stddef.h"}));

    ZEXPECT(adjacent(argv, "-resource-dir", resource_dir()));
    ZEXPECT(adjacent(argv, "-internal-isystem", path::join(resource_dir(), "include")));
    ZEXPECT(adjacent(argv, "-include", path::join(resource_dir(), "include/stddef.h")));
    ZEXPECT(contains(
        argv,
        "-fsanitize-system-ignorelist=/clice-fake/lib/clang/22//share/asan_ignorelist.txt"));
}

ZEST_CASE(MinGWKeepsResourceDir, skip = Windows) {
    // A matched LLVM-MinGW installation keeps the resource tree it derives
    // (the query hands it none), also when warmed (the driver gone, only the
    // warmed cache can resolve); another target's tree becomes ours.
    tmp.mkdir("llvm-mingw/lib/clang/22/include");
    auto external = tmp.path("llvm-mingw/lib/clang/22");
    auto job = [&](llvm::StringRef triple) {
        return std::format(
            R"( "/usr/bin/clang-22" "-cc1" "-triple" "{0}" "-resource-dir" "{1}" "-internal-isystem" "{1}/include")",
            triple,
            external);
    };

    auto mingw = fake_driver("x86_64-w64-mingw32-clang++",
                             std::format(R"(case " $* " in *" -resource-dir "*) exit 1 ;; esac
echo '{}' >&2
)",
                                         job("x86_64-w64-windows-gnu")));
    auto ref = add("a.cpp", {mingw.c_str()});
    llvm::SmallVector<CommandRef> refs = {ref};
    f.toolchain.warm(refs);
    ZASSERT(!vfs::remove(mingw));
    auto argv = f.resolve(ref);
    ZEXPECT(adjacent(argv, "-resource-dir", external));
    ZEXPECT(!contains(argv, resource_dir()));

    auto other = fake_clang(job("x86_64-unknown-linux-gnu"));
    ZEXPECT(adjacent(f.resolve(add("b.cpp", {other.c_str()})), "-resource-dir", resource_dir()));
}

ZEST_CASE(ForeignBuiltinHeaders, skip = Windows) {
    /// Another clang's builtin headers, named by the command itself.
    tmp.touch("clang-23/include/__stddef_max_align_t.h");
    auto foreign = tmp.path("clang-23/include");
    auto driver = fake_clang(clang_job);
    auto argv = f.resolve(add("a.cpp", {driver.c_str(), "-isystem", foreign.c_str()}));
    ZEXPECT(adjacent(argv, "-isystem", path::join(resource_dir(), "include")));
    ZEXPECT(!contains(argv, foreign));
}

};  // ZEST_SUITE(ToolchainTests)

}  // namespace

}  // namespace clice::testing
