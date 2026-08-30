#pragma once

#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"

namespace clice {

/// nvcc options only nvcc itself can resolve (`-ccbin=<path>`,
/// `--allow-unsupported-compiler`, `--target-directory=<name>`, and
/// non-numeric architecture selections like `-arch=native`): clang's
/// option table has no spelling for them, so the translation re-emits them
/// as normalized verbatim tokens. They survive CDB classification into the
/// canonical command — keying the toolchain cache — and the NVCC query
/// forwards them to its `nvcc --dryrun` probe so the probe runs against the
/// build's real toolchain.
bool is_nvcc_probe_flag(llvm::StringRef arg);

/// Rewrite an nvcc driver command into flags clang's option table parses,
/// so the regular CDB classification (canonical / user-content / discarded)
/// applies unchanged:
///
/// - The `-gencode` entries and the (last-wins) `-arch` choice — nvcc
///   compiles their union, one device pass each — collapse to the single
///   best `--cuda-gpu-arch=`. Only `arch=` clauses count: they name the
///   virtual architecture the device front end compiles for, while `code=`
///   entries are ptxas targets that never set `__CUDA_ARCH__`. The newest
///   wins; at equal number 'a' > 'f' > plain ('a' unlocks e.g. Hopper
///   GMMA/TMA). A non-numeric `-arch` value (`native`, `all`, ...)
///   persists as an `-arch=<value>` probe token instead — the dryrun
///   resolves it to a concrete architecture.
/// - Preprocessor options split their comma-separated values, short
///   spellings included: `-Ia,b` names two directories, `-DA=1,B=2` two
///   macros.
/// - `-Xcompiler` values unwrap into plain host flags, `-x cu` becomes
///   `-x cuda`, and macro-defining toggles (`--extended-lambda`,
///   `-rdc=true`, `-dc`, `-ewp`, `--default-stream per-thread`, ...) become
///   their exact macro effect. `--use_fast_math` becomes clang's
///   `-fgpu-approx-transcendentals`, which selects the same fast
///   transcendentals in the math wrapper.
/// - `--options-file` response files are expanded in place, resolved
///   against `directory` like a relative `-ccbin` — nvcc resolves both
///   against its working directory.
/// - Remaining nvcc-only options are dropped, together with their values
///   when those could be mistaken for host flags (`-Xptxas -O3`).
///
/// With `edit` set the flags are a config-rule edit appended after an
/// already-translated base command, not a self-contained command: stateful
/// options set to their default state emit the flags that cancel the base's
/// translated state instead of nothing (`-rdc=false` becomes `-fno-gpu-rdc
/// -U__CUDACC_RDC__`, `--default-stream=legacy` undefines the per-thread
/// macro). An `-arch` edit reads as picking the view outright — nvcc
/// itself would union it with the base's `-gencode` entries — and is
/// preceded by `--no-offload-arch=all`; a `-gencode` entry accumulates
/// like nvcc's own, emitted bare for `collapse_gpu_arch_flags` to resolve
/// against the base's.
std::vector<std::string> translate_nvcc_command(llvm::ArrayRef<const char*> arguments,
                                                llvm::StringRef directory,
                                                bool edit = false);

/// Collapse the architecture flags of a combined command (translated base
/// plus appended edits) so only the single best survives, extending the
/// newest-wins policy of the translation across the append seam. Left
/// accumulated, clang would build one device job per architecture in
/// ascending order and the toolchain query reads the first — pinning the
/// oldest architecture instead of the newest.
///
/// The caller hands the arch-flag values in command order (the value of
/// each --cuda-gpu-arch= / --offload-arch= / --no-offload-arch= argument)
/// and erases the returned indices. nullopt means a value outside the
/// ranking (a raw clang spelling like --offload-arch=native) was present —
/// leave the whole command to clang's own semantics.
enum class ArchFlagKind : std::uint8_t {
    GpuArch,
    OffloadArch,
    NoOffloadArch,
};

std::optional<llvm::SmallVector<std::size_t>>
    collapse_gpu_archs(llvm::ArrayRef<std::pair<ArchFlagKind, llvm::StringRef>> sequence);

/// What one `nvcc --dryrun` run reveals about the toolchain. The dryrun
/// prints the whole compilation pipeline (host preprocess, cudafe++, device
/// preprocess, cicc, ...) without executing it — CMake detects CUDA
/// toolchains from the same output.
struct NVCCDryrunInfo {
    /// The toolkit root (`TOP=` line, else derived from
    /// `NVVMIR_LIBRARY_DIR=`), valid for clang's `--cuda-path`; empty when
    /// the dryrun names neither. Deriving it from the nvcc binary's location
    /// instead is wrong for split layouts (conda puts it under
    /// `targets/<triple>`).
    std::string cuda_path;

    /// argv[0] of the host preprocess line (or of the single host compile
    /// line when the input is host-language) — the compiler nvcc actually
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
