module;

#include "support/prelude.h"

module clice:command.toolchains.clang;

namespace clice {

/// A cc1 job as a driver runs it.
struct Job {
    /// The program, `-cc1`, then its arguments.
    std::vector<std::string> args;

    /// The search directories of `args` that hold another compiler's
    /// builtin headers, clang's own right before each: GCC's private
    /// include directory. clang's take the place of theirs (see
    /// Toolchain::Frontend::hidden).
    std::vector<std::string> shadowed;
};

/// The job clang's own driver builds for `args` (a driver, its flags and
/// the input) when run in `cwd`, the process's own when empty. The errors
/// the driver reports though it builds one go to `errors`, else the log.
std::expected<Job, std::string> query_driver(llvm::ArrayRef<std::string> args,
                                             llvm::StringRef cwd,
                                             std::vector<std::string>* errors = nullptr);

/// `args` (a driver and its flags) as clang's own driver is handed them,
/// with `mode` (what the driver's name does not tell clang) ahead of the
/// command's own flags.
std::vector<std::string> clang_argv(llvm::ArrayRef<std::string> args,
                                    llvm::ArrayRef<std::string> mode);

/// The job clang's own driver builds for clang_argv(args, mode).
std::expected<Job, std::string> ask_clang(llvm::ArrayRef<std::string> args,
                                          llvm::ArrayRef<std::string> mode,
                                          llvm::StringRef cwd);

/// The CPU, tuning and ABI names clang's target for a triple knows. A name
/// it does not know fails the whole compile: GCC accepts names of its own
/// (`-mtune=intel`, `-mabi=lp64` on Darwin), and a newer external clang
/// knows newer CPUs.
class TargetNames {
public:
    explicit TargetNames(llvm::StringRef triple);

    /// Whether the target knows the name the cc1 option `id` gives
    /// (-target-cpu, -tune-cpu, -target-abi); true for any other option,
    /// and for a target clang does not know.
    bool knows(unsigned id, llvm::StringRef name);

private:
    clang::DiagnosticOptions diagnostic_options;

    clang::DiagnosticsEngine engine;

    /// The target keeps a pointer to its options.
    clang::TargetOptions target_options;

    llvm::IntrusiveRefCntPtr<clang::TargetInfo> target;
};

/// The first clang job a driver prints under `-v`: its program, -cc1 and
/// its arguments as printed. Empty when there is none.
std::vector<std::string> parse_clang_job(llvm::StringRef output);

/// The -std value for the macros a driver predefines: `__cplusplus` names
/// the C++ standard, `__STDC_VERSION__` the C one, and `__STRICT_ANSI__`
/// tells the ISO mode from the GNU one. A value between two standards
/// (GCC's c++2b is 202100L) counts as the newer. Empty when neither names
/// a standard: an assembler input, or C89, the default of no GCC since 5.
std::string standard_from_macros(llvm::StringRef cplusplus,
                                 llvm::StringRef stdc_version,
                                 bool strict);

bool names_standard(llvm::ArrayRef<std::string> args);

/// A flag of a compiler that is not clang (GCC, cl, nvcc) as clang takes
/// it, nothing when nothing of it stays: the warnings that compiler makes
/// errors stay warnings, `-Werror` goes and `-Werror=<group>` is
/// `-W<group>`. clang warns of what that compiler does not, and the build
/// never shows those as errors.
std::optional<std::string> warnings_not_errors(llvm::StringRef arg);

}  // namespace clice
