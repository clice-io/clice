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
    /// Windows).
    for(auto cfg: {"--config=" + tmp.path("clang.cfg"), std::string("--config=clang.cfg")}) {
        auto c = f.add("/fake/a", "/tmp/c.cpp", {"clang++", cfg.c_str(), "/tmp/c.cpp"});
        auto d = f.add("/fake/b", "/tmp/d.cpp", {"clang++", cfg.c_str(), "/tmp/d.cpp"});
        ZEXPECT(f.key(c) == f.key(d));
    }
}

ZEST_CASE(BareDriverSharesKey) {
    /// A bare driver name is looked up on PATH, on Windows too, so entries
    /// in different directories share one query.
    auto a = f.add("/fake/a", "/tmp/a.cpp", {"clang++", "/tmp/a.cpp"});
    auto b = f.add("/fake/b", "/tmp/b.cpp", {"clang++", "/tmp/b.cpp"});
    ZEXPECT(f.key(a) == f.key(b));
    auto relative = f.add("/fake/c", "/tmp/c.cpp", {"bin/clang++", "/tmp/c.cpp"});
    auto elsewhere = f.add("/fake/d", "/tmp/d.cpp", {"bin/clang++", "/tmp/d.cpp"});
    ZEXPECT(f.key(relative) != f.key(elsewhere));
}

ZEST_CASE(FailedQueryCooldown, skip = Windows) {
    // A failed query is cached: within the cooldown the driver is not
    // spawned again, so removing it changes nothing.
    auto driver = fake_driver("clang", "exit 1\n");
    auto ref = add("a.cpp", {driver.c_str()});

    auto first = f.failure(ref);
    ZEXPECT(f.toolchain.failed_count() == std::size_t(1));

    ZASSERT(fs::remove(driver));
    ZEXPECT(f.failure(ref) == first);
}

ZEST_CASE(FailedQueryRetries, skip = Windows) {
    // A transient driver failure must not poison the key for the session:
    // past the cooldown, resolve() and warm() query the driver again.
    f.toolchain.set_failed_retry(chrono::seconds(0));
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

    /// The driver command it reads goes without the ABI name too.
    auto driver = f.toolchain.render_for_clang(ref, f.db.spelling(ref.file));
    ZEXPECT(contains(driver, "--target=aarch64-linux-gnu"));
    ZEXPECT(contains(driver, "-fno-delayed-template-parsing"));
    ZEXPECT(!contains(driver, "-mabi=lp64"));
}

ZEST_CASE(ReadAsClangTarget) {
    // clang's own driver reads a command for the target its compiler's
    // name sets, unless the command names one.
    auto ref = add("a.c", {tmp.c_path("riscv32-wch-elf-gcc"), "-mcmodel=medany", "-c"});
    auto argv = f.toolchain.render_for_clang(ref, f.db.spelling(ref.file));
    ZEXPECT(contains(argv, "--target=riscv32-wch-elf"));

    auto own = add("b.c", {tmp.c_path("riscv32-wch-elf-gcc"), "-target", "riscv64-unknown-elf"});
    argv = f.toolchain.render_for_clang(own, f.db.spelling(own.file));
    ZEXPECT(!contains(argv, "--target=riscv32-wch-elf"));
    ZEXPECT(adjacent(argv, "-target", "riscv64-unknown-elf"));
}

ZEST_CASE(ReadAsClangSpellings) {
    // GCC's own spellings reach clang's own driver as it takes them, in
    // the command it reads and in the job it builds: WCH's `xw` goes from
    // the ISA string, the two-core tuning form keeps its first core.
    auto riscv =
        add("a.c", {tmp.c_path("riscv32-wch-elf-gcc"), "-march=rv32imc_zba_xw", "-mabi=ilp32"});
    ZEXPECT(contains(f.toolchain.render_for_clang(riscv, f.db.spelling(riscv.file)),
                     "-march=rv32imc_zba"));
    ZEXPECT(contains(f.resolve(riscv), "+zba"));

    auto arm = add("b.c", {tmp.c_path("aarch64-linux-gnu-gcc"), "-mcpu=cortex-a72.cortex-a53"});
    ZEXPECT(
        contains(f.toolchain.render_for_clang(arm, f.db.spelling(arm.file)), "-mcpu=cortex-a72"));
    ZEXPECT(adjacent(f.resolve(arm), "-target-cpu", "cortex-a72"));
}

ZEST_CASE(ReadAsClangUnknownNames) {
    // Only the option naming what clang's target does not know goes: an
    // architecture x86 knows that it knows as no tuning CPU stays for
    // -march; one -mabi overriding another goes, and then so does that.
    auto x86 =
        add("a.c", {tmp.c_path("x86_64-linux-gnu-gcc"), "-march=x86-64-v3", "-mtune=x86-64-v3"});
    auto argv = f.toolchain.render_for_clang(x86, f.db.spelling(x86.file));
    ZEXPECT(contains(argv, "-march=x86-64-v3"));
    ZEXPECT(!contains(argv, "-mtune=x86-64-v3"));
    ZEXPECT(adjacent(f.resolve(x86), "-target-cpu", "x86-64-v3"));

    auto abi = add("b.c", {tmp.c_path("aarch64-linux-gnu-gcc"), "-mabi=ilp64x", "-mabi=lp64"});
    argv = f.toolchain.render_for_clang(abi, f.db.spelling(abi.file));
    ZEXPECT(!contains(argv, "-mabi=lp64"));
    ZEXPECT(!contains(argv, "-mabi=ilp64x"));
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
    ZASSERT(fs::remove(driver));
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

    auto cores = sys::parallelism();
    llvm::SmallVector<CommandRef> refs;
    for(unsigned i = 0; i < 2 * cores; i += 1) {
        auto depth = std::format("-ftemplate-depth={}", 100 + i);
        refs.push_back(add(std::format("{}.cpp", i), {driver.c_str(), depth.c_str()}));
    }
    f.toolchain.warm(refs);
    ZEXPECT(f.toolchain.query_count() == std::size_t(2 * cores));

    auto counts = vfs::read_text(tmp.path("counts"));
    ZASSERT(counts);
    llvm::SmallVector<llvm::StringRef> lines;
    llvm::StringRef(*counts).split(lines, '\n', -1, false);
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

ZEST_CASE(XclangAfterUserContent, skip = Windows) {
    // As CMake builds a target's PCH: the driver passes the -Xclang values
    // on after the command's own -include, which the PCH header needs first.
    auto driver = fake_clang(
        R"( "/usr/bin/clang-22" "-cc1" "-triple" "x86_64-unknown-linux-gnu" "-include" "/build/cmake_pch.hxx" "-x" "c++-header")");
    auto argv = f.resolve(add("a.cpp",
                              {driver.c_str(),
                               "-include",
                               "/src/global.h",
                               "-Xclang",
                               "-include",
                               "-Xclang",
                               "/build/cmake_pch.hxx"}));
    auto at = [&](std::string_view arg) {
        return ranges::find(argv, arg) - argv.begin();
    };
    ZEXPECT(contains(argv, "/build/cmake_pch.hxx"));
    ZEXPECT(at("/src/global.h") < at("/build/cmake_pch.hxx"));
}

ZEST_CASE(PrecompileFullBMI, skip = Windows) {
    auto driver = fake_clang(
        R"( "/usr/bin/clang-22" "-cc1" "-triple" "x86_64-unknown-linux-gnu" "-fmodules-reduced-bmi")");

    // Whether the module unit's cc1 asks for a reduced BMI.
    auto reduced = [&](std::vector<const char*> flags) {
        flags.insert(flags.begin(), driver.c_str());
        flags.push_back(tmp.c_path("a.cppm"));
        ToolchainEntries fresh;
        auto ref = fresh.add(tmp.root, tmp.path("a.cppm"), flags);
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

ZEST_CASE(OtherBuiltinHeadersStay, skip = Windows) {
    /// Another clang's builtin headers stay right after ours: a header only
    /// it has is found there, one ours have as well stays hidden.
    tmp.touch("clang-22/include/stddef.h");
    tmp.touch("clang-22/include/xray/xray_interface.h");
    auto theirs = tmp.path("clang-22/include");
    auto driver = fake_clang(std::format(
        R"( "/usr/bin/clang-22" "-cc1" "-triple" "x86_64-unknown-linux-gnu" "-resource-dir" "{0}" "-internal-isystem" "{0}/include")",
        tmp.path("clang-22")));
    auto ref = add("a.cpp", {driver.c_str()});
    auto argv = f.resolve(ref);
    auto builtin_headers = path::join(resource_dir(), "include");
    auto ours = ranges::find_if(argv, [&](llvm::StringRef arg) { return arg == builtin_headers; });
    ZASSERT(argv.end() - ours > 2);
    ZEXPECT(ours[1] == "-internal-isystem"sv);
    ZEXPECT(ours[2] == theirs);

    auto hidden = f.toolchain.render(ref, f.db.spelling(ref.file)).hidden;
    ZEXPECT(hidden.hides(path::join(theirs, "stddef.h")));
    ZEXPECT(!hidden.hides(path::join(theirs, "xray", "xray_interface.h")));

    /// A command another tool reads, which could not hide them, leaves
    /// them out.
    auto converted = f.toolchain.render(ref, f.db.spelling(ref.file), {.shadowed_dirs = false});
    ZEXPECT(!contains(converted.argv, theirs));
    ZEXPECT(adjacent(converted.argv, "-internal-isystem", builtin_headers));
}

ZEST_CASE(AliasedBuiltinHeaders, skip = Windows) {
    /// Directories of builtin headers the job names twice, by a symlink as
    /// well (CPATH's -I): clice's own, and another clang's, whose files
    /// clang's own have stay hidden.
    tmp.touch("clang-22/include/__stddef_max_align_t.h");
    tmp.touch("clang-22/include/stddef.h");
    auto theirs = tmp.path("clang-22/include");
    auto own = tmp.path("own");
    auto alias = tmp.path("alias");
    ZASSERT(!fs::create_directory_symlink(resource_dir(), own));
    ZASSERT(!fs::create_directory_symlink(theirs, alias));
    auto driver = fake_clang(std::format(
        R"( "/usr/bin/clang-22" "-cc1" "-triple" "x86_64-unknown-linux-gnu" "-resource-dir" "{0}" "-internal-isystem" "{0}/include" "-I" "{1}" "-internal-isystem" "{2}")",
        own,
        alias,
        theirs));
    auto ref = add("a.cpp", {driver.c_str()});
    ZASSERT(f.toolchain.resolve(ref.command, ref.input));
    auto hidden = f.toolchain.render(ref, f.db.spelling(ref.file)).hidden;
    ZEXPECT(hidden.hides(path::join(theirs, "stddef.h")));
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
    ZASSERT(fs::remove(mingw));
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
