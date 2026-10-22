module;

#include "modules/prelude.h"

module clice:command.toolchains.gcc;

import :command.toolchains.clang;

namespace clice {

/// One directory of a GCC driver's header search list.
struct GCCSearchDir {
    std::string path;

    /// Listed as a macOS framework directory.
    bool framework = false;
};

/// What a GCC-style driver prints when asked to preprocess an empty file
/// verbosely, printing its macros (`driver <flags> -E -dM -v`): the job it
/// runs and the language standard it compiles in. A driver that is clang
/// underneath (Apple clang, zig, emcc, icx, a `c++` that is clang) shows a
/// clang job; GCC shows its own cc1 job, its target and its header search
/// list.
struct GCCStyleOutput {
    /// The clang job: its program, -cc1 and its arguments as printed;
    /// empty for GCC.
    std::vector<std::string> clang_job;

    /// The compiler proper GCC ran (cc1, cc1plus) as its job names it;
    /// empty when it ran none. The fields up to `private_dir` are GCC's.
    std::string gcc_job;

    /// The target triple, from the `Target:` line, with the width
    /// `__riscv_xlen` names when it names a RISC-V target of no width
    /// (xPack's `riscv-none-elf`), which clang cannot read.
    std::string target;

    /// The options GCC's driver passed its cc1 job that reach clang's
    /// driver too: macros (`-D_REENTRANT` from the specs), target defaults
    /// (`-march=armv7-a+fp -mthumb`), hardening (`-fstack-protector-strong`).
    std::vector<std::string> gcc_options;

    /// GCC's `#include <...>` search list, in order.
    std::vector<GCCSearchDir> search_dirs;

    /// The entry of `search_dirs` that is GCC's own private include
    /// directory (lib/gcc/<target>/<version>/include), whose place clang's
    /// builtin headers take; none under -nostdinc.
    std::optional<std::size_t> private_dir;

    /// The language standard the driver compiles in, as a -std value clang
    /// knows.
    std::string standard;
};

/// Read the output from the program's stdout (`out`) and stderr (`err`).
GCCStyleOutput parse_gcc_style_output(llvm::StringRef out, llvm::StringRef err);

/// Runs a GCC-style driver over `args`, the driver (its first
/// `driver_tokens`: two for `zig cc`) followed by its flags and the input:
/// `-E -dM -v` has it preprocess the input verbosely, printing the job it
/// runs and its predefined macros. A clang job that then fails still
/// answers: the command it runs is what clice compiles, and fails the same
/// way there (an unknown `--target`). A GCC job answers only when it
/// succeeds: GCC prints its macros and LIBRARY_PATH only then.
kota::task<std::expected<GCCStyleOutput, std::string>>
    run_gcc_style(llvm::ArrayRef<std::string> args, std::size_t driver_tokens, std::string cwd);

/// The job clang's own driver builds for `args` (a driver, its flags and
/// the input) set up as GCC set itself up: GCC's target, the options GCC's
/// driver gave its cc1 job that clang's driver knows, and GCC's header
/// search list in place of clang's own idea of the system headers. clang's
/// builtin headers take the place of GCC's own: those are GCC's versions of
/// the same headers, which clang cannot parse, and clang's would reach them
/// through #include_next (clang's stdatomic.h defers to a later one).
std::expected<Job, std::string> query_as_gcc(const GCCStyleOutput& output,
                                             llvm::ArrayRef<std::string> args,
                                             llvm::StringRef cwd);

/// A GCC-style driver, which every driver but cl and nvcc is: asked how it
/// compiles the input, it shows its job, and the job tells clang underneath (Apple clang, zig,
/// emcc, a `c++` that is clang) from GCC. There is neither for an input the
/// driver only passes on (assembler), or a driver that is neither GCC nor
/// clang (Intel's icc).
kota::task<std::expected<Job, std::string>> ask_gcc_style(std::vector<std::string> args,
                                                          std::size_t driver_tokens,
                                                          std::string cwd);

/// Whether clang knows the warning option `-W<value>`, read the way its
/// frontend reads one (clang/lib/Basic/Warnings.cpp): a warning group,
/// negated or not, or one of the forms that name none.
bool clang_warning(llvm::StringRef value);

}  // namespace clice
