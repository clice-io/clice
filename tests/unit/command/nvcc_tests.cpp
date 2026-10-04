#include <algorithm>

#include "test/cdb_helper.h"
#include "test/test.h"
#include "command/command.h"
#include "command/nvcc.h"
#include "command/toolchain.h"
#include "vfs/path.h"

namespace clice::testing {
namespace {

using namespace std::string_view_literals;

ZEST_SUITE(NVCCTests) {

std::vector<std::string> translate(std::vector<const char*> arguments,
                                   llvm::StringRef directory = "",
                                   bool edit = false) {
    return translate_nvcc_command(arguments, directory, edit);
}

bool contains(llvm::ArrayRef<std::string> arguments, llvm::StringRef flag) {
    return std::ranges::contains(arguments, flag);
}

ZEST_CASE(TranslateCMakeShape) {
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

    ZEXPECT(args[0] == "nvcc"sv);
    ZEXPECT(contains(args, "--cuda-gpu-arch=sm_75"));
    ZEXPECT(contains(args, "MY_FLAG=1"));
    ZEXPECT(contains(args, "-x"));
    ZEXPECT(contains(args, "cuda"));
    ZEXPECT(!contains(args, "cu"));
    for(llvm::StringRef arg: args) {
        ZEXPECT(!arg.starts_with("--generate-code"));
    }
}

ZEST_CASE(GencodeSelectsBest) {
    // Only arch= clauses count: code= entries are ptxas targets and never
    // set __CUDA_ARCH__ (arch=compute_75,code=sm_90 preprocesses as 750).
    auto mismatch = translate({"nvcc", "-gencode", "arch=compute_75,code=sm_90"});
    ZEXPECT(contains(mismatch, "--cuda-gpu-arch=sm_75"));

    // The newest architecture wins; 'a' outranks plain at the same number;
    // lto_ entries are intermediates, not architectures.
    auto multi = translate({"nvcc",
                            "-gencode",
                            "arch=compute_75,code=sm_75",
                            "-gencode=arch=compute_90a,code=[compute_90a,sm_90a,lto_120]"});
    ZEXPECT(contains(multi, "--cuda-gpu-arch=sm_90a"));

    ZEXPECT(contains(translate({"nvcc", "-arch=compute_86"}), "--cuda-gpu-arch=sm_86"));
    ZEXPECT(contains(translate({"nvcc", "-arch=sm_100f"}), "--cuda-gpu-arch=sm_100f"));

    // -arch is a scalar option: the last one wins, unlike -gencode which
    // accumulates.
    auto repeated = translate({"nvcc", "-arch=compute_80", "-arch=compute_75"});
    ZEXPECT(contains(repeated, "--cuda-gpu-arch=sm_75"));

    // Bare -code and arch-less commands pin no architecture.
    for(auto& args: {translate({"nvcc", "-code=sm_90"}), translate({"nvcc", "-c", "a.cu"})}) {
        for(llvm::StringRef arg: args) {
            ZEXPECT(!arg.starts_with("--cuda-gpu-arch="));
        }
    }
}

ZEST_CASE(ArchGencodeUnion) {
    // nvcc accepts -arch next to -gencode and compiles their union, one
    // device pass each — the newest wins whichever side carries it.
    auto arch_newer = translate({"nvcc", "-gencode=arch=compute_75,code=sm_75", "-arch=sm_90"});
    ZEXPECT(contains(arch_newer, "--cuda-gpu-arch=sm_90"));

    auto gencode_newer = translate({"nvcc", "-arch=sm_75", "-gencode=arch=compute_90,code=sm_90"});
    ZEXPECT(contains(gencode_newer, "--cuda-gpu-arch=sm_90"));
}

ZEST_CASE(SpecialArchCarried) {
    // Non-numeric selections only nvcc can resolve persist as probe tokens
    // for the dryrun instead of vanishing into no architecture at all.
    for(auto arguments: {
            std::vector<const char*>{"nvcc", "-arch=native"},
            std::vector<const char*>{"nvcc", "-arch", "native"},
            std::vector<const char*>{"nvcc", "--gpu-architecture=native"}
    }) {
        auto args = translate(arguments);
        ZEXPECT(contains(args, "-arch=native"));
        ZEXPECT(!contains(args, "native"));
    }
    ZEXPECT(is_nvcc_probe_flag("-arch=native"));
    ZEXPECT(contains(translate({"nvcc", "-arch=all"}), "-arch=all"));

    // -arch stays last-wins across numeric and special values.
    auto numeric_wins = translate({"nvcc", "-arch=native", "-arch=sm_80"});
    ZEXPECT(contains(numeric_wins, "--cuda-gpu-arch=sm_80"));
    ZEXPECT(!contains(numeric_wins, "-arch=native"));
    auto special_wins = translate({"nvcc", "-arch=sm_80", "-arch=native"});
    ZEXPECT(contains(special_wins, "-arch=native"));
    ZEXPECT(!contains(special_wins, "--cuda-gpu-arch=sm_80"));

    // As an edit it clears the base architectures like a numeric choice.
    auto edit = translate({"nvcc", "-arch=native"}, "", true);
    auto clear = std::ranges::find(edit, "--no-offload-arch=all");
    ZASSERT((clear != edit.end() && clear + 1 != edit.end()));
    ZEXPECT(*(clear + 1) == "-arch=native"sv);
}

ZEST_CASE(CcbinBecomesToken) {
    for(auto arguments: {
            std::vector<const char*>{"nvcc", "-ccbin", "/usr/bin/g++-12"},
            std::vector<const char*>{"nvcc", "-ccbin=/usr/bin/g++-12"},
            std::vector<const char*>{"nvcc", "--compiler-bindir", "/usr/bin/g++-12"}
    }) {
        auto args = translate(arguments);
        ZEXPECT(contains(args, "-ccbin=/usr/bin/g++-12"));
        ZEXPECT(!contains(args, "/usr/bin/g++-12"));
    }
}

ZEST_CASE(ProbeFlagsCarried) {
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
        ZEXPECT(contains(args, flag));
        ZEXPECT(is_nvcc_probe_flag(flag));
    }
    ZEXPECT(!is_nvcc_probe_flag("-I/base"));

    // A bare name resolves on PATH like nvcc would — never anchored — while
    // dot-relative values are directory-relative.
    ZEXPECT(contains(translate({"nvcc", "-ccbin=g++-13"}, "/base"), "-ccbin=g++-13"));
    auto dot = translate({"nvcc", "-ccbin=."}, "/base");
    ZEXPECT(std::ranges::any_of(dot, [](llvm::StringRef arg) {
        return arg.starts_with("-ccbin=/base");
    }));
}

ZEST_CASE(XcompilerUnwrapped) {
    auto args = translate({"nvcc", "-Xcompiler=-fPIC,-pthread", "-Xcompiler", "-Wall"});
    ZEXPECT(contains(args, "-fPIC"));
    ZEXPECT(contains(args, "-pthread"));
    ZEXPECT(contains(args, "-Wall"));
    ZEXPECT(!contains(args, "-Xcompiler"));

    // The value follows the same `\,` escape as every other list option.
    auto escaped = translate({"nvcc", R"(-Xcompiler=-Wl\,-z\,defs)"});
    ZEXPECT(contains(escaped, "-Wl,-z,defs"));
}

ZEST_CASE(MacroToggles) {
    auto args = translate({"nvcc",
                           "--expt-relaxed-constexpr",
                           "--extended-lambda",
                           "-rdc=true",
                           "-default-stream",
                           "per-thread"});
    ZEXPECT(contains(args, "-D__CUDACC_RELAXED_CONSTEXPR__"));
    ZEXPECT(contains(args, "-D__CUDACC_EXTENDED_LAMBDA__"));
    ZEXPECT(contains(args, "-fgpu-rdc"));
    ZEXPECT(contains(args, "-D__CUDACC_RDC__"));
    ZEXPECT(contains(args, "-DCUDA_API_PER_THREAD_DEFAULT_STREAM=1"));

    ZEXPECT(!contains(translate({"nvcc", "-rdc=false"}), "-fgpu-rdc"));

    // Separate compilation implies -rdc=true; -ewp has its own macro.
    auto dc = translate({"nvcc", "-dc"});
    ZEXPECT(contains(dc, "-fgpu-rdc"));
    ZEXPECT(contains(dc, "-D__CUDACC_RDC__"));
    ZEXPECT(contains(translate({"nvcc", "--extensible-whole-program"}), "-D__CUDACC_EWP__"));

    // Device debug becomes its macro; -G must not survive, clang reads it
    // as the small-data-threshold option.
    auto debug = translate({"nvcc", "-G"});
    ZEXPECT(contains(debug, "-D__CUDACC_DEBUG__"));
    ZEXPECT(!contains(debug, "-G"));
    ZEXPECT(contains(translate({"nvcc", "--device-debug"}), "-D__CUDACC_DEBUG__"));

    // Fast math becomes clang's approx-transcendentals flag, which selects
    // the same fast variants in the math wrapper.
    for(const char* spelling: {"--use_fast_math", "-use_fast_math"}) {
        auto fast = translate({"nvcc", spelling});
        ZEXPECT(contains(fast, "-fgpu-approx-transcendentals"));
        ZEXPECT(!contains(fast, spelling));
    }

    // Stateful options are last-wins, matching nvcc.
    ZEXPECT(!contains(translate({"nvcc", "-rdc=true", "-rdc=false"}), "-fgpu-rdc"));
    auto stream = translate({"nvcc", "-default-stream=per-thread", "-default-stream=legacy"});
    ZEXPECT(!contains(stream, "-DCUDA_API_PER_THREAD_DEFAULT_STREAM=1"));

    // Synthetic macros render ahead of user flags, so a later -U can undo
    // them the way it does under nvcc.
    auto undef = llvm::join(translate({"nvcc", "-dc", "-U__CUDACC_RDC__"}), " ");
    ZEXPECT(llvm::StringRef(undef).find("-D__CUDACC_RDC__") <
            llvm::StringRef(undef).find("-U __CUDACC_RDC__"));

    // The stream macro is nvcc's one exception: it lands after user flags,
    // so their -U cannot undo it.
    auto stream_undef = llvm::join(
        translate({"nvcc", "-default-stream=per-thread", "-UCUDA_API_PER_THREAD_DEFAULT_STREAM"}),
        " ");
    ZEXPECT(llvm::StringRef(stream_undef).find("-U CUDA_API_PER_THREAD_DEFAULT_STREAM") <
            llvm::StringRef(stream_undef).find("-DCUDA_API_PER_THREAD_DEFAULT_STREAM=1"));
}

ZEST_CASE(PairedValueDrops) {
    // The wrapped values would parse as host flags on their own; bare
    // nvcc-only flags pass through for the CDB classification to discard.
    auto args = translate({"nvcc", "-Xptxas", "-O3", "-t", "4", "-lineinfo"});
    std::vector<std::string> expected = {"nvcc", "-lineinfo"};
    ZEXPECT(args == expected);
}

ZEST_CASE(LongFormAliases) {
    auto args = translate({"nvcc",
                           "--include-path=/opt/inc",
                           "--define-macro",
                           "FOO=1",
                           "--undefine-macro=BAR",
                           "--pre-include",
                           "config.h",
                           "--system-include=/opt/sys"});
    auto joined = llvm::join(args, " ");
    ZEXPECT(llvm::StringRef(joined).contains("-I /opt/inc"));
    ZEXPECT(llvm::StringRef(joined).contains("-D FOO=1"));
    ZEXPECT(llvm::StringRef(joined).contains("-U BAR"));
    ZEXPECT(llvm::StringRef(joined).contains("-include config.h"));
    ZEXPECT(llvm::StringRef(joined).contains("-isystem /opt/sys"));
}

ZEST_CASE(ListValuesSplit) {
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
        ZEXPECT(llvm::StringRef(joined).contains(piece));
    }

    // `\,` reads as a literal comma; other backslashes stay verbatim so
    // native Windows paths survive (deliberately shallower than nvcc's
    // Linux-side escape processing, which consumes every backslash).
    ZEXPECT(contains(translate({"nvcc", R"(-DP=a\,b)"}), "P=a,b"));
    auto windows = translate({"nvcc", R"(-IC:\inc,D:\other)"});
    ZEXPECT(contains(windows, R"(C:\inc)"));
    ZEXPECT(contains(windows, R"(D:\other)"));
}

ZEST_CASE(OptionsFileExpanded) {
    auto file = vfs::temp_file("clice-nvcc", "rsp");
    ZASSERT(file);
    ZASSERT(!vfs::write(*file, "-Igenerated -DAPI=2 -std=c++20\n"));

    auto args = translate({"nvcc", "--options-file", file->c_str()});
    auto joined = llvm::join(args, " ");
    ZEXPECT(llvm::StringRef(joined).contains("-I generated"));
    ZEXPECT(llvm::StringRef(joined).contains("-D API=2"));
    ZEXPECT(contains(args, "-std=c++20"));
    ZEXPECT(!contains(args, "--options-file"));
    ZEXPECT(!llvm::StringRef(joined).contains(*file));

    // The value is a comma-separated file list: every element expands.
    auto second = vfs::temp_file("clice-nvcc", "rsp");
    ZASSERT(second);
    ZASSERT(!vfs::write(*second, "-DFROM_SECOND=2\n"));

    auto pair = *file + "," + *second;
    auto both = llvm::join(translate({"nvcc", "-optf", pair.c_str()}), " ");
    ZEXPECT(llvm::StringRef(both).contains("-D API=2"));
    ZEXPECT(llvm::StringRef(both).contains("-D FROM_SECOND=2"));
    ZEXPECT(!llvm::StringRef(both).contains("-optf"));
    ZEXPECT(!llvm::StringRef(both).contains(*file));
    ZEXPECT(!llvm::StringRef(both).contains(*second));

    // An unreadable file drops with a warning; the rest of the command
    // still translates.
    auto missing = translate({"nvcc", "--options-file=missing.rsp", "-DX"}, "/clice-nonexistent");
    ZEXPECT(contains(missing, "X"));
    ZEXPECT(!contains(missing, "--options-file=missing.rsp"));
    ZEXPECT(!contains(missing, "missing.rsp"));

    vfs::remove(*file);
    vfs::remove(*second);
}

ZEST_CASE(OptionsFileMarkSkipped) {
    // A byte order mark does not glue itself to the first option.
    auto file = vfs::temp_file("clice-nvcc", "rsp");
    ZASSERT(file);
    ZASSERT(!vfs::write(*file, "\xEF\xBB\xBF-DMARKED=1\n"));
    auto joined = llvm::join(translate({"nvcc", "--options-file", file->c_str()}), " ");
    ZEXPECT(llvm::StringRef(joined).contains("-D MARKED=1"));
    vfs::remove(*file);
}

ZEST_CASE(StdNormalized) {
    ZEXPECT(contains(translate({"nvcc", "-std", "c++17"}), "-std=c++17"));
    ZEXPECT(contains(translate({"nvcc", "--std=c++20"}), "-std=c++20"));
}

ZEST_CASE(MachineNormalized) {
    for(auto arguments: {
            std::vector<const char*>{"nvcc", "--machine=64"},
            std::vector<const char*>{"nvcc", "--machine", "64"},
            std::vector<const char*>{"nvcc", "-m=64"},
            std::vector<const char*>{"nvcc", "-m", "64"}
    }) {
        auto args = translate(arguments);
        ZEXPECT(contains(args, "-m64"));
        ZEXPECT(!contains(args, "64"));
    }
    ZEXPECT(contains(translate({"nvcc", "--machine=32"}), "-m32"));

    // The joined short form is already clang's own spelling.
    ZEXPECT(contains(translate({"nvcc", "-m64"}), "-m64"));

    // A value nvcc rejects pins no machine model.
    auto invalid = translate({"nvcc", "--machine=16"});
    ZEXPECT(!contains(invalid, "-m16"));
    ZEXPECT(!contains(invalid, "--machine=16"));
}

ZEST_CASE(OptimizeNormalized) {
    ZEXPECT(contains(translate({"nvcc", "--optimize=3"}), "-O3"));
    ZEXPECT(contains(translate({"nvcc", "-O", "2"}), "-O2"));
    ZEXPECT(contains(translate({"nvcc", "-O3"}), "-O3"));

    // Joined -O3 is nvcc's own option too, last-wins like the long form.
    auto repeated = translate({"nvcc", "-O2", "--optimize=3"});
    ZEXPECT(contains(repeated, "-O3"));
    ZEXPECT(!contains(repeated, "-O2"));
}

ZEST_CASE(HostFlagsFollowXcompiler) {
    // nvcc places its own -O/-m after the -Xcompiler payloads on the host
    // line, beating them regardless of input order.
    auto find_order = [](std::vector<std::string> args, llvm::StringRef a, llvm::StringRef b) {
        auto joined = llvm::join(args, " ");
        return llvm::StringRef(joined).find(a) < llvm::StringRef(joined).find(b);
    };
    ZEXPECT(find_order(translate({"nvcc", "--optimize=3", "-Xcompiler=-O0"}), "-O0", "-O3"));
    ZEXPECT(find_order(translate({"nvcc", "-O3", "-Xcompiler=-O0"}), "-O0", "-O3"));
    ZEXPECT(find_order(translate({"nvcc", "--machine=64", "-Xcompiler=-m32"}), "-m32", "-m64"));
}

ZEST_CASE(DisableWarningsMapped) {
    for(const char* spelling: {"--disable-warnings", "-disable-warnings"}) {
        auto args = translate({"nvcc", spelling});
        ZEXPECT(contains(args, "-w"));
        ZEXPECT(!contains(args, spelling));
    }
}

ZEST_CASE(EditEmitsStateOverrides) {
    // An edit lands after an already-translated base command: explicitly
    // disabled stateful options must cancel the base's translated state,
    // while a standalone command emits nothing for the default state.
    auto off = translate({"nvcc", "-rdc=false", "--default-stream=legacy"}, "", true);
    ZEXPECT(contains(off, "-fno-gpu-rdc"));
    ZEXPECT(contains(off, "-U__CUDACC_RDC__"));
    ZEXPECT(contains(off, "-UCUDA_API_PER_THREAD_DEFAULT_STREAM"));

    auto standalone = translate({"nvcc", "-rdc=false", "--default-stream=legacy"});
    ZEXPECT(!contains(standalone, "-fno-gpu-rdc"));
    ZEXPECT(!contains(standalone, "-U__CUDACC_RDC__"));
    ZEXPECT(!contains(standalone, "-UCUDA_API_PER_THREAD_DEFAULT_STREAM"));

    // The cancellations render ahead of the segment's own flags: an
    // explicit -D of the macro inside the edit survives them, like it
    // survives nvcc's absent injection.
    auto explicit_d = llvm::join(
        translate({"nvcc", "-DCUDA_API_PER_THREAD_DEFAULT_STREAM=7", "--default-stream=legacy"},
                  "",
                  true),
        " ");
    ZEXPECT(llvm::StringRef(explicit_d).find("-UCUDA_API_PER_THREAD_DEFAULT_STREAM") <
            llvm::StringRef(explicit_d).find("CUDA_API_PER_THREAD_DEFAULT_STREAM=7"));

    // Untouched state stays silent even as an edit.
    auto untouched = translate({"nvcc", "-DX"}, "", true);
    ZEXPECT(!contains(untouched, "-fno-gpu-rdc"));
    ZEXPECT(!contains(untouched, "-U__CUDACC_RDC__"));
    ZEXPECT(!contains(untouched, "-UCUDA_API_PER_THREAD_DEFAULT_STREAM"));
    ZEXPECT(!contains(untouched, "--no-offload-arch=all"));

    // clang accumulates --cuda-gpu-arch while nvcc's -arch is last-wins: an
    // arch edit clears the base architectures right before its own choice.
    auto arch = translate({"nvcc", "-arch=sm_80"}, "", true);
    auto clear = std::ranges::find(arch, "--no-offload-arch=all");
    ZASSERT((clear != arch.end() && clear + 1 != arch.end()));
    ZEXPECT(*(clear + 1) == "--cuda-gpu-arch=sm_80"sv);
    ZEXPECT(!contains(translate({"nvcc", "-arch=sm_80"}), "--no-offload-arch=all"));

    // -gencode accumulates in nvcc, so as an edit it adds its architecture
    // without erasing the base's.
    auto gencode = translate({"nvcc", "-gencode=arch=compute_75,code=sm_75"}, "", true);
    ZEXPECT(contains(gencode, "--cuda-gpu-arch=sm_75"));
    ZEXPECT(!contains(gencode, "--no-offload-arch=all"));
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

ZEST_CASE(DryrunParsed) {
    auto info = parse_nvcc_dryrun(fake_dryrun);
    ZASSERT(info);

    ZEXPECT(info->cuda_path == "/opt/cuda/targets/x86_64-linux");
    ZEXPECT(info->host_compiler == "g++");
    ZEXPECT(info->cpp_dialect == "c++17");
    ZEXPECT(info->default_arch == "sm_52");
    ZEXPECT(std::ranges::contains(info->search_path, "/opt/host/bin"));

    // clang derives the blocklisted three itself; the rest must survive.
    for(auto defines: {&info->host_defines, &info->device_defines}) {
        ZEXPECT(std::ranges::contains(*defines, "__CUDACC_VER_MAJOR__=12"));
        ZEXPECT(std::ranges::contains(*defines, "__NV_LEGACY_LAUNCH"));
        ZEXPECT(!std::ranges::contains(*defines, "__CUDACC__"));
        for(llvm::StringRef define: *defines) {
            ZEXPECT(!define.starts_with("__CUDA_ARCH__"));
            ZEXPECT(!define.starts_with("__CUDA_ARCH_LIST__"));
        }
    }
    ZEXPECT(std::ranges::contains(info->device_defines, "CUDA_DOUBLE_MATH_FUNCTIONS"));
    ZEXPECT(!std::ranges::contains(info->host_defines, "CUDA_DOUBLE_MATH_FUNCTIONS"));

    // A probe carrying -arch=all runs one cicc per architecture; the newest
    // wins regardless of emission order.
    constexpr llvm::StringRef multi = R"(#$ g++ -E -x c++ "/tmp/a.cu" -o "/tmp/a.ii"
#$ cicc --c++17 -arch compute_90 "/tmp/a.cpp1.ii" -o "/tmp/a.ptx"
#$ cicc --c++17 -arch compute_75 "/tmp/a.cpp1.ii" -o "/tmp/b.ptx"
)";
    auto multi_info = parse_nvcc_dryrun(multi);
    ZASSERT(multi_info);
    ZEXPECT(multi_info->default_arch == "sm_90");
}

ZEST_CASE(DryrunRejectsIncomplete) {
    ZEXPECT(!parse_nvcc_dryrun("#$ PATH=/usr/bin").has_value());
    ZEXPECT(!parse_nvcc_dryrun("#$ TOP=/opt/cuda").has_value());
}

ZEST_CASE(DryrunHostOnly) {
    // A host-language input (nvcc -c foo.cpp) has no preprocess stage: the
    // single host compile line names the compiler, and its defines survive
    // unfiltered — outside CUDA mode clang derives none of them.
    constexpr llvm::StringRef host_only = R"(#$ TOP=/opt/cuda
#$ INCLUDES="-I/opt/cuda/include"
#$ g++ -D__CUDA_ARCH_LIST__=520 -c -x c++ -D__NVCC__ -D__CUDACC_VER_MAJOR__=12 -m64 "/tmp/a.cpp" -o "/tmp/a.o"
)";
    auto info = parse_nvcc_dryrun(host_only);
    ZASSERT(info);
    ZEXPECT(info->host_compiler == "g++");
    ZEXPECT(std::ranges::contains(info->host_defines, "__NVCC__"));
    ZEXPECT(std::ranges::contains(info->host_defines, "__CUDA_ARCH_LIST__=520"));
    ZEXPECT(std::ranges::contains(info->host_defines, "__CUDACC_VER_MAJOR__=12"));
    ZEXPECT(info->device_defines.empty());
}

ZEST_CASE(DryrunTopFallback) {
    // A wrapper may swallow TOP=; NVVMIR_LIBRARY_DIR is <root>/nvvm/libdevice.
    constexpr llvm::StringRef no_top = R"(#$ NVVMIR_LIBRARY_DIR=/opt/cuda/nvvm/libdevice
#$ g++ -D__CUDACC_VER_MAJOR__=12 -E -x c++ "/tmp/a.cu" -o "/tmp/a.ii"
)";
    auto info = parse_nvcc_dryrun(no_top);
    ZASSERT(info);
    ZEXPECT(info->cuda_path == "/opt/cuda");

    // With neither line the toolchain still resolves; CUDA detection is
    // left to clang's own search.
    auto bare = parse_nvcc_dryrun(R"(#$ g++ -E -x c++ "/tmp/a.cu")");
    ZASSERT(bare);
    ZEXPECT(bare->cuda_path.empty());
    ZEXPECT(bare->host_compiler == "g++");
}

ZEST_CASE(CcbinAffectsKey) {
    /// -ccbin persists as an unknown probe token; two commands differing
    /// only in host compiler must not share a probe.
    FileTable file_table;
    CompilationDatabase db{file_table};
    db.add_command("/tmp", "/tmp/a.cu", "nvcc -ccbin=/usr/bin/g++-12 -c /tmp/a.cu"sv);
    db.add_command("/tmp", "/tmp/b.cu", "nvcc -ccbin=/usr/bin/g++-13 -c /tmp/b.cu"sv);

    auto key_of = [&](llvm::StringRef file) {
        auto& entry = db.candidate_entries(file).front();
        return db.toolchain().probe_key_for(entry.config, db.input_kind(entry.config, file));
    };
    ZEXPECT(key_of("/tmp/a.cu") != key_of("/tmp/b.cu"));
    ZEXPECT(key_of("/tmp/a.cu") == key_of("/tmp/a.cu"));
}

ZEST_CASE(DatabaseTranslatesNVCC) {
    FileTable file_table;
    CompilationDatabase db{file_table};
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

    // Probe tokens (-ccbin, ...) are unknown-class: visible in the full
    // view and the probe, never in the compile render.
    auto flags = db.render_full(db.candidate_entries("/tmp/kern.cu").front().config);
    auto has = [&](llvm::StringRef flag) {
        return std::ranges::contains(flags, flag);
    };
    // --cuda-gpu-arch renders through its unaliased spelling.
    ZEXPECT(has("--offload-arch=sm_75"));
    ZEXPECT(has("-ccbin=/usr/bin/g++-12"));
    ZEXPECT(has("--allow-unsupported-compiler"));
    ZEXPECT(has("-x"));
    ZEXPECT(has("cuda"));
    ZEXPECT(has("MY_FLAG=1"));
    /// nvcc-only leftovers are unknown-class: full view keeps them for
    /// identity, the compile render must not pass them to clang.
    ZEXPECT(!std::ranges::contains(render_entry(db, "/tmp/kern.cu"),
                                   llvm::StringRef("-forward-unknown-to-host-compiler")));
    for(llvm::StringRef flag: flags) {
        ZEXPECT(!flag.starts_with("--generate-code"));
    }
}

ZEST_CASE(RuleFlagsTranslated) {
    FileTable file_table;
    CompilationDatabase db{file_table};
    std::vector<const char*> arguments = {"nvcc",
                                          "--generate-code=arch=compute_75,code=sm_75",
                                          "-c",
                                          "/tmp/kern.cu"};
    db.add_command("/tmp", "/tmp/kern.cu", arguments);

    // Config rule flags for an NVCC entry go through the same translation
    // as the command: the remove matches the arch through its translated
    // spelling and the append reaches clang translated, not as raw nvcc
    // tokens.
    std::vector<CommandEdit> edits = {
        {CommandEdit::Kind::Remove, {"-gencode", "arch=compute_75,code=sm_75"}},
        {CommandEdit::Kind::Append,
         {"--extended-lambda", "--generate-code=arch=compute_90a,code=sm_90a"}},
    };
    CommandOptions options{.edits = edits};

    auto flags = db.render_full(
        db.apply_rules(db.candidate_entries("/tmp/kern.cu").front().config, options));
    auto has = [&](llvm::StringRef flag) {
        return std::ranges::contains(flags, flag);
    };
    ZEXPECT(!has("--offload-arch=sm_75"));
    ZEXPECT(has("--offload-arch=sm_90a"));
    ZEXPECT(has("__CUDACC_EXTENDED_LAMBDA__"));
    for(llvm::StringRef flag: flags) {
        ZEXPECT(!flag.starts_with("--generate-code"));
        ZEXPECT(!flag.starts_with("--extended-lambda"));
    }
}

ZEST_CASE(AppendOverridesBase) {
    FileTable file_table;
    CompilationDatabase db{file_table};
    std::vector<const char*> arguments =
        {"nvcc", "-rdc=true", "-default-stream=per-thread", "-arch=sm_75", "-c", "/tmp/kern.cu"};
    db.add_command("/tmp", "/tmp/kern.cu", arguments);

    // NVCC's stateful options are last-wins across the whole command, so an
    // appended disable must beat the state the base already translated.
    std::vector<CommandEdit> edits = {
        {CommandEdit::Kind::Append, {"-rdc=false", "--default-stream=legacy", "-arch=sm_80"}},
    };
    CommandOptions options{.edits = edits};

    auto flags = render_entry(db, "/tmp/kern.cu", options);
    auto count = std::ptrdiff_t(flags.size());
    auto index_of = [&](llvm::StringRef flag) {
        return std::ranges::find(flags,
                                 flag,
                                 [](const char* arg) { return llvm::StringRef(arg); }) -
               flags.begin();
    };
    /// Defines and undefs render as two tokens; locate the value preceded
    /// by the given option token.
    auto pair_index = [&](llvm::StringRef option_token, llvm::StringRef value) {
        for(std::ptrdiff_t i = 0; i + 1 < count; i += 1) {
            if(llvm::StringRef(flags[i]) == option_token &&
               llvm::StringRef(flags[i + 1]) == value) {
                return i;
            }
        }
        return count;
    };

    // rdc: the appended negation follows the base's enable, so clang's own
    // last-wins turns it off and undefines the macro the base defined.
    ZEXPECT(index_of("-fgpu-rdc") < index_of("-fno-gpu-rdc"));
    ZEXPECT(index_of("-fno-gpu-rdc") < count);
    ZEXPECT(pair_index("-D", "__CUDACC_RDC__") < pair_index("-U", "__CUDACC_RDC__"));
    ZEXPECT(pair_index("-U", "__CUDACC_RDC__") < count);

    // stream: undef after the base's define.
    ZEXPECT(pair_index("-D", "CUDA_API_PER_THREAD_DEFAULT_STREAM=1") <
            pair_index("-U", "CUDA_API_PER_THREAD_DEFAULT_STREAM"));
    ZEXPECT(pair_index("-U", "CUDA_API_PER_THREAD_DEFAULT_STREAM") < count);

    // arch: the base's architecture is cleared before the appended one, not
    // accumulated into a second device pass.
    ZEXPECT(index_of("--offload-arch=sm_75") < index_of("--no-offload-arch=all"));
    ZEXPECT(index_of("--no-offload-arch=all") < index_of("--offload-arch=sm_80"));
    ZEXPECT(index_of("--offload-arch=sm_80") < count);
}

ZEST_CASE(ExtrasStayClangDialect) {
    FileTable file_table;
    CompilationDatabase db{file_table};
    std::vector<const char*> arguments = {"nvcc", "-rdc=true", "-c", "/tmp/kern.cu"};
    db.add_command("/tmp", "/tmp/kern.cu", arguments);

    // A lint plan's extra args are clang args by definition (clang-tidy
    // semantics): on an NVCC entry they join the translated command
    // verbatim, never through the nvcc rule translation.
    CommandOptions options;
    llvm::SmallVector<std::string> prepend = {"-fno-gpu-rdc"};
    llvm::SmallVector<std::string> append = {"-fgpu-rdc"};
    options.extra_prepend = prepend;
    options.extra_append = append;

    auto flags = render_entry(db, "/tmp/kern.cu", options);
    ZEXPECT(llvm::StringRef(flags[1]) == "-fno-gpu-rdc");
    /// The input sits at its slot after the extra append.
    ZEXPECT(llvm::StringRef(flags[flags.size() - 2]) == "-fgpu-rdc");
}

ZEST_CASE(GencodeAppendAccumulates) {
    FileTable file_table;
    CompilationDatabase db{file_table};
    std::vector<const char*> arguments = {"nvcc",
                                          "--generate-code=arch=compute_90,code=sm_90",
                                          "-c",
                                          "/tmp/kern.cu"};
    db.add_command("/tmp", "/tmp/kern.cu", arguments);

    auto arch_flags = [&](std::vector<std::string> append) {
        std::vector<CommandEdit> edits = {
            {CommandEdit::Kind::Append, std::move(append)}
        };
        CommandOptions options{.edits = edits};
        std::vector<std::string> result;
        for(llvm::StringRef flag: render_entry(db, "/tmp/kern.cu", options)) {
            if(flag.contains("arch")) {
                result.push_back(flag.str());
            }
        }
        return result;
    };

    // Appended -gencode entries accumulate onto the base's like nvcc's own,
    // and the newest architecture keeps winning: an older append changes
    // nothing.
    std::vector<std::string> older = {"-gencode=arch=compute_75,code=sm_75"};
    auto kept = arch_flags(older);
    ZEXPECT(std::ranges::contains(kept, "--offload-arch=sm_90"));
    ZEXPECT(!std::ranges::contains(kept, "--offload-arch=sm_75"));
    ZEXPECT(!std::ranges::contains(kept, "--no-offload-arch=all"));

    // A newer append takes over — by numeric rank, not string order, which
    // would sort sm_100a below sm_90.
    std::vector<std::string> newer = {"-gencode=arch=compute_100a,code=sm_100a"};
    auto switched = arch_flags(newer);
    ZEXPECT(std::ranges::contains(switched, "--offload-arch=sm_100a"));
    ZEXPECT(!std::ranges::contains(switched, "--offload-arch=sm_90"));
}

ZEST_CASE(CollapseHonorsNegatives) {
    using K = ArchFlagKind;

    // A specific --no-offload-arch erases only its matches from the ranking;
    // the negated flag pair stays for clang to consume, and the newest of
    // the survivors wins (dropping the rest).
    auto dropped = collapse_gpu_archs({
        {
         {K::GpuArch, "sm_90"},
         {K::NoOffloadArch, "sm_90"},
         {K::GpuArch, "sm_75"},
         {K::GpuArch, "sm_86"},
         }
    });
    ZASSERT(dropped);
    ZASSERT(dropped->size() == 1U);
    ZEXPECT((*dropped)[0] == 2U);

    // A single survivor leaves nothing to collapse.
    auto single = collapse_gpu_archs({
        {
         {K::GpuArch, "sm_90"},
         {K::NoOffloadArch, "sm_90"},
         {K::GpuArch, "sm_75"},
         }
    });
    ZASSERT(single);
    ZEXPECT(single->empty());

    // An unrankable negative leaves the whole command to clang.
    auto native = collapse_gpu_archs({
        {
         {K::GpuArch, "sm_90"},
         {K::NoOffloadArch, "native"},
         {K::GpuArch, "sm_75"},
         }
    });
    ZEXPECT(!native.has_value());
}

ZEST_CASE(WildcardRemoveClearsArch) {
    FileTable file_table;
    CompilationDatabase db{file_table};
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
        auto applied = db.apply_rules(db.candidate_entries(file).front().config, options);
        std::vector<std::string> result;
        for(llvm::StringRef flag: db.render_full(applied)) {
            if(flag.contains("arch")) {
                result.push_back(flag.str());
            }
        }
        return result;
    };

    ZEXPECT(std::ranges::contains(arch_flags("/tmp/kern.cu", {}), "--offload-arch=sm_75"));
    ZEXPECT(std::ranges::contains(arch_flags("/tmp/other.cu", {}), "--offload-arch=sm_80"));
    ZEXPECT(std::ranges::contains(arch_flags("/tmp/native.cu", {}), "-arch=native"));

    // The wildcard must clear whichever form the base carries: numeric archs
    // translate to --offload-arch, non-numeric ones persist as probe tokens.
    std::vector<CommandEdit> edits = {
        {CommandEdit::Kind::Remove, {"--generate-code=*"}}
    };
    CommandOptions options{.edits = edits};
    ZEXPECT(arch_flags("/tmp/kern.cu", options).empty());

    edits = {
        {CommandEdit::Kind::Remove, {"-arch", "*"}}
    };
    options.edits = edits;
    ZEXPECT(arch_flags("/tmp/other.cu", options).empty());
    ZEXPECT(arch_flags("/tmp/native.cu", options).empty());

    // Removes edit the base before appends land: replacing the architecture
    // through remove-wildcard + append keeps the appended one.
    edits.push_back({CommandEdit::Kind::Append, {"-gencode=arch=compute_90a,code=sm_90a"}});
    options.edits = edits;
    auto replaced = arch_flags("/tmp/other.cu", options);
    ZEXPECT(!std::ranges::contains(replaced, "--offload-arch=sm_80"));
    ZEXPECT(std::ranges::contains(replaced, "--offload-arch=sm_90a"));
}

ZEST_CASE(RemoveMatchesUnknownSpelling) {
    FileTable file_table;
    CompilationDatabase db{file_table};
    std::vector<const char*> arguments = {"nvcc",
                                          "-ccbin=/usr/bin/g++-12",
                                          "-allow-unsupported-compiler",
                                          "-target-dir",
                                          "sbsa-linux",
                                          "-c",
                                          "/tmp/kern.cu"};
    db.add_command("/tmp", "/tmp/kern.cu", arguments);

    std::vector<CommandEdit> edits = {
        {CommandEdit::Kind::Remove, {"--allow-unsupported-compiler"}},
    };
    CommandOptions options{.edits = edits};

    // Probe flags all parse as the shared unknown id; removal keys on the
    // spelling, so the other probe tokens survive.
    auto flags = db.render_full(
        db.apply_rules(db.candidate_entries("/tmp/kern.cu").front().config, options));
    auto has = [&](llvm::StringRef flag) {
        return std::ranges::contains(flags, flag);
    };
    ZEXPECT(!has("--allow-unsupported-compiler"));
    ZEXPECT(has("-ccbin=/usr/bin/g++-12"));
    ZEXPECT(has("--target-directory=sbsa-linux"));
}

ZEST_CASE(RemoveListAlternatives) {
    FileTable file_table;
    CompilationDatabase db{file_table};
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
    std::vector<CommandEdit> edits = {
        {CommandEdit::Kind::Remove,
         {"-ccbin=/usr/bin/g++-13", "-ccbin=/usr/bin/g++-12", "--default-stream=per-thread"}},
    };
    CommandOptions options{.edits = edits};

    auto flags = db.render_full(
        db.apply_rules(db.candidate_entries("/tmp/kern.cu").front().config, options));
    auto has = [&](llvm::StringRef flag) {
        return std::ranges::contains(flags, flag);
    };
    ZEXPECT(!has("-ccbin=/usr/bin/g++-12"));
    ZEXPECT(!has("CUDA_API_PER_THREAD_DEFAULT_STREAM=1"));
}

ZEST_CASE(WildcardRemovesProbeValue) {
    FileTable file_table;
    CompilationDatabase db{file_table};
    std::vector<const char*> arguments =
        {"nvcc", "-ccbin=/usr/bin/g++-12", "-target-dir", "sbsa-linux", "-c", "/tmp/kern.cu"};
    db.add_command("/tmp", "/tmp/kern.cu", arguments);

    auto probe_flags = [&](std::vector<std::string> remove) {
        std::vector<CommandEdit> edits = {
            {CommandEdit::Kind::Remove, std::move(remove)}
        };
        CommandOptions options{.edits = edits};
        auto applied = db.apply_rules(db.candidate_entries("/tmp/kern.cu").front().config, options);
        std::vector<std::string> result;
        for(llvm::StringRef flag: db.render_full(applied)) {
            if(is_nvcc_probe_flag(flag)) {
                result.push_back(flag.str());
            }
        }
        return result;
    };

    // A probe token's identity is its whole spelling; `=*` wildcards the
    // value so the rule clears the concrete host compiler it never spelled.
    std::vector<std::string> ccbin = {"-ccbin=*"};
    std::vector<std::string> target_only = {"--target-directory=sbsa-linux"};
    ZEXPECT(probe_flags(ccbin) == target_only);

    std::vector<std::string> target = {"--target-directory=*"};
    std::vector<std::string> ccbin_only = {"-ccbin=/usr/bin/g++-12"};
    ZEXPECT(probe_flags(target) == ccbin_only);
}

};  // ZEST_SUITE(NVCCTests)

}  // namespace
}  // namespace clice::testing
