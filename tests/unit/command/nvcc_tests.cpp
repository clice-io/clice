#include <algorithm>

#include "test/test.h"
#include "command/command.h"
#include "command/nvcc.h"
#include "command/toolchain.h"
#include "support/filesystem.h"

namespace clice::testing {
namespace {

using namespace std::string_view_literals;

TEST_SUITE(NVCCTests) {

std::vector<std::string> translate(std::vector<const char*> arguments,
                                   llvm::StringRef directory = "",
                                   bool edit = false) {
    return translate_nvcc_command(arguments, directory, edit);
}

bool contains(llvm::ArrayRef<std::string> arguments, llvm::StringRef flag) {
    return std::ranges::contains(arguments, flag);
}

TEST_CASE(TranslateCMakeShape) {
    auto args = translate({"nvcc",
                           "-forward-unknown-to-host-compiler",
                           "-DMY_FLAG=1",
                           "--generate-code=arch=compute_75,code=[compute_75,sm_75]",
                           "-x",
                           "cu",
                           "-c",
                           "kern.cu",
                           "-o",
                           "kern.cu.o"});

    EXPECT_EQ(args[0], "nvcc"sv);
    EXPECT_TRUE(contains(args, "--cuda-gpu-arch=sm_75"));
    EXPECT_TRUE(contains(args, "MY_FLAG=1"));
    EXPECT_TRUE(contains(args, "-x"));
    EXPECT_TRUE(contains(args, "cuda"));
    EXPECT_FALSE(contains(args, "cu"));
    for(llvm::StringRef arg: args) {
        EXPECT_FALSE(arg.starts_with("--generate-code"));
    }
}

TEST_CASE(GencodeSelectsBest) {
    // Only arch= clauses count: code= entries are ptxas targets and never
    // set __CUDA_ARCH__ (arch=compute_75,code=sm_90 preprocesses as 750).
    auto mismatch = translate({"nvcc", "-gencode", "arch=compute_75,code=sm_90"});
    EXPECT_TRUE(contains(mismatch, "--cuda-gpu-arch=sm_75"));

    // The newest architecture wins; 'a' outranks plain at the same number;
    // lto_ entries are intermediates, not architectures.
    auto multi = translate({"nvcc",
                            "-gencode",
                            "arch=compute_75,code=sm_75",
                            "-gencode=arch=compute_90a,code=[compute_90a,sm_90a,lto_120]"});
    EXPECT_TRUE(contains(multi, "--cuda-gpu-arch=sm_90a"));

    EXPECT_TRUE(contains(translate({"nvcc", "-arch=compute_86"}), "--cuda-gpu-arch=sm_86"));
    EXPECT_TRUE(contains(translate({"nvcc", "-arch=sm_100f"}), "--cuda-gpu-arch=sm_100f"));

    // -arch is a scalar option: the last one wins, unlike -gencode which
    // accumulates.
    auto repeated = translate({"nvcc", "-arch=compute_80", "-arch=compute_75"});
    EXPECT_TRUE(contains(repeated, "--cuda-gpu-arch=sm_75"));

    // Bare -code and arch-less commands pin no architecture.
    for(auto& args: {translate({"nvcc", "-code=sm_90"}), translate({"nvcc", "-c", "a.cu"})}) {
        for(llvm::StringRef arg: args) {
            EXPECT_FALSE(arg.starts_with("--cuda-gpu-arch="));
        }
    }
}

TEST_CASE(ArchGencodeUnion) {
    // nvcc accepts -arch next to -gencode and compiles their union, one
    // device pass each — the newest wins whichever side carries it.
    auto arch_newer = translate({"nvcc", "-gencode=arch=compute_75,code=sm_75", "-arch=sm_90"});
    EXPECT_TRUE(contains(arch_newer, "--cuda-gpu-arch=sm_90"));

    auto gencode_newer = translate({"nvcc", "-arch=sm_75", "-gencode=arch=compute_90,code=sm_90"});
    EXPECT_TRUE(contains(gencode_newer, "--cuda-gpu-arch=sm_90"));
}

TEST_CASE(SpecialArchCarried) {
    // Non-numeric selections only nvcc can resolve persist as probe tokens
    // for the dryrun instead of vanishing into no architecture at all.
    for(auto arguments: {
            std::vector<const char*>{"nvcc", "-arch=native"},
            std::vector<const char*>{"nvcc", "-arch", "native"},
            std::vector<const char*>{"nvcc", "--gpu-architecture=native"}
    }) {
        auto args = translate(arguments);
        EXPECT_TRUE(contains(args, "-arch=native"));
        EXPECT_FALSE(contains(args, "native"));
    }
    EXPECT_TRUE(is_nvcc_probe_flag("-arch=native"));
    EXPECT_TRUE(contains(translate({"nvcc", "-arch=all"}), "-arch=all"));

    // -arch stays last-wins across numeric and special values.
    auto numeric_wins = translate({"nvcc", "-arch=native", "-arch=sm_80"});
    EXPECT_TRUE(contains(numeric_wins, "--cuda-gpu-arch=sm_80"));
    EXPECT_FALSE(contains(numeric_wins, "-arch=native"));
    auto special_wins = translate({"nvcc", "-arch=sm_80", "-arch=native"});
    EXPECT_TRUE(contains(special_wins, "-arch=native"));
    EXPECT_FALSE(contains(special_wins, "--cuda-gpu-arch=sm_80"));

    // As an edit it clears the base architectures like a numeric choice.
    auto edit = translate({"nvcc", "-arch=native"}, "", true);
    auto clear = std::ranges::find(edit, "--no-offload-arch=all");
    ASSERT_TRUE(clear != edit.end() && clear + 1 != edit.end());
    EXPECT_EQ(*(clear + 1), "-arch=native"sv);
}

TEST_CASE(CcbinBecomesToken) {
    for(auto arguments: {
            std::vector<const char*>{"nvcc", "-ccbin", "/usr/bin/g++-12"},
            std::vector<const char*>{"nvcc", "-ccbin=/usr/bin/g++-12"},
            std::vector<const char*>{"nvcc", "--compiler-bindir", "/usr/bin/g++-12"}
    }) {
        auto args = translate(arguments);
        EXPECT_TRUE(contains(args, "-ccbin=/usr/bin/g++-12"));
        EXPECT_FALSE(contains(args, "/usr/bin/g++-12"));
    }
}

TEST_CASE(ProbeFlagsCarried) {
    // Toolchain-selecting options become normalized verbatim tokens; a
    // relative -ccbin anchors to the compile directory like nvcc would.
    auto args = translate(
        {"nvcc", "-allow-unsupported-compiler", "-target-dir", "sbsa-linux", "-ccbin", "tools/g++"},
        "/base");
    // The join spells the separator natively, so the expectation must too.
    auto anchored = "-ccbin=" + path::join("/base", "tools/g++");
    for(llvm::StringRef flag: {llvm::StringRef("--allow-unsupported-compiler"),
                               llvm::StringRef("--target-directory=sbsa-linux"),
                               llvm::StringRef(anchored)}) {
        EXPECT_TRUE(contains(args, flag));
        EXPECT_TRUE(is_nvcc_probe_flag(flag));
    }
    EXPECT_FALSE(is_nvcc_probe_flag("-I/base"));

    // A bare name resolves on PATH like nvcc would — never anchored — while
    // dot-relative values are directory-relative.
    EXPECT_TRUE(contains(translate({"nvcc", "-ccbin=g++-13"}, "/base"), "-ccbin=g++-13"));
    auto dot = translate({"nvcc", "-ccbin=."}, "/base");
    EXPECT_TRUE(std::ranges::any_of(dot, [](llvm::StringRef arg) {
        return arg.starts_with("-ccbin=/base");
    }));
}

TEST_CASE(XcompilerUnwrapped) {
    auto args = translate({"nvcc", "-Xcompiler=-fPIC,-pthread", "-Xcompiler", "-Wall"});
    EXPECT_TRUE(contains(args, "-fPIC"));
    EXPECT_TRUE(contains(args, "-pthread"));
    EXPECT_TRUE(contains(args, "-Wall"));
    EXPECT_FALSE(contains(args, "-Xcompiler"));

    // The value follows the same `\,` escape as every other list option.
    auto escaped = translate({"nvcc", R"(-Xcompiler=-Wl\,-z\,defs)"});
    EXPECT_TRUE(contains(escaped, "-Wl,-z,defs"));
}

TEST_CASE(MacroToggles) {
    auto args = translate({"nvcc",
                           "--expt-relaxed-constexpr",
                           "--extended-lambda",
                           "-rdc=true",
                           "-default-stream",
                           "per-thread"});
    EXPECT_TRUE(contains(args, "-D__CUDACC_RELAXED_CONSTEXPR__"));
    EXPECT_TRUE(contains(args, "-D__CUDACC_EXTENDED_LAMBDA__"));
    EXPECT_TRUE(contains(args, "-fgpu-rdc"));
    EXPECT_TRUE(contains(args, "-D__CUDACC_RDC__"));
    EXPECT_TRUE(contains(args, "-DCUDA_API_PER_THREAD_DEFAULT_STREAM=1"));

    EXPECT_FALSE(contains(translate({"nvcc", "-rdc=false"}), "-fgpu-rdc"));

    // Separate compilation implies -rdc=true; -ewp has its own macro.
    auto dc = translate({"nvcc", "-dc"});
    EXPECT_TRUE(contains(dc, "-fgpu-rdc"));
    EXPECT_TRUE(contains(dc, "-D__CUDACC_RDC__"));
    EXPECT_TRUE(contains(translate({"nvcc", "--extensible-whole-program"}), "-D__CUDACC_EWP__"));

    // Device debug becomes its macro; -G must not survive, clang reads it
    // as the small-data-threshold option.
    auto debug = translate({"nvcc", "-G"});
    EXPECT_TRUE(contains(debug, "-D__CUDACC_DEBUG__"));
    EXPECT_FALSE(contains(debug, "-G"));
    EXPECT_TRUE(contains(translate({"nvcc", "--device-debug"}), "-D__CUDACC_DEBUG__"));

    // Fast math becomes clang's approx-transcendentals flag, which selects
    // the same fast variants in the math wrapper.
    for(const char* spelling: {"--use_fast_math", "-use_fast_math"}) {
        auto fast = translate({"nvcc", spelling});
        EXPECT_TRUE(contains(fast, "-fgpu-approx-transcendentals"));
        EXPECT_FALSE(contains(fast, spelling));
    }

    // Stateful options are last-wins, matching nvcc.
    EXPECT_FALSE(contains(translate({"nvcc", "-rdc=true", "-rdc=false"}), "-fgpu-rdc"));
    auto stream = translate({"nvcc", "-default-stream=per-thread", "-default-stream=legacy"});
    EXPECT_FALSE(contains(stream, "-DCUDA_API_PER_THREAD_DEFAULT_STREAM=1"));

    // Synthetic macros render ahead of user flags, so a later -U can undo
    // them the way it does under nvcc.
    auto undef = llvm::join(translate({"nvcc", "-dc", "-U__CUDACC_RDC__"}), " ");
    EXPECT_TRUE(llvm::StringRef(undef).find("-D__CUDACC_RDC__") <
                llvm::StringRef(undef).find("-U __CUDACC_RDC__"));

    // The stream macro is nvcc's one exception: it lands after user flags,
    // so their -U cannot undo it.
    auto stream_undef = llvm::join(
        translate({"nvcc", "-default-stream=per-thread", "-UCUDA_API_PER_THREAD_DEFAULT_STREAM"}),
        " ");
    EXPECT_TRUE(llvm::StringRef(stream_undef).find("-U CUDA_API_PER_THREAD_DEFAULT_STREAM") <
                llvm::StringRef(stream_undef).find("-DCUDA_API_PER_THREAD_DEFAULT_STREAM=1"));
}

TEST_CASE(PairedValueDrops) {
    // The wrapped values would parse as host flags on their own; bare
    // nvcc-only flags pass through for the CDB classification to discard.
    auto args = translate({"nvcc", "-Xptxas", "-O3", "-t", "4", "-lineinfo"});
    std::vector<std::string> expected = {"nvcc", "-lineinfo"};
    EXPECT_EQ(args, expected);
}

TEST_CASE(LongFormAliases) {
    auto args = translate({"nvcc",
                           "--include-path=/opt/inc",
                           "--define-macro",
                           "FOO=1",
                           "--undefine-macro=BAR",
                           "--pre-include",
                           "config.h",
                           "--system-include=/opt/sys"});
    auto joined = llvm::join(args, " ");
    EXPECT_TRUE(llvm::StringRef(joined).contains("-I /opt/inc"));
    EXPECT_TRUE(llvm::StringRef(joined).contains("-D FOO=1"));
    EXPECT_TRUE(llvm::StringRef(joined).contains("-U BAR"));
    EXPECT_TRUE(llvm::StringRef(joined).contains("-include config.h"));
    EXPECT_TRUE(llvm::StringRef(joined).contains("-isystem /opt/sys"));
}

TEST_CASE(ListValuesSplit) {
    // nvcc splits every preprocessor value on commas, short spellings
    // included: -Ia,b preprocesses with two include directories.
    auto args = translate(
        {"nvcc", "-Ia,b", "-DA=1,B=2", "-U", "X,Y", "-isystem=s1,s2", "-include", "h1.h,h2.h"});
    auto joined = llvm::join(args, " ");
    for(llvm::StringRef piece: {"-I a",
                                "-I b",
                                "-D A=1",
                                "-D B=2",
                                "-U X",
                                "-U Y",
                                "-isystem s1",
                                "-isystem s2",
                                "-include h1.h",
                                "-include h2.h"}) {
        EXPECT_TRUE(llvm::StringRef(joined).contains(piece));
    }

    // `\,` reads as a literal comma; other backslashes stay verbatim so
    // native Windows paths survive (deliberately shallower than nvcc's
    // Linux-side escape processing, which consumes every backslash).
    EXPECT_TRUE(contains(translate({"nvcc", R"(-DP=a\,b)"}), "P=a,b"));
    auto windows = translate({"nvcc", R"(-IC:\inc,D:\other)"});
    EXPECT_TRUE(contains(windows, R"(C:\inc)"));
    EXPECT_TRUE(contains(windows, R"(D:\other)"));
}

TEST_CASE(OptionsFileExpanded) {
    auto file = fs::createTemporaryFile("clice-nvcc", "rsp");
    ASSERT_TRUE(file.has_value());
    ASSERT_TRUE(fs::write(*file, "-Igenerated -DAPI=2 -std=c++20\n"));

    auto args = translate({"nvcc", "--options-file", file->c_str()});
    auto joined = llvm::join(args, " ");
    EXPECT_TRUE(llvm::StringRef(joined).contains("-I generated"));
    EXPECT_TRUE(llvm::StringRef(joined).contains("-D API=2"));
    EXPECT_TRUE(contains(args, "-std=c++20"));
    EXPECT_FALSE(contains(args, "--options-file"));
    EXPECT_FALSE(llvm::StringRef(joined).contains(*file));

    // The value is a comma-separated file list: every element expands.
    auto second = fs::createTemporaryFile("clice-nvcc", "rsp");
    ASSERT_TRUE(second.has_value());
    ASSERT_TRUE(fs::write(*second, "-DFROM_SECOND=2\n"));

    auto pair = *file + "," + *second;
    auto both = llvm::join(translate({"nvcc", "-optf", pair.c_str()}), " ");
    EXPECT_TRUE(llvm::StringRef(both).contains("-D API=2"));
    EXPECT_TRUE(llvm::StringRef(both).contains("-D FROM_SECOND=2"));
    EXPECT_FALSE(llvm::StringRef(both).contains("-optf"));
    EXPECT_FALSE(llvm::StringRef(both).contains(*file));
    EXPECT_FALSE(llvm::StringRef(both).contains(*second));

    // An unreadable file drops with a warning; the rest of the command
    // still translates.
    auto missing = translate({"nvcc", "--options-file=missing.rsp", "-DX"}, "/clice-nonexistent");
    EXPECT_TRUE(contains(missing, "X"));
    EXPECT_FALSE(contains(missing, "--options-file=missing.rsp"));
    EXPECT_FALSE(contains(missing, "missing.rsp"));

    fs::remove(*file);
    fs::remove(*second);
}

TEST_CASE(StdNormalized) {
    EXPECT_TRUE(contains(translate({"nvcc", "-std", "c++17"}), "-std=c++17"));
    EXPECT_TRUE(contains(translate({"nvcc", "--std=c++20"}), "-std=c++20"));
}

TEST_CASE(MachineNormalized) {
    for(auto arguments: {
            std::vector<const char*>{"nvcc", "--machine=64"},
            std::vector<const char*>{"nvcc", "--machine", "64"},
            std::vector<const char*>{"nvcc", "-m=64"},
            std::vector<const char*>{"nvcc", "-m", "64"}
    }) {
        auto args = translate(arguments);
        EXPECT_TRUE(contains(args, "-m64"));
        EXPECT_FALSE(contains(args, "64"));
    }
    EXPECT_TRUE(contains(translate({"nvcc", "--machine=32"}), "-m32"));

    // The joined short form is already clang's own spelling.
    EXPECT_TRUE(contains(translate({"nvcc", "-m64"}), "-m64"));

    // A value nvcc rejects pins no machine model.
    auto invalid = translate({"nvcc", "--machine=16"});
    EXPECT_FALSE(contains(invalid, "-m16"));
    EXPECT_FALSE(contains(invalid, "--machine=16"));
}

TEST_CASE(OptimizeNormalized) {
    EXPECT_TRUE(contains(translate({"nvcc", "--optimize=3"}), "-O3"));
    EXPECT_TRUE(contains(translate({"nvcc", "-O", "2"}), "-O2"));
    EXPECT_TRUE(contains(translate({"nvcc", "-O3"}), "-O3"));

    // Joined -O3 is nvcc's own option too, last-wins like the long form.
    auto repeated = translate({"nvcc", "-O2", "--optimize=3"});
    EXPECT_TRUE(contains(repeated, "-O3"));
    EXPECT_FALSE(contains(repeated, "-O2"));
}

TEST_CASE(HostFlagsFollowXcompiler) {
    // nvcc places its own -O/-m after the -Xcompiler payloads on the host
    // line, beating them regardless of input order.
    auto find_order = [](std::vector<std::string> args, llvm::StringRef a, llvm::StringRef b) {
        auto joined = llvm::join(args, " ");
        return llvm::StringRef(joined).find(a) < llvm::StringRef(joined).find(b);
    };
    EXPECT_TRUE(find_order(translate({"nvcc", "--optimize=3", "-Xcompiler=-O0"}), "-O0", "-O3"));
    EXPECT_TRUE(find_order(translate({"nvcc", "-O3", "-Xcompiler=-O0"}), "-O0", "-O3"));
    EXPECT_TRUE(find_order(translate({"nvcc", "--machine=64", "-Xcompiler=-m32"}), "-m32", "-m64"));
}

TEST_CASE(DisableWarningsMapped) {
    for(const char* spelling: {"--disable-warnings", "-disable-warnings"}) {
        auto args = translate({"nvcc", spelling});
        EXPECT_TRUE(contains(args, "-w"));
        EXPECT_FALSE(contains(args, spelling));
    }
}

TEST_CASE(EditEmitsStateOverrides) {
    // An edit lands after an already-translated base command: explicitly
    // disabled stateful options must cancel the base's translated state,
    // while a standalone command emits nothing for the default state.
    auto off = translate({"nvcc", "-rdc=false", "--default-stream=legacy"}, "", true);
    EXPECT_TRUE(contains(off, "-fno-gpu-rdc"));
    EXPECT_TRUE(contains(off, "-U__CUDACC_RDC__"));
    EXPECT_TRUE(contains(off, "-UCUDA_API_PER_THREAD_DEFAULT_STREAM"));

    auto standalone = translate({"nvcc", "-rdc=false", "--default-stream=legacy"});
    EXPECT_FALSE(contains(standalone, "-fno-gpu-rdc"));
    EXPECT_FALSE(contains(standalone, "-U__CUDACC_RDC__"));
    EXPECT_FALSE(contains(standalone, "-UCUDA_API_PER_THREAD_DEFAULT_STREAM"));

    // The cancellations render ahead of the segment's own flags: an
    // explicit -D of the macro inside the edit survives them, like it
    // survives nvcc's absent injection.
    auto explicit_d = llvm::join(
        translate({"nvcc", "-DCUDA_API_PER_THREAD_DEFAULT_STREAM=7", "--default-stream=legacy"},
                  "",
                  true),
        " ");
    EXPECT_TRUE(llvm::StringRef(explicit_d).find("-UCUDA_API_PER_THREAD_DEFAULT_STREAM") <
                llvm::StringRef(explicit_d).find("CUDA_API_PER_THREAD_DEFAULT_STREAM=7"));

    // Untouched state stays silent even as an edit.
    auto untouched = translate({"nvcc", "-DX"}, "", true);
    EXPECT_FALSE(contains(untouched, "-fno-gpu-rdc"));
    EXPECT_FALSE(contains(untouched, "-U__CUDACC_RDC__"));
    EXPECT_FALSE(contains(untouched, "-UCUDA_API_PER_THREAD_DEFAULT_STREAM"));
    EXPECT_FALSE(contains(untouched, "--no-offload-arch=all"));

    // clang accumulates --cuda-gpu-arch while nvcc's -arch is last-wins: an
    // arch edit clears the base architectures right before its own choice.
    auto arch = translate({"nvcc", "-arch=sm_80"}, "", true);
    auto clear = std::ranges::find(arch, "--no-offload-arch=all");
    ASSERT_TRUE(clear != arch.end() && clear + 1 != arch.end());
    EXPECT_EQ(*(clear + 1), "--cuda-gpu-arch=sm_80"sv);
    EXPECT_FALSE(contains(translate({"nvcc", "-arch=sm_80"}), "--no-offload-arch=all"));

    // -gencode accumulates in nvcc, so as an edit it adds its architecture
    // without erasing the base's.
    auto gencode = translate({"nvcc", "-gencode=arch=compute_75,code=sm_75"}, "", true);
    EXPECT_TRUE(contains(gencode, "--cuda-gpu-arch=sm_75"));
    EXPECT_FALSE(contains(gencode, "--no-offload-arch=all"));
}

constexpr static llvm::StringRef fake_dryrun = R"(#$ _NVVM_BRANCH_=nvvm
#$ _SPACE_=
#$ TOP=/opt/cuda/targets/x86_64-linux
#$ NVVMIR_LIBRARY_DIR=/opt/cuda/targets/x86_64-linux/nvvm/libdevice
#$ LD_LIBRARY_PATH=/opt/cuda/targets/x86_64-linux/lib:
#$ PATH=/opt/host/bin
#$ INCLUDES="-I/opt/cuda/targets/x86_64-linux/include"
#$ g++ -D__CUDA_ARCH_LIST__=520 -D__NV_LEGACY_LAUNCH -E -x c++ -D__CUDACC__ -D__NVCC__ "-I/opt/cuda/targets/x86_64-linux/include" -D__CUDACC_VER_MAJOR__=12 -D__CUDACC_VER_MINOR__=9 -include "cuda_runtime.h" -m64 "/tmp/a.cu" -o "/tmp/a.cpp4.ii"
#$ cudafe++ --c++17 --gnu_version=140400 "/tmp/a.cpp4.ii"
#$ g++ -D__CUDA_ARCH__=520 -D__CUDA_ARCH_LIST__=520 -D__NV_LEGACY_LAUNCH -E -x c++ -DCUDA_DOUBLE_MATH_FUNCTIONS -D__CUDACC__ -D__NVCC__ -D__CUDACC_VER_MAJOR__=12 -D__CUDACC_VER_MINOR__=9 -include "cuda_runtime.h" -m64 "/tmp/a.cu" -o "/tmp/a.cpp1.ii"
#$ cicc --c++17 --gnu_version=140400 -arch compute_52 -m64 "/tmp/a.cpp1.ii" -o "/tmp/a.ptx"
#$ ptxas -arch=sm_52 -m64 "/tmp/a.ptx" -o "/tmp/a.cubin"
)";

TEST_CASE(DryrunParsed) {
    auto info = parse_nvcc_dryrun(fake_dryrun);
    ASSERT_TRUE(info.has_value());

    EXPECT_EQ(info->cuda_path, "/opt/cuda/targets/x86_64-linux");
    EXPECT_EQ(info->host_compiler, "g++");
    EXPECT_EQ(info->cpp_dialect, "c++17");
    EXPECT_EQ(info->default_arch, "sm_52");
    EXPECT_TRUE(std::ranges::contains(info->search_path, "/opt/host/bin"));

    // clang derives the blocklisted three itself; the rest must survive.
    for(auto defines: {&info->host_defines, &info->device_defines}) {
        EXPECT_TRUE(std::ranges::contains(*defines, "__CUDACC_VER_MAJOR__=12"));
        EXPECT_TRUE(std::ranges::contains(*defines, "__NV_LEGACY_LAUNCH"));
        EXPECT_FALSE(std::ranges::contains(*defines, "__CUDACC__"));
        for(llvm::StringRef define: *defines) {
            EXPECT_FALSE(define.starts_with("__CUDA_ARCH__"));
            EXPECT_FALSE(define.starts_with("__CUDA_ARCH_LIST__"));
        }
    }
    EXPECT_TRUE(std::ranges::contains(info->device_defines, "CUDA_DOUBLE_MATH_FUNCTIONS"));
    EXPECT_FALSE(std::ranges::contains(info->host_defines, "CUDA_DOUBLE_MATH_FUNCTIONS"));

    // A probe carrying -arch=all runs one cicc per architecture; the newest
    // wins regardless of emission order.
    constexpr llvm::StringRef multi = R"(#$ g++ -E -x c++ "/tmp/a.cu" -o "/tmp/a.ii"
#$ cicc --c++17 -arch compute_90 "/tmp/a.cpp1.ii" -o "/tmp/a.ptx"
#$ cicc --c++17 -arch compute_75 "/tmp/a.cpp1.ii" -o "/tmp/b.ptx"
)";
    auto multi_info = parse_nvcc_dryrun(multi);
    ASSERT_TRUE(multi_info.has_value());
    EXPECT_EQ(multi_info->default_arch, "sm_90");
}

TEST_CASE(DryrunRejectsIncomplete) {
    EXPECT_FALSE(parse_nvcc_dryrun("#$ PATH=/usr/bin").has_value());
    EXPECT_FALSE(parse_nvcc_dryrun("#$ TOP=/opt/cuda").has_value());
}

TEST_CASE(DryrunHostOnly) {
    // A host-language input (nvcc -c foo.cpp) has no preprocess stage: the
    // single host compile line names the compiler, and its defines survive
    // unfiltered — outside CUDA mode clang derives none of them.
    constexpr llvm::StringRef host_only = R"(#$ TOP=/opt/cuda
#$ INCLUDES="-I/opt/cuda/include"
#$ g++ -D__CUDA_ARCH_LIST__=520 -c -x c++ -D__NVCC__ -D__CUDACC_VER_MAJOR__=12 -m64 "/tmp/a.cpp" -o "/tmp/a.o"
)";
    auto info = parse_nvcc_dryrun(host_only);
    ASSERT_TRUE(info.has_value());
    EXPECT_EQ(info->host_compiler, "g++");
    EXPECT_TRUE(std::ranges::contains(info->host_defines, "__NVCC__"));
    EXPECT_TRUE(std::ranges::contains(info->host_defines, "__CUDA_ARCH_LIST__=520"));
    EXPECT_TRUE(std::ranges::contains(info->host_defines, "__CUDACC_VER_MAJOR__=12"));
    EXPECT_TRUE(info->device_defines.empty());
}

TEST_CASE(DryrunTopFallback) {
    // A wrapper may swallow TOP=; NVVMIR_LIBRARY_DIR is <root>/nvvm/libdevice.
    constexpr llvm::StringRef no_top = R"(#$ NVVMIR_LIBRARY_DIR=/opt/cuda/nvvm/libdevice
#$ g++ -D__CUDACC_VER_MAJOR__=12 -E -x c++ "/tmp/a.cu" -o "/tmp/a.ii"
)";
    auto info = parse_nvcc_dryrun(no_top);
    ASSERT_TRUE(info.has_value());
    EXPECT_EQ(info->cuda_path, "/opt/cuda");

    // With neither line the toolchain still resolves; CUDA detection is
    // left to clang's own search.
    auto bare = parse_nvcc_dryrun(R"(#$ g++ -E -x c++ "/tmp/a.cu")");
    ASSERT_TRUE(bare.has_value());
    EXPECT_TRUE(bare->cuda_path.empty());
    EXPECT_EQ(bare->host_compiler, "g++");
}

TEST_CASE(CcbinAffectsKey) {
    Toolchain tc;
    std::vector<const char*> a = {"nvcc", "-ccbin=/usr/bin/g++-12"};
    std::vector<const char*> b = {"nvcc", "-ccbin=/usr/bin/g++-13"};
    EXPECT_NE(tc.cache_key("/tmp/a.cu", a), tc.cache_key("/tmp/a.cu", b));
    EXPECT_EQ(tc.cache_key("/tmp/a.cu", a), tc.cache_key("/tmp/a.cu", a));
}

TEST_CASE(DatabaseTranslatesNVCC) {
    CompilationDatabase db;
    std::vector<const char*> arguments = {"nvcc",
                                          "-forward-unknown-to-host-compiler",
                                          "-DMY_FLAG=1",
                                          "--generate-code=arch=compute_75,code=[compute_75,sm_75]",
                                          "-ccbin=/usr/bin/g++-12",
                                          "-allow-unsupported-compiler",
                                          "-x",
                                          "cu",
                                          "-c",
                                          "/tmp/kern.cu",
                                          "-o",
                                          "kern.cu.o"};
    db.add_command("/tmp", "/tmp/kern.cu", arguments);

    auto commands = db.lookup("/tmp/kern.cu");
    ASSERT_EQ(commands.size(), std::size_t(1));

    auto& flags = commands[0].resolved.flags;
    auto has = [&](llvm::StringRef flag) {
        return std::ranges::contains(flags, flag);
    };
    // --cuda-gpu-arch renders through its unaliased spelling.
    EXPECT_TRUE(has("--offload-arch=sm_75"));
    EXPECT_TRUE(has("-ccbin=/usr/bin/g++-12"));
    EXPECT_TRUE(has("--allow-unsupported-compiler"));
    EXPECT_TRUE(has("-x"));
    EXPECT_TRUE(has("cuda"));
    EXPECT_TRUE(has("MY_FLAG=1"));
    EXPECT_FALSE(has("-forward-unknown-to-host-compiler"));
    for(llvm::StringRef flag: flags) {
        EXPECT_FALSE(flag.starts_with("--generate-code"));
    }
}

TEST_CASE(RuleFlagsTranslated) {
    CompilationDatabase db;
    std::vector<const char*> arguments = {"nvcc",
                                          "--generate-code=arch=compute_75,code=sm_75",
                                          "-c",
                                          "/tmp/kern.cu"};
    db.add_command("/tmp", "/tmp/kern.cu", arguments);

    // Config rule flags for an NVCC entry go through the same translation
    // as the command: the remove matches the arch through its translated
    // spelling and the append reaches clang translated, not as raw nvcc
    // tokens.
    CommandOptions options;
    llvm::SmallVector<std::string> remove = {"-gencode", "arch=compute_75,code=sm_75"};
    options.remove = remove;
    llvm::SmallVector<std::string> append = {"--extended-lambda",
                                             "--generate-code=arch=compute_90a,code=sm_90a"};
    options.append = append;

    auto commands = db.lookup("/tmp/kern.cu", options);
    ASSERT_EQ(commands.size(), std::size_t(1));

    auto& flags = commands[0].resolved.flags;
    auto has = [&](llvm::StringRef flag) {
        return std::ranges::contains(flags, flag);
    };
    EXPECT_FALSE(has("--offload-arch=sm_75"));
    EXPECT_TRUE(has("--cuda-gpu-arch=sm_90a"));
    EXPECT_TRUE(has("-D__CUDACC_EXTENDED_LAMBDA__"));
    for(llvm::StringRef flag: flags) {
        EXPECT_FALSE(flag.starts_with("--generate-code"));
        EXPECT_FALSE(flag.starts_with("--extended-lambda"));
    }
}

TEST_CASE(AppendOverridesBase) {
    CompilationDatabase db;
    std::vector<const char*> arguments =
        {"nvcc", "-rdc=true", "-default-stream=per-thread", "-arch=sm_75", "-c", "/tmp/kern.cu"};
    db.add_command("/tmp", "/tmp/kern.cu", arguments);

    // NVCC's stateful options are last-wins across the whole command, so an
    // appended disable must beat the state the base already translated.
    CommandOptions options;
    llvm::SmallVector<std::string> append = {"-rdc=false",
                                             "--default-stream=legacy",
                                             "-arch=sm_80"};
    options.append = append;

    auto commands = db.lookup("/tmp/kern.cu", options);
    ASSERT_EQ(commands.size(), std::size_t(1));

    auto& flags = commands[0].resolved.flags;
    auto index_of = [&](llvm::StringRef flag) {
        return std::ranges::find(flags,
                                 flag,
                                 [](const char* arg) { return llvm::StringRef(arg); }) -
               flags.begin();
    };
    auto count = std::ptrdiff_t(flags.size());

    // rdc: the appended negation follows the base's enable, so clang's own
    // last-wins turns it off and undefines the macro the base defined.
    EXPECT_TRUE(index_of("-fgpu-rdc") < index_of("-fno-gpu-rdc"));
    EXPECT_TRUE(index_of("-fno-gpu-rdc") < count);
    EXPECT_TRUE(index_of("__CUDACC_RDC__") < index_of("-U__CUDACC_RDC__"));
    EXPECT_TRUE(index_of("-U__CUDACC_RDC__") < count);

    // stream: undef after the base's define.
    EXPECT_TRUE(index_of("CUDA_API_PER_THREAD_DEFAULT_STREAM=1") <
                index_of("-UCUDA_API_PER_THREAD_DEFAULT_STREAM"));
    EXPECT_TRUE(index_of("-UCUDA_API_PER_THREAD_DEFAULT_STREAM") < count);

    // arch: the base's architecture is cleared before the appended one, not
    // accumulated into a second device pass.
    EXPECT_TRUE(index_of("--offload-arch=sm_75") < index_of("--no-offload-arch=all"));
    EXPECT_TRUE(index_of("--no-offload-arch=all") < index_of("--cuda-gpu-arch=sm_80"));
    EXPECT_TRUE(index_of("--cuda-gpu-arch=sm_80") < count);
}

TEST_CASE(GencodeAppendAccumulates) {
    CompilationDatabase db;
    std::vector<const char*> arguments = {"nvcc",
                                          "--generate-code=arch=compute_90,code=sm_90",
                                          "-c",
                                          "/tmp/kern.cu"};
    db.add_command("/tmp", "/tmp/kern.cu", arguments);

    auto arch_flags = [&](llvm::ArrayRef<std::string> append) {
        CommandOptions options;
        options.append = append;
        auto commands = db.lookup("/tmp/kern.cu", options);
        std::vector<std::string> result;
        for(llvm::StringRef flag: commands[0].resolved.flags) {
            if(flag.contains("arch")) {
                result.push_back(flag.str());
            }
        }
        return result;
    };

    // Appended -gencode entries accumulate onto the base's like nvcc's own,
    // and the newest architecture keeps winning: an older append changes
    // nothing.
    llvm::SmallVector<std::string> older = {"-gencode=arch=compute_75,code=sm_75"};
    auto kept = arch_flags(older);
    EXPECT_TRUE(std::ranges::contains(kept, "--offload-arch=sm_90"));
    EXPECT_FALSE(std::ranges::contains(kept, "--cuda-gpu-arch=sm_75"));
    EXPECT_FALSE(std::ranges::contains(kept, "--no-offload-arch=all"));

    // A newer append takes over — by numeric rank, not string order, which
    // would sort sm_100a below sm_90.
    llvm::SmallVector<std::string> newer = {"-gencode=arch=compute_100a,code=sm_100a"};
    auto switched = arch_flags(newer);
    EXPECT_TRUE(std::ranges::contains(switched, "--cuda-gpu-arch=sm_100a"));
    EXPECT_FALSE(std::ranges::contains(switched, "--offload-arch=sm_90"));
}

TEST_CASE(CollapseHonorsNegatives) {
    // A specific --no-offload-arch erases only its matches from the ranking;
    // the negated flag pair stays for clang to consume, and the newest of
    // the survivors wins.
    std::vector<const char*> flags = {"clang",
                                      "--cuda-gpu-arch=sm_90",
                                      "--no-offload-arch=sm_90",
                                      "--cuda-gpu-arch=sm_75",
                                      "--cuda-gpu-arch=sm_86"};
    collapse_gpu_arch_flags(flags);
    std::vector<std::string> collapsed(flags.begin(), flags.end());
    std::vector<std::string> expected = {"clang",
                                         "--cuda-gpu-arch=sm_90",
                                         "--no-offload-arch=sm_90",
                                         "--cuda-gpu-arch=sm_86"};
    EXPECT_EQ(collapsed, expected);

    // A single survivor leaves nothing to collapse.
    std::vector<const char*> single = {"clang",
                                       "--cuda-gpu-arch=sm_90",
                                       "--no-offload-arch=sm_90",
                                       "--cuda-gpu-arch=sm_75"};
    auto kept = single;
    collapse_gpu_arch_flags(single);
    EXPECT_EQ(single, kept);

    // An unrankable negative leaves the whole command to clang.
    std::vector<const char*> native = {"clang",
                                       "--cuda-gpu-arch=sm_90",
                                       "--no-offload-arch=native",
                                       "--cuda-gpu-arch=sm_75"};
    auto untouched = native;
    collapse_gpu_arch_flags(native);
    EXPECT_EQ(native, untouched);
}

TEST_CASE(WildcardRemoveClearsArch) {
    CompilationDatabase db;
    std::vector<const char*> gencode = {"nvcc",
                                        "--generate-code=arch=compute_75,code=sm_75",
                                        "-c",
                                        "/tmp/kern.cu"};
    db.add_command("/tmp", "/tmp/kern.cu", gencode);
    std::vector<const char*> arch = {"nvcc", "-arch=sm_80", "-c", "/tmp/other.cu"};
    db.add_command("/tmp", "/tmp/other.cu", arch);
    std::vector<const char*> native = {"nvcc", "-arch=native", "-c", "/tmp/native.cu"};
    db.add_command("/tmp", "/tmp/native.cu", native);

    auto arch_flags = [&](llvm::StringRef file, const CommandOptions& options) {
        auto commands = db.lookup(file, options);
        std::vector<std::string> result;
        for(llvm::StringRef flag: commands[0].resolved.flags) {
            if(flag.contains("arch")) {
                result.push_back(flag.str());
            }
        }
        return result;
    };

    EXPECT_TRUE(std::ranges::contains(arch_flags("/tmp/kern.cu", {}), "--offload-arch=sm_75"));
    EXPECT_TRUE(std::ranges::contains(arch_flags("/tmp/other.cu", {}), "--offload-arch=sm_80"));
    EXPECT_TRUE(std::ranges::contains(arch_flags("/tmp/native.cu", {}), "-arch=native"));

    // The wildcard must clear whichever form the base carries: numeric archs
    // translate to --offload-arch, non-numeric ones persist as probe tokens.
    CommandOptions options;
    llvm::SmallVector<std::string> remove = {"--generate-code=*"};
    options.remove = remove;
    EXPECT_TRUE(arch_flags("/tmp/kern.cu", options).empty());

    llvm::SmallVector<std::string> separate = {"-arch", "*"};
    options.remove = separate;
    EXPECT_TRUE(arch_flags("/tmp/other.cu", options).empty());
    EXPECT_TRUE(arch_flags("/tmp/native.cu", options).empty());

    // Removes edit the base before appends land: replacing the architecture
    // through remove-wildcard + append keeps the appended one.
    llvm::SmallVector<std::string> append = {"-gencode=arch=compute_90a,code=sm_90a"};
    options.append = append;
    auto replaced = arch_flags("/tmp/other.cu", options);
    EXPECT_FALSE(std::ranges::contains(replaced, "--offload-arch=sm_80"));
    EXPECT_TRUE(std::ranges::contains(replaced, "--cuda-gpu-arch=sm_90a"));
}

TEST_CASE(RemoveMatchesUnknownSpelling) {
    CompilationDatabase db;
    std::vector<const char*> arguments = {"nvcc",
                                          "-ccbin=/usr/bin/g++-12",
                                          "-allow-unsupported-compiler",
                                          "-target-dir",
                                          "sbsa-linux",
                                          "-c",
                                          "/tmp/kern.cu"};
    db.add_command("/tmp", "/tmp/kern.cu", arguments);

    CommandOptions options;
    llvm::SmallVector<std::string> remove = {"--allow-unsupported-compiler"};
    options.remove = remove;

    auto commands = db.lookup("/tmp/kern.cu", options);
    ASSERT_EQ(commands.size(), std::size_t(1));

    // Probe flags all parse as the shared unknown id; removal keys on the
    // spelling, so the other probe tokens survive.
    auto& flags = commands[0].resolved.flags;
    auto has = [&](llvm::StringRef flag) {
        return std::ranges::contains(flags, flag);
    };
    EXPECT_FALSE(has("--allow-unsupported-compiler"));
    EXPECT_TRUE(has("-ccbin=/usr/bin/g++-12"));
    EXPECT_TRUE(has("--target-directory=sbsa-linux"));
}

TEST_CASE(RemoveListAlternatives) {
    CompilationDatabase db;
    std::vector<const char*> arguments = {"nvcc",
                                          "-ccbin=/usr/bin/g++-12",
                                          "-default-stream=per-thread",
                                          "-c",
                                          "/tmp/kern.cu"};
    db.add_command("/tmp", "/tmp/kern.cu", arguments);

    // Remove patterns are alternatives, not one command: every value of the
    // same stateful option becomes a pattern, where nvcc's last-wins over
    // the whole list would keep only the final one and leave the base's
    // g++-12 token in place.
    CommandOptions options;
    llvm::SmallVector<std::string> remove = {"-ccbin=/usr/bin/g++-13",
                                             "-ccbin=/usr/bin/g++-12",
                                             "--default-stream=per-thread"};
    options.remove = remove;

    auto commands = db.lookup("/tmp/kern.cu", options);
    ASSERT_EQ(commands.size(), std::size_t(1));

    auto& flags = commands[0].resolved.flags;
    auto has = [&](llvm::StringRef flag) {
        return std::ranges::contains(flags, flag);
    };
    EXPECT_FALSE(has("-ccbin=/usr/bin/g++-12"));
    EXPECT_FALSE(has("CUDA_API_PER_THREAD_DEFAULT_STREAM=1"));
}

TEST_CASE(WildcardRemovesProbeValue) {
    CompilationDatabase db;
    std::vector<const char*> arguments =
        {"nvcc", "-ccbin=/usr/bin/g++-12", "-target-dir", "sbsa-linux", "-c", "/tmp/kern.cu"};
    db.add_command("/tmp", "/tmp/kern.cu", arguments);

    auto probe_flags = [&](llvm::ArrayRef<std::string> remove) {
        CommandOptions options;
        options.remove = remove;
        auto commands = db.lookup("/tmp/kern.cu", options);
        std::vector<std::string> result;
        for(llvm::StringRef flag: commands[0].resolved.flags) {
            if(is_nvcc_probe_flag(flag)) {
                result.push_back(flag.str());
            }
        }
        return result;
    };

    // A probe token's identity is its whole spelling; `=*` wildcards the
    // value so the rule clears the concrete host compiler it never spelled.
    llvm::SmallVector<std::string> ccbin = {"-ccbin=*"};
    std::vector<std::string> target_only = {"--target-directory=sbsa-linux"};
    EXPECT_EQ(probe_flags(ccbin), target_only);

    llvm::SmallVector<std::string> target = {"--target-directory=*"};
    std::vector<std::string> ccbin_only = {"-ccbin=/usr/bin/g++-12"};
    EXPECT_EQ(probe_flags(target), ccbin_only);
}

};  // TEST_SUITE(NVCCTests)
}  // namespace
}  // namespace clice::testing
