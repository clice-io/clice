#include <algorithm>
#include <chrono>
#include <optional>

#include "test/temp_dir.h"
#include "test/test.h"
#include "command/argument_parser.h"
#include "command/command.h"
#include "command/toolchain.h"
#include "compile/compilation.h"
#include "support/logging.h"

namespace clice::testing {
namespace {

using namespace std::string_view_literals;

TEST_SUITE(ToolchainTests) {

void EXPECT_FAMILY(llvm::StringRef name, CompilerFamily family) {
    ASSERT_EQ(Toolchain::driver_family(name), family);
};

TEST_CASE(Family) {
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

TEST_CASE(GCC, skip = !(CIEnvironment && (Windows || Linux))) {
    auto file = fs::createTemporaryFile("clice", "cpp");
    if(!file) {
        LOG_ERROR_RET(void(), "{}", file.error());
    }

    auto result = Toolchain::query(
        {"g++", "-std=c++23", "-resource-dir", resource_dir().data(), "-xc++", file->c_str()});
    ASSERT_TRUE(result.has_value());

    ASSERT_TRUE(result->size() > 2);
    ASSERT_EQ((*result)[1], "-cc1"sv);

    CompilationParams params;
    for(auto& arg: *result) {
        params.arguments.push_back(arg.c_str());
    }
    params.add_remapped_file(file->c_str(), R"(
            #include <print>
            int main() {
                std::println("Hello world!");
                return 0;
            }
        )");

    auto unit = compile(params);
    ASSERT_TRUE(unit.completed());
    ASSERT_TRUE(unit.diagnostics().empty());
};

TEST_CASE(Clang, skip = !CIEnvironment) {
    auto file = fs::createTemporaryFile("clice", "cpp");
    if(!file) {
        LOG_ERROR_RET(void(), "{}", file.error());
    }

    auto result = Toolchain::query(
        {"clang++", "-std=c++23", "-resource-dir", resource_dir().data(), "-xc++", file->c_str()});
    ASSERT_TRUE(result.has_value());

    ASSERT_TRUE(result->size() > 2);
    ASSERT_EQ((*result)[1], "-cc1"sv);

    CompilationParams params;
    for(auto& arg: *result) {
        params.arguments.push_back(arg.c_str());
    }
    params.add_remapped_file(file->c_str(), R"(
            #include <print>
            int main() {
                std::println("Hello world!");
                return 0;
            }
        )");

    auto unit = compile(params);
    ASSERT_TRUE(unit.completed());
    ASSERT_TRUE(unit.diagnostics().empty());
};

TEST_CASE(NVCC, skip = !(CIEnvironment && Linux)) {
    auto file = fs::createTemporaryFile("clice", "cu");
    if(!file) {
        LOG_ERROR_RET(void(), "{}", file.error());
    }

    auto result = Toolchain::query({"nvcc", "-resource-dir", resource_dir().data()}, file->c_str());
    ASSERT_TRUE(result.has_value());

    ASSERT_TRUE(result->size() > 2);
    ASSERT_EQ((*result)[1], "-cc1"sv);

    // The default view is the device pass — the __CUDA_ARCH__ world.
    EXPECT_TRUE(std::ranges::contains(*result, "-fcuda-is-device"));

    CompilationParams params;
    for(auto& arg: *result) {
        params.arguments.push_back(arg.c_str());
    }
    params.add_remapped_file(file->c_str(), R"(
            #ifndef __CUDACC__
            #error clang's CUDA wrapper presents __CUDACC__ to user code
            #endif
            __global__ void kern(float* p) { p[threadIdx.x] = 1.0f; }
            int main() {
                float* d = nullptr;
                cudaMalloc(&d, 16);
                kern<<<1, 1>>>(d);
                return 0;
            }
        )");

    auto unit = compile(params);
    ASSERT_TRUE(unit.completed());
    ASSERT_TRUE(unit.diagnostics().empty());
};

TEST_CASE(NVCCCudaHeader, skip = !(CIEnvironment && Linux)) {
    auto file = fs::createTemporaryFile("clice", "cuh");
    if(!file) {
        LOG_ERROR_RET(void(), "{}", file.error());
    }

    auto result = Toolchain::query({"nvcc", "-resource-dir", resource_dir().data()}, file->c_str());
    ASSERT_TRUE(result.has_value());

    ASSERT_TRUE(result->size() > 2);
    ASSERT_EQ((*result)[1], "-cc1"sv);
    EXPECT_TRUE(std::ranges::contains(*result, "-fcuda-is-device"));

    CompilationParams params;
    for(auto& arg: *result) {
        params.arguments.push_back(arg.c_str());
    }
    params.add_remapped_file(file->c_str(), R"(
            __device__ float scale(float* p) { return p[threadIdx.x]; }
        )");

    auto unit = compile(params);
    ASSERT_TRUE(unit.completed());
    ASSERT_TRUE(unit.diagnostics().empty());
};

TEST_CASE(NVCCViewSelector, skip = !(CIEnvironment && Linux)) {
    auto file = fs::createTemporaryFile("clice", "cu");
    if(!file) {
        LOG_ERROR_RET(void(), "{}", file.error());
    }

    auto has_define = [](llvm::ArrayRef<std::string> args, llvm::StringRef name) {
        return std::ranges::any_of(args, [&](llvm::StringRef arg) { return arg.contains(name); });
    };

    // The last view selector wins, as in clang's driver, and the injected
    // defines follow the selected pass (CUDA_DOUBLE_MATH_FUNCTIONS appears
    // only on nvcc's device preprocess line).
    auto device = Toolchain::query(
        {"nvcc", "--cuda-host-only", "--cuda-device-only", "-resource-dir", resource_dir().data()},
        file->c_str());
    ASSERT_TRUE(device.has_value());
    EXPECT_TRUE(std::ranges::contains(*device, "-fcuda-is-device"));
    EXPECT_TRUE(has_define(*device, "CUDA_DOUBLE_MATH_FUNCTIONS"));

    auto host = Toolchain::query(
        {"nvcc", "--cuda-device-only", "--cuda-host-only", "-resource-dir", resource_dir().data()},
        file->c_str());
    ASSERT_TRUE(host.has_value());
    EXPECT_FALSE(std::ranges::contains(*host, "-fcuda-is-device"));
    EXPECT_FALSE(has_define(*host, "CUDA_DOUBLE_MATH_FUNCTIONS"));
};

TEST_CASE(NVCCArchEdit, skip = !(CIEnvironment && Linux)) {
    auto file = fs::createTemporaryFile("clice", "cu");
    if(!file) {
        LOG_ERROR_RET(void(), "{}", file.error());
    }

    // An edit-appended -arch=<special> arrives as `--no-offload-arch=all`
    // plus the probe token; the dryrun's resolution must land after the
    // clear, or clang erases it again and falls back to its sm_52 default.
    // -arch=all runs one cicc per toolkit architecture and the newest wins.
    auto result = Toolchain::query(
        {"nvcc", "--no-offload-arch=all", "-arch=all", "-resource-dir", resource_dir().data()},
        file->c_str());
    ASSERT_TRUE(result.has_value());

    auto cpu = std::ranges::find(*result, "-target-cpu");
    ASSERT_TRUE(cpu != result->end() && cpu + 1 != result->end());
    EXPECT_NE(*(cpu + 1), "sm_52"sv);
};

TEST_CASE(NVCCHostInput, skip = !(CIEnvironment && Linux)) {
    auto file = fs::createTemporaryFile("clice", "cpp");
    if(!file) {
        LOG_ERROR_RET(void(), "{}", file.error());
    }

    // A host-language input compiles in a single host step: no CUDA mode,
    // but nvcc's injected identity macros still apply.
    auto result = Toolchain::query({"nvcc", "-resource-dir", resource_dir().data()}, file->c_str());
    ASSERT_TRUE(result.has_value());
    EXPECT_FALSE(std::ranges::contains(*result, "-fcuda-is-device"));

    CompilationParams params;
    for(auto& arg: *result) {
        params.arguments.push_back(arg.c_str());
    }
    params.add_remapped_file(file->c_str(), R"(
            #ifndef __NVCC__
            #error the host step keeps nvcc's identity macros
            #endif
            #ifdef __CUDACC__
            #error a host-language input is not a CUDA compile
            #endif
            int main() { return 0; }
        )");

    auto unit = compile(params);
    ASSERT_TRUE(unit.completed());
    ASSERT_TRUE(unit.diagnostics().empty());
};

/// A database wrapping test entries — the unit tests' handle on the two
/// toolchain layers.
struct Fixture {
    CompilationDatabase db;

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
};

TEST_CASE(InitiallyEmpty) {
    CompilationDatabase db;
    EXPECT_FALSE(db.toolchain().has_cache());
}

TEST_CASE(KeyIgnoresUserContent) {
    Fixture f;
    auto base = f.add("/fake", "/tmp/a.cpp", {"clang++", "-std=c++23", "/tmp/a.cpp"});
    auto user = f.add("/fake",
                      "/tmp/b.cpp",
                      {"clang++",
                       "-std=c++23",
                       "-I/usr/include",
                       "-DFOO=1",
                       "-include",
                       "foo.h",
                       "-isystem",
                       "/opt/include",
                       "/tmp/b.cpp"});
    EXPECT_EQ(f.key(base), f.key(user));
}

TEST_CASE(KeyTracksSemantics) {
    Fixture f;
    auto base = f.add("/fake", "/tmp/a.cpp", {"clang++", "-std=c++23", "/tmp/a.cpp"});

    auto driver = f.add("/fake", "/tmp/b.cpp", {"g++", "-std=c++23", "/tmp/b.cpp"});
    EXPECT_NE(f.key(base), f.key(driver));

    auto target = f.add("/fake",
                        "/tmp/c.cpp",
                        {"clang++", "-std=c++23", "--target=aarch64-linux-gnu", "/tmp/c.cpp"});
    EXPECT_NE(f.key(base), f.key(target));

    /// The language dimension: an -x selector and a C extension both
    /// change the key.
    auto lang = f.add("/fake", "/tmp/d.cpp", {"clang++", "-std=c++23", "-x", "c", "/tmp/d.cpp"});
    EXPECT_NE(f.key(base), f.key(lang));

    auto ext = f.add("/fake", "/tmp/e.c", {"clang++", "-std=c++23", "/tmp/e.c"});
    EXPECT_NE(f.key(base), f.key(ext));

    // Any non-user-content flag affects the key, not just toolchain options.
    auto semantic =
        f.add("/fake", "/tmp/g.cpp", {"clang++", "-std=c++23", "-fno-exceptions", "/tmp/g.cpp"});
    EXPECT_NE(f.key(base), f.key(semantic));
}

TEST_CASE(KeyTracksConfigFile) {
    /// A relative --config resolves against the compilation directory, so
    /// identical commands in different directories must not share a probe.
    Fixture f;
    auto a = f.add("/fake/a", "/tmp/a.cpp", {"clang++", "--config", "clang.cfg", "/tmp/a.cpp"});
    auto b = f.add("/fake/b", "/tmp/b.cpp", {"clang++", "--config", "clang.cfg", "/tmp/b.cpp"});
    EXPECT_NE(f.key(a), f.key(b));

    /// An absolute config file is directory-independent. Both paths are
    /// platform-native absolute (POSIX spellings are not absolute on
    /// Windows), and the driver is absolute too: a bare driver name is
    /// never cwd-exempt on Windows and would tie the key to the directory
    /// on its own.
    TempDir tmp;
    auto cfg = "--config=" + tmp.path("clang.cfg");
    auto driver = tmp.path("clang++");
    auto c = f.add("/fake/a", "/tmp/c.cpp", {driver.c_str(), cfg.c_str(), "/tmp/c.cpp"});
    auto d = f.add("/fake/b", "/tmp/d.cpp", {driver.c_str(), cfg.c_str(), "/tmp/d.cpp"});
    EXPECT_EQ(f.key(c), f.key(d));
}

TEST_CASE(QueryEmptyArgs) {
    EXPECT_FALSE(Toolchain::query({}).has_value());
}

TEST_CASE(QueryMissingDriver) {
    EXPECT_FALSE(Toolchain::query({"clice-nonexistent-driver"}).has_value());
}

TEST_CASE(ParseCC1FirstLine) {
    auto args = Toolchain::parse_cc1(R"(clang version 22.0.0
Target: x86_64-unknown-linux-gnu
 "/usr/bin/clang-22" "-cc1" "-triple" "x86_64-unknown-linux-gnu" "-std=c++23" "a.cpp"
 "/usr/bin/clang-22" "-cc1" "-std=c++17" "b.cpp"
 "/usr/bin/ld" "-o" "a.out")");

    std::vector<std::string> expected =
        {"/usr/bin/clang-22", "-cc1", "-triple", "x86_64-unknown-linux-gnu", "-std=c++23", "a.cpp"};
    EXPECT_EQ(args, expected);

    EXPECT_TRUE(Toolchain::parse_cc1("clang version 22.0.0\nno cc1 line here").empty());
}

TEST_CASE(ParseCC1DropsUnknown) {
    // A newer external driver may emit cc1 flags our linked clang does not
    // know; they must be dropped together with their values (greedy_unknown)
    // instead of the values being misparsed as input files.
    auto args = Toolchain::parse_cc1(
        R"( "/usr/bin/clang-22" "-cc1" "-clice-future-flag" "val1" "val2" "-std=c++23")");

    std::vector<std::string> expected = {"/usr/bin/clang-22", "-cc1", "-std=c++23"};
    EXPECT_EQ(args, expected);
}

TEST_CASE(ParseCC1DropsCodegen) {
    auto args = Toolchain::parse_cc1(
        R"( "/usr/bin/clang-22" "-cc1" "-mframe-pointer=non-leaf-no-reserve" "-std=c++23")");

    std::vector<std::string> expected = {"/usr/bin/clang-22", "-cc1", "-std=c++23"};
    EXPECT_EQ(args, expected);
}

/// Canned `-###` output covering version-skew hardening: an unknown future
/// flag with a value, plus BMI emission flags that query() must strip.
constexpr static llvm::StringRef fake_cc1_line =
    R"( "/usr/bin/clang-22" "-cc1" "-triple" "x86_64-unknown-linux-gnu" "-fmodules-reduced-bmi" "-fmodule-output=/tmp/probe.pcm" "-clice-future-flag" "val" "-std=c++23")";

/// Create an executable shell script named `*.clang` (detected as the Clang
/// family) that prints a canned `-###` line to stderr, standing in for a
/// real external driver.
std::optional<std::string> create_fake_clang(llvm::StringRef cc1_line) {
    auto file = fs::createTemporaryFile("clice-fake", "clang");
    if(!file)
        return std::nullopt;

    auto script = "#!/bin/sh\necho '" + cc1_line.str() + "' >&2\n";
    if(!fs::write(*file, script))
        return std::nullopt;

    if(fs::setPermissions(*file, fs::all_read | fs::all_exe))
        return std::nullopt;

    return *file;
}

TEST_CASE(QueryFakeDriver, skip = Windows) {
    auto driver = create_fake_clang(fake_cc1_line);
    ASSERT_TRUE(driver.has_value());

    auto result = Toolchain::query({driver->c_str()}, "/tmp/a.cpp");
    ASSERT_TRUE(result.has_value());

    // Unknown flag + value dropped, BMI emission flags stripped, known kept.
    std::vector<std::string> expected = {"/usr/bin/clang-22",
                                         "-cc1",
                                         "-triple",
                                         "x86_64-unknown-linux-gnu",
                                         "-std=c++23"};
    EXPECT_EQ(*result, expected);
}

TEST_CASE(FailedQueryRetries, skip = Windows) {
    // A transient driver failure must not poison the key for the session:
    // the negative cache expires after the retry cooldown, and the next
    // resolve re-queries the real driver.
    TempDir tmp;
    auto driver = tmp.path("cc.clang");
    auto src = tmp.path("a.cpp");
    auto script = "#!/bin/sh\necho '" + std::string(fake_cc1_line) + "' >&2\n";

    Fixture eager;
    eager.db.toolchain().set_failed_retry(std::chrono::seconds(0));
    auto ref = eager.add(tmp.root.str(), src, {driver.c_str(), "-std=c++23", src.c_str()});

    ASSERT_FALSE(eager.db.toolchain().resolve(ref.config, ref.input).has_value());
    EXPECT_EQ(eager.db.toolchain().failed_count(), std::size_t(1));

    // The driver appears; the expired entry re-queries and succeeds.
    ASSERT_TRUE(fs::write(driver, script));
    ASSERT_TRUE(!fs::setPermissions(driver, fs::all_read | fs::all_exe));
    ASSERT_TRUE(eager.db.toolchain().resolve(ref.config, ref.input).has_value());
    EXPECT_EQ(eager.db.toolchain().failed_count(), std::size_t(0));
    EXPECT_TRUE(eager.db.toolchain().has_cache());

    // Control: within the cooldown the cached failure replays untouched
    // even after the driver appears.
    auto late = tmp.path("cc2.clang");
    Fixture patient;
    auto ref2 = patient.add(tmp.root.str(), src, {late.c_str(), "-std=c++23", src.c_str()});

    ASSERT_FALSE(patient.db.toolchain().resolve(ref2.config, ref2.input).has_value());
    ASSERT_TRUE(fs::write(late, script));
    ASSERT_TRUE(!fs::setPermissions(late, fs::all_read | fs::all_exe));
    ASSERT_FALSE(patient.db.toolchain().resolve(ref2.config, ref2.input).has_value());
    EXPECT_EQ(patient.db.toolchain().failed_count(), std::size_t(1));
    EXPECT_FALSE(patient.db.toolchain().has_cache());
}

TEST_CASE(WarmRetriesExpired, skip = Windows) {
    // warm() honors the same negative-cache expiry as resolve(): a
    // cooled-down failure re-queries instead of being skipped forever.
    TempDir tmp;
    auto driver = tmp.path("cc.clang");
    auto src = tmp.path("a.cpp");

    Fixture f;
    f.db.toolchain().set_failed_retry(std::chrono::seconds(0));
    auto ref = f.add(tmp.root.str(), src, {driver.c_str(), "-std=c++23", src.c_str()});
    llvm::SmallVector<CommandRef> refs = {ref};

    f.db.warm(refs);
    EXPECT_EQ(f.db.toolchain().failed_count(), std::size_t(1));
    EXPECT_FALSE(f.db.toolchain().has_cache());

    auto script = "#!/bin/sh\necho '" + std::string(fake_cc1_line) + "' >&2\n";
    ASSERT_TRUE(fs::write(driver, script));
    ASSERT_TRUE(!fs::setPermissions(driver, fs::all_read | fs::all_exe));

    f.db.warm(refs);
    EXPECT_EQ(f.db.toolchain().failed_count(), std::size_t(0));
    EXPECT_TRUE(f.db.toolchain().has_cache());
}

TEST_CASE(WarmPartialFailure, skip = Windows) {
    auto driver = create_fake_clang(fake_cc1_line);
    ASSERT_TRUE(driver.has_value());

    Fixture f;
    auto good = f.add("/tmp", "/tmp/a.cpp", {driver->c_str(), "-std=c++23", "/tmp/a.cpp"});
    auto bad =
        f.add("/tmp", "/tmp/b.cpp", {"clice-nonexistent-driver", "-std=c++23", "/tmp/b.cpp"});

    llvm::SmallVector<CommandRef> refs = {good, bad};
    f.db.warm(refs);

    // The successful query is cached; the failed one is negatively cached
    // so later resolve() calls fail fast without re-probing the driver.
    EXPECT_EQ(f.db.toolchain().probe_count(), std::size_t(1));
    EXPECT_EQ(f.db.toolchain().failed_count(), std::size_t(1));

    auto resolved = f.db.toolchain().resolve(good.config, good.input);
    ASSERT_TRUE(resolved.has_value());
    EXPECT_TRUE(f.db.toolchain().resolved(*resolved).is_cc1);
    EXPECT_FALSE(f.db.toolchain().resolve(bad.config, bad.input).has_value());
    EXPECT_EQ(f.db.toolchain().failed_count(), std::size_t(1));
}

TEST_CASE(ResolveFailNegativeCache, skip = Windows) {
    // A fake driver whose -### output contains no cc1 line, so the query fails.
    auto driver = create_fake_clang("this is not a cc1 line");
    ASSERT_TRUE(driver.has_value());

    Fixture f;
    auto ref = f.add("/tmp", "/tmp/a.cpp", {driver->c_str(), "-std=c++23", "/tmp/a.cpp"});

    auto first = f.db.toolchain().resolve(ref.config, ref.input);
    ASSERT_FALSE(first.has_value());
    EXPECT_EQ(f.db.toolchain().probe_count(), std::size_t(0));
    EXPECT_EQ(f.db.toolchain().failed_count(), std::size_t(1));

    // Remove the driver: a re-probe would now fail differently ("not found or
    // not executable"), so getting the original error back proves the second
    // resolve() hit the negative cache without spawning the driver again.
    ASSERT_TRUE(!fs::remove(*driver));
    auto second = f.db.toolchain().resolve(ref.config, ref.input);
    ASSERT_FALSE(second.has_value());
    EXPECT_EQ(second.error(), first.error());
    EXPECT_EQ(f.db.toolchain().failed_count(), std::size_t(1));
}

TEST_CASE(ResolveReplacesResourceDir, skip = Windows) {
    constexpr llvm::StringRef line =
        R"( "/usr/bin/clang-22" "-cc1" "-resource-dir" "/clice-fake/lib/clang/22" "-internal-isystem" "/clice-fake/lib/clang/22/include" "-std=c++23")";
    auto driver = create_fake_clang(line);
    ASSERT_TRUE(driver.has_value());

    Fixture f;
    auto ref = f.add("/tmp", "/tmp/a.cpp", {driver->c_str(), "-std=c++23", "/tmp/a.cpp"});
    ASSERT_TRUE(f.db.toolchain().resolve(ref.config, ref.input).has_value());

    // The external driver's resource dir is rewritten to ours, including
    // derived paths sharing the prefix.
    auto argv = f.db.render(ref);
    auto expected_include = resource_dir().str() + "/include";
    EXPECT_TRUE(std::ranges::contains(argv, resource_dir()));
    EXPECT_TRUE(std::ranges::contains(argv, llvm::StringRef(expected_include)));
    for(llvm::StringRef arg: argv) {
        EXPECT_FALSE(arg.starts_with("/clice-fake"));
    }
}

/// A fake driver whose -### line reflects the -resource-dir it was invoked
/// with, falling back to `fallback_dir` when none was passed — the same
/// observable difference a real driver shows between a forced resource dir
/// and a derived one. An empty `name` produces the usual `*.clang` temp
/// file; otherwise the script gets that exact file name so the target can
/// be derived from a prefixed driver spelling.
std::optional<std::string> create_echo_clang(llvm::StringRef fallback_dir, llvm::StringRef name) {
    std::string file;
    if(name.empty()) {
        auto temp = fs::createTemporaryFile("clice-fake", "clang");
        if(!temp)
            return std::nullopt;
        file = *temp;
    } else {
        llvm::SmallString<128> dir;
        if(llvm::sys::fs::createUniqueDirectory("clice-fake-driver", dir))
            return std::nullopt;
        file = (llvm::Twine(dir) + "/" + name).str();
    }

    auto script = R"(#!/bin/sh
rd=')" + fallback_dir.str() +
                  R"('
prev=""
for a in "$@"; do
  if [ "$prev" = "-resource-dir" ]; then rd="$a"; fi
  prev="$a"
done
echo " \"/usr/bin/clang-22\" \"-cc1\" \"-resource-dir\" \"$rd\" \"-internal-isystem\" \"$rd/include\" \"-std=c++23\"" >&2
)";
    if(!fs::write(file, script))
        return std::nullopt;

    if(fs::setPermissions(file, fs::all_read | fs::all_exe))
        return std::nullopt;

    return file;
}

/// The MinGW preservation tests share one shape: an external resource tree,
/// an echoing driver (deriving that tree when no -resource-dir is forced),
/// and the expectation that the resolved config keeps the external tree and
/// never mentions clice's own.
void EXPECT_KEEPS_EXTERNAL(llvm::StringRef driver_name,
                           llvm::ArrayRef<const char*> extra_flags,
                           bool warm_first = false) {
    llvm::SmallString<128> external_dir_buf;
    auto create_error =
        llvm::sys::fs::createUniqueDirectory("clice-external-resource", external_dir_buf);
    ASSERT_TRUE(!create_error);
    auto external_dir = external_dir_buf.str().str();
    auto driver = create_echo_clang(external_dir, driver_name);
    ASSERT_TRUE(driver.has_value());

    Fixture f;
    std::vector<const char*> arguments = {driver->c_str(), "-std=c++23"};
    arguments.insert(arguments.end(), extra_flags.begin(), extra_flags.end());
    arguments.push_back("/tmp/a.cpp");
    auto ref = f.add("/tmp", "/tmp/a.cpp", arguments);

    if(warm_first) {
        llvm::SmallVector<CommandRef> refs = {ref};
        f.db.warm(refs);
        // The driver disappears after warming: the resolve below can only
        // succeed from the warmed cache entry, never from a fresh query.
        fs::remove(*driver);
    }
    ASSERT_TRUE(f.db.toolchain().resolve(ref.config, ref.input).has_value());
    auto argv = f.db.render(ref);
    EXPECT_TRUE(std::ranges::contains(argv, llvm::StringRef(external_dir)));
    EXPECT_FALSE(std::ranges::contains(argv, resource_dir()));

    fs::remove(*driver);
    if(!driver_name.empty()) {
        llvm::sys::fs::remove(llvm::sys::path::parent_path(*driver));
    }
    llvm::sys::fs::remove(external_dir_buf);
}

TEST_CASE(ResolveKeepsExternalResource, skip = Windows) {
    EXPECT_KEEPS_EXTERNAL("", {"--target=x86_64-w64-windows-gnu"});
}

TEST_CASE(WarmKeepsExternalResource, skip = Windows) {
    EXPECT_KEEPS_EXTERNAL("", {"--target=x86_64-w64-windows-gnu"}, /*warm_first=*/true);
}

TEST_CASE(PrefixedDriverKeepsResource, skip = Windows) {
    EXPECT_KEEPS_EXTERNAL("x86_64-w64-mingw32-clang++", {});
}

TEST_CASE(LastTargetFlagWins, skip = Windows) {
    EXPECT_KEEPS_EXTERNAL("",
                          {"--target=x86_64-unknown-linux-gnu", "--target=x86_64-w64-windows-gnu"});
}

TEST_CASE(ResolveReplacesNonMingwResource, skip = Windows) {
    llvm::SmallString<128> external_dir_buf;
    auto create_error =
        llvm::sys::fs::createUniqueDirectory("clice-external-resource", external_dir_buf);
    ASSERT_TRUE(!create_error);
    auto external_dir = external_dir_buf.str().str();
    auto driver = create_echo_clang(external_dir, "");
    ASSERT_TRUE(driver.has_value());

    Fixture f;
    auto ref = f.add("/tmp", "/tmp/a.cpp", {driver->c_str(), "-std=c++23", "/tmp/a.cpp"});
    ASSERT_TRUE(f.db.toolchain().resolve(ref.config, ref.input).has_value());

    auto argv = f.db.render(ref);
    auto expected_include = resource_dir().str() + "/include";
    EXPECT_TRUE(std::ranges::contains(argv, resource_dir()));
    EXPECT_TRUE(std::ranges::contains(argv, llvm::StringRef(expected_include)));

    fs::remove(*driver);
    llvm::sys::fs::remove(external_dir_buf);
}

TEST_CASE(ResolveMainFileName, skip = Windows) {
    constexpr llvm::StringRef line =
        R"( "/usr/bin/clang-22" "-cc1" "-main-file-name" "probe.cpp" "-std=c++23")";
    auto driver = create_fake_clang(line);
    ASSERT_TRUE(driver.has_value());

    Fixture f;
    auto ref = f.add("/tmp", "/tmp/dir/a.cpp", {driver->c_str(), "-std=c++23", "/tmp/dir/a.cpp"});
    ASSERT_TRUE(f.db.toolchain().resolve(ref.config, ref.input).has_value());

    // The probe file's -main-file-name is stripped; the render re-injects
    // it with the real file's basename, exactly once.
    auto argv = f.db.render(ref);
    int injected = 0;
    for(std::size_t i = 0; i + 1 < argv.size(); i += 1) {
        if(argv[i] == "-main-file-name"sv) {
            EXPECT_EQ(llvm::StringRef(argv[i + 1]), "a.cpp");
            injected += 1;
        }
    }
    EXPECT_EQ(injected, 1);
}

TEST_CASE(ResolveKeepsSemanticFlags, skip = !CIEnvironment) {
    auto file = fs::createTemporaryFile("clice", "cpp");
    if(!file) {
        LOG_ERROR_RET(void(), "{}", file.error());
    }

    Fixture f;
    auto ref =
        f.add("/tmp",
              *file,
              {"clang++", "-std=c++23", "-fms-extensions", "-Wno-everything", file->c_str()});
    ASSERT_TRUE(f.db.toolchain().resolve(ref.config, ref.input).has_value());

    // Semantic flags must survive resolution to cc1 (they were dropped when
    // the query only forwarded toolchain options).
    auto argv = f.db.render(ref);
    bool has_ms_extensions = false;
    bool has_wno_everything = false;
    for(auto* arg: argv) {
        if(arg == "-fms-extensions"sv)
            has_ms_extensions = true;
        if(arg == "-Wno-everything"sv)
            has_wno_everything = true;
    }
    EXPECT_TRUE(has_ms_extensions);
    EXPECT_TRUE(has_wno_everything);
}

TEST_CASE(Resolve, skip = !CIEnvironment) {
    auto file = fs::createTemporaryFile("clice", "cpp");
    if(!file) {
        LOG_ERROR_RET(void(), "{}", file.error());
    }

    /// A platform-native absolute include dir: a POSIX spelling would be
    /// re-anchored (and separator-normalized) on Windows.
    TempDir tmp;
    auto inc = tmp.path("inc");
    auto inc_flag = "-I" + inc;

    Fixture f;
    auto ref =
        f.add("/tmp", *file, {"clang++", "-std=c++23", inc_flag.c_str(), "-DFOO=1", file->c_str()});
    auto resolved = f.db.toolchain().resolve(ref.config, ref.input);
    ASSERT_TRUE(resolved.has_value());
    EXPECT_TRUE(f.db.toolchain().has_cache());
    EXPECT_TRUE(f.db.toolchain().resolved(*resolved).is_cc1);

    auto argv = f.db.render(ref);
    bool has_cc1 = false;
    bool has_include = false;
    bool has_define = false;
    bool has_main_file = false;
    for(std::size_t i = 0; i < argv.size(); ++i) {
        if(argv[i] == "-cc1"sv)
            has_cc1 = true;
        if(argv[i] == "-I"sv && i + 1 < argv.size() && llvm::StringRef(argv[i + 1]) == inc)
            has_include = true;
        if(argv[i] == "-D"sv && i + 1 < argv.size() && argv[i + 1] == "FOO=1"sv)
            has_define = true;
        if(argv[i] == "-main-file-name"sv)
            has_main_file = true;
    }
    EXPECT_TRUE(has_cc1);
    EXPECT_TRUE(has_include);
    EXPECT_TRUE(has_define);
    EXPECT_TRUE(has_main_file);
}

TEST_CASE(Warm, skip = !CIEnvironment) {
    auto file1 = fs::createTemporaryFile("clice", "cpp");
    auto file2 = fs::createTemporaryFile("clice", "cpp");
    auto file3 = fs::createTemporaryFile("clice", "cpp");
    if(!file1 || !file2 || !file3) {
        LOG_ERROR_RET(void(), "failed to create temp files");
    }

    Fixture f;
    auto ref1 = f.add("/tmp", *file1, {"clang++", "-std=c++23", file1->c_str()});
    auto ref2 = f.add("/tmp", *file2, {"clang++", "-std=c++23", file2->c_str()});
    auto ref3 = f.add("/tmp", *file3, {"clang++", "-std=c++17", file3->c_str()});

    llvm::SmallVector<CommandRef> refs = {ref1, ref2, ref3};
    f.db.warm(refs);
    EXPECT_TRUE(f.db.toolchain().has_cache());

    // After warm, resolve should hit the probe cache (no subprocess).
    auto resolved = f.db.toolchain().resolve(ref1.config, ref1.input);
    ASSERT_TRUE(resolved.has_value());
    EXPECT_TRUE(f.db.toolchain().resolved(*resolved).is_cc1);
}

};  // TEST_SUITE(ToolchainTests)
}  // namespace
}  // namespace clice::testing
