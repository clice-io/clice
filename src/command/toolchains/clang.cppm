module;

#include "support/prelude.h"

module clice:command.toolchains.clang;

namespace clice {

/// A cc1 job as a driver runs it: the program, `-cc1`, then its arguments.
using Job = std::vector<std::string>;

/// The job clang's own driver builds for `args` (a driver, its flags and
/// the input) when run in `cwd`, the process's own when empty.
std::expected<Job, std::string> query_driver(llvm::ArrayRef<std::string> args, llvm::StringRef cwd);

/// The job clang's own driver builds for `args`, with `mode` (what the
/// driver's name does not tell clang) ahead of the command's own flags.
std::expected<Job, std::string> ask_clang(llvm::ArrayRef<std::string> args,
                                          llvm::ArrayRef<std::string> mode,
                                          llvm::StringRef cwd);

/// A driver clice does not query: clang's own driver stands in for it, as a
/// clang installed under its name would — the name's mode (`g++` compiles
/// C++) and target prefix (`arm-none-eabi-gcc`, when its architecture is
/// one clang knows), clang's defaults for the rest.
std::expected<Job, std::string> ask_clang_for(std::vector<std::string> args,
                                              std::size_t driver_tokens,
                                              llvm::StringRef cwd);

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
