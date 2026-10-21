module;

#include "modules/prelude.h"

module clice;

import :command.argument_parser;
import :command.command;
import :command.toolchain;
import :unittests.utils.temp_dir;
import :unittests.utils;
import :vfs.path;

namespace clice::testing {
namespace {

using namespace std::string_view_literals;

ZEST_SUITE(ToolchainTests) {

/// A database wrapping test entries — the unit tests' handle on the two
/// toolchain layers.
struct Fixture {
    FileTable file_table;
    CompilationDatabase db{file_table};

    CommandRef add(llvm::StringRef directory,
                   llvm::StringRef file,
                   llvm::ArrayRef<const char*> arguments) {
        db.add_command(directory, file, arguments);
        auto& entry = db.candidate_entries(file).front();
        return {entry.file,
                entry.config,
                db.input_kind(entry.config, file),
                CommandSource::CDBExact};
    }

    std::string key(const CommandRef& ref) {
        return db.toolchain().probe_key_for(ref.config, ref.input);
    }

    /// The command clice compiles `ref` with, resolved through the probe.
    std::vector<const char*> resolve(const CommandRef& ref) {
        ZASSERT(db.toolchain().resolve(ref.config, ref.input));
        return db.render(ref);
    }
};

TempDir tmp;
Fixture f;

/// `arguments`, then a source file named `file` in the temp directory, as
/// that file's command.
CommandRef add(llvm::StringRef file, std::vector<const char*> arguments) {
    arguments.push_back(tmp.c_path(file));
    return f.add(tmp.root.str(), tmp.path(file), arguments);
}

/// An executable shell script `name` in the temp directory, standing in
/// for a compiler driver.
std::string fake_driver(llvm::StringRef name, llvm::StringRef script) {
    auto driver = tmp.path(name);
    ZASSERT(!vfs::write(driver, "#!/bin/sh\n" + script.str()));
    ZASSERT(
        !llvm::sys::fs::setPermissions(driver, llvm::sys::fs::all_read | llvm::sys::fs::all_exe));
    return driver;
}

/// A clang driver that prints `job` as the job it runs, and C++14's macro.
std::string fake_clang(llvm::StringRef job, llvm::StringRef name = "clang") {
    return fake_driver(name,
                       std::format(R"(echo '{}' >&2
echo '#define __cplusplus 201402L'
)",
                                   job));
}

/// A GCC driver that prints `report` on stderr, and C++14's macro.
std::string fake_gcc(llvm::StringRef name, llvm::StringRef report) {
    return fake_driver(name,
                       std::format(R"(cat >&2 <<'REPORT'
{}
REPORT
echo '#define __cplusplus 201402L'
)",
                                   report));
}

/// An nvcc whose dryrun names `host` as its host compiler.
std::string fake_nvcc(llvm::StringRef host) {
    return fake_driver("nvcc",
                       std::format(R"(echo '#$ {} -c' >&2
)",
                                   host));
}

/// Whether `argv` holds `first` directly followed by `second`.
bool adjacent(llvm::ArrayRef<const char*> argv, llvm::StringRef first, llvm::StringRef second) {
    for(std::size_t i = 0; i + 1 < argv.size(); i += 1) {
        if(argv[i] == first && argv[i + 1] == second) {
            return true;
        }
    }
    return false;
}

bool contains(llvm::ArrayRef<const char*> argv, llvm::StringRef arg) {
    return std::ranges::contains(argv, arg);
}

void EXPECT_FAMILY(llvm::StringRef name, CompilerFamily family) {
    ZASSERT(Toolchain::driver_family(name) == family);
};

ZEST_CASE(Family) {
    using enum CompilerFamily;

    EXPECT_FAMILY("gcc", GCC);
    EXPECT_FAMILY("g++", GCC);
    EXPECT_FAMILY("cc", GCC);
    EXPECT_FAMILY("c++", GCC);
    EXPECT_FAMILY("gcc-13", GCC);
    EXPECT_FAMILY("g++-13.2", GCC);
    EXPECT_FAMILY("x86_64-linux-gnu-g++-14", GCC);
    EXPECT_FAMILY("arm-none-eabi-gcc", GCC);

    EXPECT_FAMILY("clang", Clang);
    EXPECT_FAMILY("clang++", Clang);
    EXPECT_FAMILY("clang.exe", Clang);
    EXPECT_FAMILY("clang++.exe", Clang);
    EXPECT_FAMILY("clang-20", Clang);
    EXPECT_FAMILY("clang-20.exe", Clang);
    EXPECT_FAMILY("clang++-21", Clang);
    EXPECT_FAMILY("clang-cl", ClangCL);
    EXPECT_FAMILY("clang-cl-20", ClangCL);
    EXPECT_FAMILY("clang-cl-20.exe", ClangCL);

    EXPECT_FAMILY("cl.exe", MSVC);
    /// Windows casing is free-form: CL.exe is how msbuild spells it.
    EXPECT_FAMILY("CL.exe", MSVC);
    EXPECT_FAMILY("CL.EXE", MSVC);
    EXPECT_FAMILY("Clang-Cl.exe", ClangCL);
    EXPECT_FAMILY("nvcc", NVCC);
    EXPECT_FAMILY("icx", Intel);
    EXPECT_FAMILY("icc", Intel);
    EXPECT_FAMILY("icpc", Intel);
    EXPECT_FAMILY("dpcpp", Intel);

    EXPECT_FAMILY("zig", Zig);
    EXPECT_FAMILY("zig.exe", Zig);
};

ZEST_CASE(InitiallyEmpty) {
    ZEXPECT(!f.db.toolchain().has_cache());
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
    /// them would spawn one probe per file instead of one per toolchain.
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
    /// file would spawn one probe per unit of the kernel.
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
    /// Debug info and section layout never change how a driver sets up the
    /// parse; stack protection does, through its macros.
    auto base = f.add("/fake", "/tmp/a.cpp", {"g++", "-std=c++23", "/tmp/a.cpp"});
    auto codegen = f.add("/fake",
                         "/tmp/b.cpp",
                         {"g++", "-std=c++23", "-g", "-ffunction-sections", "/tmp/b.cpp"});
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

ZEST_CASE(KeyTracksConfigFile) {
    /// A --config path resolves against the compilation directory, so
    /// identical commands in different directories must not share a probe.
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

/// A clang job as `-v` prints it.
constexpr static llvm::StringRef clang_job =
    R"( "/usr/bin/clang-22" "-cc1" "-triple" "x86_64-unknown-linux-gnu" "-std=c++23")";

ZEST_CASE(FailedProbeCooldown, skip = Windows) {
    // A failed probe is cached: within the cooldown the driver is not
    // spawned again, so removing it changes nothing.
    auto driver = fake_driver("clang", "exit 1\n");
    auto ref = add("a.cpp", {driver.c_str()});

    auto first = f.db.toolchain().resolve(ref.config, ref.input);
    ZASSERT(!first.has_value());
    ZEXPECT(f.db.toolchain().probe_count() == std::size_t(0));
    ZEXPECT(f.db.toolchain().failed_count() == std::size_t(1));

    ZASSERT(!vfs::remove(driver));
    auto second = f.db.toolchain().resolve(ref.config, ref.input);
    ZASSERT(!second.has_value());
    ZEXPECT(second.error() == first.error());
}

ZEST_CASE(FailedProbeRetries, skip = Windows) {
    // A transient driver failure must not poison the key for the session:
    // past the cooldown, resolve() and warm() query the driver again.
    f.db.toolchain().set_failed_retry(std::chrono::seconds(0));
    auto a = add("a.cpp", {tmp.c_path("clang"), "-std=c++17"});
    auto b = add("b.cpp", {tmp.c_path("clang"), "-std=c++20"});
    llvm::SmallVector<CommandRef> refs = {a, b};
    f.db.warm(refs);
    ZEXPECT(f.db.toolchain().failed_count() == std::size_t(2));

    fake_clang(clang_job);
    ZEXPECT(f.db.toolchain().resolve(a.config, a.input).has_value());
    ZEXPECT(f.db.toolchain().failed_count() == std::size_t(1));
    f.db.warm(refs);
    ZEXPECT(f.db.toolchain().failed_count() == std::size_t(0));
    ZEXPECT(f.db.toolchain().probe_count() == std::size_t(2));
}

ZEST_CASE(NoCompilerJob, skip = Windows) {
    /// A driver that is neither GCC nor clang (Intel's icc).
    auto ref = add("a.cpp", {fake_driver("icc", "").c_str()});
    auto resolved = f.db.toolchain().resolve(ref.config, ref.input);
    ZASSERT(!resolved);
    ZEXPECT(llvm::StringRef(resolved.error()).contains("runs no compiler job"));
}

ZEST_CASE(WarmPartialFailure, skip = Windows) {
    auto driver = fake_clang(clang_job);
    auto good = add("a.cpp", {driver.c_str()});
    auto bad = add("b.cpp", {"clice-nonexistent-driver"});

    llvm::SmallVector<CommandRef> refs = {good, bad};
    f.db.warm(refs);

    // The successful query is cached; the failed one is negatively cached
    // so later resolve() calls fail fast without re-probing the driver.
    ZEXPECT(f.db.toolchain().probe_count() == std::size_t(1));
    ZEXPECT(f.db.toolchain().failed_count() == std::size_t(1));
    ZEXPECT(f.db.toolchain().resolve(good.config, good.input).has_value());
    ZEXPECT(!f.db.toolchain().resolve(bad.config, bad.input).has_value());
    ZEXPECT(f.db.toolchain().failed_count() == std::size_t(1));
}

ZEST_CASE(WarmDedupesProbes, skip = Windows) {
    auto driver = fake_clang(clang_job);
    auto a = add("a.cpp", {driver.c_str(), "-std=c++23"});
    auto b = add("b.cpp", {driver.c_str(), "-std=c++23"});
    auto c = add("c.cpp", {driver.c_str(), "-std=c++17"});

    llvm::SmallVector<CommandRef> refs = {a, b, c};
    f.db.warm(refs);
    ZEXPECT(f.db.toolchain().probe_count() == std::size_t(2));

    // With the driver gone, only the probe cache can still resolve.
    ZASSERT(!vfs::remove(driver));
    ZEXPECT(f.db.toolchain().resolve(a.config, a.input).has_value());
}

ZEST_CASE(WarmBoundsDrivers, skip = Windows) {
    // Each driver records how many drivers run beside it: twice as many
    // probes as the process has CPUs never run more drivers than CPUs.
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
    f.db.warm(refs);
    ZEXPECT(f.db.toolchain().probe_count() == std::size_t(2 * cores));

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
    /// What only the probe asked for goes, and so does what the linked cc1
    /// cannot take: a newer clang's flag with its value, a codegen flag's
    /// newer value, a CPU name the target does not know.
    auto driver = fake_clang(
        R"( "/usr/bin/clang-22" "-cc1" "-triple" "x86_64-unknown-linux-gnu" "-E" "-dM" "-v" "-main-file-name" "probe.cpp" "-clice-future-flag" "val" "-mframe-pointer=non-leaf-no-reserve" "-target-cpu" "clice-future-cpu" "-fmodules-reduced-bmi" "-fmodule-output=/tmp/probe.pcm" "-o" "-" "-x" "c++" "/tmp/probe.cpp")");
    auto argv = f.resolve(add("dir/a.cpp", {driver.c_str()}));

    ZEXPECT(contains(argv, "-fmodules-reduced-bmi"));
    ZEXPECT(std::ranges::count(argv, "-fsyntax-only"sv) == 1);
    ZEXPECT(std::ranges::count(argv, "-main-file-name"sv) == 1);
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
        ZEXPECT(!arg.ends_with("probe.cpp"));
    }
    /// The command names no standard: the one the driver compiles in.
    ZEXPECT(contains(argv, "-std=gnu++14"));
}

ZEST_CASE(ClangJobKeepsStandard, skip = Windows) {
    auto driver = fake_clang(clang_job);
    auto argv = f.resolve(add("a.cpp", {driver.c_str(), "-std=c++23"}));
    ZEXPECT(contains(argv, "-std=c++23"));
    ZEXPECT(!contains(argv, "-std=gnu++14"));
}

/// A cl answering the macro probe as cl 19.44 does, and rejecting a flag
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
    for(llvm::StringRef arg: llvm::reverse(argv)) {
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

ZEST_CASE(UserContentAttached, skip = Windows) {
    // The probe never sees user content; the render puts it back on the
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
        Fixture fresh;
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
    // (the probe hands it none), also when warmed (the driver gone, only the
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
    f.db.warm(refs);
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
    /// A GCC whose report names its target, a macro of its specs, a CPU
    /// clang does not know, and a search list with its private include
    /// directory and a framework directory.
    auto frameworks = tmp.path("sysroot/Frameworks");
    auto driver = fake_gcc("x86_64-linux-gnu-g++",
                           std::format(R"(Target: x86_64-linux-gnu
 /usr/libexec/gcc/x86_64-linux-gnu/13/cc1plus -E -quiet -v -D_REENTRANT probe.cpp -mtune=intel -march=x86-64 -dM
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
    ZEXPECT(!std::ranges::any_of(argv, [](llvm::StringRef arg) {
        return arg.starts_with("-debug-info-kind");
    }));

    /// GCC's list in GCC's order, clang's builtin headers in place of
    /// GCC's own, and nothing of clang's own idea of the system headers.
    std::vector<std::string> dirs;
    for(std::size_t i = 0; i + 1 < argv.size(); i += 1) {
        if(argv[i] == "-internal-isystem"sv || argv[i] == "-internal-externc-isystem"sv) {
            dirs.push_back(argv[i + 1]);
        }
    }
    std::vector<std::string> expected = {tmp.path("gcc/include/c++"),
                                         path::join(resource_dir(), "include"),
                                         tmp.path("sysroot/usr/include")};
    ZEXPECT(dirs == expected);
    ZEXPECT(adjacent(argv, "-internal-iframework", frameworks));
}

ZEST_CASE(GCCTargetNames, skip = Windows) {
    auto render = [&](llvm::StringRef target, llvm::StringRef options) {
        auto driver = fake_gcc(target.str() + "-g++",
                               std::format(R"(Target: {0}
 /usr/libexec/gcc/{0}/13/cc1plus -E -quiet -v probe.cpp {1} -dM)",
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
}

ZEST_CASE(GCCSeesBareLTO, skip = Windows) {
    /// clang spells LTO -flto=full, which GCC rejects.
    auto driver = fake_driver("g++", R"(case " $* " in
*" -flto=full "*)
  echo "cc1plus: error: unrecognized argument to '-flto=' option: 'full'" >&2
  exit 1 ;;
esac
echo 'Target: x86_64-linux-gnu' >&2
echo ' /usr/libexec/gcc/x86_64-linux-gnu/13/cc1plus -E -quiet probe.cpp -flto -dM' >&2
)");
    auto ref = add("a.cpp", {driver.c_str(), "-flto=auto", "-c"});
    ZEXPECT(f.db.toolchain().resolve(ref.config, ref.input).has_value());
}

ZEST_CASE(GCCFailureRejected, skip = Windows) {
    /// GCC prints its job and search list before its compile fails, but
    /// LIBRARY_PATH and the macros only after it succeeds.
    auto driver = fake_driver("g++", R"(echo 'Target: x86_64-linux-gnu' >&2
echo ' /usr/libexec/gcc/x86_64-linux-gnu/13/cc1plus -E -quiet -imacros missing.h probe.cpp -dM' >&2
echo 'cc1plus: fatal error: missing.h: No such file or directory' >&2
exit 1
)");
    auto ref = add("a.cpp", {driver.c_str(), "-c"});
    auto resolved = f.db.toolchain().resolve(ref.config, ref.input);
    ZASSERT(!resolved);
    ZEXPECT(llvm::StringRef(resolved.error()).contains("missing.h"));
}

ZEST_CASE(NVCCHostLanguage, skip = Windows) {
    /// nvcc runs its host compiler in the input's language: GCC's C++
    /// driver adds _GNU_SOURCE, its C driver does not.
    auto host = fake_driver("g++", R"(echo 'Target: x86_64-linux-gnu' >&2
case " $* " in
*" c++ "*) echo ' /usr/libexec/gcc/x86_64-linux-gnu/13/cc1plus -E -quiet -D_GNU_SOURCE probe.cpp -dM' >&2 ;;
*) echo ' /usr/libexec/gcc/x86_64-linux-gnu/13/cc1 -E -quiet probe.c -dM' >&2 ;;
esac
)");
    auto nvcc = fake_nvcc(host);
    ZEXPECT(adjacent(f.resolve(add("a.cpp", {nvcc.c_str(), "-c"})), "-D", "_GNU_SOURCE"));
    ZEXPECT(!adjacent(f.resolve(add("b.c", {nvcc.c_str(), "-c"})), "-D", "_GNU_SOURCE"));
}

ZEST_CASE(NVCCHostSysroot, skip = Windows) {
    /// A cross host compiler's headers come from the sysroot the command
    /// hands it.
    auto host = fake_driver("aarch64-linux-gnu-g++", R"(for arg in "$@"; do
  case "$arg" in --sysroot=*) root="${arg#--sysroot=}" ;; esac
done
echo 'Target: aarch64-linux-gnu' >&2
echo ' /usr/libexec/gcc/aarch64-linux-gnu/13/cc1plus -E -quiet probe.cpp -dM' >&2
echo '#include <...> search starts here:' >&2
echo " $root/usr/include" >&2
echo 'End of search list.' >&2
)");
    auto nvcc = fake_nvcc(host);
    auto sysroot = tmp.path("rootfs");
    auto flag = "--sysroot=" + sysroot;
    auto argv = f.resolve(add("a.cpp", {nvcc.c_str(), "-Xcompiler", flag.c_str(), "-c"}));
    ZEXPECT(adjacent(argv, "-internal-isystem", sysroot + "/usr/include"));
}

ZEST_CASE(NVCCUnknownHost, skip = Windows) {
    /// A host compiler that is neither GCC nor clang.
    auto nvcc = fake_nvcc(fake_driver("icpc", ""));
    auto ref = add("a.cpp", {nvcc.c_str(), "-c"});
    auto resolved = f.db.toolchain().resolve(ref.config, ref.input);
    ZASSERT(!resolved);
    ZEXPECT(llvm::StringRef(resolved.error()).starts_with("Unsupported nvcc host compiler"));
}

/// `ref` under a rule turning the toolchain query off.
CommandRef unqueried(const CommandRef& ref) {
    return {ref.file,
            f.db.apply_rules(ref.config, {.query_toolchain = false}),
            ref.input,
            ref.source};
}

ZEST_CASE(UnqueriedRunsNothing, skip = Windows) {
    // Neither resolve() nor warm() runs the driver: clang's own builds the
    // job. Queried, the same command runs it.
    auto ran = tmp.path("ran");
    auto driver = fake_driver("g++", std::format("touch '{}'\n", ran));
    auto ref = add("a.cpp", {driver.c_str(), "-std=c++17"});
    auto off = unqueried(ref);
    ZEXPECT(f.key(off) != f.key(ref));
    ZEXPECT(f.db.entry_hash(off.config) != f.db.entry_hash(ref.config));

    llvm::SmallVector<CommandRef> refs = {off};
    f.db.warm(refs);
    auto argv = f.resolve(off);
    ZEXPECT(contains(argv, "-std=c++17"));
    ZEXPECT(adjacent(argv, "-resource-dir", resource_dir()));
    ZEXPECT(!vfs::exists(ran));

    ZEXPECT(!f.db.toolchain().resolve(ref.config, ref.input));
    ZEXPECT(vfs::exists(ran));
}

ZEST_CASE(UnqueriedCLRunsNothing, skip = Windows) {
    // Nor is cl asked its standard: clang's own cl mode reads the command.
    auto ran = tmp.path("ran");
    auto cl = fake_driver("cl", std::format("touch '{}'\n", ran));
    auto argv = f.resolve(unqueried(add("a.cpp", {cl.c_str(), "/std:c++17"})));
    ZEXPECT(contains(argv, "-std=c++17"));
    ZEXPECT(!vfs::exists(ran));
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

ZEST_CASE(UnqueriedNVCC) {
    // clang's own CUDA support reads an nvcc that is not here on the device
    // side, as for an nvcc clice runs; the tokens only nvcc's dryrun reads
    // are unknown to clang and leave its first job, the device one, alone.
    auto nvcc = tmp.path("nvcc");
    auto argv = f.resolve(unqueried(add("a.cu", {nvcc.c_str(), "-ccbin", "g++", "-c"})));
    ZEXPECT(contains(argv, "-fcuda-is-device"));
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

};  // ZEST_SUITE(ToolchainTests)

}  // namespace
}  // namespace clice::testing
