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
                                   llvm::StringRef directory = "") {
    return translate_nvcc_command(arguments, directory);
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
    for(llvm::StringRef flag: {"--allow-unsupported-compiler",
                               "--target-directory=sbsa-linux",
                               "-ccbin=/base/tools/g++"}) {
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

    // Stateful options are last-wins, matching nvcc.
    EXPECT_FALSE(contains(translate({"nvcc", "-rdc=true", "-rdc=false"}), "-fgpu-rdc"));
    auto stream = translate({"nvcc", "-default-stream=per-thread", "-default-stream=legacy"});
    EXPECT_FALSE(contains(stream, "-DCUDA_API_PER_THREAD_DEFAULT_STREAM=1"));

    // Synthetic macros render ahead of user flags, so a later -U can undo
    // them the way it does under nvcc.
    auto undef = llvm::join(translate({"nvcc", "-dc", "-U__CUDACC_RDC__"}), " ");
    EXPECT_TRUE(llvm::StringRef(undef).find("-D__CUDACC_RDC__") <
                llvm::StringRef(undef).find("-U __CUDACC_RDC__"));
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

    // Backslash escapes the next character unconditionally: `\,` is a
    // literal comma, `\\,` is a trailing backslash then a separator.
    EXPECT_TRUE(contains(translate({"nvcc", R"(-DP=a\,b)"}), "P=a,b"));
    auto parity = translate({"nvcc", R"(-DQ=a\\,b)"});
    EXPECT_TRUE(contains(parity, R"(Q=a\)"));
    EXPECT_TRUE(contains(parity, "b"));
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

    // An unreadable file drops with a warning; the rest of the command
    // still translates.
    auto missing = translate({"nvcc", "--options-file=missing.rsp", "-DX"}, "/clice-nonexistent");
    EXPECT_TRUE(contains(missing, "X"));

    fs::remove(*file);
}

TEST_CASE(StdNormalized) {
    EXPECT_TRUE(contains(translate({"nvcc", "-std", "c++17"}), "-std=c++17"));
    EXPECT_TRUE(contains(translate({"nvcc", "--std=c++20"}), "-std=c++20"));
}

constexpr static llvm::StringRef fake_dryrun = R"(#$ _NVVM_BRANCH_=nvvm
#$ _SPACE_=
#$ TOP=/opt/cuda/targets/x86_64-linux
#$ NVVMIR_LIBRARY_DIR=/opt/cuda/targets/x86_64-linux/nvvm/libdevice
#$ LD_LIBRARY_PATH=/opt/cuda/targets/x86_64-linux/lib:
#$ PATH=/opt/cuda/targets/x86_64-linux/bin:/opt/host/bin:/usr/bin
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
}

TEST_CASE(DryrunRejectsIncomplete) {
    EXPECT_FALSE(parse_nvcc_dryrun("#$ PATH=/usr/bin").has_value());
    EXPECT_FALSE(parse_nvcc_dryrun("#$ TOP=/opt/cuda").has_value());
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

};  // TEST_SUITE(NVCCTests)
}  // namespace
}  // namespace clice::testing
