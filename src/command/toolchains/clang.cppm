module;

#include "support/prelude.h"

module clice:command.toolchains.clang;

namespace clice {

/// A cc1 job as a driver runs it: the program, `-cc1`, then its arguments.
using Job = std::vector<std::string>;

/// The job clang's own driver builds for `args` (a driver, its flags and
/// the input) when run in `cwd`, the process's own when empty. The errors
/// the driver reports though it builds one are logged unless `quiet`.
std::expected<Job, std::string> query_driver(llvm::ArrayRef<std::string> args,
                                             llvm::StringRef cwd,
                                             bool quiet = false);

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

}  // namespace clice
