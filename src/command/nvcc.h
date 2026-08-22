#pragma once

#include <expected>
#include <string>
#include <vector>

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"

namespace clice {

/// clang's option table has no spelling for nvcc's `-ccbin`, so the host
/// compiler travels through the canonical command as this verbatim token
/// (`-ccbin=<path>`), written by the CDB translation below and consumed by
/// the NVCC toolchain query.
constexpr inline llvm::StringLiteral nvcc_ccbin_prefix = "-ccbin=";

/// An nvcc driver command rewritten into spellings clang's option table
/// parses, so the regular CDB classification (canonical / user-content /
/// discarded) applies unchanged.
struct NVCCCommand {
    /// The driver followed by translated flags: `-gencode`/`-arch`/`-code`
    /// become the single best `--cuda-gpu-arch=`, `-x cu` becomes `-x cuda`,
    /// `-Xcompiler` values are unwrapped into plain host flags, long-form
    /// preprocessor aliases become their short spellings, and macro-defining
    /// toggles (`--expt-extended-lambda`, ...) become their `-D` effect.
    /// nvcc options that cannot affect parsing are dropped, together with
    /// their values when those could be mistaken for flags (`-Xptxas -O3`).
    /// A `-ccbin=<path>` token is appended when a host compiler was named.
    std::vector<std::string> arguments;
};

NVCCCommand translate_nvcc_command(llvm::ArrayRef<const char*> arguments);

/// What one `nvcc --dryrun` run reveals about the toolchain. The dryrun
/// prints the whole compilation pipeline (host preprocess, cudafe++, device
/// preprocess, cicc, ...) without executing it — CMake detects CUDA
/// toolchains from the same output.
struct NVCCDryrunInfo {
    /// The toolkit root (`TOP=` line), valid for clang's `--cuda-path`.
    /// Deriving it from the nvcc binary's location instead is wrong for
    /// split layouts (conda puts it under `targets/<triple>`).
    std::string cuda_path;

    /// argv[0] of the host preprocess line — the compiler nvcc actually
    /// drives, resolved from `-ccbin`, environment, or its defaults. Often
    /// a bare program name that only exists on `search_path`.
    std::string host_compiler;

    /// The PATH nvcc augments for its sub-commands (`PATH=` line) — where a
    /// bare `host_compiler` resolves. Layouts that activate an environment
    /// around nvcc (conda) keep the host toolchain here, not on our PATH.
    std::vector<std::string> search_path;

    /// nvcc's default C++ dialect (`cudafe++ --c++17` → "c++17"), applied
    /// when the user command names none.
    std::string cpp_dialect;

    /// nvcc's default GPU architecture (`cicc -arch compute_52` → "sm_52"),
    /// applied when the user command names none.
    std::string default_arch;

    /// Macros nvcc injects into the host and device preprocess (without the
    /// `-D`), minus the ones clang derives itself: `__CUDACC__`,
    /// `__CUDA_ARCH__` and `__CUDA_ARCH_LIST__` come from the language mode
    /// and `--cuda-gpu-arch`, and redefining them would conflict. The rest
    /// (`__CUDACC_VER_MAJOR__`, ...) clang never defines, yet headers gate
    /// on them — CUTLASS keeps every SM90 path invisible without them.
    std::vector<std::string> host_defines;
    std::vector<std::string> device_defines;
};

std::expected<NVCCDryrunInfo, std::string> parse_nvcc_dryrun(llvm::StringRef output);

}  // namespace clice
