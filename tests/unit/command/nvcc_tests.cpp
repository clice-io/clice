#include <algorithm>

#include "test/test.h"
#include "command/command.h"
#include "command/nvcc.h"
#include "command/toolchain.h"

namespace clice::testing {
namespace {

using namespace std::string_view_literals;

TEST_SUITE(NVCCTests) {

std::vector<std::string> translate(std::vector<const char*> arguments) {
    return translate_nvcc_command(arguments).arguments;
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
    EXPECT_TRUE(contains(args, "-DMY_FLAG=1"));
    EXPECT_TRUE(contains(args, "-x"));
    EXPECT_TRUE(contains(args, "cuda"));
    EXPECT_FALSE(contains(args, "cu"));
    for(llvm::StringRef arg: args) {
        EXPECT_FALSE(arg.starts_with("--generate-code"));
    }
}

TEST_CASE(GencodeSelectsBest) {
    // The newest architecture wins; 'a' outranks 'f' and plain at the same
    // number; lto_ entries are intermediates, not architectures.
    auto args = translate({"nvcc",
                           "-gencode",
                           "arch=compute_75,code=sm_75",
                           "-gencode",
                           "arch=compute_90a,code=[compute_90a,sm_90a,lto_120]",
                           "-gencode=arch=compute_80,code=sm_80"});
    EXPECT_TRUE(contains(args, "--cuda-gpu-arch=sm_90a"));

    auto compute_only = translate({"nvcc", "-arch=compute_86"});
    EXPECT_TRUE(contains(compute_only, "--cuda-gpu-arch=sm_86"));

    auto family = translate({"nvcc", "-code=sm_100f,sm_100"});
    EXPECT_TRUE(contains(family, "--cuda-gpu-arch=sm_100f"));

    EXPECT_FALSE(contains(translate({"nvcc", "-c", "a.cu"}), "--cuda-gpu-arch=sm_52"));
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

    auto off = translate({"nvcc", "-rdc=false"});
    EXPECT_FALSE(contains(off, "-fgpu-rdc"));
}

TEST_CASE(PairedValueDrops) {
    // The wrapped values would parse as host flags on their own; bare
    // nvcc-only flags pass through for the CDB classification to discard.
    auto args = translate(
        {"nvcc", "-Xptxas", "-O3", "-t", "4", "--options-file", "extra.rsp", "-lineinfo"});
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
